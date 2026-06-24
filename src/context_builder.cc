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

// True iff `cp` is punctuation/symbol the model may emit but that must never
// appear in a candidate. Matched by Unicode RANGE, not an enumerated denylist:
// the model can emit any CJK punctuation it saw in training, and a hand-listed
// set inevitably misses one (a fullwidth `）` once leaked into `编译）`).
bool IsPunctCodepoint(uint32_t cp) {
  // ASCII punctuation ([a-zA-Z0-9] and apostrophe fall in the gaps).
  if (cp == '.' || cp == ',' || cp == '!' || cp == '?' || cp == ';' ||
      cp == ':')
    return true;
  // CJK Symbols and Punctuation: 。、《》「」『』【】〈〉… etc.
  if (cp >= 0x3000 && cp <= 0x303F)
    return true;
  // Fullwidth ASCII forms: ！（），：；？ etc.
  if ((cp >= 0xFF01 && cp <= 0xFF0F) || (cp >= 0xFF1A && cp <= 0xFF20) ||
      (cp >= 0xFF3B && cp <= 0xFF40) || (cp >= 0xFF5B && cp <= 0xFF65))
    return true;
  // General Punctuation: “ ” ‘ ’ … — – etc.
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

// True iff `text` contains at least one CJK Unified Ideograph. Decides whether
// a window carries real Chinese context: a window of only punctuation (a lone
// "。" left from a prior commit) must NOT count, or it bypasses the cold-start
// length gate and spawns low-value inferences.
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
/// Skip policy:
///   - "thru" / "raw" records are dropped: they are literal ASCII keystrokes,
///     not Chinese context, and feeding ASCII to the model makes it replay the
///     ASCII as a candidate prefix (window="quickstart" → output "QUICKSTART…").
///   - "punct" records are KEPT: the model is trained on punctuated text and
///     predicts more accurately with sentence structure, so committed
///     punctuation is signal, not noise. It can never reach a candidate because
///     the display layer strips it (see ExtractDisplayText).
///   - Everything else, including our own committed "ai_predict" candidates, is
///     ordinary user-confirmed content -- committing one took a keypress, so it
///     carries the same coherence signal as any other Hanzi commit.
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
    // No separator: committed punctuation already supplies sentence structure.
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

  // Context-first trigger: real Chinese context always fires (the committed
  // prefix is our strongest signal); cold start requires prompt >= threshold.
  // "Real context" means at least one Hanzi, not merely non-empty -- see
  // ContainsHan.
  bool has_context = ContainsHan(window_text);
  if (!has_context && static_cast<int>(prompt.length()) < threshold) {
    return std::nullopt;
  }

  PredictionContext ctx;
  ctx.effective_prompt = prompt;
  ctx.window_text = window_text;
  ctx.windowed = has_context;
  // Pipe is a safe separator: window_text is Hanzi, prompt is [a-z'] only.
  ctx.cache_key = window_text + "|" + prompt;
  ctx.ct2_input = window_text + "<pinyin_start>" + prompt + "</pinyin_start>";
  return ctx;
}

string ExtractDisplayText(const string& model_output) {
  return StripAllPunctuation(model_output);
}

}  // namespace predict
}  // namespace rime
