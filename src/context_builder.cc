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

string BuildHistoryContextString(Context* ctx, int max_records) {
  if (!ctx)
    return string();
  const CommitHistory& history = ctx->commit_history();
  vector<string> recent_commits;
  size_t collected = 0;
  for (auto it = history.rbegin();
       it != history.rend() && collected < static_cast<size_t>(max_records);
       ++it) {
    // Skip auxiliary records and our own AI candidates to avoid feeding
    // the model its own past output (which causes runaway snowballing).
    if (it->type == "punct" || it->type == "thru" || it->type == "ai_predict")
      continue;
    recent_commits.push_back(it->text);
    ++collected;
  }
  std::reverse(recent_commits.begin(), recent_commits.end());
  string context_string;
  for (const auto& text : recent_commits) {
    if (!context_string.empty())
      context_string += " ";
    context_string += text;
  }
  return context_string;
}

}  // namespace

std::optional<PredictionContext> ContextBuilder::Build(
    Engine* engine,
    const string& raw_input,
    const ContextBuilderOptions& opt) {
  string prompt = StripSpaces(raw_input);
  int threshold = opt.min_effective_length;
  if (threshold < 1)
    threshold = 10;

  string window_pinyin;
  string window_text;
  string last_punct;

  if (engine && engine->context()) {
    const CommitHistory& history = engine->context()->commit_history();
    // Snapshot the most recent commit record: if (and only if) it is of type
    // punct/thru we remember its text as `last_punct`, mirroring shell-input.
    // Anything else (e.g. a Hanzi commit) leaves last_punct empty -- we only
    // want to strip a punctuation that is *immediately* adjacent to the new
    // pinyin input, not an arbitrary historical one.
    if (!history.empty()) {
      const auto& back = *history.rbegin();
      if (back.type == "punct" || back.type == "thru") {
        last_punct = back.text;
      }
    }
    size_t count = 0;
    for (auto it = history.rbegin();
         it != history.rend() && count < static_cast<size_t>(opt.context_window_size);
         ++it) {
      if (it->type == "punct" || it->type == "thru" || it->type == "ai_predict")
        continue;
      // CommitRecord only has type + text (see rime/commit_history.h). Walk recent
      // raw ASCII segments (pinyin still being composed); stop at committed Hanzi.
      const string& t = it->text;
      if (t.empty())
        break;
      bool all_alpha = true;
      for (unsigned char c : t) {
        if (c < 'a' || c > 'z') {
          all_alpha = false;
          break;
        }
      }
      if (!all_alpha)
        break;
      window_pinyin = t + window_pinyin;
      ++count;
    }
  }

  int switch_point =
      std::min(static_cast<int>(window_pinyin.length()), threshold);
  bool has_window = !window_pinyin.empty() &&
                    static_cast<int>(prompt.length()) <= switch_point;
  string effective_prompt;
  if (has_window) {
    effective_prompt = window_pinyin + prompt;
  } else {
    effective_prompt = prompt;
    window_text = "";
  }

  int effective_len = static_cast<int>(effective_prompt.length());
  if (effective_len < threshold || effective_prompt.empty()) {
    return std::nullopt;
  }

  PredictionContext ctx;
  ctx.effective_prompt = effective_prompt;
  ctx.window_pinyin = window_pinyin;
  ctx.window_text = window_text;
  ctx.last_punct = last_punct;
  ctx.windowed = has_window;

  // Assemble CT2 input (same convention as shell-input LlmTranslator).
  string raw_pinyin = effective_prompt;
  string ct2_prefix;
  if (!window_text.empty() && !window_pinyin.empty() &&
      effective_prompt.length() >= window_pinyin.length() &&
      effective_prompt.substr(0, window_pinyin.length()) == window_pinyin) {
    raw_pinyin = effective_prompt.substr(window_pinyin.length());
    ct2_prefix = window_text;
  } else if (window_text.empty() && engine && engine->context()) {
    string ct2_context =
        BuildHistoryContextString(engine->context(), opt.context_window_size);
    ct2_context.erase(std::remove(ct2_context.begin(), ct2_context.end(), ' '),
                      ct2_context.end());
    ct2_prefix = ct2_context;
  }

  ctx.ct2_input =
      ct2_prefix + "<pinyin_start>" + raw_pinyin + "</pinyin_start>";
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
