#pragma once

#include "curation/feedback/curation-feedback-store.hpp"
#include "curation/scoring/candidate-builder.hpp"
#include "curation/scoring/feedback-guided-candidate-generator.hpp"
#include "curation/scoring/clip-ranker.hpp"

namespace Curation::Scoring {

struct FeedbackGuidedCandidateStageOptions {
	CandidateGenerationOptions generation;
	CheapScoringContext scoring;
	CandidateQualityGateOptions qualityGate;
	ClipRankerOptions ranking;
	QString videoPath;
};

class FeedbackGuidedCandidateStage {
public:
	QVector<ClipCandidate> appendCandidates(const TranscriptIndex &index, QVector<ClipCandidate> candidates,
						const Curation::Feedback::FeedbackRangeMemory &memory,
						const FeedbackGuidedCandidateStageOptions &options) const;
};

} // namespace Curation::Scoring
