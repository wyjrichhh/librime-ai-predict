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
  /// True when prediction was issued with a non-empty `window_text`
  /// (context-aware mode). False for context-free mode (pinyin alone, used
  /// only on cold start when no usable commit history exists).
  bool windowed = false;
};

struct ContextBuilderOptions {
  /// Minimum prompt length (bytes) required to trigger prediction WHEN there
  /// is no usable Chinese context. With context, prediction triggers on any
  /// non-empty prompt -- the committed text already carries enough signal.
  /// Without context, we require a longer prompt so the model has something
  /// concrete to work with (a 2-3 letter fragment alone hallucinates).
  int min_effective_length = 12;
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

/// Clean a raw model output for display in the candidate menu.
///
/// The CT2 model emits exactly the new suffix (it does NOT echo the window
/// prefix and does NOT replay the user's just-committed punctuation), so the
/// only postprocessing we need is to strip any punctuation the model itself
/// produced -- candidates with trailing "。" / "！" / "（…）" feel noisy in
/// the menu. If a future model changes its output convention, the place to
/// reintroduce window-prefix stripping is here.
string ExtractDisplayText(const string& model_output);

}  // namespace predict
}  // namespace rime

#endif  // RIME_PREDICT_CONTEXT_BUILDER_H_
