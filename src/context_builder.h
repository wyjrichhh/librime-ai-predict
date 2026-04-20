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
  /// Pure raw pinyin currently being composed (spaces stripped). Used both as
  /// the model's `<pinyin_start>...</pinyin_start>` payload and, in the
  /// PredictionEngine, as the key for "did the user's composition still match
  /// the request we sent" comparisons.
  string effective_prompt;
  /// Reconstructed Chinese context from commit history (most recent N commits
  /// concatenated, oldest first). Empty when no usable context exists.
  string window_text;
  /// Cache key for the prediction result. Combines `window_text` and
  /// `effective_prompt` so that the same pinyin under different upstream
  /// contexts produces distinct cache entries (no cross-context pollution).
  string cache_key;
  /// Full line passed to CT2Backend::Predict (Chinese prefix + pinyin tags).
  string ct2_input;
  /// Most recent committed punctuation (only set when the very last commit
  /// record is of type "punct" or "thru"). When set, the model output is
  /// stripped of this prefix on the display side -- the model often echoes a
  /// connecting punctuation that the user just typed (e.g. user committed "，"
  /// then typed pinyin and the suggestion comes back as "，你好"); without
  /// this strip the candidate would visually duplicate the punctuation.
  string last_punct;
  /// True when prediction was triggered in "windowed" mode (short pinyin +
  /// non-empty Chinese context). False for "direct" mode (long pinyin alone).
  bool windowed = false;
};

struct ContextBuilderOptions {
  /// Switch point AND minimum length for context-less prediction:
  ///   - prompt.length >= min_effective_length → direct mode (no context)
  ///   - prompt.length <  min_effective_length → windowed mode (requires
  ///                                              non-empty window_text)
  /// Lower values surface AI candidates earlier when context exists.
  int min_effective_length = 6;
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
