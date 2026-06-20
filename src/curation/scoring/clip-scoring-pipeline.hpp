#pragma once

#include "curation/scoring/candidate-generator.hpp"
#include "curation/scoring/cheap-clip-scorer.hpp"
#include "curation/scoring/clip-ranker.hpp"

namespace Curation::Scoring {

struct ClipScoringPipelineOptions {
	CandidateGenerationOptions generation;
	CheapScoringContext scoring;
	ClipRankerOptions ranking;
};

class ClipScoringPipeline {
public:
	ClipScoringResult score(const RecordingTranscript &transcript, const ClipScoringPipelineOptions &options) const;

private:
	QString buildSummary(const QVector<ClipCandidate> &candidates) const;
};

} // namespace Curation::Scoring
