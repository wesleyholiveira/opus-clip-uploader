#pragma once

#include "curation/scoring/candidate-generator.hpp"
#include "curation/scoring/candidate-quality-gate.hpp"
#include "curation/scoring/cheap-clip-scorer.hpp"
#include "curation/scoring/clip-ranker.hpp"
#include "curation/scoring/semantic-clip-scorer.hpp"
#include "curation/scoring/semantic-coarse-retriever.hpp"
#include "curation/scoring/semantic-reranker.hpp"

#include <QString>

#include <functional>

namespace Curation::Scoring {

struct ClipScoringPipelineBudget {
	int maxCandidatesBeforeEmbedding = 24;
	double preSemanticMinFinalScore = 0.18;
	double preSemanticMinSpacingSec = 0.0;
	bool requireSemanticScoringWhenEmbeddingProviderEnabled = true;
};

struct ClipScoringPipelineProgressUpdate {
	QString message;
	int value = 0;
	int maximum = 100;
};

using ClipScoringPipelineProgressCallback = std::function<void(ClipScoringPipelineProgressUpdate)>;
using ClipScoringPipelineCancellationCallback = std::function<bool()>;

struct ClipScoringPipelineOptions {
	CandidateGenerationOptions generation;
	CheapScoringContext scoring;
	SemanticCoarseRetrievalOptions coarseRetrieval;
	SemanticClipScoringOptions semantic;
	SemanticRerankerOptions rerankerOptions;
	CandidateQualityGateOptions qualityGate;
	ClipRankerOptions ranking;
	ClipScoringPipelineBudget budget;
	const SemanticEmbeddingProvider *embeddingProvider = nullptr;
	const SemanticReranker *reranker = nullptr;
	ClipScoringPipelineProgressCallback progressCallback = {};
	ClipScoringPipelineCancellationCallback cancellationCallback = {};
};

class ClipScoringPipeline {
public:
	ClipScoringResult score(const RecordingTranscript &transcript, const ClipScoringPipelineOptions &options) const;

private:
	SemanticCoarseRetrievalContext coarseContextFromOptions(const ClipScoringPipelineOptions &options) const;
	SemanticScoringContext semanticContextFromOptions(const ClipScoringPipelineOptions &options) const;
	SemanticRerankerContext rerankerContextFromOptions(const ClipScoringPipelineOptions &options) const;
	ClipRankerOptions preSemanticRankingOptionsFromOptions(const ClipScoringPipelineOptions &options) const;
	CandidateQualityGateOptions qualityGateOptionsFromOptions(const ClipScoringPipelineOptions &options,
										 bool rerankerWasAvailable) const;
	QVector<ClipCandidate> candidatesFromSemanticCoarseRegions(const TranscriptIndex &index,
							 const ClipScoringPipelineOptions &options,
							 const QVector<SemanticCoarseRegion> &regions) const;
	QVector<ClipCandidate> fallbackCandidatesFromLocalHeuristics(const TranscriptIndex &index,
							 const ClipScoringPipelineOptions &options) const;
	QVector<ClipCandidate> refineCandidatesToSemanticTopicSpans(const TranscriptIndex &index,
							 const ClipScoringPipelineOptions &options,
							 QVector<ClipCandidate> candidates) const;
	ClipCandidate refineCandidateToSemanticTopicSpan(const TranscriptIndex &index,
						       const ClipScoringPipelineOptions &options,
						       const ClipCandidate &candidate) const;
	ClipCandidate buildCandidateForRange(const TranscriptIndex &index, const ClipScoringPipelineOptions &options,
					     const ClipDuration &range, const SemanticCoarseRegion &region) const;
	ClipCandidate scoreStructurally(const TranscriptIndex &index, const ClipCandidate &candidate) const;
	ClipCandidate buildCandidateForSegmentWindow(const TranscriptIndex &index,
						       const ClipScoringPipelineOptions &options, int firstIndex, int lastIndex,
						       const SemanticCoarseRegion &region) const;
	bool isStructurallyViable(const ClipCandidate &candidate, const ClipScoringPipelineOptions &options) const;
	double durationScore(double durationSec) const;
	double boundaryScore(const TranscriptIndex &index, const ClipCandidate &candidate) const;
	QVector<ClipCandidate> enforceSemanticAvailability(QVector<ClipCandidate> candidates,
						     const ClipScoringPipelineOptions &options) const;
	QString buildSummary(const QVector<ClipCandidate> &candidates) const;
	QString rejectionSummary(const QVector<ClipCandidate> &candidates) const;
	QString noCandidateSummary(const QString &reason, const QVector<ClipCandidate> &candidates) const;
};

} // namespace Curation::Scoring
