#pragma once

#include "curation/scoring/candidate-generator.hpp"
#include "curation/scoring/candidate-quality-gate.hpp"
#include "curation/scoring/cheap-clip-scorer.hpp"
#include "curation/scoring/semantic-coarse-retriever.hpp"
#include "curation/scoring/transcript-index.hpp"

namespace Curation::Scoring {

class CandidateBuilder {
public:
	static ClipCandidate buildForRange(const TranscriptIndex &index,
		const CandidateGenerationOptions &generation,
		const CheapScoringContext &scoring,
		const CandidateQualityGateOptions &qualityGate,
		const ClipDuration &range,
		const SemanticCoarseRegion &region);
	static ClipCandidate buildForSegmentWindow(const TranscriptIndex &index,
		const CandidateGenerationOptions &generation,
		const CheapScoringContext &scoring,
		const CandidateQualityGateOptions &qualityGate,
		int firstIndex,
		int lastIndex,
		const SemanticCoarseRegion &region);
	static ClipCandidate buildVariantFromSeed(const TranscriptIndex &index,
		const CandidateGenerationOptions &generation,
		const CheapScoringContext &scoring,
		const CandidateQualityGateOptions &qualityGate,
		const ClipCandidate &seed,
		const ClipDuration &range,
		const QString &variantEvidence);
	static ClipCandidate scoreStructurally(const TranscriptIndex &index, const ClipCandidate &candidate);
	static bool isStructurallyViable(const ClipCandidate &candidate,
		const CandidateGenerationOptions &generation,
		const CandidateQualityGateOptions &qualityGate);
	static QVector<ClipCandidate> enforceSemanticAvailability(QVector<ClipCandidate> candidates,
		bool requireSemanticScoring,
		bool embeddingProviderConfigured);

private:
	static double durationScore(double durationSec);
	static double boundaryScore(const TranscriptIndex &index, const ClipCandidate &candidate);
};

} // namespace Curation::Scoring
