#include "curation/scoring/clip-scoring-pipeline.hpp"

#include "curation/scoring/semantic-prototypes.hpp"

#include <QMap>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

using namespace Curation::Scoring;

namespace {

static double boundedScore(double value)
{
	return std::clamp(value, 0.0, 1.0);
}

static QString rangeKey(const ClipDuration &range)
{
	return QStringLiteral("%1:%2")
		.arg(QString::number(range.startSec, 'f', 1))
		.arg(QString::number(range.endSec, 'f', 1));
}

static ClipDuration clampCandidateRange(const TranscriptIndex &index, const ClipDuration &range,
					       const ClipScoringPipelineOptions &options)
{
	ClipDuration clamped = index.clampRange(range, options.generation.searchRange);
	const double maxDurationSec = std::max(options.generation.minDurationSec, options.generation.maxDurationSec);
	if ((clamped.endSec - clamped.startSec) > maxDurationSec)
		clamped.endSec = std::min(options.generation.searchRange.endSec, clamped.startSec + maxDurationSec);
	return clamped;
}

static bool isSelectionEvidence(const QString &evidence)
{
	return evidence == QStringLiteral("mmr_diversity_selected") || evidence.startsWith(QStringLiteral("selected_rank:")) ||
	       evidence.startsWith(QStringLiteral("mmr:")) || evidence.startsWith(QStringLiteral("mmr_similarity:"));
}


static void reportPipelineProgress(const ClipScoringPipelineOptions &options, const QString &message, int value,
	int maximum = 100)
{
	if (!options.progressCallback)
		return;

	ClipScoringPipelineProgressUpdate update;
	update.message = message;
	update.value = std::clamp(value, 0, std::max(0, maximum));
	update.maximum = std::max(0, maximum);
	options.progressCallback(std::move(update));
}

static bool isPipelineCanceled(const ClipScoringPipelineOptions &options)
{
	return options.cancellationCallback && options.cancellationCallback();
}

static bool stopIfCanceled(const ClipScoringPipelineOptions &options, ClipScoringResult &result, const QString &summary)
{
	if (!isPipelineCanceled(options))
		return false;
	reportPipelineProgress(options, QStringLiteral("Clip suggestion analysis canceled."), 100);
	result.summary = summary;
	return true;
}

static QString candidateProgressMessage(const QString &phase, const ClipCandidate &candidate, int index, int total)
{
	return QStringLiteral("%1 %2/%3 (%4-%5s)")
		.arg(phase)
		.arg(index)
		.arg(total)
		.arg(QString::number(candidate.range.startSec, 'f', 2))
		.arg(QString::number(candidate.range.endSec, 'f', 2));
}

static void clearSelectionMetadata(QVector<ClipCandidate> &candidates)
{
	for (ClipCandidate &candidate : candidates) {
		candidate.selectedRank = 0;
		candidate.selectedMmrScore = 0.0;
		candidate.selectedMmrSimilarity = 0.0;
		candidate.evidence.erase(std::remove_if(candidate.evidence.begin(), candidate.evidence.end(), isSelectionEvidence),
					     candidate.evidence.end());
	}
}

struct TopicSpanChunk {
	int firstIndex = -1;
	int lastIndex = -1;
	double startSec = 0.0;
	double endSec = 0.0;
	QString text;
	SemanticEmbedding embedding;
	double target = 0.0;
	double value = 0.0;
	double hook = 0.0;
	double resolution = 0.0;
	double meta = 0.0;
	double shift = 0.0;
};

static double topicSpanDurationScore(double durationSec, const ClipScoringPipelineOptions &options)
{
	const double minSec = std::max(1.0, options.generation.minDurationSec);
	const double maxSec = std::max(minSec, options.generation.maxDurationSec);
	if (durationSec < minSec || durationSec > maxSec)
		return 0.0;

	// Viewer-message clips often have a short, self-contained local answer. When the
	// active clip-length bounds are short, prefer the compact resolved exchange over
	// the longest semantically similar answer tail. This prevents the topic span
	// refiner from drifting from a 8-12s hook/answer into a 40-60s live segment.
	if (maxSec <= 40.0) {
		if (durationSec <= 14.0)
			return 1.0;
		if (durationSec <= 22.0)
			return 0.94;
		return boundedScore(0.82 - ((durationSec - 22.0) / std::max(1.0, maxSec - 22.0)) * 0.34);
	}

	if (durationSec >= 22.0 && durationSec <= 48.0)
		return 1.0;
	if (durationSec < 22.0)
		return boundedScore(0.72 + ((durationSec - minSec) / std::max(1.0, 22.0 - minSec)) * 0.28);
	return boundedScore(1.0 - ((durationSec - 48.0) / std::max(1.0, maxSec - 48.0)) * 0.45);
}

static QVector<TopicSpanChunk> topicSpanChunksForCandidate(const TranscriptIndex &index,
	const ClipCandidate &candidate, const ClipScoringPipelineOptions &options)
{
	QVector<TopicSpanChunk> chunks;
	if (!options.embeddingProvider || !options.embeddingProvider->isAvailable())
		return chunks;

	const QVector<int> indices = index.segmentIndicesForRange(candidate.range);
	if (indices.isEmpty())
		return chunks;

	TopicSpanChunk current;
	double previousEndSec = -1.0;
	auto flushCurrent = [&]() {
		const QString text = current.text.simplified();
		if (current.firstIndex >= 0 && current.lastIndex >= current.firstIndex && text.size() >= 24) {
			current.text = text;
			current.embedding = options.embeddingProvider->embed(current.text);
			if (current.embedding.isValid())
				chunks.append(current);
		}
		current = {};
	};

	for (const int segmentIndex : indices) {
		const TranscriptSegment *segment = index.segmentAt(segmentIndex);
		if (!segment || segment->text.trimmed().isEmpty())
			continue;

		const QString text = segment->text.trimmed();
		const bool startsNewChunk = current.firstIndex < 0;
		const double gapSec = previousEndSec >= 0.0 ? std::max(0.0, segment->startSec - previousEndSec) : 0.0;
		const double nextDurationSec = startsNewChunk ? std::max(0.0, segment->endSec - segment->startSec)
							 : std::max(0.0, segment->endSec - current.startSec);
		const int nextChars = current.text.size() + text.size() + 1;
		const bool shouldFlush = !startsNewChunk &&
			(gapSec > 1.25 || nextDurationSec > 4.25 || nextChars > 220);
		if (shouldFlush)
			flushCurrent();

		if (current.firstIndex < 0) {
			current.firstIndex = segmentIndex;
			current.startSec = segment->startSec;
		}
		current.lastIndex = segmentIndex;
		current.endSec = segment->endSec;
		if (!current.text.isEmpty())
			current.text.append(QLatin1Char(' '));
		current.text.append(text);
		previousEndSec = segment->endSec;
	}
	flushCurrent();
	return chunks;
}

static double topicSpanCohesion(const QVector<TopicSpanChunk> &chunks, int first, int last)
{
	if (first < 0 || last < first || last >= static_cast<int>(chunks.size()))
		return 0.0;
	if (first == last)
		return 0.66;

	double sum = 0.0;
	double minSimilarity = 1.0;
	int comparisons = 0;
	for (int i = first; i < last; ++i) {
		const double similarity = cosineSimilarity(chunks.at(i).embedding, chunks.at(i + 1).embedding);
		sum += similarity;
		minSimilarity = std::min(minSimilarity, similarity);
		++comparisons;
	}
	if (comparisons <= 0)
		return 0.0;
	const double average = sum / static_cast<double>(comparisons);
	return boundedScore((average * 0.72) + (minSimilarity * 0.28));
}

static QString topicSpanEvidence(double cohesion, double score)
{
	return QStringLiteral("semantic_topic_span_cohesion:%1 score:%2")
		.arg(QString::number(cohesion, 'f', 2), QString::number(score, 'f', 2));
}


static QStringList uniqueTexts(QStringList values)
{
	QStringList result;
	for (const QString &value : values) {
		const QString text = value.simplified();
		if (!text.isEmpty() && !result.contains(text))
			result.append(text);
	}
	return result;
}

static double maxPrototypeSimilarityForEmbedding(const SemanticEmbeddingProvider &provider,
	const SemanticEmbedding &embedding, const QStringList &prototypes)
{
	if (!embedding.isValid() || prototypes.isEmpty())
		return 0.0;

	double best = 0.0;
	for (const QString &prototype : prototypes) {
		const QString text = prototype.simplified();
		if (text.isEmpty())
			continue;
		best = std::max(best, cosineSimilarity(embedding, provider.embed(text)));
	}
	return boundedScore(best);
}

static void scoreTopicSpanChunks(QVector<TopicSpanChunk> &chunks, const ClipScoringPipelineOptions &options)
{
	if (!options.embeddingProvider || !options.embeddingProvider->isAvailable() || chunks.isEmpty())
		return;

	const SemanticPrototypeSet &defaults = defaultSemanticPrototypes();
	QStringList targetPrototypes = targetPrototypesForPreset(options.scoring.presetId, options.scoring.mainTarget);
	QStringList valuePrototypes = uniqueTexts(QStringList(defaults.clipValue) + targetPrototypes);
	QStringList hookPrototypes = uniqueTexts(QStringList(defaults.hook) + defaults.viewerMessage + targetPrototypes);
	QStringList resolutionPrototypes = uniqueTexts(QStringList(defaults.resolution) + defaults.directAnswer + targetPrototypes);
	QStringList metaPrototypes = uniqueTexts(QStringList(defaults.metaNoise) + defaults.greetingNoise + defaults.streamManagement);
	QStringList shiftPrototypes = uniqueTexts(QStringList(defaults.topicShift) + defaults.streamManagement);

	for (TopicSpanChunk &chunk : chunks) {
		chunk.target = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, chunk.embedding, targetPrototypes);
		chunk.value = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, chunk.embedding, valuePrototypes);
		chunk.hook = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, chunk.embedding, hookPrototypes);
		chunk.resolution = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, chunk.embedding, resolutionPrototypes);
		chunk.meta = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, chunk.embedding, metaPrototypes);
		chunk.shift = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, chunk.embedding, shiftPrototypes);
	}
}

