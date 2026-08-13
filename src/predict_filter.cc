// Copyright RIME Developers
// Distributed under the BSD License

#include "predict_filter.h"

#include <algorithm>
#include <vector>

#include <glog/logging.h>

#include <rime/candidate.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/schema.h>
#include <rime/translation.h>

#include "frontend_protocol.h"

namespace rime {
namespace predict {

namespace {

constexpr const char* kAITextProperty = "ai_predict/text";
constexpr const char* kAICandidateType = "ai_predict";
constexpr const char* kAICommentMarker = "AI";

// Publish the index of the AI row so opt-in frontends accent-colour its
// comment (see frontend_protocol.h). Idempotent: skip the set_property when
// unchanged, to avoid a spurious notification on every Compose().
void PublishCommentHighlight(Engine* engine, int index) {
  if (!engine || !engine->context()) return;
  Context* ctx = engine->context();
  const std::string value = protocol::MakeCommentHighlightPayload(index);
  if (ctx->get_property(protocol::kCommentHighlight) != value) {
    ctx->set_property(protocol::kCommentHighlight, value);
  }
}

/// Wraps an upstream translation, prefetches up to `search_range` candidates,
/// then places the AI prediction (read from the engine's context property) at
/// `target_index`. Placement policy:
/// 1. AI text already at slot #1 → do nothing (the top slot already carries it;
///    a relabel or duplicate row would just be noise).
/// 2. AI text matches a candidate further down → promote that one (no dup row).
/// 3. AI text is novel → insert a fresh candidate.
///
/// AI-typed candidates emitted upstream by PredictTranslator are stripped up
/// front, so this filter is the single source of truth for the AI row.
class AIPredictFilteredTranslation : public Translation {
 public:
  AIPredictFilteredTranslation(an<Translation> upstream,
                               const string& ai_text,
                               size_t target_index,
                               size_t search_range)
      : upstream_(std::move(upstream)),
        ai_text_(ai_text),
        target_index_(target_index),
        search_range_(search_range) {
    Build();
    set_exhausted(cursor_ >= reordered_.size() &&
                  (!upstream_ || upstream_->exhausted()));
  }

  // -1 means we did not insert an AI-tagged row this round (either the AI
  // text was empty, slot #1 already matched it so no relabelling happened,
  // or upstream produced nothing). PredictFilter::Apply forwards this to
  // PublishCommentHighlight, which encodes it as the wire payload.
  int ai_inserted_index() const { return ai_inserted_index_; }

  bool Next() override {
    if (exhausted()) return false;
    if (cursor_ < reordered_.size()) {
      ++cursor_;
    } else if (upstream_ && !upstream_->exhausted()) {
      upstream_->Next();
    }
    if (cursor_ >= reordered_.size() &&
        (!upstream_ || upstream_->exhausted())) {
      set_exhausted(true);
    }
    return !exhausted();
  }

  an<Candidate> Peek() override {
    if (exhausted()) return nullptr;
    if (cursor_ < reordered_.size()) {
      return reordered_[cursor_];
    }
    if (upstream_ && !upstream_->exhausted()) {
      return upstream_->Peek();
    }
    return nullptr;
  }

