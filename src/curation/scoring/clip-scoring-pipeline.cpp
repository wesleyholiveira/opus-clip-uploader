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
#include "curation/scoring/feedback-trained-ranker.hpp"
#include "curation/scoring/viewer-arc-gate.hpp"
#include "curation/scoring/clip-scoring-pipeline-summary.hpp"
#include "curation/scoring/pipeline-progress.hpp"

#include <QStringList>

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

using namespace Curation::Scoring;

namespace {

static bool isViewerMessagePreset(const ClipScoringPipelineOptions &options);

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
		const auto has = [&candidate](const QString &evidence) { return candidate.evidence.contains(evidence); };
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
		const QVector<SemanticCoarseRegion> regions = retriever.retrieve(index, coarseContextFromOptions(options),
			options.coarseRetrieval, options.embeddingProvider);
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

	const int semanticTotal = static_cast<int>(candidates.size());
	if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_before_semantic_scoring")))
		return result;
	PipelineProgress::report(options, QStringLiteral("Embedding/scoring %1 marker candidates...").arg(semanticTotal), 40);
	candidates = semanticScoreCandidates(index, options, std::move(candidates));
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
	candidates = rerankerStage.apply(std::move(candidates), rerankerContextFromOptions(options), options.rerankerOptions,
		options.reranker);
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
		candidates = refinementStage.apply(index, std::move(candidates), boundaryRefinementOptionsFromOptions(options));
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
	candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [](const ClipCandidate &candidate) {
		return candidate.rejectedByQualityGate || candidate.rejectedAsNoise;
	}), candidates.end());

	if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_before_final_ranking")))
		return result;
	PipelineProgress::report(options, QStringLiteral("Selecting final marker suggestions..."), 92);

	if (candidates.isEmpty()) {
		result.summary = ClipScoringPipelineSummary::noCandidate(QStringLiteral("feedback_guided_scoring_found_no_consistent_complete_arcs"), candidates);
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
		candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [](const ClipCandidate &candidate) {
			return candidate.rejectedByQualityGate || candidate.rejectedAsNoise;
		}), candidates.end());
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
