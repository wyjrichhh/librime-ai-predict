// Copyright RIME Developers
// Distributed under the BSD License

#include "prediction_engine.h"

#include <glog/logging.h>

#include <algorithm>
#include <chrono>

#include <rime/context.h>
#include <rime/engine.h>

#include "frontend_protocol.h"

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
  pending_cache_key_ = ctx.cache_key;
  pending_prompt_ = ctx.effective_prompt;
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
    string prompt_snap;
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
      prompt_snap = pending_prompt_;

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
          prompt_snap = pending_prompt_;
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
      // 只有「当前输入的拼音部分」和推理时的 prompt 一致才刷新。拼音部分是 current
      // 去掉前导 raw/uppercase 前缀(如 "IBM")后的后缀, 故用「key 是 current 后缀」判定。
      // key 是 current 前缀(继续输入)或两者无关(退格/改字)时跳过: 否则刷新后必然
      // cache MISS 并重新调度, 形成「推理→刷新→MISS→推理」追赶循环, 让候选来回闪烁。
      string key = StripSpaces(prompt_snap);
      bool current_matches =
          key.empty() || current == key ||
          (current.size() > key.size() &&
           current.compare(current.size() - key.size(), key.size(), key) == 0);
      if (!current_matches) {
        LOG(INFO) << "PredictionEngine: skip refresh, composition changed"
                  << " (prompt='" << prompt_snap << "' current_input='"
                  << ctx->input() << "')";
        continue;
      }
      // Don't disturb the user mid-navigation. RefreshNonConfirmedComposition
      // re-runs Compose() and resets selected_index to 0, which would yank
      // the menu back to page 1 / candidate 1 if the user has already paged
      // (=) or moved the highlight. In that case we keep the cache populated
      // (so the next keystroke's PredictTranslator::Query will hit it) but
      // skip the active refresh.
      if (!ctx->composition().empty() &&
          ctx->composition().back().selected_index > 0) {
        LOG(INFO) << "PredictionEngine: skip refresh, user already navigated"
                  << " (selected_index="
                  << ctx->composition().back().selected_index << ")";
        continue;
      }
      // A plain update_notifier won't do: ConcreteEngine::Compose skips
      // segments with status >= kGuess, so it would reuse the old menu and
      // never re-invoke PredictTranslator::Query. RefreshNonConfirmedComposition
      // pops the non-selected tail, forcing a re-translate that hits our cache.
      LOG(INFO) << "PredictionEngine: refreshing composition to surface "
                << "AI candidate for cache_key='" << cache_key << "'";
      ctx->RefreshNonConfirmedComposition();
      // Refresh ran Compose() synchronously, so the new menu is fully built
      // now -- only now is it safe to tell the frontend to re-read it. We
      // signal off kRefreshUI rather than "ai_predict/text" because the latter
      // is set during Compose, so a frontend racing on it would see a torn menu
      // mid-build. See frontend_protocol.h for the protocol itself.
      ctx->set_property(
          protocol::kRefreshUI,
          protocol::MakeRefreshUIPayload(protocol::kPluginCodename));
    }
  }
}

}  // namespace predict
}  // namespace rime
