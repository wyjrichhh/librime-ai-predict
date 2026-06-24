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
  /// Pure pinyin being composed (spaces stripped). Doubles as the key for the
  /// engine's "does the composition still match the request we sent" check.
  string effective_prompt;
  /// Chinese context reconstructed from recent commit history. Empty when none.
  string window_text;
  /// Result cache key. Combines window_text + effective_prompt so the same
  /// pinyin under different contexts can't collide.
  string cache_key;
  /// Full line passed to the backend (Chinese prefix + pinyin tags).
  string ct2_input;
  /// True when issued with a non-empty window_text (context-aware mode); false
  /// for context-free cold start.
  bool windowed = false;
};

struct ContextBuilderOptions {
  /// Min prompt length (bytes) to trigger prediction WITHOUT context. With
  /// context any non-empty prompt triggers; without it a short fragment alone
  /// just hallucinates, so we wait for more.
  int min_effective_length = 12;
  /// Max history records to scan for the sliding window.
  int context_window_size = 10;
};

class ContextBuilder {
 public:
  /// Returns empty optional if prediction should be skipped (length gate).
  static std::optional<PredictionContext> Build(Engine* engine,
                                                const string& raw_input,
                                                const ContextBuilderOptions& opt);
};

/// Clean a raw model output for display in the candidate menu: strips any
/// punctuation the model emitted (see BuildWindowContext for why punctuation
/// is tolerated on input but must never reach a candidate).
string ExtractDisplayText(const string& model_output);

}  // namespace predict
}  // namespace rime

#endif  // RIME_PREDICT_CONTEXT_BUILDER_H_
