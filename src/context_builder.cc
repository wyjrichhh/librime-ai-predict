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

/// Walk commit history (most recent first, capped at `max_records`) and
/// reconstruct the Chinese context window.
///
/// Skip policy (denylist):
///   - "punct" / "thru" records never enter `window_text` (not semantic
///     Chinese context).
///   - "raw" records never enter `window_text` either. librime emits "raw"
///     when a segment has no translation candidate and the user commits the
///     literal ASCII (e.g. typed `quickstart` then hit Return), or when a
///     translator calls Engine::CommitText directly. Feeding this ASCII
///     string into a Chinese LLM as context causes the model to faithfully
///     replay it as a prefix (e.g. window="quickstart" + pinyin="baoliu" →
///     model output "QUICKSTART保留"), which then leaks into the candidate
///     verbatim.
///   - Everything else, including our own previously committed "ai_predict"
///     candidates, is treated as ordinary user-confirmed content. Rationale:
///     committing an AI suggestion requires an explicit user keypress, so by
///     the time it lands in commit_history it is semantically equivalent to
///     any other Hanzi commit -- excluding it would discard exactly the
///     coherence signal we want the next prediction to build on.
void BuildWindowContext(Context* ctx,
                        int max_records,
                        string* window_text_out) {
  if (!ctx || !window_text_out)
    return;
  const CommitHistory& history = ctx->commit_history();

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
  int threshold = opt.min_effective_length > 0 ? opt.min_effective_length : 12;

  string window_text;
  if (engine && engine->context()) {
    BuildWindowContext(engine->context(), opt.context_window_size,
                       &window_text);
  }

  // Trigger policy (context-first):
  //   - If `window_text` is non-empty, ALWAYS feed it to the model regardless
  //     of prompt length -- the committed Hanzi prefix is the strongest signal
  //     we have, and dropping it for "long" prompts (the previous behavior)
  //     made mid-length inputs like `chuangkou + 现在在什么情况下会` lose all
  //     coherence and produce dictionary-style noise like `（创客）`.
  //   - If `window_text` is empty (cold start, just after BackSpace/Return,
  //     or only punct/thru/raw in history), require `prompt.length >=
  //     threshold` so the model has enough to chew on; otherwise skip.
  bool has_context = !window_text.empty();
  if (!has_context && static_cast<int>(prompt.length()) < threshold) {
    return std::nullopt;
  }

  PredictionContext ctx;
  ctx.effective_prompt = prompt;
  ctx.window_text = window_text;
  ctx.windowed = has_context;
  // Cache key is "window_text|prompt" so the same pinyin under different
  // upstream contexts gets separate cache entries. The pipe is safe because
  // neither component contains a literal '|' (window_text is Hanzi, prompt
  // is a-z only).
  ctx.cache_key = window_text + "|" + prompt;
  ctx.ct2_input = window_text + "<pinyin_start>" + prompt + "</pinyin_start>";
  return ctx;
}

string ExtractDisplayText(const string& model_output) {
  return StripAllPunctuation(model_output);
}

}  // namespace predict
}  // namespace rime
