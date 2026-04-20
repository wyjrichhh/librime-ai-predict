// Copyright RIME Developers
// Distributed under the BSD License
//
// Abstract inference backend for librime-ai-predict (CT2 now, llama.cpp later).

#ifndef RIME_PREDICT_INFERENCE_BACKEND_H_
#define RIME_PREDICT_INFERENCE_BACKEND_H_

#include <memory>
#include <optional>
#include <string>

namespace rime {
namespace predict {

struct InferenceBackendConfig {
  std::string model_path;
  std::string device = "cpu";
};

class InferenceBackend {
 public:
  virtual ~InferenceBackend() = default;

  virtual bool Initialize(const InferenceBackendConfig& config) = 0;
  virtual void Shutdown() = 0;
  virtual bool IsInitialized() const = 0;

  /// Run seq2seq inference. Returns nullopt on failure or empty output.
  virtual std::optional<std::string> Predict(const std::string& prompt,
                                             int max_tokens) = 0;
};

}  // namespace predict
}  // namespace rime

#endif  // RIME_PREDICT_INFERENCE_BACKEND_H_
