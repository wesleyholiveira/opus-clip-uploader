#pragma once

#include "curation/feedback/curation-feedback-store.hpp"
#include "curation/scoring/clip-candidate.hpp"
#include "curation/scoring/feedback-similarity-scorer.hpp"
#include "curation/scoring/transcript-index.hpp"

#include <QVector>

namespace Curation::Scoring {

struct FeedbackConsistencyGateOptions {
	bool viewerMessagePreset = false;
	double minPositiveMargin = 0.06;
	double hardNegativeMargin = 0.12;
};

class FeedbackConsistencyGate {
public:
	QVector<ClipCandidate> apply(QVector<ClipCandidate> candidates, const TranscriptIndex &index,
		const Curation::Feedback::FeedbackRangeMemory &memory,
		const FeedbackConsistencyGateOptions &options) const;

private:
	bool shouldReject(const ClipCandidate &candidate, const FeedbackSimilarityFeatures &features,
		const FeedbackConsistencyGateOptions &options) const;
	void applyPositiveBoost(ClipCandidate &candidate, const FeedbackSimilarityFeatures &features,
		const FeedbackConsistencyGateOptions &options) const;
};

} // namespace Curation::Scoring
