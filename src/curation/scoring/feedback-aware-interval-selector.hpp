#pragma once

#include "curation/feedback/curation-feedback-store.hpp"
#include "curation/scoring/clip-candidate.hpp"
#include "curation/scoring/clip-ranker.hpp"
#include "curation/scoring/feedback-similarity-scorer.hpp"
#include "curation/scoring/transcript-index.hpp"

#include <QVector>

namespace Curation::Scoring {

class FeedbackAwareIntervalSelector {
public:
	QVector<ClipCandidate> select(QVector<ClipCandidate> candidates, const TranscriptIndex &index,
				      const Curation::Feedback::FeedbackRangeMemory &memory,
				      const ClipRankerOptions &options) const;

private:
	double selectionScore(const ClipCandidate &candidate, const FeedbackSimilarityFeatures &features) const;
};

} // namespace Curation::Scoring
