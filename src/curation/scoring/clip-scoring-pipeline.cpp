#include "curation/scoring/clip-scoring-pipeline.hpp"

#include <QStringList>

#include <algorithm>
#include <utility>

using namespace Curation::Scoring;

ClipScoringResult ClipScoringPipeline::score(const RecordingTranscript &transcript,
						     const ClipScoringPipelineOptions &options) const
{
	TranscriptIndex index(transcript);
	ClipScoringResult result;
	if (index.isEmpty()) {
		result.summary = QStringLiteral("no_candidates: empty_transcript");
		return result;
	}

	CandidateGenerator generator;
	QVector<ClipCandidate> candidates = generator.generate(index, options.generation);

	CheapClipScorer cheapScorer;
	for (ClipCandidate &candidate : candidates)
		candidate = cheapScorer.score(index, candidate, options.scoring);

	SemanticClipScorer semanticScorer;
	const SemanticScoringContext semanticContext = semanticContextFromOptions(options);
	for (ClipCandidate &candidate : candidates)
		candidate = semanticScorer.score(index, candidate, semanticContext, options.semantic, options.embeddingProvider);

	SemanticRerankerStage rerankerStage;
	candidates = rerankerStage.apply(std::move(candidates), rerankerContextFromOptions(options), options.rerankerOptions,
					     options.reranker);

	CandidateQualityGate qualityGate;
	candidates = qualityGate.apply(std::move(candidates), qualityGateOptionsFromOptions(options));

	ClipRanker ranker;
	result.candidates = ranker.rank(std::move(candidates), options.ranking);
	result.summary = buildSummary(result.candidates);
	return result;
}

SemanticScoringContext ClipScoringPipeline::semanticContextFromOptions(const ClipScoringPipelineOptions &options) const
{
	SemanticScoringContext context;
	context.presetId = options.scoring.presetId;
	context.mainTarget = options.scoring.mainTarget;
	context.reliableMainTarget = options.scoring.reliableMainTarget;
	return context;
}

SemanticRerankerContext ClipScoringPipeline::rerankerContextFromOptions(const ClipScoringPipelineOptions &options) const
{
	SemanticRerankerContext context;
	context.presetId = options.scoring.presetId;
	context.mainTarget = options.scoring.mainTarget;
	context.reliableMainTarget = options.scoring.reliableMainTarget;
	return context;
}

CandidateQualityGateOptions ClipScoringPipeline::qualityGateOptionsFromOptions(
	const ClipScoringPipelineOptions &options) const
{
	CandidateQualityGateOptions qualityOptions = options.qualityGate;
	if (qualityOptions.presetId.trimmed().isEmpty())
		qualityOptions.presetId = options.scoring.presetId;
	if (qualityOptions.mainTarget.trimmed().isEmpty())
		qualityOptions.mainTarget = options.scoring.mainTarget;
	qualityOptions.reliableMainTarget = options.scoring.reliableMainTarget;
	qualityOptions.minFinalScore = std::max(qualityOptions.minFinalScore, options.ranking.minFinalScore * 0.72);
	return qualityOptions;
}

QString ClipScoringPipeline::buildSummary(const QVector<ClipCandidate> &candidates) const
{
	if (candidates.isEmpty())
		return QStringLiteral("no_candidates: scoring_pipeline_found_no_viable_ranges");

	QStringList parts;
	const int limit = static_cast<int>(std::min(static_cast<long long>(5), static_cast<long long>(candidates.size())));
	for (int i = 0; i < limit; ++i) {
		const ClipCandidate &candidate = candidates.at(i);
		parts.append(QStringLiteral("#%1 %2-%3s score=%4 semantic=%5 viewer=%6 boundary=%7 source=%8 evidence=%9")
			     .arg(i + 1)
			     .arg(QString::number(candidate.range.startSec, 'f', 2))
			     .arg(QString::number(candidate.range.endSec, 'f', 2))
			     .arg(QString::number(candidate.scores.final, 'f', 2))
			     .arg(QString::number(candidate.scores.semanticTarget, 'f', 2))
			     .arg(QString::number(candidate.scores.viewerResponse, 'f', 2))
			     .arg(QString::number(candidate.scores.boundary, 'f', 2))
			     .arg(candidate.source)
			     .arg(candidate.evidence.join(QLatin1Char('|'))));
	}
	return parts.join(QStringLiteral("; "));
}
