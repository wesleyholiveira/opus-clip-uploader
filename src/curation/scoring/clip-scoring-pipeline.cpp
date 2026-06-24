#include "curation/scoring/clip-scoring-pipeline.hpp"

#include "curation/feedback/curation-feedback-store.hpp"
#include "curation/scoring/candidate-stage-limiter.hpp"
#include "curation/scoring/candidate-beam-expander.hpp"
#include "curation/scoring/candidate-builder.hpp"
#include "curation/scoring/candidate-source-builder.hpp"
#include "curation/scoring/boundary-refinement-stage.hpp"
#include "curation/scoring/feedback-aware-interval-selector.hpp"
#include "curation/scoring/feedback-consistency-gate.hpp"
#include "curation/scoring/feedback-guided-candidate-stage.hpp"
#include "curation/scoring/feedback-similarity-scorer.hpp"
#include "curation/scoring/feedback-trained-ranker.hpp"
#include "curation/scoring/viewer-arc-gate.hpp"
#include "curation/scoring/clip-scoring-pipeline-summary.hpp"
#include "curation/scoring/pipeline-progress.hpp"

#include <QStringList>
#include <QElapsedTimer>

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

#include <algorithm>
#include <cmath>
#include <utility>
#include <future>
#include <thread>
#include <vector>
#include <initializer_list>

using namespace Curation::Scoring;

