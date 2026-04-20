// Predict filter: reorders the candidate list so that the AI prediction
// (published by PredictTranslator via Context property) lands at slot #2,
// reusing existing candidates when they collide on text.
//
// Copyright RIME Developers
// Distributed under the BSD License

#ifndef RIME_PREDICT_PREDICT_FILTER_H_
#define RIME_PREDICT_PREDICT_FILTER_H_

#include <rime/filter.h>
#include <rime/gear/filter_commons.h>
#include <rime/translation.h>

namespace rime {
namespace predict {

class PredictFilter : public Filter, TagMatching {
 public:
  explicit PredictFilter(const Ticket& ticket);

  an<Translation> Apply(an<Translation> translation,
                        CandidateList* candidates) override;

  bool AppliesToSegment(Segment* segment) override { return TagsMatch(segment); }

 private:
  /// Where to place the AI candidate (0-based; default 1 → second slot).
  size_t target_index_ = 1;
  /// How many upstream candidates to scan when looking for a text-match dedup.
  size_t search_range_ = 10;
};

}  // namespace predict
}  // namespace rime

#endif  // RIME_PREDICT_PREDICT_FILTER_H_
