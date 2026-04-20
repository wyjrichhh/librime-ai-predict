// Copyright RIME Developers
// Distributed under the BSD License

#include "prediction_engine.h"

#include <glog/logging.h>

#include <algorithm>
#include <chrono>

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

}  // namespace

PredictionEngine::PredictionEngine(Engine* engine,
                                   std::unique_ptr<InferenceBackend> backend,
                                   const PredictionEngineOptions& opt)
    : engine_(engine), backend_(std::move(backend)), opt_(opt) {
  worker_ = std::thread(&PredictionEngine::WorkerLoop, this);
}

PredictionEngine::~PredictionEngine() {
  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    shutdown_ = true;
    worker_cv_.notify_one();
  }
  if (worker_.joinable()) {
    worker_.join();
  }
}

void PredictionEngine::Schedule(const PredictionContext& ctx) {
  std::lock_guard<std::mutex> lock(worker_mutex_);
  pending_ct2_input_ = ctx.ct2_input;
  pending_cache_key_ = ctx.effective_prompt;
  pending_window_text_ = ctx.window_text;
  worker_cv_.notify_one();
}

std::optional<std::string> PredictionEngine::GetCachedResult(
    const std::string& cache_key) const {
  std::lock_guard<std::mutex> lock(result_mutex_);
  if (cache_key == last_cache_key_ && last_raw_result_) {
    return last_raw_result_;
  }
  return std::nullopt;
}

void PredictionEngine::ClearCache() {
  std::lock_guard<std::mutex> lock(result_mutex_);
  last_cache_key_.clear();
  last_raw_result_.reset();
}

void PredictionEngine::WorkerLoop() {
  while (true) {
    string ct2_input;
    string cache_key;
    string window_text_snap;
    {
      std::unique_lock<std::mutex> lock(worker_mutex_);
      worker_cv_.wait(lock, [this] {
        return shutdown_ || !pending_ct2_input_.empty();
      });
      if (shutdown_) {
        return;
      }
      ct2_input = pending_ct2_input_;
      pending_ct2_input_.clear();
      cache_key = pending_cache_key_;
      window_text_snap = pending_window_text_;

      // Debounce: wait for quiet period, coalescing rapid updates.
      while (true) {
        if (worker_cv_.wait_for(lock, std::chrono::milliseconds(opt_.debounce_ms)) ==
            std::cv_status::timeout) {
          break;
        }
        if (shutdown_)
          return;
        if (!pending_ct2_input_.empty()) {
          ct2_input = pending_ct2_input_;
          pending_ct2_input_.clear();
          cache_key = pending_cache_key_;
          window_text_snap = pending_window_text_;
        }
      }
    }

    if (ct2_input.empty() || !backend_ || !backend_->IsInitialized()) {
      continue;
    }

    LOG(INFO) << "PredictionEngine: invoking backend ct2_input='" << ct2_input
              << "' max_tokens=" << opt_.max_tokens;
    auto t0 = std::chrono::steady_clock::now();
    auto result = backend_->Predict(ct2_input, opt_.max_tokens);
    auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - t0)
                     .count();
    if (!result || result->empty()) {
      LOG(WARNING) << "PredictionEngine: inference failed or empty (took "
                   << dt_ms << " ms)";
      continue;
    }
    LOG(INFO) << "PredictionEngine: backend returned raw='" << *result
              << "' (took " << dt_ms << " ms)";

    {
      std::lock_guard<std::mutex> lock(result_mutex_);
      last_cache_key_ = cache_key;
      last_raw_result_ = result;
    }

    if (engine_ && engine_->context()) {
      Context* ctx = engine_->context();
      string current = StripSpaces(ctx->input());
      string key = StripSpaces(cache_key);
      if (!key.empty() && current.find(key) == string::npos &&
          key.find(current) == string::npos) {
        LOG(INFO) << "PredictionEngine: skip refresh, composition changed"
                  << " (cache_key='" << cache_key << "' current_input='"
                  << ctx->input() << "')";
        continue;
      }
      // We can't just fire update_notifier: ConcreteEngine::Compose skips
      // segments whose status >= kGuess, so the existing menu would be
      // reused and PredictTranslator::Query would never be re-invoked.
      // RefreshNonConfirmedComposition pops the non-selected tail of the
      // composition (status < kSelected), forcing the next Compose to
      // re-segment and re-translate -- which triggers our cache HIT path.
      LOG(INFO) << "PredictionEngine: refreshing composition to surface "
                << "AI candidate for cache_key='" << cache_key << "'";
      ctx->RefreshNonConfirmedComposition();
      // RefreshNonConfirmedComposition() runs Compose() synchronously on this
      // worker thread, so by the time it returns the new menu (including the
      // AI candidate, after PredictFilter) is fully built. Only NOW is it
      // safe to ask the frontend (e.g. Squirrel) to re-read the context.
      //
      // We deliberately use a dedicated property here instead of relying on
      // "ai_predict/text": the latter is set from inside
      // AIPredictTranslator::Query (i.e. *during* Compose), so a frontend that
      // refreshes on it would race with the still-building menu and observe
      // an empty/torn state, then end up hiding the panel.
      ctx->set_property("ai_predict/ready", cache_key);
    }
  }
}

}  // namespace predict
}  // namespace rime
