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

// True iff `s` is a usable pinyin prompt: at least one lowercase letter and
// nothing but [a-z] plus the syllable-separator apostrophe (e.g. "xi'an").
// librime hands us a fresh segment for every keystroke, including punctuation
// (",", ".", "?"), ASCII symbols ("+", "/", "\\", "_"), and literal uppercase
// acronyms ("SSO"). None are pinyin; the CT2 model can only echo them back as
// garbage (window="...日志" + prompt="," -> candidate ","), so we reject them
// before they reach the trigger policy.
bool IsPinyinPrompt(const string& s) {
  bool has_letter = false;
  for (char c : s) {
    if (c >= 'a' && c <= 'z') {
      has_letter = true;
    } else if (c != '\'') {
      return false;
    }
  }
  return has_letter;
}

// True iff `cp` is a punctuation or symbol code point the model may emit but
// that must never appear in a candidate. We match by Unicode RANGE rather than
// an enumerated denylist: the CT2 model can emit any CJK punctuation it has
// seen in training (parentheses, brackets, quotes, dashes, ...), and a hand
// listed set inevitably misses some -- e.g. it previously let the fullwidth
// parenthesis `）`(U+FF09) leak into candidates like `编译）`. Covering the
// whole block keeps new symbols (「」【】《》 etc.) handled for free.
bool IsPunctCodepoint(uint32_t cp) {
  // ASCII punctuation we care about (keep [a-zA-Z0-9] and apostrophe intact,
  // those are handled by the gaps between these ranges).
  if (cp == '.' || cp == ',' || cp == '!' || cp == '?' || cp == ';' ||
      cp == ':')
    return true;
  // CJK Symbols and Punctuation: 。、《》「」『』【】〈〉… etc.
  if (cp >= 0x3000 && cp <= 0x303F)
    return true;
  // Fullwidth ASCII forms: ！＂＃…（）…：；？ etc. (U+FF01..U+FF0F,
  // U+FF1A..U+FF20, U+FF3B..U+FF40, U+FF5B..U+FF65). Covers fullwidth
  // parentheses （）, comma ，, colon ：, semicolon ；, ! ? and friends.
  if ((cp >= 0xFF01 && cp <= 0xFF0F) || (cp >= 0xFF1A && cp <= 0xFF20) ||
      (cp >= 0xFF3B && cp <= 0xFF40) || (cp >= 0xFF5B && cp <= 0xFF65))
    return true;
  // General Punctuation: “ ” ‘ ’ … — – etc. (U+2010..U+205E covers dashes,
  // quotation marks, the horizontal ellipsis U+2026).
  if (cp >= 0x2010 && cp <= 0x205E)
    return true;
  return false;
}

string StripAllPunctuation(const string& text) {
  string result;
  auto it = text.begin();
  while (it != text.end()) {
    auto start = it;
    uint32_t cp = utf8::next(it, text.end());
    if (!IsPunctCodepoint(cp))
      result.append(start, it);
  }
  return result;
}

// True iff `text` contains at least one CJK Unified Ideograph (U+4E00..U+9FFF).
// Used to decide whether a window carries real Chinese context: a window made
// of nothing but punctuation (e.g. a lone "。" left after the model's prior
// output was committed) must NOT count as context, or it would bypass the cold
// start `min_input_length` gate and spawn low-value inferences.
bool ContainsHan(const string& text) {
  auto it = text.begin();
  while (it != text.end()) {
    uint32_t cp = utf8::next(it, text.end());
    if (cp >= 0x4E00 && cp <= 0x9FFF)
      return true;
  }
  return false;
}

/// Walk commit history (most recent first, capped at `max_records`) and
/// reconstruct the Chinese context window.
///
/// Skip policy (denylist):
///   - "thru" records never enter `window_text` (raw ASCII keystrokes, not
///     semantic Chinese context).
///   - "raw" records never enter `window_text` either. librime emits "raw"
///     when a segment has no translation candidate and the user commits the
///     literal ASCII (e.g. typed `quickstart` then hit Return), or when a
///     translator calls Engine::CommitText directly. Feeding this ASCII
///     string into a Chinese LLM as context causes the model to faithfully
///     replay it as a prefix (e.g. window="quickstart" + pinyin="baoliu" →
///     model output "QUICKSTART保留"), which then leaks into the candidate
///     verbatim.
///   - "punct" records ARE now kept. The CT2 model is trained on punctuated
///     text and predicts MORE accurately with sentence structure present, so
///     feeding back committed punctuation (。，！？ …) is a positive signal,
///     not noise. The reason punct was originally dropped was a display
///     concern -- the model would sometimes echo context punctuation into a
///     candidate -- and that is now handled independently at the display layer
///     by StripAllPunctuation (see ExtractDisplayText), which strips
///     punctuation from any position in a candidate. So punctuation can safely
///     enter the window for inference while never surfacing in a candidate.
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
    if (it->type == "thru" || it->type == "raw")
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
    // No separator: committed text (now including its punctuation) is
    // concatenated as-is; the punctuation itself supplies sentence structure.
    out += text;
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
  // Reject non-pinyin segments (lone punctuation, ASCII symbols, uppercase
  // acronyms). Without this, a committed-context window + a single "," prompt
  // reaches the model and comes back as a junk "," candidate.
  if (!IsPinyinPrompt(prompt)) {
    return std::nullopt;
  }
  int threshold = opt.min_effective_length > 0 ? opt.min_effective_length : 12;

  string window_text;
  if (engine && engine->context()) {
    BuildWindowContext(engine->context(), opt.context_window_size,
                       &window_text);
  }

  // Trigger policy (context-first):
  //   - If `window_text` carries real Chinese context, ALWAYS feed it to the
  //     model regardless of prompt length -- the committed Hanzi prefix is the
  //     strongest signal we have, and dropping it for "long" prompts (the
  //     previous behavior) made mid-length inputs like `chuangkou + 现在在什么
  //     情况下会` lose all coherence and produce dictionary-style noise like
  //     `（创客）`.
  //   - If there is no real context (cold start, just after BackSpace/Return,
  //     or a window that is only punctuation/thru/raw), require `prompt.length
  //     >= threshold` so the model has enough to chew on; otherwise skip.
  //
  // "Real context" means the window contains at least one Hanzi, not merely
  // that it is non-empty: now that punct records enter the window, a window of
  // only "。" must not bypass the cold-start threshold and spawn a low-value
  // inference off a lone punctuation mark.
  bool has_context = ContainsHan(window_text);
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
