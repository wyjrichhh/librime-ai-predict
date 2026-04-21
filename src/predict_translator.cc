// Copyright RIME Developers
// Distributed under the BSD License

#include "predict_translator.h"

#include "context_builder.h"
#include "ct2_backend.h"

#include <glog/logging.h>

#include <rime/context.h>
#include <rime/engine.h>
#include <rime/candidate.h>
#include <rime/config.h>
#include <rime/schema.h>
#include <rime/segmentation.h>
#include <rime/service.h>
#include <rime/translation.h>

namespace rime {
namespace predict {

namespace {

constexpr const char* kAITextProperty = "ai_predict/text";

/// Slot policy for the AI prediction candidate produced by PredictTranslator.
///
/// Slot #1 (candidates.empty()) → yield to whoever runs after us in the
///   schema's `translators` list (e.g. script_translator). Requires that
///   `ai_predict_translator` is listed FIRST so MergedTranslation::Elect
///   actually consults us before others.
/// Slot #2 (candidates.size() == 1) → claim, except when the slot #1 text
///   already equals ours (then yield to avoid an immediate duplicate row).
/// Subsequent slots are unreachable: UniqueTranslation exhausts after one Next.
///
/// When `ai_predict_filter` is installed it always strips translator-emitted
/// AI candidates and re-injects (or suppresses) the AI row based on the full
/// `search_range`. So this Compare is effectively a fallback for setups that
/// only use the translator: it gets slot #2 right and dedups against slot #1.
class AIPredictTranslation : public UniqueTranslation {
 public:
  explicit AIPredictTranslation(an<Candidate> candidate)
      : UniqueTranslation(std::move(candidate)) {}

  int Compare(an<Translation> other, const CandidateList& candidates) override {
    if (exhausted()) {
      return 1;
    }
    if (!other || other->exhausted()) {
      return -1;
    }
    if (candidates.empty()) {
      return 1;
    }
    if (candidates.size() == 1) {
      auto ours = Peek();
      if (ours && candidates[0] && candidates[0]->text() == ours->text()) {
        return 1;  // dedup against slot #1.
      }
      return -1;
    }
    return UniqueTranslation::Compare(other, candidates);
  }
};

void PublishAITextProperty(Engine* engine, const string& text) {
  if (!engine || !engine->context()) {
    return;
  }
  Context* ctx = engine->context();
  if (ctx->get_property(kAITextProperty) != text) {
    ctx->set_property(kAITextProperty, text);
  }
}

}  // namespace

PredictTranslator::PredictTranslator(const Ticket& ticket) : Translator(ticket) {
  LOG(INFO) << "ai_predict_translator: ctor (engine=" << engine_
            << ", schema_id="
            << (engine_ && engine_->schema() ? engine_->schema()->schema_id() : "<null>")
            << ")";
  LoadOptions();
}

PredictTranslator::~PredictTranslator() = default;

void PredictTranslator::LoadOptions() {
  Config* config = engine_ && engine_->schema() ? engine_->schema()->config() : nullptr;
  if (!config) {
    return;
  }
  string path;
  if (config->GetString("ai_predict/model_path", &path)) {
    model_path_raw_ = path;
  }
  int n = 0;
  if (config->GetInt("ai_predict/min_input_length", &n) && n > 0) {
    ctx_opt_.min_effective_length = n;
  }
  if (config->GetInt("ai_predict/context_window_size", &n) && n > 0) {
    ctx_opt_.context_window_size = n;
  }
  if (config->GetInt("ai_predict/debounce_ms", &n) && n > 0) {
    engine_opt_.debounce_ms = n;
  }
  if (config->GetInt("ai_predict/max_tokens", &n) && n > 0) {
    engine_opt_.max_tokens = n;
  }
  string dev = "cpu";
  if (config->GetString("ai_predict/device", &dev)) {
    device_ = dev;
  }
  double q = -1.0;
  if (config->GetDouble("ai_predict/quality", &q)) {
    ai_quality_ = q;
  }
  LOG(INFO) << "ai_predict_translator: options loaded"
            << " model_path=" << model_path_raw_
            << " min_input_length=" << ctx_opt_.min_effective_length
            << " context_window_size=" << ctx_opt_.context_window_size
            << " debounce_ms=" << engine_opt_.debounce_ms
            << " max_tokens=" << engine_opt_.max_tokens
            << " device=" << device_
            << " quality=" << ai_quality_;
}

std::filesystem::path PredictTranslator::ResolveModelPath() const {
  if (model_path_raw_.empty()) {
    return {};
  }
  std::filesystem::path p(model_path_raw_);
  if (p.is_absolute()) {
    return p;
  }
  return Service::instance().deployer().user_data_dir / p;
}

bool PredictTranslator::EnsureEngine() {
  if (init_ok_ && prediction_) {
    return true;
  }
  if (init_attempted_ && !init_ok_) {
    return false;
  }
  init_attempted_ = true;
  auto path = ResolveModelPath();
  if (path.empty()) {
    LOG(ERROR) << "ai_predict_translator: ai_predict/model_path is not set";
    return false;
  }
  if (!std::filesystem::exists(path / "shared_vocabulary.json")) {
    LOG(ERROR) << "ai_predict_translator: model directory missing shared_vocabulary.json: "
               << path;
    return false;
  }
  auto backend = std::make_unique<CT2Backend>();
  InferenceBackendConfig cfg;
  cfg.model_path = path.string();
  cfg.device = device_;
  if (!backend->Initialize(cfg)) {
    LOG(ERROR) << "ai_predict_translator: failed to initialize CT2 backend";
    return false;
  }
  prediction_ =
      std::make_unique<PredictionEngine>(engine_, std::move(backend), engine_opt_);
  init_ok_ = true;
  LOG(INFO) << "ai_predict_translator: ready, model=" << path;
  return true;
}

an<Translation> PredictTranslator::Query(const string& input,
                                         const Segment& segment) {
  LOG(INFO) << "ai_predict_translator: Query input='" << input << "' segment=["
            << segment.start << "," << segment.end << ")";
  if (!EnsureEngine()) {
    LOG(INFO) << "ai_predict_translator: Query early-return (engine not ready)";
    return nullptr;
  }

  auto built = ContextBuilder::Build(engine_, input, ctx_opt_);
  if (!built) {
    LOG(INFO) << "ai_predict_translator: Query no context built (input too short / no commit history)";
    if (prediction_) {
      prediction_->ClearCache();
    }
    PublishAITextProperty(engine_, "");
    return nullptr;
  }
  LOG(INFO) << "ai_predict_translator: built context"
            << " mode=" << (built->windowed ? "windowed" : "direct")
            << " window_text='" << built->window_text << "'"
            << " effective_prompt='" << built->effective_prompt << "'"
            << " cache_key='" << built->cache_key << "'";

  if (auto raw = prediction_->GetCachedResult(built->cache_key)) {
    if (!raw->empty()) {
      const string display = ExtractDisplayText(*raw);
      if (!display.empty()) {
        LOG(INFO) << "ai_predict_translator: cache HIT display='" << display << "'";
        PublishAITextProperty(engine_, display);
        auto cand = New<SimpleCandidate>("ai_predict", segment.start, segment.end,
                                         display, "AI");
        cand->set_quality(ai_quality_);
        return New<AIPredictTranslation>(cand);
      }
    }
  }

  LOG(INFO) << "ai_predict_translator: cache MISS, scheduling inference";
  PublishAITextProperty(engine_, "");
  prediction_->Schedule(*built);
  return nullptr;
}

}  // namespace predict
}  // namespace rime
