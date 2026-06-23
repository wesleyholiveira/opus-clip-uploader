#pragma once

#include "curation/scoring/candidate-builder.hpp"
#include "curation/scoring/exchange-arc-boundary-refiner.hpp"

namespace Curation::Scoring {

struct BoundaryRefinementStageOptions {
	CandidateGenerationOptions generation;
	CheapScoringContext scoring;
	CandidateQualityGateOptions qualityGate;
	const SemanticEmbeddingProvider *embeddingProvider = nullptr;
	bool viewerMessagePreset = false;
};

class BoundaryRefinementStage {
public:
	QVector<ClipCandidate> apply(const TranscriptIndex &index,
		QVector<ClipCandidate> candidates,
		const BoundaryRefinementStageOptions &options) const;

private:
	ClipCandidate refineOne(const TranscriptIndex &index,
		const ClipCandidate &candidate,
		const BoundaryRefinementStageOptions &options) const;
};

} // namespace Curation::Scoring
