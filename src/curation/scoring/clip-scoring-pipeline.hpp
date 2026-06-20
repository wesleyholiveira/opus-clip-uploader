#pragma once

#include "curation/scoring/candidate-generator.hpp"
#include "curation/scoring/candidate-quality-gate.hpp"
#include "curation/scoring/cheap-clip-scorer.hpp"
#include "curation/scoring/clip-ranker.hpp"
#include "curation/scoring/semantic-clip-scorer.hpp"
#include "curation/scoring/semantic-reranker.hpp"

namespace Curation::Scoring {

struct ClipScoringPipelineOptions {
	CandidateGenerationOptions generation;
	CheapScoringContext scoring;
	SemanticClipScoringOptions semantic;
	SemanticRerankerOptions rerankerOptions;
	CandidateQualityGateOptions qualityGate;
	ClipRankerOptions ranking;
	const SemanticEmbeddingProvider *embeddingProvider = nullptr;
	const SemanticReranker *reranker = nullptr;
};

class ClipScoringPipeline {
public:
	ClipScoringResult score(const RecordingTranscript &transcript, const ClipScoringPipelineOptions &options) const;

private:
	SemanticScoringContext semanticContextFromOptions(const ClipScoringPipelineOptions &options) const;
	SemanticRerankerContext rerankerContextFromOptions(const ClipScoringPipelineOptions &options) const;
	CandidateQualityGateOptions qualityGateOptionsFromOptions(const ClipScoringPipelineOptions &options) const;
	QString buildSummary(const QVector<ClipCandidate> &candidates) const;
};

} // namespace Curation::Scoring
