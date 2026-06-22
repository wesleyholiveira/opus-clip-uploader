#pragma once

#include "curation/scoring/candidate-generator.hpp"
#include "curation/scoring/candidate-quality-gate.hpp"
#include "curation/scoring/cheap-clip-scorer.hpp"
#include "curation/scoring/clip-candidate.hpp"
#include "curation/scoring/semantic-model.hpp"
#include "curation/scoring/transcript-index.hpp"

namespace Curation::Scoring {

struct ExchangeArcBoundaryRefinementOptions {
	CandidateGenerationOptions generation;
	CheapScoringContext scoring;
	CandidateQualityGateOptions qualityGate;
	const SemanticEmbeddingProvider *embeddingProvider = nullptr;
};

class ExchangeArcBoundaryRefiner {
public:
	ClipCandidate refine(const TranscriptIndex &index, const ClipCandidate &candidate,
				     const ExchangeArcBoundaryRefinementOptions &options) const;
};

} // namespace Curation::Scoring