namespace {

static bool isViewerMessagePreset(const ClipScoringPipelineOptions &options);
static CandidateSourceBuilderOptions candidateSourceOptionsFromOptions(const ClipScoringPipelineOptions &options);

static bool isSelectionEvidence(const QString &evidence)
{
	return evidence == QStringLiteral("mmr_diversity_selected") || evidence.startsWith(QStringLiteral("selected_rank:")) ||
	       evidence.startsWith(QStringLiteral("mmr:")) || evidence.startsWith(QStringLiteral("mmr_similarity:"));
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

static int usableCandidateCount(const QVector<ClipCandidate> &candidates)
{
	return static_cast<int>(std::count_if(candidates.constBegin(), candidates.constEnd(), [](const ClipCandidate &candidate) {
		return candidate.range.endSec > candidate.range.startSec && !candidate.text.trimmed().isEmpty() &&
		       !candidate.rejectedByQualityGate && !candidate.rejectedAsNoise;
	}));
}

static int rejectedCandidateCount(const QVector<ClipCandidate> &candidates)
{
	return static_cast<int>(std::count_if(candidates.constBegin(), candidates.constEnd(), [](const ClipCandidate &candidate) {
		return candidate.rejectedByQualityGate || candidate.rejectedAsNoise;
	}));
}

static int positiveFeedbackCandidateCount(const QVector<ClipCandidate> &candidates)
{
	return static_cast<int>(std::count_if(candidates.constBegin(), candidates.constEnd(), [](const ClipCandidate &candidate) {
		return candidate.source.contains(QStringLiteral("feedback_positive")) ||
		       candidate.evidence.contains(QStringLiteral("feedback_positive_exact_seed_preserved")) ||
		       candidate.evidence.contains(QStringLiteral("complete_viewer_arc_gate_passed_by_user_feedback")) ||
		       candidate.evidence.contains(QStringLiteral("complete_viewer_arc_gate_passed_by_feedback_trained_ranker")) ||
		       candidate.evidence.contains(QStringLiteral("feedback_trained_ranker_strong_accept")) ||
		       candidate.evidence.contains(QStringLiteral("feedback_trained_ranker_accept"));
	}));
}

static int learnedFeedbackAcceptedCandidateCount(const QVector<ClipCandidate> &candidates)
{
	return static_cast<int>(std::count_if(candidates.constBegin(), candidates.constEnd(), [](const ClipCandidate &candidate) {
		return candidate.evidence.contains(QStringLiteral("complete_viewer_arc_gate_passed_by_feedback_trained_ranker")) ||
		       candidate.evidence.contains(QStringLiteral("feedback_trained_ranker_strong_accept")) ||
		       candidate.evidence.contains(QStringLiteral("feedback_trained_ranker_accept"));
	}));
}


static bool isRejectedCandidate(const ClipCandidate &candidate)
{
	return candidate.rejectedByQualityGate || candidate.rejectedAsNoise;
}

static bool candidateHasEvidence(const ClipCandidate &candidate, const QString &needle)
{
	for (const QString &evidence : candidate.evidence) {
		if (evidence == needle || evidence.contains(needle))
			return true;
	}
	return false;
}

static bool isFeedbackSuppressedRejectedCandidate(const ClipCandidate &candidate)
{
	return candidateHasEvidence(candidate, QStringLiteral("feedback_negative_suppressed")) ||
		candidateHasEvidence(candidate, QStringLiteral("feedback_consistency_rejected"));
}

static QString diagnosticEvidenceSummary(const ClipCandidate &candidate)
{
	QStringList evidence;
	for (const QString &item : candidate.evidence) {
		const bool useful = item.startsWith(QStringLiteral("arc_gate_")) ||
			item.startsWith(QStringLiteral("exchange_arc_")) ||
			item.startsWith(QStringLiteral("feedback_")) ||
			item.startsWith(QStringLiteral("semantic_quality_gate_")) ||
			item.startsWith(QStringLiteral("quality_gate_")) ||
			item.startsWith(QStringLiteral("reranker_"));
		if (!useful)
			continue;
		evidence.append(item.left(180));
		if (evidence.size() >= 8)
			break;
	}
	return evidence.isEmpty() ? QStringLiteral("none") : evidence.join(QLatin1Char('|'));
}

static double diagnosticRangeCenter(const ClipDuration &range)
{
	return range.startSec + ((range.endSec - range.startSec) * 0.5);
}

static bool hasSimilarDiagnosticRange(const QVector<ClipCandidate> &diagnostics, const ClipDuration &range)
{
	for (const ClipCandidate &candidate : diagnostics) {
		if (FeedbackSimilarityScorer::rangeSimilarity(candidate.range, range) >= 0.64)
			return true;
		const double centerDistance = std::fabs(diagnosticRangeCenter(candidate.range) - diagnosticRangeCenter(range));
		const double boundaryDistance = std::fabs(candidate.range.startSec - range.startSec) +
			std::fabs(candidate.range.endSec - range.endSec);
		if (centerDistance <= 28.0 && boundaryDistance <= 52.0)
			return true;
	}
	return false;
}

static QVector<ClipCandidate> topRejectedCandidatesForDiagnostics(const QVector<ClipCandidate> &candidates, int limit = 12)
{
	QVector<ClipCandidate> rejected;
	for (const ClipCandidate &candidate : candidates) {
		if (candidate.range.endSec <= candidate.range.startSec || !isRejectedCandidate(candidate))
			continue;
		if (isFeedbackSuppressedRejectedCandidate(candidate))
			continue;
		rejected.append(candidate);
	}
	std::sort(rejected.begin(), rejected.end(), [](const ClipCandidate &left, const ClipCandidate &right) {
		if (std::fabs(left.scores.final - right.scores.final) > 0.0001)
			return left.scores.final > right.scores.final;
		if (std::fabs(left.scores.semanticClipValue - right.scores.semanticClipValue) > 0.0001)
			return left.scores.semanticClipValue > right.scores.semanticClipValue;
		if (std::fabs(left.scores.rerankerRaw - right.scores.rerankerRaw) > 0.0001)
			return left.scores.rerankerRaw > right.scores.rerankerRaw;
		return left.range.startSec < right.range.startSec;
	});

	QVector<ClipCandidate> diverse;
	diverse.reserve(limit > 0 ? std::min(limit, static_cast<int>(rejected.size())) : rejected.size());
	for (const ClipCandidate &candidate : rejected) {
		if (limit > 0 && diverse.size() >= limit)
			break;
		if (hasSimilarDiagnosticRange(diverse, candidate.range))
			continue;
		diverse.append(candidate);
	}
	return diverse;
}

static void logRejectedCandidateDiagnostics(const QString &videoPath, const QString &stage,
	const QVector<ClipCandidate> &diagnostics)
{
	if (diagnostics.isEmpty())
		return;
	blog(LOG_INFO,
	     "[clip-cropper] Top rejected candidate diagnostics preserved. video=%s stage=%s count=%d",
	     videoPath.toUtf8().constData(), stage.toUtf8().constData(), static_cast<int>(diagnostics.size()));
	for (int i = 0; i < diagnostics.size(); ++i) {
		const ClipCandidate &candidate = diagnostics.at(i);
		const QString reason = candidate.rejectionReason.trimmed().isEmpty()
			? QStringLiteral("unknown")
			: candidate.rejectionReason.trimmed().left(80);
		blog(LOG_INFO,
		     "[clip-cropper] Rejected candidate diagnostic #%d. video=%s stage=%s range=%.2f-%.2f duration=%.2f reason=%s final=%.3f value=%.3f hook=%.3f openingHook=%.3f resolution=%.3f endingResolution=%.3f arc=%.3f arcOpen=%.3f arcDev=%.3f arcEnd=%.3f topicContinuity=%.3f rerankerRaw=%.3f margin=%.3f source=%s evidence=%s",
		     i + 1, videoPath.toUtf8().constData(), stage.toUtf8().constData(), candidate.range.startSec,
		     candidate.range.endSec, candidate.range.endSec - candidate.range.startSec,
		     reason.toUtf8().constData(), candidate.scores.final, candidate.scores.semanticClipValue,
		     candidate.scores.semanticHook, candidate.scores.semanticOpeningHook,
		     candidate.scores.semanticResolution, candidate.scores.semanticEndingResolution,
		     candidate.scores.arcCompleteness, candidate.scores.arcOpening, candidate.scores.arcDevelopment,
		     candidate.scores.arcConclusion, candidate.scores.topicContinuity, candidate.scores.rerankerRaw,
		     candidate.scores.rerankerClipQualityMargin, candidate.source.left(160).toUtf8().constData(),
		     diagnosticEvidenceSummary(candidate).toUtf8().constData());
	}
}

static QString feedbackGateRejectionDiagnostics(const QVector<ClipCandidate> &candidates)
{
	int incompleteArc = 0;
	int hardBlocker = 0;
	int missingOrigin = 0;
	int learnedRejected = 0;
	int notPositiveBacked = 0;
	int trainedBelow = 0;
	int trainedNegative = 0;
	int trainedHard = 0;
	int feedbackSuppressed = 0;
	for (const ClipCandidate &candidate : candidates) {
		if (!candidate.rejectedByQualityGate && !candidate.rejectedAsNoise)
			continue;
		const auto has = [&candidate](const QString &evidence) { return candidateHasEvidence(candidate, evidence); };
		if (candidate.rejectionReason == QStringLiteral("incomplete_viewer_arc"))
			++incompleteArc;
		if (has(QStringLiteral("arc_gate_hard_context_blocker")))
			++hardBlocker;
		if (has(QStringLiteral("arc_gate_missing_viewer_origin")))
			++missingOrigin;
		if (has(QStringLiteral("arc_gate_learned_accept_rejected")))
			++learnedRejected;
		if (has(QStringLiteral("feedback_trained_ranker_not_positive_backed")))
			++notPositiveBacked;
		if (has(QStringLiteral("feedback_trained_ranker_rejected_below_threshold")))
			++trainedBelow;
		if (has(QStringLiteral("feedback_trained_ranker_rejected_negative_contamination")))
			++trainedNegative;
		if (has(QStringLiteral("feedback_trained_ranker_rejected_hard_context_blocker")))
			++trainedHard;
		if (has(QStringLiteral("feedback_negative_suppressed")) || has(QStringLiteral("feedback_consistency_rejected")))
			++feedbackSuppressed;
	}
	return QStringLiteral("incomplete=%1 hard=%2 missingOrigin=%3 learnedRejected=%4 notPositive=%5 below=%6 negative=%7 rankerHard=%8 feedbackSuppressed=%9")
		.arg(incompleteArc)
		.arg(hardBlocker)
		.arg(missingOrigin)
		.arg(learnedRejected)
		.arg(notPositiveBacked)
		.arg(trainedBelow)
		.arg(trainedNegative)
		.arg(trainedHard)
		.arg(feedbackSuppressed);
}

static int feedbackSuppressedCandidateCount(const QVector<ClipCandidate> &candidates)
{
	return static_cast<int>(std::count_if(candidates.constBegin(), candidates.constEnd(), [](const ClipCandidate &candidate) {
		return isFeedbackSuppressedRejectedCandidate(candidate);
	}));
}

static int semanticPrototypePositiveRangeCount(const Curation::Feedback::FeedbackRangeMemory &memory)
{
	return static_cast<int>(std::count_if(memory.positiveRanges.constBegin(), memory.positiveRanges.constEnd(),
		[](const Curation::Feedback::FeedbackRangeSignal &signal) {
			return signal.semanticPrototypeEligible;
		}));
}


struct NoveltyExplorationBuildStats {
	int rawCandidates = 0;
	int localAccepted = 0;
	int selectedForExpensiveScoring = 0;
	int workerCount = 1;
	long long generationMs = 0;
	long long localScoreMs = 0;
	long long diversityMs = 0;
};

static double localExplorationScore(const ClipCandidate &candidate, const FeedbackSimilarityFeatures &feedback)
{
	const double viewer = std::max({candidate.scores.viewerResponse, candidate.scores.semanticViewerMessage,
		candidate.scores.semanticDirectAnswer});
	const double structure = std::max({candidate.scores.arcCompleteness, candidate.scores.arcOpening,
		candidate.scores.arcDevelopment, candidate.scores.arcConclusion});
	const double hook = std::max(candidate.scores.hook, candidate.scores.semanticHook);
	const double resolution = std::max(candidate.scores.semanticResolution, candidate.scores.semanticEndingResolution);
	const double positive = std::max(0.0, feedback.positiveScore) * 0.16;
	const double noveltyPenalty = std::max(0.0, feedback.negativeScore) * 0.22;
	return (viewer * 0.30) + (structure * 0.22) + (hook * 0.18) + (resolution * 0.18) + positive - noveltyPenalty;
}

static bool hasSimilarExplorationRange(const QVector<ClipCandidate> &candidates, const ClipDuration &range)
{
	for (const ClipCandidate &candidate : candidates) {
		if (FeedbackSimilarityScorer::rangeSimilarity(candidate.range, range) >= 0.70)
			return true;
		const double boundaryDistance = std::fabs(candidate.range.startSec - range.startSec) +
			std::fabs(candidate.range.endSec - range.endSec);
		if (boundaryDistance <= 8.0)
			return true;
	}
	return false;
}

static QVector<ClipCandidate> buildNoveltyExplorationCandidates(const TranscriptIndex &index,
	const ClipScoringPipelineOptions &options,
	const Curation::Feedback::FeedbackRangeMemory &memory,
	NoveltyExplorationBuildStats *stats = nullptr)
{
	NoveltyExplorationBuildStats localStats;
	QElapsedTimer stageTimer;
	stageTimer.start();

	CandidateSourceBuilderOptions sourceOptions = candidateSourceOptionsFromOptions(options);
	const int semanticPositiveCount = semanticPrototypePositiveRangeCount(memory);
	const int explorationRawLimit = semanticPositiveCount > 0
		? std::min(220, std::max(120, options.budget.maxCandidatesBeforeEmbedding))
		: std::min(260, std::max(160, options.budget.maxCandidatesBeforeEmbedding * 2));
	sourceOptions.maxRawCandidates = explorationRawLimit;
	sourceOptions.generation.maxRawCandidates = explorationRawLimit;
	sourceOptions.generation.slidingWindowStepSec = std::max(30.0, options.generation.slidingWindowStepSec);

	CandidateSourceBuilder sourceBuilder;
	QVector<ClipCandidate> raw = sourceBuilder.fromLocalHeuristics(index, sourceOptions);
	localStats.generationMs = stageTimer.elapsed();
	localStats.rawCandidates = static_cast<int>(raw.size());

	if (raw.isEmpty()) {
		if (stats)
			*stats = localStats;
		return {};
	}

	stageTimer.restart();
	const unsigned hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
	const int maxWorkers = std::min(6, std::max(1, static_cast<int>(hardwareThreads) - 1));
	const int workerCount = std::clamp(static_cast<int>(raw.size() / 24) + 1, 1, maxWorkers);
	localStats.workerCount = workerCount;

	struct LocalScoredCandidate {
		ClipCandidate candidate;
		double score = 0.0;
	};
	const auto scoreRange = [&index, &memory, &sourceOptions](QVector<ClipCandidate> slice) {
		FeedbackSimilarityScorer feedbackScorer;
		QVector<LocalScoredCandidate> accepted;
		accepted.reserve(slice.size());
		for (ClipCandidate candidate : slice) {
			if (candidate.range.endSec <= candidate.range.startSec || candidate.text.trimmed().size() < 80)
				continue;
			if (!CandidateBuilder::isStructurallyViable(candidate, sourceOptions.generation, sourceOptions.qualityGate))
				continue;

			const FeedbackSimilarityFeatures feedback = feedbackScorer.score(candidate, index, memory);
			if (feedback.negativeRangeContamination)
				continue;
			if (feedback.negativeRangeSimilarity >= 0.58 || feedback.negativeOverlapSec >= 10.0)
				continue;
			if (feedback.negativeScore >= 0.68 && feedback.margin <= 0.02)
				continue;

			candidate.source = QStringLiteral("feedback_novelty_exploration:%1").arg(candidate.source.left(80));
			candidate.evidence.append(QStringLiteral("feedback_novelty_exploration"));
			candidate.evidence.append(QStringLiteral("feedback_novelty_outside_suppressed_ranges"));
			candidate.evidence.append(feedback.evidence);
			candidate.evidence.append(QStringLiteral("feedback_novelty_parallel_local_score"));
			candidate.evidence.removeDuplicates();
			const double localScore = localExplorationScore(candidate, feedback);
			candidate.scores.final = std::max(candidate.scores.final, 0.34 + std::max(0.0, localScore) * 0.10);
			accepted.append(LocalScoredCandidate{candidate, localScore});
		}
		return accepted;
	};

	std::vector<std::future<QVector<LocalScoredCandidate>>> futures;
	futures.reserve(workerCount);
	const int chunkSize = std::max(1, static_cast<int>(std::ceil(static_cast<double>(raw.size()) / workerCount)));
	for (int offset = 0; offset < raw.size(); offset += chunkSize) {
		QVector<ClipCandidate> slice;
		const int end = std::min(static_cast<int>(raw.size()), offset + chunkSize);
		slice.reserve(end - offset);
		for (int i = offset; i < end; ++i)
			slice.append(raw.at(i));
		futures.push_back(std::async(std::launch::async, scoreRange, std::move(slice)));
	}

	QVector<LocalScoredCandidate> scored;
	for (std::future<QVector<LocalScoredCandidate>> &future : futures) {
		const QVector<LocalScoredCandidate> partial = future.get();
		for (const LocalScoredCandidate &candidate : partial)
			scored.append(candidate);
	}
	localStats.localScoreMs = stageTimer.elapsed();
	localStats.localAccepted = static_cast<int>(scored.size());

	stageTimer.restart();
	std::sort(scored.begin(), scored.end(), [](const LocalScoredCandidate &left, const LocalScoredCandidate &right) {
		if (std::fabs(left.score - right.score) > 0.0001)
			return left.score > right.score;
		if (std::fabs(left.candidate.scores.viewerResponse - right.candidate.scores.viewerResponse) > 0.0001)
			return left.candidate.scores.viewerResponse > right.candidate.scores.viewerResponse;
		if (std::fabs(left.candidate.scores.hook - right.candidate.scores.hook) > 0.0001)
			return left.candidate.scores.hook > right.candidate.scores.hook;
		return left.candidate.range.startSec < right.candidate.range.startSec;
	});

	QVector<ClipCandidate> exploration;
	const int expensiveLimit = semanticPositiveCount > 0 ? 10 : 12;
	exploration.reserve(expensiveLimit);
	for (const LocalScoredCandidate &candidate : scored) {
		if (exploration.size() >= expensiveLimit)
			break;
		if (hasSimilarExplorationRange(exploration, candidate.candidate.range))
			continue;
		exploration.append(candidate.candidate);
	}
	localStats.diversityMs = stageTimer.elapsed();
	localStats.selectedForExpensiveScoring = static_cast<int>(exploration.size());
	if (stats)
		*stats = localStats;
	return exploration;
}

static QVector<ClipCandidate> explorationDiagnosticsFromCandidates(const QVector<ClipCandidate> &candidates, int limit = 12)
{
	QVector<ClipCandidate> diagnostics = topRejectedCandidatesForDiagnostics(candidates, limit);
	if (diagnostics.size() >= limit)
		return diagnostics;

	for (ClipCandidate candidate : candidates) {
		if (diagnostics.size() >= limit)
			break;
		if (candidate.range.endSec <= candidate.range.startSec || isFeedbackSuppressedRejectedCandidate(candidate))
			continue;
		if (isRejectedCandidate(candidate))
			continue;
		if (hasSimilarExplorationRange(diagnostics, candidate.range))
			continue;

		candidate.rejectedByQualityGate = true;
		candidate.rejectionReason = QStringLiteral("novelty_exploration_review_required");
		candidate.evidence.append(QStringLiteral("feedback_novelty_exploration_review_required"));
		candidate.evidence.removeDuplicates();
		diagnostics.append(candidate);
	}
	return diagnostics;
}

static ViewerArcGateOptions viewerArcGateOptionsFromOptions(const ClipScoringPipelineOptions &options)
{
	ViewerArcGateOptions gateOptions;
	gateOptions.enabled = isViewerMessagePreset(options);
	gateOptions.presetId = options.scoring.presetId;
	gateOptions.searchRange = options.generation.searchRange;
	gateOptions.minDurationSec = options.generation.minDurationSec;
	gateOptions.maxDurationSec = options.generation.maxDurationSec;
	return gateOptions;
}

static CandidateSourceBuilderOptions candidateSourceOptionsFromOptions(const ClipScoringPipelineOptions &options)
{
	CandidateSourceBuilderOptions sourceOptions;
	sourceOptions.generation = options.generation;
	sourceOptions.scoring = options.scoring;
	sourceOptions.qualityGate = options.qualityGate;
	sourceOptions.maxRawCandidates = options.generation.maxRawCandidates;
	sourceOptions.viewerMessagePreset = isViewerMessagePreset(options);
	return sourceOptions;
}

static CandidateBeamExpansionOptions beamExpansionOptionsFromOptions(const ClipScoringPipelineOptions &options)
{
	CandidateBeamExpansionOptions beamOptions;
	beamOptions.generation = options.generation;
	beamOptions.scoring = options.scoring;
	beamOptions.qualityGate = options.qualityGate;
	beamOptions.finalBudget = CandidateSourceBuilder::semanticCandidateBudget(candidateSourceOptionsFromOptions(options),
		options.budget.maxCandidatesBeforeEmbedding);
	beamOptions.viewerMessagePreset = isViewerMessagePreset(options);
	return beamOptions;
}

static BoundaryRefinementStageOptions boundaryRefinementOptionsFromOptions(const ClipScoringPipelineOptions &options)
{
	BoundaryRefinementStageOptions refinementOptions;
	refinementOptions.generation = options.generation;
	refinementOptions.scoring = options.scoring;
	refinementOptions.qualityGate = options.qualityGate;
	refinementOptions.embeddingProvider = options.embeddingProvider;
	refinementOptions.viewerMessagePreset = isViewerMessagePreset(options);
	return refinementOptions;
}

static FeedbackGuidedCandidateStageOptions feedbackGuidedStageOptionsFromOptions(const ClipScoringPipelineOptions &options)
{
	FeedbackGuidedCandidateStageOptions stageOptions;
	stageOptions.generation = options.generation;
	stageOptions.scoring = options.scoring;
	stageOptions.qualityGate = options.qualityGate;
	stageOptions.ranking = options.ranking;
	stageOptions.videoPath = options.videoPath;
	return stageOptions;
}

static bool isViewerMessagePreset(const ClipScoringPipelineOptions &options)
{
	return options.scoring.presetId == QStringLiteral("viewer_message_response");
}

} // namespace

ClipScoringResult ClipScoringPipeline::score(const RecordingTranscript &transcript,
	const ClipScoringPipelineOptions &options) const
{
	TranscriptIndex index(transcript);
	ClipScoringResult result;
	if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_before_scoring")))
		return result;
	PipelineProgress::report(options, QStringLiteral("Indexing transcript for clip suggestions..."), 1);
	if (index.isEmpty()) {
		result.summary = QStringLiteral("no_candidates: empty_transcript");
		return result;
	}

	const bool viewerPreset = isViewerMessagePreset(options);
	Curation::Feedback::FeedbackRangeMemory feedbackMemory;
	if (viewerPreset && !options.videoPath.trimmed().isEmpty()) {
		feedbackMemory = Curation::Feedback::CurationFeedbackStore::loadRangeMemoryForVideo(
			options.videoPath, options.scoring.presetId, options.contentIds);
	}

	QVector<ClipCandidate> candidates;
	const bool semanticBackendAvailable = options.embeddingProvider && options.embeddingProvider->isAvailable() &&
		options.coarseRetrieval.enabled;
	if (semanticBackendAvailable) {
		if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_before_coarse_retrieval")))
			return result;
		PipelineProgress::report(options, QStringLiteral("Embedding coarse transcript windows..."), 5);
		SemanticCoarseRetriever retriever;
		QElapsedTimer coarseRetrievalTimer;
		coarseRetrievalTimer.start();
		const QVector<SemanticCoarseRegion> regions = retriever.retrieve(index, coarseContextFromOptions(options),
			options.coarseRetrieval, options.embeddingProvider);
		blog(LOG_INFO, "[clip-cropper] Semantic coarse retrieval stage finished. video=%s regions=%d elapsedMs=%lld",
		     options.videoPath.toUtf8().constData(), static_cast<int>(regions.size()),
		     static_cast<long long>(coarseRetrievalTimer.elapsed()));
		if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_during_coarse_retrieval")))
			return result;
		PipelineProgress::report(options, QStringLiteral("Coarse semantic regions selected: %1").arg(static_cast<int>(regions.size())), 18);
		if (!regions.isEmpty()) {
			CandidateSourceBuilder sourceBuilder;
			candidates = sourceBuilder.fromSemanticCoarseRegions(index, regions, candidateSourceOptionsFromOptions(options));
		}
		PipelineProgress::report(options, QStringLiteral("Generated candidate marker windows: %1").arg(static_cast<int>(candidates.size())), 28);
	} else {
		CandidateSourceBuilder sourceBuilder;
		candidates = sourceBuilder.fromLocalHeuristics(index, candidateSourceOptionsFromOptions(options));
		PipelineProgress::report(options, QStringLiteral("Generated local heuristic candidate marker windows: %1").arg(static_cast<int>(candidates.size())), 28);
	}

	if (viewerPreset && feedbackMemory.loaded) {
		FeedbackGuidedCandidateStage feedbackStage;
		candidates = feedbackStage.appendCandidates(index, std::move(candidates), feedbackMemory,
			feedbackGuidedStageOptionsFromOptions(options));
	}

if (candidates.isEmpty()) {
		result.summary = QStringLiteral("no_candidates: candidate_generation_found_no_viable_ranges");
		return result;
	}

	ClipRanker ranker;
	PipelineProgress::report(options, QStringLiteral("Pre-ranking candidate marker windows..."), 32);
	candidates = ranker.rank(std::move(candidates), preSemanticRankingOptionsFromOptions(options));
	clearSelectionMetadata(candidates);
	if (semanticBackendAvailable) {
		CandidateBeamExpander beamExpander;
		candidates = beamExpander.expand(index, std::move(candidates), beamExpansionOptionsFromOptions(options));
		PipelineProgress::report(options, QStringLiteral("Expanded candidate beam variants: %1").arg(static_cast<int>(candidates.size())), 37);
	}
	if (candidates.isEmpty()) {
		result.summary = semanticBackendAvailable
			? QStringLiteral("no_candidates: semantic_coarse_structural_ranking_found_no_viable_ranges")
			: QStringLiteral("no_candidates: pre_semantic_ranking_found_no_viable_ranges");
		return result;
	}

	if (semanticBackendAvailable) {
		const int embeddingLimit = std::max(1, options.budget.maxCandidatesBeforeEmbedding);
		candidates = CandidateStageLimiter::limit(std::move(candidates), embeddingLimit,
			QStringLiteral("embedding_top_candidate_stage_limit:%1").arg(embeddingLimit));
	}

	const int semanticTotal = static_cast<int>(candidates.size());
	if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_before_semantic_scoring")))
		return result;
	PipelineProgress::report(options, QStringLiteral("Embedding/scoring %1 marker candidates...").arg(semanticTotal), 40);
	QElapsedTimer semanticStageTimer;
	semanticStageTimer.start();
	candidates = semanticScoreCandidates(index, options, std::move(candidates));
	blog(LOG_INFO, "[clip-cropper] Semantic embedding/scoring stage finished. video=%s candidates=%d elapsedMs=%lld",
	     options.videoPath.toUtf8().constData(), semanticTotal, static_cast<long long>(semanticStageTimer.elapsed()));
	if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_during_semantic_scoring")))
		return result;
	PipelineProgress::report(options, QStringLiteral("Semantic scoring finished for %1 marker candidates").arg(semanticTotal), 66);

	if (feedbackMemory.loaded) {
		FeedbackConsistencyGate feedbackGate;
		FeedbackConsistencyGateOptions feedbackGateOptions;
		feedbackGateOptions.viewerMessagePreset = viewerPreset;
		candidates = feedbackGate.apply(std::move(candidates), index, feedbackMemory, feedbackGateOptions);
		FeedbackTrainedRanker trainedRanker;
		candidates = trainedRanker.apply(std::move(candidates), index, feedbackMemory, options.scoring.presetId, options.videoPath);
	}

	candidates = CandidateBuilder::enforceSemanticAvailability(std::move(candidates),
		options.budget.requireSemanticScoringWhenEmbeddingProviderEnabled, options.embeddingProvider != nullptr);
	candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [](const ClipCandidate &candidate) {
		return candidate.rejectedByQualityGate;
	}), candidates.end());
	if (candidates.isEmpty()) {
		result.summary = QStringLiteral("no_candidates: semantic_embedding_required_but_unavailable_or_failed");
		return result;
	}

	if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_before_reranking")))
		return result;

	const bool rerankerWasAvailable = options.rerankerOptions.enabled && options.reranker && options.reranker->isAvailable();
	if (rerankerWasAvailable) {
		const int rerankerLimit = CandidateStageLimiter::rerankerLimit(options);
		candidates = CandidateStageLimiter::limit(std::move(candidates), rerankerLimit,
			QStringLiteral("reranker_top_candidate_stage_limit:%1").arg(rerankerLimit));
	}

	SemanticRerankerStage rerankerStage;
	PipelineProgress::report(options, QStringLiteral("Reranking top %1 marker candidates...").arg(static_cast<int>(candidates.size())), 72);
	QElapsedTimer rerankerStageTimer;
	rerankerStageTimer.start();
	const int rerankerCandidateCount = static_cast<int>(candidates.size());
	candidates = rerankerStage.apply(std::move(candidates), rerankerContextFromOptions(options), options.rerankerOptions,
		options.reranker);
	blog(LOG_INFO, "[clip-cropper] Reranker stage finished. video=%s candidates=%d elapsedMs=%lld",
	     options.videoPath.toUtf8().constData(), rerankerCandidateCount, static_cast<long long>(rerankerStageTimer.elapsed()));
	if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_during_reranking")))
		return result;
	PipelineProgress::report(options, QStringLiteral("Reranking completed for %1 marker candidates").arg(static_cast<int>(candidates.size())), 80);

	if (semanticBackendAvailable && options.embeddingProvider && options.embeddingProvider->isAvailable()) {
		const int boundaryLimit = CandidateStageLimiter::boundaryDpLimit(options);
		candidates = CandidateStageLimiter::limit(std::move(candidates), boundaryLimit,
			QStringLiteral("boundary_dp_top_candidate_stage_limit:%1").arg(boundaryLimit));
		if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_before_boundary_dp")))
			return result;
		PipelineProgress::report(options, QStringLiteral("Refining top %1 marker candidates with boundary DP...").arg(static_cast<int>(candidates.size())), 82);
		BoundaryRefinementStage refinementStage;
		QElapsedTimer boundaryStageTimer;
		boundaryStageTimer.start();
		const int boundaryCandidateCount = static_cast<int>(candidates.size());
		candidates = refinementStage.apply(index, std::move(candidates), boundaryRefinementOptionsFromOptions(options));
		blog(LOG_INFO, "[clip-cropper] Boundary DP stage finished. video=%s candidates=%d elapsedMs=%lld",
		     options.videoPath.toUtf8().constData(), boundaryCandidateCount, static_cast<long long>(boundaryStageTimer.elapsed()));
		PipelineProgress::report(options, QStringLiteral("Boundary DP refinement finished for %1 marker candidates").arg(static_cast<int>(candidates.size())), 85);
	}

	if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_before_quality_gate")))
		return result;

	ViewerArcGate viewerArcGate;
	const ViewerArcGateOptions arcGateOptions = viewerArcGateOptionsFromOptions(options);
	viewerArcGate.recoverMissingOpenings(index, candidates, arcGateOptions);

	CandidateQualityGate qualityGate;
	PipelineProgress::report(options, QStringLiteral("Applying semantic quality gate..."), 86);
	candidates = qualityGate.apply(std::move(candidates), qualityGateOptionsFromOptions(options, rerankerWasAvailable));

	if (feedbackMemory.loaded) {
		FeedbackConsistencyGate feedbackGate;
		FeedbackConsistencyGateOptions feedbackGateOptions;
		feedbackGateOptions.viewerMessagePreset = viewerPreset;
		candidates = feedbackGate.apply(std::move(candidates), index, feedbackMemory, feedbackGateOptions);
		FeedbackTrainedRanker trainedRanker;
		candidates = trainedRanker.apply(std::move(candidates), index, feedbackMemory, options.scoring.presetId, options.videoPath);
	}
	candidates = viewerArcGate.apply(std::move(candidates), arcGateOptions);
	if (viewerPreset) {
		blog(LOG_INFO,
		     "[clip-cropper] Feedback-guided candidate gate summary. video=%s total=%d usable=%d rejected=%d positive=%d learned=%d",
		     options.videoPath.toUtf8().constData(), static_cast<int>(candidates.size()), usableCandidateCount(candidates),
		     rejectedCandidateCount(candidates), positiveFeedbackCandidateCount(candidates),
		     learnedFeedbackAcceptedCandidateCount(candidates));
		blog(LOG_INFO, "[clip-cropper] Feedback-guided candidate rejection diagnostics. video=%s %s",
		     options.videoPath.toUtf8().constData(), feedbackGateRejectionDiagnostics(candidates).toUtf8().constData());
	}
	const QVector<ClipCandidate> candidatesBeforeRejectedErase = candidates;
	const QVector<ClipCandidate> rejectedAfterArcGate = topRejectedCandidatesForDiagnostics(candidates);
	candidates.erase(std::remove_if(candidates.begin(), candidates.end(), isRejectedCandidate), candidates.end());

	if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_before_final_ranking")))
		return result;
	PipelineProgress::report(options, QStringLiteral("Selecting final marker suggestions..."), 92);

	if (candidates.isEmpty()) {
		QVector<ClipCandidate> diagnostics = rejectedAfterArcGate;
		const int feedbackSuppressedAfterArcGate = feedbackSuppressedCandidateCount(candidatesBeforeRejectedErase);
		if (viewerPreset && feedbackMemory.loaded && diagnostics.isEmpty() && feedbackSuppressedAfterArcGate > 0) {
			PipelineProgress::report(options, QStringLiteral("Exploring new diagnostic candidates outside rejected feedback ranges..."), 93);
			QElapsedTimer noveltyTimer;
			noveltyTimer.start();
			NoveltyExplorationBuildStats noveltyStats;
			QVector<ClipCandidate> exploration = buildNoveltyExplorationCandidates(index, options, feedbackMemory, &noveltyStats);
			blog(LOG_INFO,
			     "[clip-cropper] Starting novelty exploration fallback. video=%s suppressed=%d raw=%d localAccepted=%d filtered=%d workers=%d generationMs=%lld localScoreMs=%lld diversityMs=%lld negative=%d positive=%d semanticPositive=%d",
			     options.videoPath.toUtf8().constData(), feedbackSuppressedAfterArcGate, noveltyStats.rawCandidates,
			     noveltyStats.localAccepted, static_cast<int>(exploration.size()), noveltyStats.workerCount,
			     noveltyStats.generationMs, noveltyStats.localScoreMs, noveltyStats.diversityMs,
			     static_cast<int>(feedbackMemory.negativeRanges.size()),
			     static_cast<int>(feedbackMemory.positiveRanges.size()), semanticPrototypePositiveRangeCount(feedbackMemory));

			long long semanticMs = 0;
			long long rerankMs = 0;
			long long boundaryMs = 0;
			long long gateMs = 0;
			QVector<ClipCandidate> diagnosticFallbackPool = exploration;
			QVector<ClipCandidate> diagnosticPreGatePool;
			if (!exploration.isEmpty()) {
				QElapsedTimer stageTimer;
				if (semanticBackendAvailable && options.embeddingProvider && options.embeddingProvider->isAvailable()) {
					stageTimer.start();
					exploration = semanticScoreCandidates(index, options, std::move(exploration));
					semanticMs = stageTimer.elapsed();
				}
				exploration = CandidateBuilder::enforceSemanticAvailability(std::move(exploration),
					options.budget.requireSemanticScoringWhenEmbeddingProviderEnabled, options.embeddingProvider != nullptr);
				exploration.erase(std::remove_if(exploration.begin(), exploration.end(), [](const ClipCandidate &candidate) {
					return candidate.rejectedByQualityGate && candidate.rejectionReason == QStringLiteral("semantic_embedding_unavailable");
				}), exploration.end());
				SemanticRerankerStage explorationReranker;
				stageTimer.restart();
				exploration = explorationReranker.apply(std::move(exploration), rerankerContextFromOptions(options), options.rerankerOptions,
					options.reranker);
				rerankMs = stageTimer.elapsed();
				if (semanticBackendAvailable && options.embeddingProvider && options.embeddingProvider->isAvailable()) {
					BoundaryRefinementStage explorationRefinement;
					stageTimer.restart();
					exploration = explorationRefinement.apply(index, std::move(exploration), boundaryRefinementOptionsFromOptions(options));
					boundaryMs = stageTimer.elapsed();
				}
				diagnosticPreGatePool = exploration;
				stageTimer.restart();
				CandidateQualityGate explorationQualityGate;
				exploration = explorationQualityGate.apply(std::move(exploration), qualityGateOptionsFromOptions(options, rerankerWasAvailable));
				FeedbackConsistencyGate explorationFeedbackGate;
				FeedbackConsistencyGateOptions explorationFeedbackOptions;
				explorationFeedbackOptions.viewerMessagePreset = true;
				exploration = explorationFeedbackGate.apply(std::move(exploration), index, feedbackMemory, explorationFeedbackOptions);
				FeedbackTrainedRanker explorationTrainedRanker;
				exploration = explorationTrainedRanker.apply(std::move(exploration), index, feedbackMemory, options.scoring.presetId, options.videoPath);
				ViewerArcGate explorationArcGate;
				explorationArcGate.recoverMissingOpenings(index, exploration, arcGateOptions);
				exploration = explorationArcGate.apply(std::move(exploration), arcGateOptions);
				gateMs = stageTimer.elapsed();
				diagnostics = explorationDiagnosticsFromCandidates(exploration);
				if (diagnostics.isEmpty()) {
					diagnostics = explorationDiagnosticsFromCandidates(diagnosticPreGatePool);
					if (!diagnostics.isEmpty())
						blog(LOG_INFO, "[clip-cropper] Novelty exploration diagnostics recovered from pre-gate pool. video=%s count=%d",
						     options.videoPath.toUtf8().constData(), static_cast<int>(diagnostics.size()));
				}
				if (diagnostics.isEmpty()) {
					diagnostics = explorationDiagnosticsFromCandidates(diagnosticFallbackPool);
					if (!diagnostics.isEmpty())
						blog(LOG_INFO, "[clip-cropper] Novelty exploration diagnostics recovered from local candidate pool. video=%s count=%d",
						     options.videoPath.toUtf8().constData(), static_cast<int>(diagnostics.size()));
				}
			}

			blog(LOG_INFO,
			     "[clip-cropper] Novelty exploration fallback finished. video=%s diagnostics=%d semanticMs=%lld rerankMs=%lld boundaryMs=%lld gateMs=%lld elapsedMs=%lld",
			     options.videoPath.toUtf8().constData(), static_cast<int>(diagnostics.size()),
			     semanticMs, rerankMs, boundaryMs, gateMs, static_cast<long long>(noveltyTimer.elapsed()));
		}

		result.rejectedCandidateDiagnostics = diagnostics;
		const QString reason = diagnostics.isEmpty()
			? QStringLiteral("feedback_guided_scoring_found_no_consistent_complete_arcs")
			: QStringLiteral("feedback_novelty_exploration_candidates_require_review");
		result.summary = ClipScoringPipelineSummary::noCandidate(reason, diagnostics);
		logRejectedCandidateDiagnostics(options.videoPath, diagnostics.isEmpty() ? QStringLiteral("viewer_arc_gate") : QStringLiteral("novelty_exploration"), diagnostics);
		PipelineProgress::report(options, QStringLiteral("Marker suggestion analysis finished."), 100);
		return result;
	}

	ClipRankerOptions finalRanking = options.ranking;
	if (viewerPreset && feedbackMemory.loaded) {
		FeedbackConsistencyGate feedbackGate;
		FeedbackConsistencyGateOptions feedbackGateOptions;
		feedbackGateOptions.viewerMessagePreset = true;
		candidates = feedbackGate.apply(std::move(candidates), index, feedbackMemory, feedbackGateOptions);
		FeedbackTrainedRanker trainedRanker;
		candidates = trainedRanker.apply(std::move(candidates), index, feedbackMemory, options.scoring.presetId, options.videoPath);
		const QVector<ClipCandidate> rejectedAfterFinalFeedbackGate = topRejectedCandidatesForDiagnostics(candidates);
		candidates.erase(std::remove_if(candidates.begin(), candidates.end(), isRejectedCandidate), candidates.end());
		if (candidates.isEmpty()) {
			result.rejectedCandidateDiagnostics = rejectedAfterFinalFeedbackGate;
			result.summary = ClipScoringPipelineSummary::noCandidate(QStringLiteral("feedback_final_selection_found_no_consistent_ranges"), rejectedAfterFinalFeedbackGate);
			logRejectedCandidateDiagnostics(options.videoPath, QStringLiteral("final_feedback_gate"), rejectedAfterFinalFeedbackGate);
			PipelineProgress::report(options, QStringLiteral("Marker suggestion analysis finished."), 100);
			return result;
		}
		FeedbackAwareIntervalSelector feedbackSelector;
		QVector<ClipCandidate> selectedByFeedbackDp = feedbackSelector.select(candidates, index, feedbackMemory, finalRanking);
		if (!selectedByFeedbackDp.isEmpty()) {
			blog(LOG_INFO,
			     "[clip-cropper] Feedback-aware interval DP selected marker pool. video=%s before=%d after=%d",
			     options.videoPath.toUtf8().constData(), static_cast<int>(candidates.size()),
			     static_cast<int>(selectedByFeedbackDp.size()));
			candidates = std::move(selectedByFeedbackDp);
		}
	}

	result.candidates = ranker.rank(std::move(candidates), finalRanking);
	if (result.candidates.isEmpty())
		result.summary = ClipScoringPipelineSummary::noCandidate(QStringLiteral("final_ranking_found_no_viable_ranges"), candidates);
	else {
		for (int i = 0; i < static_cast<int>(result.candidates.size()); ++i)
			PipelineProgress::report(options,
				PipelineProgress::candidateMessage(QStringLiteral("Selected marker"), result.candidates.at(i), i + 1,
					static_cast<int>(result.candidates.size())),
				94 + ((i * 5) / std::max(1, static_cast<int>(result.candidates.size()))));
		result.summary = ClipScoringPipelineSummary::build(result.candidates);
	}
	PipelineProgress::report(options, QStringLiteral("Marker suggestion analysis finished."), 100);
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
	const int requested = std::max(1, options.budget.maxCandidatesBeforeEmbedding);
	const int rawBudget = std::max(1, options.generation.maxRawCandidates);
	if (isViewerMessagePreset(options)) {
		// Viewer-message boundaries need several nearby variants from the same semantic region.
		// Do not apply the final marker spacing here; final MMR will de-duplicate later.
		ranking.maxCandidates = std::min(rawBudget, std::max(requested * 2, requested + 24));
		ranking.minSpacingSec = 0.0;
	} else {
		ranking.maxCandidates = std::max(1, std::min(24, requested));
		ranking.minSpacingSec = options.budget.preSemanticMinSpacingSec;
	}
	ranking.minFinalScore = options.budget.preSemanticMinFinalScore;
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
	if (isViewerMessagePreset(options)) {
		// In the clean feedback-guided architecture, rejected candidates are not recovered
		// by a quality-gate failsafe. Feedback-guided generation and the final consistency
		// gate are the only recovery paths.
		qualityOptions.maxFailsafeRecoveredCandidates = 0;
	}
	return qualityOptions;
}

QVector<ClipCandidate> ClipScoringPipeline::semanticScoreCandidates(const TranscriptIndex &index,
	const ClipScoringPipelineOptions &options, QVector<ClipCandidate> candidates) const
{
	if (candidates.isEmpty())
		return candidates;

	SemanticClipScorer semanticScorer;
	QVector<ClipCandidate> scored = semanticScorer.scoreBatch(index, candidates, semanticContextFromOptions(options),
		options.semantic, options.embeddingProvider);
	for (ClipCandidate &candidate : scored)
		candidate.evidence.append(QStringLiteral("semantic_embedding_batch_stage"));
	return scored;
}