 private:
  void Build() {
    if (!upstream_) return;

    std::vector<an<Candidate>> buf;
    buf.reserve(search_range_);
    while (buf.size() < search_range_ && !upstream_->exhausted()) {
      if (auto c = upstream_->Peek()) {
        buf.push_back(c);
      }
      upstream_->Next();
    }

    // 引擎的 uniquifier 按「已弹出的候选列表」去重,而此刻引擎还没弹出任何候选、
    // 那批列表为空,所以它没法对这批预取的候选去重。这里按文本补一道去重,
    // 避免词库词被句模式/用户词库重复(如 haishi → 还是 + 还是)。
    for (size_t i = 0; i < buf.size(); ++i) {
      for (size_t j = i + 1; j < buf.size();) {
        if (buf[j]->text() == buf[i]->text()) {
          buf.erase(buf.begin() + j);
        } else {
          ++j;
        }
      }
    }

    if (ai_text_.empty()) {
      // No AI suggestion in flight; pass everything through unchanged.
      reordered_ = std::move(buf);
      return;
    }

    // We are the single source of truth for the AI row, so drop any AI-typed
    // candidates the translator emitted before deciding our own placement.
    auto first_ai = std::stable_partition(
        buf.begin(), buf.end(), [](const an<Candidate>& c) {
          return !c || c->type() != kAICandidateType;
        });
    buf.erase(first_ai, buf.end());

    if (buf.empty()) {
      // No upstream candidate means no segment range to anchor an AI candidate.
      reordered_ = std::move(buf);
      return;
    }

    if (buf.front() && buf.front()->text() == ai_text_) {
      LOG(INFO) << "ai_predict_filter: dedup -- AI text '" << ai_text_
                << "' already at slot #1; leaving candidate list unchanged";
      reordered_ = std::move(buf);
      return;
    }

    // Promote an existing match (wrapped so type/preedit/quality survive and
    // only the comment changes) rather than inserting a second copy.
    an<Candidate> ai_cand;
    auto dup = std::find_if(buf.begin() + 1, buf.end(),
                            [this](const an<Candidate>& c) {
                              return c && c->text() == ai_text_;
                            });
    if (dup != buf.end()) {
      auto matched = *dup;
      size_t orig_pos = std::distance(buf.begin(), dup);
      buf.erase(dup);
      ai_cand = New<ShadowCandidate>(matched, kAICandidateType,
                                     /*text=*/string(),
                                     /*comment=*/kAICommentMarker,
                                     /*inherit_comment=*/false);
      LOG(INFO) << "ai_predict_filter: promoted existing candidate '"
                << matched->text() << "' (was at slot #" << orig_pos
                << ", type=" << matched->type() << ") to slot #"
                << target_index_ << " with AI marker";
    } else {
      ai_cand = New<SimpleCandidate>(kAICandidateType,
                                     buf.front()->start(),
                                     buf.front()->end(),
                                     ai_text_, kAICommentMarker);
      LOG(INFO) << "ai_predict_filter: inserted new AI candidate '"
                << ai_text_ << "' at slot #" << target_index_;
    }

    // 4. Splice with AI at target_index_.
    reordered_.reserve(buf.size() + 1);
    size_t pos = (std::min)(target_index_, buf.size());
    for (size_t i = 0; i < pos; ++i) reordered_.push_back(buf[i]);
    reordered_.push_back(ai_cand);
    for (size_t i = pos; i < buf.size(); ++i) reordered_.push_back(buf[i]);
    ai_inserted_index_ = static_cast<int>(pos);
  }

  an<Translation> upstream_;
  string ai_text_;
  size_t target_index_;
  size_t search_range_;

  std::vector<an<Candidate>> reordered_;
  size_t cursor_ = 0;
  int ai_inserted_index_ = -1;
};

}  // namespace

PredictFilter::PredictFilter(const Ticket& ticket)
    : Filter(ticket), TagMatching(ticket) {
  if (!engine_ || !engine_->schema() || !engine_->schema()->config()) {
    return;
  }
  Config* cfg = engine_->schema()->config();
  int n = 0;
  if (cfg->GetInt("ai_predict/target_index", &n) && n >= 0) {
    target_index_ = static_cast<size_t>(n);
  }
  if (cfg->GetInt("ai_predict/search_range", &n) && n > 0) {
    search_range_ = static_cast<size_t>(n);
  }
  LOG(INFO) << "ai_predict_filter: ctor target_index=" << target_index_
            << " search_range=" << search_range_;
}

an<Translation> PredictFilter::Apply(an<Translation> translation,
                                     CandidateList* /*candidates*/) {
  if (!engine_ || !engine_->context() || !translation) {
    return translation;
  }
  string ai_text = engine_->context()->get_property(kAITextProperty);
  if (ai_text.empty()) {
    // No AI in flight this Compose; clear any stale highlight from the
    // previous frame so the frontend doesn't keep colouring a row that no
    // longer carries AI semantics.
    PublishCommentHighlight(engine_, -1);
    return translation;
  }
  auto wrapped = New<AIPredictFilteredTranslation>(
      std::move(translation), ai_text, target_index_, search_range_);
  // Publish the index where the AI row landed (or -1 if none was inserted,
  // e.g. dedup against slot #1). Synchronous on Compose() so the frontend
  // has the index before it observes the new menu.
  PublishCommentHighlight(engine_, wrapped->ai_inserted_index());
  return wrapped;
}

}  // namespace predict
}  // namespace rime