static double averageChunkScore(const QVector<TopicSpanChunk> &chunks, int first, int last,
	double TopicSpanChunk::*field)
{
	if (first < 0 || last < first || last >= static_cast<int>(chunks.size()))
		return 0.0;

	double sum = 0.0;
	for (int i = first; i <= last; ++i)
		sum += chunks.at(i).*field;
	return boundedScore(sum / static_cast<double>(last - first + 1));
}

static double maxChunkScore(const QVector<TopicSpanChunk> &chunks, int first, int last,
	double TopicSpanChunk::*field)
{
	if (first < 0 || last < first || last >= static_cast<int>(chunks.size()))
		return 0.0;

	double best = 0.0;
	for (int i = first; i <= last; ++i)
		best = std::max(best, chunks.at(i).*field);
	return boundedScore(best);
}

static QString topicBoundaryEvidence(const QVector<TopicSpanChunk> &chunks, int first, int last)
{
	if (first < 0 || last < first || last >= static_cast<int>(chunks.size()))
		return {};

	return QStringLiteral("semantic_boundary openingHook:%1 openingMeta:%2 endingResolution:%3 endingMeta:%4 endingShift:%5")
		.arg(QString::number(chunks.at(first).hook, 'f', 2), QString::number(chunks.at(first).meta, 'f', 2),
		     QString::number(chunks.at(last).resolution, 'f', 2), QString::number(chunks.at(last).meta, 'f', 2),
		     QString::number(chunks.at(last).shift, 'f', 2));
}


static double openingContextScore(const TopicSpanChunk &chunk)
{
	const double positive = std::max({chunk.target, chunk.value, chunk.hook});
	return positive - (chunk.meta * 0.38) - (chunk.shift * 0.18);
}

static bool isUsefulLeadInChunk(const TopicSpanChunk &chunk, const TopicSpanChunk &currentOpening)
{
	const double leadScore = openingContextScore(chunk);
	const double currentScore = openingContextScore(currentOpening);
	const bool hasUsefulSignal = std::max({chunk.target, chunk.value, chunk.hook}) >= 0.54;
	const bool notMostlyMeta = chunk.meta < 0.74 && chunk.meta <= std::max({chunk.target, chunk.value, chunk.hook}) + 0.14;
	return hasUsefulSignal && notMostlyMeta && leadScore >= currentScore - 0.12;
}

static bool isUsefulTailContinuationChunk(const QVector<TopicSpanChunk> &chunks, int currentLast, int candidateNext)
{
	if (currentLast < 0 || candidateNext <= currentLast || candidateNext >= static_cast<int>(chunks.size()))
		return false;

	const TopicSpanChunk &current = chunks.at(currentLast);
	const TopicSpanChunk &next = chunks.at(candidateNext);
	const double continuity = cosineSimilarity(current.embedding, next.embedding);
	const double nextPositive = std::max({next.target, next.value, next.resolution, next.hook * 0.82});
	const double nextNoise = std::max(next.meta, next.shift);
	const bool hasContinuationValue = nextPositive >= 0.52 || next.resolution >= 0.56 || continuity >= 0.66;
	const bool sameExchange = continuity >= 0.58 || next.target >= current.target - 0.08 || next.value >= 0.54;
	const bool clearlyDifferentMetaTopic = nextNoise >= 0.84 && nextNoise >= nextPositive + 0.10 && continuity < 0.68;
	return hasContinuationValue && sameExchange && !clearlyDifferentMetaTopic;
}

static bool isHardNoisyEndingChunk(const TopicSpanChunk &chunk)
{
	const double endingNoise = std::max(chunk.meta, chunk.shift);
	const double endingPositive = std::max({chunk.resolution, chunk.value, chunk.target});
	return endingNoise >= 0.84 && endingNoise >= endingPositive + 0.12;
}

