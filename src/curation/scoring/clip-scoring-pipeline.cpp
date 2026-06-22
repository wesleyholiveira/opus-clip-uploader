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

static double rangeCenterSec(const ClipDuration &range)
{
	return range.startSec + ((range.endSec - range.startSec) * 0.5);
}

static double overlapSec(const ClipDuration &left, const ClipDuration &right)
{
	return std::max(0.0, std::min(left.endSec, right.endSec) - std::max(left.startSec, right.startSec));
}

static bool substantiallyOverlapsFocus(const ClipDuration &candidate, const ClipDuration &focus)
{
	const double focusDuration = std::max(0.0, focus.endSec - focus.startSec);
	if (focusDuration <= 0.0)
		return true;
	const double overlap = overlapSec(candidate, focus);
	return overlap >= std::min(8.0, focusDuration * 0.35) ||
		(candidate.startSec <= rangeCenterSec(focus) && candidate.endSec >= rangeCenterSec(focus));
}

static void appendUniqueStart(QVector<double> &starts, double startSec)
{
	if (!std::isfinite(startSec))
		return;
	for (double existing : starts) {
		if (std::fabs(existing - startSec) < 0.75)
			return;
	}
	starts.append(startSec);
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
	context.transcriptionLanguage = options.scoring.transcriptionLanguage;
	context.sourceLanguage = options.scoring.sourceLanguage;
	context.reliableMainTarget = options.scoring.reliableMainTarget;
	return context;
}

SemanticScoringContext ClipScoringPipeline::semanticContextFromOptions(const ClipScoringPipelineOptions &options) const
{
	SemanticScoringContext context;
	context.presetId = options.scoring.presetId;
	context.mainTarget = options.scoring.mainTarget;
	context.transcriptionLanguage = options.scoring.transcriptionLanguage;
	context.sourceLanguage = options.scoring.sourceLanguage;
	context.reliableMainTarget = options.scoring.reliableMainTarget;
	return context;
}

