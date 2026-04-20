// Async prediction with debounce + cache; notifies Rime context on completion.
//
// Copyright RIME Developers
// Distributed under the BSD License

#ifndef RIME_PREDICT_PREDICTION_ENGINE_H_
#define RIME_PREDICT_PREDICTION_ENGINE_H_

#include "context_builder.h"
#include "inference_backend.h"

#include <rime/common.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace rime {

class Context;
class Engine;

namespace predict {

struct PredictionEngineOptions {
  int debounce_ms = 200;
  int max_tokens = 256;
};

class PredictionEngine {
 public:
  PredictionEngine(Engine* engine,
                   std::unique_ptr<InferenceBackend> backend,
                   const PredictionEngineOptions& opt);
  ~PredictionEngine();

  InferenceBackend* backend() { return backend_.get(); }

  /// Enqueue CT2 inference; returns immediately. On completion, notifies context.
  void Schedule(const PredictionContext& ctx);

  /// Last completed raw model output for cache_key (effective_prompt).
  std::optional<std::string> GetCachedResult(const std::string& cache_key) const;

  void ClearCache();

 private:
  void WorkerLoop();

  Engine* engine_;
  std::unique_ptr<InferenceBackend> backend_;
  PredictionEngineOptions opt_;

  std::thread worker_;
  std::mutex worker_mutex_;
  std::condition_variable worker_cv_;
  bool shutdown_ = false;

  std::string pending_ct2_input_;
  std::string pending_cache_key_;
  std::string pending_window_text_;

  mutable std::mutex result_mutex_;
  std::string last_cache_key_;
  std::optional<std::string> last_raw_result_;
};

}  // namespace predict
}  // namespace rime

#endif  // RIME_PREDICT_PREDICTION_ENGINE_H_
