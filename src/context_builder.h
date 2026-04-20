// Build CT2 prompt and sliding-window context from Rime composition / history.
//
// Copyright RIME Developers
// Distributed under the BSD License

#ifndef RIME_PREDICT_CONTEXT_BUILDER_H_
#define RIME_PREDICT_CONTEXT_BUILDER_H_

#include <rime/common.h>

#include <optional>
#include <string>

namespace rime {

class Context;
class Engine;

namespace predict {

struct PredictionContext {
  string effective_prompt;
  string window_text;
  string window_pinyin;
  /// Full line passed to CT2Backend::Predict (Chinese prefix + pinyin tags).
  string ct2_input;
  /// Most recent committed punctuation (only set when the very last commit
  /// record is of type "punct" or "thru"). When set, the model output is
  /// stripped of this prefix on the display side -- the model often echoes a
  /// connecting punctuation that the user just typed (e.g. user committed "，"
  /// then typed pinyin and the suggestion comes back as "，你好"); without
  /// this strip the candidate would visually duplicate the punctuation.
  string last_punct;
  bool windowed = false;
};

struct ContextBuilderOptions {
  /// Minimum effective prompt length (bytes) before triggering prediction.
  int min_effective_length = 10;
  /// Max history records to scan for sliding window.
  int context_window_size = 10;
};

class ContextBuilder {
 public:
  /// Returns empty optional if prediction should be skipped (length gate).
  static std::optional<PredictionContext> Build(Engine* engine,
                                                const string& raw_input,
                                                const ContextBuilderOptions& opt);
};

/// Strip model prefix that duplicates committed context (windowed mode), then
/// strip a leading `last_punct` if the model echoed the most recent committed
/// punctuation. `last_punct` may be empty (no-op).
string ExtractDisplayText(const string& model_output,
                          const string& window_text_for_extract,
                          const string& last_punct = string());

}  // namespace predict
}  // namespace rime

#endif  // RIME_PREDICT_CONTEXT_BUILDER_H_