static bool canUseTopicSpanBounds(const QVector<TopicSpanChunk> &chunks, int first, int last,
	const ClipScoringPipelineOptions &options)
{
	if (first < 0 || last < first || last >= static_cast<int>(chunks.size()))
		return false;
	const double durationSec = chunks.at(last).endSec - chunks.at(first).startSec;
	const double minDurationSec = std::max(8.0, options.generation.boundaryMinDurationSec);
	const double maxDurationSec = std::max(minDurationSec, options.generation.maxDurationSec);
	return durationSec >= minDurationSec && durationSec <= maxDurationSec + 0.1;
}

} // namespace

ClipScoringResult ClipScoringPipeline::score(const RecordingTranscript &transcript,
	const ClipScoringPipelineOptions &options) const
{
	TranscriptIndex index(transcript);
	ClipScoringResult result;
	if (stopIfCanceled(options, result, QStringLiteral("canceled_before_scoring")))
		return result;
	reportPipelineProgress(options, QStringLiteral("Indexing transcript for clip suggestions..."), 1);
	if (index.isEmpty()) {
		result.summary = QStringLiteral("no_candidates: empty_transcript");
		return result;
	}

	QVector<ClipCandidate> candidates;
	const bool semanticBackendAvailable = options.embeddingProvider && options.embeddingProvider->isAvailable() &&
		options.coarseRetrieval.enabled;
	if (semanticBackendAvailable) {
		if (stopIfCanceled(options, result, QStringLiteral("canceled_before_coarse_retrieval")))
			return result;
		reportPipelineProgress(options, QStringLiteral("Embedding coarse transcript windows..."), 5);
		SemanticCoarseRetriever retriever;
		const QVector<SemanticCoarseRegion> regions = retriever.retrieve(index, coarseContextFromOptions(options),
			options.coarseRetrieval, options.embeddingProvider);
		if (stopIfCanceled(options, result, QStringLiteral("canceled_during_coarse_retrieval")))
			return result;
		reportPipelineProgress(options, QStringLiteral("Coarse semantic regions selected: %1").arg(static_cast<int>(regions.size())), 18);
		if (regions.isEmpty()) {
			result.summary = QStringLiteral("no_candidates: semantic_coarse_retrieval_found_no_regions");
			return result;
		}

		candidates = candidatesFromSemanticCoarseRegions(index, options, regions);
		reportPipelineProgress(options, QStringLiteral("Generated candidate marker windows: %1").arg(static_cast<int>(candidates.size())), 28);
	} else {
		candidates = fallbackCandidatesFromLocalHeuristics(index, options);
		reportPipelineProgress(options, QStringLiteral("Generated fallback candidate marker windows: %1").arg(static_cast<int>(candidates.size())), 28);
	}

	if (candidates.isEmpty()) {
		result.summary = QStringLiteral("no_candidates: candidate_generation_found_no_viable_ranges");
		return result;
	}

	ClipRanker ranker;
	reportPipelineProgress(options, QStringLiteral("Pre-ranking candidate marker windows..."), 32);
	candidates = ranker.rank(std::move(candidates), preSemanticRankingOptionsFromOptions(options));
	clearSelectionMetadata(candidates);
	reportPipelineProgress(options, QStringLiteral("Candidate marker windows after pre-ranking: %1").arg(static_cast<int>(candidates.size())), 36);
	if (semanticBackendAvailable && options.embeddingProvider && options.embeddingProvider->isAvailable()) {
		candidates = refineCandidatesToSemanticTopicSpans(index, options, std::move(candidates));
		reportPipelineProgress(options, QStringLiteral("Refined candidate marker windows to semantic topic spans: %1").arg(static_cast<int>(candidates.size())), 38);
	}
	if (candidates.isEmpty()) {
		result.summary = semanticBackendAvailable
			? QStringLiteral("no_candidates: semantic_coarse_structural_ranking_found_no_viable_ranges")
			: QStringLiteral("no_candidates: pre_semantic_ranking_found_no_viable_ranges");
		return result;
	}

	SemanticClipScorer semanticScorer;
	const SemanticScoringContext semanticContext = semanticContextFromOptions(options);
	const int semanticTotal = static_cast<int>(candidates.size());
	for (int i = 0; i < semanticTotal; ++i) {
		if (stopIfCanceled(options, result, QStringLiteral("canceled_during_semantic_scoring")))
			return result;
		reportPipelineProgress(options, candidateProgressMessage(QStringLiteral("Embedding/scoring marker"), candidates[i], i + 1, semanticTotal),
			40 + ((i * 24) / std::max(1, semanticTotal)));
		candidates[i] = semanticScorer.score(index, candidates[i], semanticContext, options.semantic, options.embeddingProvider);
	}
	reportPipelineProgress(options, QStringLiteral("Semantic scoring finished for %1 marker candidates").arg(semanticTotal), 66);

	candidates = enforceSemanticAvailability(std::move(candidates), options);
	if (std::all_of(candidates.constBegin(), candidates.constEnd(), [](const ClipCandidate &candidate) {
		    return candidate.rejectedByQualityGate;
	    })) {
		result.summary = noCandidateSummary(QStringLiteral("semantic_embedding_required_but_unavailable"), candidates);
		return result;
	}
	candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [](const ClipCandidate &candidate) {
		return candidate.rejectedByQualityGate;
	}), candidates.end());

	if (stopIfCanceled(options, result, QStringLiteral("canceled_before_reranking")))
		return result;

	const bool rerankerWasAvailable = options.rerankerOptions.enabled && options.reranker && options.reranker->isAvailable();
	SemanticRerankerStage rerankerStage;
	reportPipelineProgress(options, QStringLiteral("Reranking %1 marker candidates...").arg(static_cast<int>(candidates.size())), 72);
	candidates = rerankerStage.apply(std::move(candidates), rerankerContextFromOptions(options), options.rerankerOptions,
		options.reranker);
	if (stopIfCanceled(options, result, QStringLiteral("canceled_during_reranking")))
		return result;
	reportPipelineProgress(options, QStringLiteral("Reranking completed for %1 marker candidates").arg(static_cast<int>(candidates.size())), 82);

	if (stopIfCanceled(options, result, QStringLiteral("canceled_before_quality_gate")))
		return result;

	CandidateQualityGate qualityGate;
	reportPipelineProgress(options, QStringLiteral("Applying semantic quality gate..."), 86);
	candidates = qualityGate.apply(std::move(candidates), qualityGateOptionsFromOptions(options, rerankerWasAvailable));
	if (std::all_of(candidates.constBegin(), candidates.constEnd(), [](const ClipCandidate &candidate) {
		    return candidate.rejectedByQualityGate;
	    })) {
		result.summary = noCandidateSummary(QStringLiteral("quality_gate_rejected_all"), candidates);
		return result;
	}

	if (stopIfCanceled(options, result, QStringLiteral("canceled_before_final_ranking")))
		return result;

	reportPipelineProgress(options, QStringLiteral("Selecting final marker suggestions..."), 92);
	result.candidates = ranker.rank(candidates, options.ranking);
	if (result.candidates.isEmpty())
		result.summary = noCandidateSummary(QStringLiteral("final_ranking_found_no_viable_ranges"), candidates);
	else {
		for (int i = 0; i < static_cast<int>(result.candidates.size()); ++i)
			reportPipelineProgress(options, candidateProgressMessage(QStringLiteral("Selected marker"), result.candidates.at(i), i + 1, static_cast<int>(result.candidates.size())),
				94 + ((i * 5) / std::max(1, static_cast<int>(result.candidates.size()))));
		result.summary = buildSummary(result.candidates);
	}
	reportPipelineProgress(options, QStringLiteral("Marker suggestion analysis finished."), 100);
	return result;
}