SemanticRerankerContext ClipScoringPipeline::rerankerContextFromOptions(const ClipScoringPipelineOptions &options) const
{
	SemanticRerankerContext context;
	context.presetId = options.scoring.presetId;
	context.mainTarget = options.scoring.mainTarget;
	context.transcriptionLanguage = options.scoring.transcriptionLanguage;
	context.sourceLanguage = options.scoring.sourceLanguage;
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
			std::clamp(45.0, options.generation.minDurationSec, options.generation.maxDurationSec),
			std::clamp(60.0, options.generation.minDurationSec, options.generation.maxDurationSec),
			std::clamp(90.0, options.generation.minDurationSec, options.generation.maxDurationSec),
			std::clamp(120.0, options.generation.minDurationSec, options.generation.maxDurationSec),
			std::clamp(150.0, options.generation.minDurationSec, options.generation.maxDurationSec),
			options.generation.maxDurationSec,
		};
	}
	targetDurations.erase(std::unique(targetDurations.begin(), targetDurations.end(), [](double left, double right) {
		return std::fabs(left - right) < 0.25;
	}), targetDurations.end());

	for (const SemanticCoarseRegion &region : regions) {
		// The coarse retriever owns only semantic retrieval. Its padded range is a broad search area, not a boundary.
		const ClipDuration searchRange = index.clampRange(region.range, options.generation.searchRange);
		ClipDuration focusRange = region.focusRange;
		if (focusRange.endSec <= focusRange.startSec)
			focusRange = searchRange;
		focusRange = index.clampRange(focusRange, searchRange);
		if ((searchRange.endSec - searchRange.startSec) < options.generation.minDurationSec)
			continue;

		const QVector<int> focusSegmentIndices = index.segmentIndicesForRange(focusRange);
		const QVector<int> searchSegmentIndices = index.segmentIndicesForRange(searchRange);
		if (focusSegmentIndices.isEmpty() || searchSegmentIndices.isEmpty())
			continue;

		QVector<int> focusAnchorIndices;
		const int desiredAnchors = std::max(5, maxCandidatesPerRegion * 2);
		const int anchorStep = std::max(1, static_cast<int>(std::ceil(static_cast<double>(focusSegmentIndices.size()) /
			static_cast<double>(desiredAnchors))));
		for (int position = 0; position < static_cast<int>(focusSegmentIndices.size()); position += anchorStep)
			focusAnchorIndices.append(focusSegmentIndices.at(position));
		if (!focusAnchorIndices.contains(focusSegmentIndices.first()))
			focusAnchorIndices.prepend(focusSegmentIndices.first());
		if (!focusAnchorIndices.contains(focusSegmentIndices.last()))
			focusAnchorIndices.append(focusSegmentIndices.last());

		QVector<ClipCandidate> regionCandidates;
		regionCandidates.reserve(maxCandidatesPerRegion * 5);
		QStringList regionSeen;

		for (const int anchorIndex : focusAnchorIndices) {
			const TranscriptSegment *anchorSegment = index.segmentAt(anchorIndex);
			if (!anchorSegment || anchorSegment->text.trimmed().isEmpty())
				continue;

			for (const double requestedDurationSec : targetDurations) {
				QVector<double> startTimes;
				appendUniqueStart(startTimes, anchorSegment->startSec - 2.0);
				appendUniqueStart(startTimes, anchorSegment->startSec - 8.0);
				appendUniqueStart(startTimes, anchorSegment->startSec - (requestedDurationSec * 0.25));
				appendUniqueStart(startTimes, anchorSegment->startSec - (requestedDurationSec * 0.50));
				appendUniqueStart(startTimes, rangeCenterSec(focusRange) - (requestedDurationSec * 0.50));
				appendUniqueStart(startTimes, focusRange.startSec - 6.0);
				appendUniqueStart(startTimes, focusRange.endSec - requestedDurationSec + 6.0);

				for (double requestedStartSec : startTimes) {
					ClipDuration range{requestedStartSec, requestedStartSec + requestedDurationSec};
					if (range.startSec < searchRange.startSec) {
						const double shift = searchRange.startSec - range.startSec;
						range.startSec += shift;
						range.endSec += shift;
					}
					if (range.endSec > searchRange.endSec) {
						const double shift = range.endSec - searchRange.endSec;
						range.startSec -= shift;
						range.endSec -= shift;
					}
					range = index.clampRange(range, searchRange);
					if ((range.endSec - range.startSec) < options.generation.minDurationSec)
						continue;
					if (!substantiallyOverlapsFocus(range, focusRange))
						continue;

					ClipCandidate candidate = buildCandidateForRange(index, options, range, region);
					candidate.evidence.append(QStringLiteral("coarse_focus_seed_only"));
					candidate.evidence.append(QStringLiteral("candidate_generated_around_coarse_focus"));
					candidate.evidence.removeDuplicates();
					const QString key = rangeKey(candidate.range);
					if (regionSeen.contains(key) || seen.contains(key) || !isStructurallyViable(candidate, options))
						continue;

					regionSeen.append(key);
					regionCandidates.append(candidate);
					if (static_cast<int>(regionCandidates.size()) >= maxCandidatesPerRegion * 5)
						break;
				}
				if (static_cast<int>(regionCandidates.size()) >= maxCandidatesPerRegion * 5)
					break;
			}
			if (static_cast<int>(regionCandidates.size()) >= maxCandidatesPerRegion * 5)
				break;
		}

		std::sort(regionCandidates.begin(), regionCandidates.end(), [](const ClipCandidate &left,
								      const ClipCandidate &right) {
			if (std::fabs(left.scores.final - right.scores.final) > 0.0001)
				return left.scores.final > right.scores.final;
			if (std::fabs(left.scores.coarseSemantic - right.scores.coarseSemantic) > 0.0001)
				return left.scores.coarseSemantic > right.scores.coarseSemantic;
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
	ExchangeArcBoundaryRefinementOptions refinerOptions;
	refinerOptions.generation = options.generation;
	refinerOptions.scoring = options.scoring;
	refinerOptions.qualityGate = options.qualityGate;
	refinerOptions.embeddingProvider = options.embeddingProvider;

	ExchangeArcBoundaryRefiner refiner;
	ClipCandidate refined = refiner.refine(index, candidate, refinerOptions);
	if (!isStructurallyViable(refined, options))
		return candidate;

	const bool materiallyChanged = std::fabs(refined.range.startSec - candidate.range.startSec) > 1.0 ||
		std::fabs(refined.range.endSec - candidate.range.endSec) > 1.0;
	if (materiallyChanged)
		return scoreStructurally(index, refined);
	return refined;
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
	candidate.timedText = index.timedTextForRange(candidate.range);
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
	if (scored.timedText.trimmed().isEmpty())
		scored.timedText = index.timedTextForRange(scored.range);
	const double durationSec = scored.range.endSec - scored.range.startSec;
	scored.scores.duration = durationScore(durationSec);
	scored.scores.pauseBeforeSec = index.silenceBeforeRange(scored.range);
	scored.scores.pauseAfterSec = index.silenceAfterRange(scored.range);
	scored.scores.maxInternalPauseSec = index.maxInternalSilenceInRange(scored.range);
	scored.scores.pauseBoundary = boundedScore((std::min(scored.scores.pauseBeforeSec, 4.0) * 0.12) +
		(std::min(scored.scores.pauseAfterSec, 4.0) * 0.18));
	scored.scores.boundary = boundedScore(boundaryScore(index, scored) + (scored.scores.pauseBoundary * 0.18));
	const double coarseScore = scored.scores.coarseSemantic;
	const double charsPerSecond = durationSec > 0.0 ? static_cast<double>(scored.text.trimmed().size()) / durationSec : 0.0;
	const double textDensityScore = boundedScore(charsPerSecond / 8.0);
	scored.scores.final = boundedScore((coarseScore * 0.34) + (scored.scores.boundary * 0.26) +
		(scored.scores.duration * 0.22) + (textDensityScore * 0.18));
	if (textDensityScore >= 0.55)
		scored.evidence.append(QStringLiteral("speech_density_ok"));
	if (scored.scores.boundary >= 0.7)
		scored.evidence.append(QStringLiteral("clean_boundary"));
	if (scored.scores.pauseAfterSec >= 3.0)
		scored.evidence.append(QStringLiteral("pause_after:%1").arg(QString::number(scored.scores.pauseAfterSec, 'f', 1)));
	if (scored.scores.maxInternalPauseSec >= 2.0)
		scored.evidence.append(QStringLiteral("internal_pause:%1").arg(QString::number(scored.scores.maxInternalPauseSec, 'f', 1)));
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
	if (durationSec <= 120.0)
		return 1.0;
	if (durationSec <= 180.0)
		return 0.90;
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
		parts.append(QStringLiteral("#%1 selectedRank=%2 %3-%4s score=%5 value=%6 hook=%7 openingHook=%8 resolution=%9 endingResolution=%10 metaNoise=%11 openingMeta=%12 endingMeta=%13 endingShift=%14 continuity=%15 arc=%16 arcOpen=%17 arcDev=%18 arcEnd=%19 arcClean=%20 arcTail=%21 semantic=%22 viewer=%23 boundary=%24 pauseBefore=%25 pauseAfter=%26 internalPause=%27 coarse=%28 reranker=%29 raw=%30 defect=%31 openDef=%32 endDef=%33 structDef=%34 margin=%35 source=%36 evidence=%37")
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
			.arg(QString::number(candidate.scores.arcCompleteness, 'f', 2))
			.arg(QString::number(candidate.scores.arcOpening, 'f', 2))
			.arg(QString::number(candidate.scores.arcDevelopment, 'f', 2))
			.arg(QString::number(candidate.scores.arcConclusion, 'f', 2))
			.arg(QString::number(candidate.scores.arcBoundaryCleanliness, 'f', 2))
			.arg(QString::number(candidate.scores.arcTailRisk, 'f', 2))
			.arg(QString::number(candidate.scores.semanticTarget, 'f', 2))
			.arg(QString::number(candidate.scores.viewerResponse, 'f', 2))
			.arg(QString::number(candidate.scores.boundary, 'f', 2))
			.arg(QString::number(candidate.scores.pauseBeforeSec, 'f', 1))
			.arg(QString::number(candidate.scores.pauseAfterSec, 'f', 1))
			.arg(QString::number(candidate.scores.maxInternalPauseSec, 'f', 1))
			.arg(QString::number(candidate.scores.coarseSemantic, 'f', 2))
			.arg(QString::number(candidate.scores.reranker, 'f', 2))
			.arg(QString::number(candidate.scores.rerankerRaw, 'f', 2))
			.arg(QString::number(candidate.scores.rerankerBadClip, 'f', 2))
			.arg(QString::number(candidate.scores.rerankerOpeningDefect, 'f', 2))
			.arg(QString::number(candidate.scores.rerankerEndingDefect, 'f', 2))
			.arg(QString::number(candidate.scores.rerankerStructureDefect, 'f', 2))
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
		rejectedParts.append(QStringLiteral("%1-%2:%3 final=%4 value=%5 hook=%6 res=%7 meta=%8 raw=%9 defect=%10 openDef=%11 endDef=%12 structDef=%13 margin=%14")
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
			.arg(QString::number(candidate.scores.rerankerOpeningDefect, 'f', 2))
			.arg(QString::number(candidate.scores.rerankerEndingDefect, 'f', 2))
			.arg(QString::number(candidate.scores.rerankerStructureDefect, 'f', 2))
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
