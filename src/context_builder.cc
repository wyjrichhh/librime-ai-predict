// Copyright RIME Developers
// Distributed under the BSD License

#include "context_builder.h"

#include <algorithm>
#include <vector>
#include <utf8.h>

#include <rime/commit_history.h>
#include <rime/context.h>
#include <rime/engine.h>

namespace rime {
namespace predict {

namespace {

string StripSpaces(const string& s) {
  string out = s;
  out.erase(std::remove(out.begin(), out.end(), ' '), out.end());
  return out;
}

bool IsPunctChar(const string& ch) {
  static const char* puncts[] = {
      "\xe3\x80\x82", "\xef\xbc\x8c", "\xef\xbc\x81", "\xef\xbc\x9f",
      "\xef\xbc\x9b", "\xef\xbc\x9a", "\xe3\x80\x81", "\xe2\x80\xa6",
      ".", ",", "!", "?", ";", ":"};
  for (const auto* p : puncts) {
    if (ch == p)
      return true;
  }
  return false;
}

string StripAllPunctuation(const string& text) {
  string result;
  auto it = text.begin();
  while (it != text.end()) {
    auto start = it;
    utf8::next(it, text.end());
    string ch(start, it);
    if (!IsPunctChar(ch))
      result += ch;
  }
  return result;
}

size_t Utf8CharCount(const string& text) {
  return static_cast<size_t>(utf8::distance(text.begin(), text.end()));
}

/// If model output echoes a known prefix, return only the new suffix for display.
string ExtractCurrentPrediction(const string& llm_result,
                                const string& window_text) {
  if (window_text.empty()) {
    return llm_result;
  }
  string clean = StripAllPunctuation(llm_result);
  size_t window_chars = Utf8CharCount(window_text);
  size_t clean_chars = Utf8CharCount(clean);
  if (clean_chars <= window_chars) {
    return string();
  }
  auto it = clean.begin();
  for (size_t i = 0; i < window_chars && it != clean.end(); ++i) {
    utf8::next(it, clean.end());
  }
  string llm_prefix(clean.begin(), it);
  if (llm_prefix != window_text) {
    return string();
  }
  return string(it, clean.end());
}

/// Walk commit history (most recent first, capped at `max_records`) and
/// reconstruct the Chinese context window plus the most recent leading
/// punctuation (if any).
///
/// Skip policy (denylist):
///   - "punct" / "thru" records never enter `window_text` (not semantic
///     Chinese context; the most recent one is captured separately into
///     `last_punct` so the display layer can strip it from the model
///     output).
///   - "raw" records never enter `window_text` either. librime emits "raw"
///     when a segment has no translation candidate and the user commits the
///     literal ASCII (e.g. typed `quickstart` then hit Return), or when a
///     translator calls Engine::CommitText directly. Feeding this ASCII
///     string into a Chinese LLM as context causes the model to faithfully
///     replay it as a prefix (e.g. window="quickstart" + pinyin="baoliu" →
///     model output "QUICKSTART保留"), which then either leaks into the
///     candidate verbatim or breaks the prefix-strip in
///     ExtractCurrentPrediction (case/encoding mismatch).
///   - Everything else, including our own previously committed "ai_predict"
///     candidates, is treated as ordinary user-confirmed content. Rationale:
///     committing an AI suggestion requires an explicit user keypress, so by
///     the time it lands in commit_history it is semantically equivalent to
///     any other Hanzi commit -- excluding it would discard exactly the
///     coherence signal we want the next prediction to build on.
void BuildWindowContext(Context* ctx,
                        int max_records,
                        string* window_text_out,
                        string* last_punct_out) {
  if (!ctx || !window_text_out)
    return;
  const CommitHistory& history = ctx->commit_history();

  // Snapshot the most recent commit record once: if (and only if) it is of
  // type punct/thru we remember its text as `last_punct`. Anything else
  // (e.g. a Hanzi commit) leaves last_punct empty -- we only want to strip a
  // punctuation that is *immediately* adjacent to the new pinyin input, not
  // an arbitrary historical one.
  if (last_punct_out && !history.empty()) {
    const auto& back = *history.rbegin();
    if (back.type == "punct" || back.type == "thru") {
      *last_punct_out = back.text;
    }
  }

  std::vector<string> recent_commits;
  size_t collected = 0;
  for (auto it = history.rbegin();
       it != history.rend() && collected < static_cast<size_t>(max_records);
       ++it) {
    if (it->type == "punct" || it->type == "thru" || it->type == "raw")
      continue;
    if (it->text.empty())
      continue;
    recent_commits.push_back(it->text);
    ++collected;
  }
  std::reverse(recent_commits.begin(), recent_commits.end());

  string& out = *window_text_out;
  out.clear();
  for (const auto& text : recent_commits) {
    out += text;  // No separator: model expects continuous Chinese text.
  }
}

}  // namespace

std::optional<PredictionContext> ContextBuilder::Build(
    Engine* engine,
    const string& raw_input,
    const ContextBuilderOptions& opt) {
  string prompt = StripSpaces(raw_input);
  if (prompt.empty()) {
    return std::nullopt;
  }
  int threshold = opt.min_effective_length > 0 ? opt.min_effective_length : 6;

  string window_text;
  string last_punct;
  if (engine && engine->context()) {
    BuildWindowContext(engine->context(), opt.context_window_size,
                       &window_text, &last_punct);
  }

  // Single-threshold mode-selection:
  //   - prompt.length >= threshold → direct mode: pinyin alone is descriptive
  //     enough; intentionally drop window_text to avoid the model being
  //     pulled toward an older topic.
  //   - prompt.length <  threshold → windowed mode: short pinyin needs the
  //     committed Chinese prefix to disambiguate. If no context exists, give
  //     up (the model would only hallucinate from a 2-3 letter fragment).
  bool windowed = static_cast<int>(prompt.length()) < threshold;
  if (windowed && window_text.empty()) {
    return std::nullopt;
  }
  if (!windowed) {
    window_text.clear();
  }

  PredictionContext ctx;
  ctx.effective_prompt = prompt;
  ctx.window_text = window_text;
  ctx.last_punct = last_punct;
  ctx.windowed = windowed;
  // Cache key is "window_text|prompt" so the same pinyin under different
  // upstream contexts gets separate cache entries. The pipe is safe because
  // neither component contains a literal '|' (window_text is Hanzi, prompt
  // is a-z only).
  ctx.cache_key = window_text + "|" + prompt;
  ctx.ct2_input = window_text + "<pinyin_start>" + prompt + "</pinyin_start>";
  return ctx;
}

string ExtractDisplayText(const string& model_output,
                          const string& window_text_for_extract,
                          const string& last_punct) {
  string display;
  if (!window_text_for_extract.empty()) {
    display = ExtractCurrentPrediction(model_output, window_text_for_extract);
  } else {
    display = model_output;
  }
  // Strip a leading punctuation that the model echoed from the user's most
  // recent commit (e.g. user just typed "，", model returns "，你好" → display
  // "你好"). Done after windowed prefix-stripping so the comparison is against
  // the final display text, not the raw model output that may still contain
  // window_text.
  if (!last_punct.empty() && display.length() >= last_punct.length() &&
      display.compare(0, last_punct.length(), last_punct) == 0) {
    display = display.substr(last_punct.length());
  }
  return display;
}

}  // namespace predict
}  // namespace rime