SemanticCoarseRetrievalContext ClipScoringPipeline::coarseContextFromOptions(
	const ClipScoringPipelineOptions &options) const
{
	SemanticCoarseRetrievalContext context;
	context.presetId = options.scoring.presetId;
	context.mainTarget = options.scoring.mainTarget;
	context.reliableMainTarget = options.scoring.reliableMainTarget;
	return context;
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

ClipRankerOptions ClipScoringPipeline::preSemanticRankingOptionsFromOptions(
	const ClipScoringPipelineOptions &options) const
{
	ClipRankerOptions ranking = options.ranking;
	ranking.maxCandidates = std::max(1, std::min(24, options.budget.maxCandidatesBeforeEmbedding));
	ranking.minFinalScore = options.budget.preSemanticMinFinalScore;
	ranking.minSpacingSec = options.budget.preSemanticMinSpacingSec;
	ranking.useMmr = false;
	return ranking;
}

CandidateQualityGateOptions ClipScoringPipeline::qualityGateOptionsFromOptions(
	const ClipScoringPipelineOptions &options, bool rerankerWasAvailable) const
{
	CandidateQualityGateOptions qualityOptions = options.qualityGate;
	if (qualityOptions.presetId.trimmed().isEmpty())
		qualityOptions.presetId = options.scoring.presetId;
	if (qualityOptions.mainTarget.trimmed().isEmpty())
		qualityOptions.mainTarget = options.scoring.mainTarget;
	qualityOptions.reliableMainTarget = options.scoring.reliableMainTarget;
	qualityOptions.minFinalScore = std::max(qualityOptions.minFinalScore, options.ranking.minFinalScore * 0.72);
	if (rerankerWasAvailable) {
		qualityOptions.requireRerankerWhenAvailable = true;
		qualityOptions.rejectInvalidRerankerWhenRequired = true;
		qualityOptions.minRerankerScoreWhenAvailable =
			std::max(qualityOptions.minRerankerScoreWhenAvailable, 0.55);
		qualityOptions.minRerankerRawScoreWhenAvailable =
			std::max(qualityOptions.minRerankerRawScoreWhenAvailable, 0.50);
		qualityOptions.minStrongRerankerRawScoreWhenAvailable =
			std::max(qualityOptions.minStrongRerankerRawScoreWhenAvailable, 0.78);
		qualityOptions.minConditionalRerankerRawScoreWhenAvailable =
			std::max(qualityOptions.minConditionalRerankerRawScoreWhenAvailable, 0.68);
	}
	return qualityOptions;
}

QVector<ClipCandidate> ClipScoringPipeline::candidatesFromSemanticCoarseRegions(const TranscriptIndex &index,
	const ClipScoringPipelineOptions &options, const QVector<SemanticCoarseRegion> &regions) const
{
	QVector<ClipCandidate> candidates;
	QStringList seen;
	if (regions.isEmpty())
		return candidates;

	const int maxRawCandidates = std::max(1, options.generation.maxRawCandidates);
	const int maxCandidatesPerRegion = std::max(4, maxRawCandidates / std::max(1, static_cast<int>(regions.size())));
	QVector<double> targetDurations;
	if (options.generation.maxDurationSec <= 40.0) {
		targetDurations = {
			std::clamp(10.0, options.generation.minDurationSec, options.generation.maxDurationSec),
			std::clamp(14.0, options.generation.minDurationSec, options.generation.maxDurationSec),
			std::clamp(18.0, options.generation.minDurationSec, options.generation.maxDurationSec),
			std::clamp(24.0, options.generation.minDurationSec, options.generation.maxDurationSec),
			options.generation.maxDurationSec,
		};
	} else {
		targetDurations = {
			std::clamp(24.0, options.generation.minDurationSec, options.generation.maxDurationSec),
			std::clamp(36.0, options.generation.minDurationSec, options.generation.maxDurationSec),
			std::clamp(48.0, options.generation.minDurationSec, options.generation.maxDurationSec),
			std::clamp(60.0, options.generation.minDurationSec, options.generation.maxDurationSec),
			options.generation.maxDurationSec,
		};
	}
	targetDurations.erase(std::unique(targetDurations.begin(), targetDurations.end(), [](double left, double right) {
		return std::fabs(left - right) < 0.25;
	}), targetDurations.end());

	for (const SemanticCoarseRegion &region : regions) {
		const ClipDuration regionRange = index.clampRange(region.range, options.generation.searchRange);
		if ((regionRange.endSec - regionRange.startSec) < options.generation.minDurationSec)
			continue;

		const QVector<int> segmentIndices = index.segmentIndicesForRange(regionRange);
		if (segmentIndices.isEmpty())
			continue;

		QVector<int> anchorPositions;
		const int desiredAnchors = std::max(4, maxCandidatesPerRegion * 2);
		const int anchorStep = std::max(1, static_cast<int>(std::ceil(static_cast<double>(segmentIndices.size()) /
			static_cast<double>(desiredAnchors))));
		for (int position = 0; position < static_cast<int>(segmentIndices.size()); position += anchorStep)
			anchorPositions.append(position);
		if (!anchorPositions.contains(0))
			anchorPositions.prepend(0);
		const int lastAnchor = static_cast<int>(segmentIndices.size()) - 1;
		if (!anchorPositions.contains(lastAnchor))
			anchorPositions.append(lastAnchor);

		QVector<ClipCandidate> regionCandidates;
		regionCandidates.reserve(maxCandidatesPerRegion * 3);
		QStringList regionSeen;

		for (const int anchorPosition : anchorPositions) {
			const int firstIndex = segmentIndices.at(anchorPosition);
			const TranscriptSegment *firstSegment = index.segmentAt(firstIndex);
			if (!firstSegment || firstSegment->text.trimmed().isEmpty())
				continue;
			if (firstSegment->startSec < regionRange.startSec - 0.5 || firstSegment->startSec > regionRange.endSec)
				continue;

			for (const double requestedDurationSec : targetDurations) {
				const double targetEndSec = firstSegment->startSec + requestedDurationSec;
				int lastIndex = -1;
				for (int position = anchorPosition; position < static_cast<int>(segmentIndices.size()); ++position) {
					const int segmentIndex = segmentIndices.at(position);
					const TranscriptSegment *segment = index.segmentAt(segmentIndex);
					if (!segment)
						continue;
					lastIndex = segmentIndex;
					if (segment->endSec >= targetEndSec || segment->endSec >= regionRange.endSec)
						break;
				}

				if (lastIndex < firstIndex)
					continue;

				ClipCandidate candidate = buildCandidateForSegmentWindow(index, options, firstIndex, lastIndex, region);
				const QString key = rangeKey(candidate.range);
				if (regionSeen.contains(key) || seen.contains(key) || !isStructurallyViable(candidate, options))
					continue;

				regionSeen.append(key);
				regionCandidates.append(candidate);
				if (static_cast<int>(regionCandidates.size()) >= maxCandidatesPerRegion * 4)
					break;
			}
			if (static_cast<int>(regionCandidates.size()) >= maxCandidatesPerRegion * 4)
				break;
		}

		std::sort(regionCandidates.begin(), regionCandidates.end(), [](const ClipCandidate &left,
								      const ClipCandidate &right) {
			if (std::fabs(left.scores.final - right.scores.final) > 0.0001)
				return left.scores.final > right.scores.final;
			if (std::fabs(left.scores.boundary - right.scores.boundary) > 0.0001)
				return left.scores.boundary > right.scores.boundary;
			return left.range.startSec < right.range.startSec;
		});

		int acceptedFromRegion = 0;
		for (const ClipCandidate &candidate : regionCandidates) {
			const QString key = rangeKey(candidate.range);
			if (seen.contains(key))
				continue;
			seen.append(key);
			candidates.append(candidate);
			++acceptedFromRegion;
			if (acceptedFromRegion >= maxCandidatesPerRegion || static_cast<int>(candidates.size()) >= maxRawCandidates)
				break;
		}

		if (static_cast<int>(candidates.size()) >= maxRawCandidates)
			break;
	}

	return candidates;
}


QVector<ClipCandidate> ClipScoringPipeline::refineCandidatesToSemanticTopicSpans(const TranscriptIndex &index,
	const ClipScoringPipelineOptions &options, QVector<ClipCandidate> candidates) const
{
	if (!options.embeddingProvider || !options.embeddingProvider->isAvailable())
		return candidates;

	QVector<ClipCandidate> refined;
	refined.reserve(candidates.size());
	QStringList seen;
	for (const ClipCandidate &candidate : candidates) {
		ClipCandidate topicCandidate = refineCandidateToSemanticTopicSpan(index, options, candidate);
		if (!isStructurallyViable(topicCandidate, options))
			topicCandidate = candidate;

		const QString key = rangeKey(topicCandidate.range);
		if (seen.contains(key))
			continue;
		seen.append(key);
		refined.append(topicCandidate);
	}
	return refined.isEmpty() ? candidates : refined;
}

ClipCandidate ClipScoringPipeline::refineCandidateToSemanticTopicSpan(const TranscriptIndex &index,
	const ClipScoringPipelineOptions &options, const ClipCandidate &candidate) const
{
	const double originalDurationSec = candidate.range.endSec - candidate.range.startSec;
	if (originalDurationSec <= std::max(options.generation.minDurationSec + 4.0, 20.0))
		return candidate;

	QVector<TopicSpanChunk> chunks = topicSpanChunksForCandidate(index, candidate, options);
	if (chunks.size() < 2)
		return candidate;
	scoreTopicSpanChunks(chunks, options);

	int bestFirst = -1;
	int bestLast = -1;
	double bestScore = 0.0;
	double bestCohesion = 0.0;
	const double minDurationSec = std::max(8.0, options.generation.boundaryMinDurationSec);
	const double maxDurationSec = std::max(minDurationSec, options.generation.maxDurationSec);

	for (int first = 0; first < static_cast<int>(chunks.size()); ++first) {
		for (int last = first; last < static_cast<int>(chunks.size()); ++last) {
			const double durationSec = chunks.at(last).endSec - chunks.at(first).startSec;
			if (durationSec < minDurationSec)
				continue;
			if (durationSec > maxDurationSec)
				break;

			const QString spanText = index.textForSegmentWindow(chunks.at(first).firstIndex, chunks.at(last).lastIndex);
			if (spanText.trimmed().size() < std::max(36, options.qualityGate.minTextChars / 2))
				continue;

			const double cohesion = topicSpanCohesion(chunks, first, last);
			const double durationFit = topicSpanDurationScore(durationSec, options);
			const double value = std::max(averageChunkScore(chunks, first, last, &TopicSpanChunk::value),
				maxChunkScore(chunks, first, last, &TopicSpanChunk::target));
			const double openingHook = std::max(chunks.at(first).hook, chunks.at(first).target * 0.92);
			const double openingMeta = chunks.at(first).meta;
			const double endingResolution = std::max(chunks.at(last).resolution, chunks.at(last).value * 0.72);
			const double endingNoise = std::max(chunks.at(last).meta, chunks.at(last).shift);
			const double averageMeta = averageChunkScore(chunks, first, last, &TopicSpanChunk::meta);
			const double openingFit = boundedScore(openingHook - (openingMeta * 0.44));
			const double endingFit = boundedScore(endingResolution - (endingNoise * 0.38));
			const double valueFit = boundedScore(value - (averageMeta * 0.24));
			const bool shortLocalBounds = options.generation.maxDurationSec <= 40.0;
			const double conciseLocalBonus = shortLocalBounds ? boundedScore(1.0 - (durationSec /
				std::max(1.0, options.generation.maxDurationSec))) * 0.08 : 0.0;
			const double spanScore = boundedScore((valueFit * 0.25) + (openingFit * 0.25) +
				(endingFit * 0.22) + (cohesion * 0.18) + (durationFit * 0.10) + conciseLocalBonus);

			const bool betterScore = spanScore > bestScore + 0.012;
			const bool similarButEarlier = shortLocalBounds && std::fabs(spanScore - bestScore) <= 0.012 &&
				bestFirst >= 0 && chunks.at(first).startSec < chunks.at(bestFirst).startSec - 0.5 &&
				openingFit >= 0.28;
			const bool similarButShorter = shortLocalBounds && std::fabs(spanScore - bestScore) <= 0.012 &&
				bestFirst >= 0 && durationSec + 1.0 < chunks.at(bestLast).endSec - chunks.at(bestFirst).startSec &&
				endingFit >= 0.28;

			if (betterScore || similarButEarlier || similarButShorter) {
				bestScore = spanScore;
				bestCohesion = cohesion;
				bestFirst = first;
				bestLast = last;
			}
		}
	}

	if (bestFirst < 0 || bestLast < bestFirst || bestScore < 0.42)
		return candidate;

	int adjustedFirst = bestFirst;
	int adjustedLast = bestLast;
	bool addedLeadInContext = false;
	bool extendedTailContinuation = false;
	bool trimmedHardNoisyEnding = false;

	// The best scoring span often identifies the strongest answer core, but a good clip
	// still needs the viewer issue/question immediately before it. Include one compact
	// previous chunk when it is semantically useful and not just stream/meta chatter.
	int leadInExtensions = 0;
	while (leadInExtensions < 3 && adjustedFirst > 0 &&
	       isUsefulLeadInChunk(chunks.at(adjustedFirst - 1), chunks.at(adjustedFirst))) {
		const int candidateFirst = adjustedFirst - 1;
		if (!canUseTopicSpanBounds(chunks, candidateFirst, adjustedLast, options))
			break;
		adjustedFirst = candidateFirst;
		++leadInExtensions;
		addedLeadInContext = true;
	}

	// The best core span can end slightly before the local answer resolves. Keep one
	// or two compact next chunks when they still look like the same exchange instead
	// of trimming merely because the last chunk has moderate meta/topic-shift scores.
	int tailExtensions = 0;
	const bool shortLocalBounds = options.generation.maxDurationSec <= 40.0;
	while (tailExtensions < (shortLocalBounds ? 1 : 2) &&
	       isUsefulTailContinuationChunk(chunks, adjustedLast, adjustedLast + 1) &&
	       canUseTopicSpanBounds(chunks, adjustedFirst, adjustedLast + 1, options)) {
		const double currentDurationSec = chunks.at(adjustedLast).endSec - chunks.at(adjustedFirst).startSec;
		const TopicSpanChunk &currentTail = chunks.at(adjustedLast);
		const TopicSpanChunk &nextTail = chunks.at(adjustedLast + 1);
		const double currentResolution = std::max(currentTail.resolution, currentTail.value * 0.76);
		const double nextPositive = std::max({nextTail.target, nextTail.value, nextTail.resolution});
		const double nextNoise = std::max(nextTail.meta, nextTail.shift);
		const bool localResolutionAlreadyReached = shortLocalBounds && currentDurationSec >= 8.0 &&
			currentResolution >= 0.60 && nextPositive < currentResolution + 0.04 &&
			nextNoise >= nextPositive - 0.02;
		if (localResolutionAlreadyReached)
			break;

		++adjustedLast;
		++tailExtensions;
		extendedTailContinuation = true;
	}

	// Only remove the final chunk when it is a very strong meta/topic break. Moderate
	// endingMeta/endingShift scores are intentionally not enough because they caused
	// useful advice clips to end a few seconds too early.
	while (adjustedLast > adjustedFirst && isHardNoisyEndingChunk(chunks.at(adjustedLast)) &&
	       canUseTopicSpanBounds(chunks, adjustedFirst, adjustedLast - 1, options)) {
		--adjustedLast;
		trimmedHardNoisyEnding = true;
	}

	ClipDuration refinedRange{chunks.at(adjustedFirst).startSec, chunks.at(adjustedLast).endSec};
	refinedRange = index.clampRange(refinedRange, options.generation.searchRange);
	const double refinedDurationSec = refinedRange.endSec - refinedRange.startSec;
	if (refinedDurationSec < minDurationSec || refinedDurationSec > maxDurationSec + 0.1)
		return candidate;

	const bool materiallyChanged = std::fabs(refinedRange.startSec - candidate.range.startSec) > 1.0 ||
		std::fabs(refinedRange.endSec - candidate.range.endSec) > 1.0;
	if (!materiallyChanged) {
		ClipCandidate kept = candidate;
		kept.evidence.append(QStringLiteral("semantic_topic_span_kept"));
		kept.evidence.append(topicSpanEvidence(bestCohesion, bestScore));
		kept.evidence.append(topicBoundaryEvidence(chunks, adjustedFirst, adjustedLast));
		if (addedLeadInContext)
			kept.evidence.append(QStringLiteral("semantic_boundary_lead_in_context"));
		if (extendedTailContinuation)
			kept.evidence.append(QStringLiteral("semantic_boundary_tail_continuation"));
		if (trimmedHardNoisyEnding)
			kept.evidence.append(QStringLiteral("semantic_boundary_hard_topic_break_trimmed"));
		kept.evidence.removeDuplicates();
		return kept;
	}

	ClipCandidate refined = candidate;
	refined.range = refinedRange;
	refined.firstSegmentIndex = index.firstSegmentIndexOverlapping(refined.range);
	refined.lastSegmentIndex = index.lastSegmentIndexOverlapping(refined.range);
	refined.text = index.textForRange(refined.range).simplified();
	refined.anchorText = refined.text.left(220);
	refined.evidence.append(QStringLiteral("semantic_topic_span_refined"));
	if (std::fabs(refinedRange.startSec - candidate.range.startSec) > 1.0)
		refined.evidence.append(QStringLiteral("semantic_boundary_start_refined"));
	if (std::fabs(refinedRange.endSec - candidate.range.endSec) > 1.0)
		refined.evidence.append(QStringLiteral("semantic_boundary_end_refined"));
	refined.evidence.append(topicSpanEvidence(bestCohesion, bestScore));
	refined.evidence.append(topicBoundaryEvidence(chunks, adjustedFirst, adjustedLast));
	if (addedLeadInContext)
		refined.evidence.append(QStringLiteral("semantic_boundary_lead_in_context"));
	if (extendedTailContinuation)
		refined.evidence.append(QStringLiteral("semantic_boundary_tail_continuation"));
	if (trimmedHardNoisyEnding)
		refined.evidence.append(QStringLiteral("semantic_boundary_hard_topic_break_trimmed"));
	refined.evidence.removeDuplicates();
	return scoreStructurally(index, refined);
}

QVector<ClipCandidate> ClipScoringPipeline::fallbackCandidatesFromLocalHeuristics(const TranscriptIndex &index,
	const ClipScoringPipelineOptions &options) const
{
	CandidateGenerator generator;
	QVector<ClipCandidate> candidates = generator.generate(index, options.generation);
	CheapClipScorer cheapScorer;
	for (ClipCandidate &candidate : candidates)
		candidate = cheapScorer.score(index, candidate, options.scoring);
	return candidates;
}

ClipCandidate ClipScoringPipeline::buildCandidateForSegmentWindow(const TranscriptIndex &index,
	const ClipScoringPipelineOptions &options, int firstIndex, int lastIndex, const SemanticCoarseRegion &region) const
{
	const TranscriptSegment *firstSegment = index.segmentAt(firstIndex);
	const TranscriptSegment *lastSegment = index.segmentAt(lastIndex);
	if (!firstSegment || !lastSegment || lastIndex < firstIndex)
		return {};

	return buildCandidateForRange(index, options, {firstSegment->startSec, lastSegment->endSec}, region);
}

bool ClipScoringPipeline::isStructurallyViable(const ClipCandidate &candidate,
	const ClipScoringPipelineOptions &options) const
{
	const double durationSec = candidate.range.endSec - candidate.range.startSec;
	if (durationSec < options.generation.minDurationSec || durationSec > options.generation.maxDurationSec + 3.0)
		return false;
	if (candidate.firstSegmentIndex < 0 || candidate.lastSegmentIndex < candidate.firstSegmentIndex)
		return false;

	const QString text = candidate.text.trimmed();
	if (text.size() < std::max(50, options.qualityGate.minTextChars))
		return false;

	const double charsPerSecond = durationSec > 0.0 ? static_cast<double>(text.size()) / durationSec : 0.0;
	return charsPerSecond >= 1.2;
}

ClipCandidate ClipScoringPipeline::buildCandidateForRange(const TranscriptIndex &index,
	const ClipScoringPipelineOptions &options, const ClipDuration &range, const SemanticCoarseRegion &region) const
{
	ClipCandidate candidate;
	candidate.range = index.clampRange(range, options.generation.searchRange);
	candidate.firstSegmentIndex = index.firstSegmentIndexOverlapping(candidate.range);
	candidate.lastSegmentIndex = index.lastSegmentIndexOverlapping(candidate.range);
	candidate.text = index.textForRange(candidate.range).simplified();
	candidate.anchorText = candidate.text.left(220);
	candidate.source = QStringLiteral("semantic_coarse_region");
	candidate.hasReliableMainTarget = options.scoring.reliableMainTarget;
	candidate.scores.coarseSemantic = region.score;
	candidate.evidence.append(region.evidence);
	candidate.evidence.append(QStringLiteral("structural_candidate_from_coarse_region"));
	candidate.evidence.removeDuplicates();
	return scoreStructurally(index, candidate);
}

ClipCandidate ClipScoringPipeline::scoreStructurally(const TranscriptIndex &index, const ClipCandidate &candidate) const
{
	ClipCandidate scored = candidate;
	const double durationSec = scored.range.endSec - scored.range.startSec;
	scored.scores.duration = durationScore(durationSec);
	scored.scores.boundary = boundaryScore(index, scored);
	const double coarseScore = scored.scores.coarseSemantic;
	const double charsPerSecond = durationSec > 0.0 ? static_cast<double>(scored.text.trimmed().size()) / durationSec : 0.0;
	const double textDensityScore = boundedScore(charsPerSecond / 8.0);
	scored.scores.final = boundedScore((coarseScore * 0.34) + (scored.scores.boundary * 0.26) +
		(scored.scores.duration * 0.22) + (textDensityScore * 0.18));
	if (textDensityScore >= 0.55)
		scored.evidence.append(QStringLiteral("speech_density_ok"));
	if (scored.scores.boundary >= 0.7)
		scored.evidence.append(QStringLiteral("clean_boundary"));
	scored.evidence.append(QStringLiteral("structural_score_only"));
	scored.evidence.removeDuplicates();
	return scored;
}

double ClipScoringPipeline::durationScore(double durationSec) const
{
	if (durationSec <= 0.0)
		return 0.0;
	if (durationSec < 12.0)
		return 0.15;
	if (durationSec < 18.0)
		return 0.45;
	if (durationSec <= 75.0)
		return 1.0;
	if (durationSec <= 90.0)
		return 0.82;
	if (durationSec <= 150.0)
		return 0.62;
	return 0.30;
}

double ClipScoringPipeline::boundaryScore(const TranscriptIndex &index, const ClipCandidate &candidate) const
{
	double score = 0.0;
	const double silenceBefore = index.silenceBeforeRange(candidate.range);
	const double silenceAfter = index.silenceAfterRange(candidate.range);

	if (silenceBefore >= 0.35)
		score += 0.25;
	if (silenceBefore >= 0.75)
		score += 0.10;
	if (silenceAfter >= 0.45)
		score += 0.30;
	if (silenceAfter >= 0.85)
		score += 0.10;

	const QString trimmed = candidate.text.trimmed();
	if (trimmed.endsWith(QLatin1Char('.')) || trimmed.endsWith(QLatin1Char('!')) ||
	    trimmed.endsWith(QLatin1Char('?')))
		score += 0.25;

	return boundedScore(score);
}

QVector<ClipCandidate> ClipScoringPipeline::enforceSemanticAvailability(
	QVector<ClipCandidate> candidates, const ClipScoringPipelineOptions &options) const
{
	if (!options.budget.requireSemanticScoringWhenEmbeddingProviderEnabled || !options.embeddingProvider)
		return candidates;

	for (ClipCandidate &candidate : candidates) {
		if (candidate.semanticScoringAvailable)
			continue;
		candidate.rejectedByQualityGate = true;
		candidate.qualityGateChecked = true;
		if (candidate.evidence.contains(QStringLiteral("semantic_embedding_failed")))
			candidate.rejectionReason = QStringLiteral("semantic_embedding_failed");
		else
			candidate.rejectionReason = QStringLiteral("semantic_embedding_unavailable");
		candidate.evidence.append(QStringLiteral("quality_rejected:%1").arg(candidate.rejectionReason));
		candidate.evidence.removeDuplicates();
	}
	return candidates;
}

QString ClipScoringPipeline::buildSummary(const QVector<ClipCandidate> &candidates) const
{
	if (candidates.isEmpty())
		return QStringLiteral("no_candidates: scoring_pipeline_found_no_viable_ranges");

	QVector<ClipCandidate> summaryCandidates = candidates;
	std::sort(summaryCandidates.begin(), summaryCandidates.end(), [](const ClipCandidate &left, const ClipCandidate &right) {
		if (left.selectedRank > 0 || right.selectedRank > 0) {
			const int leftRank = left.selectedRank > 0 ? left.selectedRank : std::numeric_limits<int>::max();
			const int rightRank = right.selectedRank > 0 ? right.selectedRank : std::numeric_limits<int>::max();
			if (leftRank != rightRank)
				return leftRank < rightRank;
		}
		if (std::fabs(left.scores.final - right.scores.final) > 0.0001)
			return left.scores.final > right.scores.final;
		return left.range.startSec < right.range.startSec;
	});

	QStringList parts;
	const int limit = static_cast<int>(std::min(static_cast<long long>(5), static_cast<long long>(summaryCandidates.size())));
	for (int i = 0; i < limit; ++i) {
		const ClipCandidate &candidate = summaryCandidates.at(i);
		parts.append(QStringLiteral("#%1 selectedRank=%2 %3-%4s score=%5 value=%6 hook=%7 openingHook=%8 resolution=%9 endingResolution=%10 metaNoise=%11 openingMeta=%12 endingMeta=%13 endingShift=%14 continuity=%15 semantic=%16 viewer=%17 boundary=%18 coarse=%19 reranker=%20 raw=%21 bad=%22 margin=%23 source=%24 evidence=%25")
			.arg(i + 1)
			.arg(candidate.selectedRank > 0 ? candidate.selectedRank : i + 1)
			.arg(QString::number(candidate.range.startSec, 'f', 2))
			.arg(QString::number(candidate.range.endSec, 'f', 2))
			.arg(QString::number(candidate.scores.final, 'f', 2))
			.arg(QString::number(candidate.scores.semanticClipValue, 'f', 2))
			.arg(QString::number(candidate.scores.semanticHook, 'f', 2))
			.arg(QString::number(candidate.scores.semanticOpeningHook, 'f', 2))
			.arg(QString::number(candidate.scores.semanticResolution, 'f', 2))
			.arg(QString::number(candidate.scores.semanticEndingResolution, 'f', 2))
			.arg(QString::number(candidate.scores.semanticMetaNoise, 'f', 2))
			.arg(QString::number(candidate.scores.semanticOpeningMetaNoise, 'f', 2))
			.arg(QString::number(candidate.scores.semanticEndingMetaNoise, 'f', 2))
			.arg(QString::number(candidate.scores.semanticEndingTopicShift, 'f', 2))
			.arg(QString::number(candidate.scores.topicContinuity, 'f', 2))
			.arg(QString::number(candidate.scores.semanticTarget, 'f', 2))
			.arg(QString::number(candidate.scores.viewerResponse, 'f', 2))
			.arg(QString::number(candidate.scores.boundary, 'f', 2))
			.arg(QString::number(candidate.scores.coarseSemantic, 'f', 2))
			.arg(QString::number(candidate.scores.reranker, 'f', 2))
			.arg(QString::number(candidate.scores.rerankerRaw, 'f', 2))
			.arg(QString::number(candidate.scores.rerankerBadClip, 'f', 2))
			.arg(QString::number(candidate.scores.rerankerClipQualityMargin, 'f', 2))
			.arg(candidate.source)
			.arg(candidate.evidence.join(QLatin1Char('|'))));
	}
	return parts.join(QStringLiteral("; "));
}

QString ClipScoringPipeline::rejectionSummary(const QVector<ClipCandidate> &candidates) const
{
	if (candidates.isEmpty())
		return QStringLiteral("candidates=0");

	QMap<QString, int> rejectedReasons;
	int rejected = 0;
	int passed = 0;
	double bestFinal = 0.0;
	double bestSemantic = 0.0;
	double bestReranker = 0.0;
	double bestRaw = 0.0;
	double bestValue = 0.0;
	double bestHook = 0.0;
	double bestResolution = 0.0;
	double bestMetaNoise = 0.0;
	double bestOpeningHook = 0.0;
	double bestEndingResolution = 0.0;
	double bestBadClip = 0.0;
	double bestMargin = -1.0;
	QString firstFailure;
	QVector<ClipCandidate> topRejected;

	for (const ClipCandidate &candidate : candidates) {
		bestFinal = std::max(bestFinal, candidate.scores.final);
		bestSemantic = std::max(bestSemantic, candidate.scores.semanticTarget);
		bestReranker = std::max(bestReranker, candidate.scores.reranker);
		bestRaw = std::max(bestRaw, candidate.scores.rerankerRaw);
		bestValue = std::max(bestValue, candidate.scores.semanticClipValue);
		bestHook = std::max(bestHook, candidate.scores.semanticHook);
		bestResolution = std::max(bestResolution, candidate.scores.semanticResolution);
		bestMetaNoise = std::max(bestMetaNoise, candidate.scores.semanticMetaNoise);
		bestOpeningHook = std::max(bestOpeningHook, candidate.scores.semanticOpeningHook);
		bestEndingResolution = std::max(bestEndingResolution, candidate.scores.semanticEndingResolution);
		bestBadClip = std::max(bestBadClip, candidate.scores.rerankerBadClip);
		bestMargin = std::max(bestMargin, candidate.scores.rerankerClipQualityMargin);
		if (candidate.rejectedByQualityGate) {
			++rejected;
			const QString reason = candidate.rejectionReason.trimmed().isEmpty() ?
				QStringLiteral("unknown") : candidate.rejectionReason.trimmed().left(96);
			rejectedReasons[reason] = rejectedReasons.value(reason) + 1;
			topRejected.append(candidate);
			if (firstFailure.isEmpty() && !candidate.rerankerFailureReason.trimmed().isEmpty())
				firstFailure = candidate.rerankerFailureReason.trimmed().left(180);
		} else {
			++passed;
		}
	}

	QStringList reasonParts;
	for (auto it = rejectedReasons.constBegin(); it != rejectedReasons.constEnd(); ++it)
		reasonParts.append(QStringLiteral("%1=%2").arg(it.key()).arg(it.value()));

	std::sort(topRejected.begin(), topRejected.end(), [](const ClipCandidate &left, const ClipCandidate &right) {
		if (std::fabs(left.scores.final - right.scores.final) > 0.0001)
			return left.scores.final > right.scores.final;
		if (std::fabs(left.scores.rerankerRaw - right.scores.rerankerRaw) > 0.0001)
			return left.scores.rerankerRaw > right.scores.rerankerRaw;
		return left.range.startSec < right.range.startSec;
	});

	QStringList rejectedParts;
	const int rejectedLimit = static_cast<int>(std::min(static_cast<long long>(3), static_cast<long long>(topRejected.size())));
	for (int i = 0; i < rejectedLimit; ++i) {
		const ClipCandidate &candidate = topRejected.at(i);
		rejectedParts.append(QStringLiteral("%1-%2:%3 final=%4 value=%5 hook=%6 res=%7 meta=%8 raw=%9 bad=%10 margin=%11")
			.arg(QString::number(candidate.range.startSec, 'f', 1))
			.arg(QString::number(candidate.range.endSec, 'f', 1))
			.arg(candidate.rejectionReason.left(40))
			.arg(QString::number(candidate.scores.final, 'f', 2))
			.arg(QString::number(candidate.scores.semanticClipValue, 'f', 2))
			.arg(QString::number(candidate.scores.semanticHook, 'f', 2))
			.arg(QString::number(candidate.scores.semanticResolution, 'f', 2))
			.arg(QString::number(candidate.scores.semanticMetaNoise, 'f', 2))
			.arg(QString::number(candidate.scores.rerankerRaw, 'f', 2))
			.arg(QString::number(candidate.scores.rerankerBadClip, 'f', 2))
			.arg(QString::number(candidate.scores.rerankerClipQualityMargin, 'f', 2)));
	}

	QString summary = QStringLiteral("candidates=%1 passed=%2 rejected=%3 bestFinal=%4 bestSemantic=%5 bestReranker=%6 bestRaw=%7 bestValue=%8 bestHook=%9 bestOpeningHook=%10 bestResolution=%11 bestEndingResolution=%12 bestMetaNoise=%13 bestBad=%14 bestMargin=%15 reasons=[%16]")
		.arg(static_cast<long long>(candidates.size()))
		.arg(passed)
		.arg(rejected)
		.arg(QString::number(bestFinal, 'f', 2))
		.arg(QString::number(bestSemantic, 'f', 2))
		.arg(QString::number(bestReranker, 'f', 2))
		.arg(QString::number(bestRaw, 'f', 2))
		.arg(QString::number(bestValue, 'f', 2))
		.arg(QString::number(bestHook, 'f', 2))
		.arg(QString::number(bestOpeningHook, 'f', 2))
		.arg(QString::number(bestResolution, 'f', 2))
		.arg(QString::number(bestEndingResolution, 'f', 2))
		.arg(QString::number(bestMetaNoise, 'f', 2))
		.arg(QString::number(bestBadClip, 'f', 2))
		.arg(QString::number(bestMargin, 'f', 2))
		.arg(reasonParts.join(QLatin1Char(',')));
	if (!rejectedParts.isEmpty())
		summary += QStringLiteral(" topRejected=[%1]").arg(rejectedParts.join(QStringLiteral("; ")));
	if (!firstFailure.isEmpty())
		summary += QStringLiteral(" firstRerankerFailure=%1").arg(firstFailure);
	return summary;
}

QString ClipScoringPipeline::noCandidateSummary(const QString &reason, const QVector<ClipCandidate> &candidates) const
{
	return QStringLiteral("no_candidates: %1; %2").arg(reason, rejectionSummary(candidates));
}
