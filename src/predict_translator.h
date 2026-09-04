// Predict translator: CT2-backed async candidates (librime-ai-predict plugin).
//
// Copyright RIME Developers
// Distributed under the BSD License

#ifndef RIME_PREDICT_PREDICT_TRANSLATOR_H_
#define RIME_PREDICT_PREDICT_TRANSLATOR_H_

#include "context_builder.h"
#include "prediction_engine.h"

#include <rime/translator.h>

#include <filesystem>
#include <memory>

namespace rime {
namespace predict {

class PredictTranslator : public Translator {
 public:
  explicit PredictTranslator(const Ticket& ticket);
  ~PredictTranslator() override;

  an<Translation> Query(const string& input, const Segment& segment) override;

 private:
  bool EnsureEngine();
  std::filesystem::path ResolveModelPath() const;
  void LoadOptions();

  ContextBuilderOptions ctx_opt_;
  PredictionEngineOptions engine_opt_;
  std::string model_path_raw_;
  std::string device_;
  /// Kill switch: false makes the translator inert without touching the
  /// schema's translators/filters lists (see ai_predict/enabled).
  bool enabled_ = true;
  /// Candidate quality for merged ordering; default -1 yields first slot to script candidates.
  double ai_quality_ = -1.0;
  /// Min Hanzi count for a prediction to be surfaced. Single-Hanzi homophones
  /// are the schema dictionary's strength and the model's weakness (no frequency
  /// signal), so we let the dictionary take those and only surface 2+ Hanzi.
  int min_hanzi_ = 2;

  std::unique_ptr<PredictionEngine> prediction_;
  bool init_attempted_ = false;
  bool init_ok_ = false;
};

}  // namespace predict
}  // namespace rime

#endif  // RIME_PREDICT_PREDICT_TRANSLATOR_H_
