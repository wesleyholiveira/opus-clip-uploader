#pragma once

#include "curation/scoring/candidate-builder.hpp"
#include "curation/scoring/semantic-coarse-retriever.hpp"

namespace Curation::Scoring {

struct CandidateSourceBuilderOptions {
	CandidateGenerationOptions generation;
	CheapScoringContext scoring;
	CandidateQualityGateOptions qualityGate;
	int maxRawCandidates = 128;
	bool viewerMessagePreset = false;
};

class CandidateSourceBuilder {
public:
	QVector<ClipCandidate> fromSemanticCoarseRegions(const TranscriptIndex &index,
							 const QVector<SemanticCoarseRegion> &regions,
							 const CandidateSourceBuilderOptions &options) const;
	QVector<ClipCandidate> fromLocalHeuristics(const TranscriptIndex &index,
						   const CandidateSourceBuilderOptions &options) const;
	static int semanticCandidateBudget(const CandidateSourceBuilderOptions &options, int requestedBeforeEmbedding);
};

} // namespace Curation::Scoring
