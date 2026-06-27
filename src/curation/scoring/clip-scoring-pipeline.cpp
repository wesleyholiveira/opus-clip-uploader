#include "curation/scoring/clip-scoring-pipeline.hpp"
#include "curation/scoring/clip-scoring-pipeline-detail.hpp"

#include "curation/feedback/curation-feedback-store.hpp"
#include "curation/scoring/candidate-stage-limiter.hpp"
#include "curation/scoring/candidate-beam-expander.hpp"
#include "curation/scoring/candidate-builder.hpp"
#include "curation/scoring/cheap-clip-scorer.hpp"
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

using namespace Curation::Scoring;

using namespace Curation::Scoring::ClipScoringPipelineDetail;

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
		if (PipelineProgress::stopIfCanceled(options, result,
						     QStringLiteral("canceled_before_coarse_retrieval")))
			return result;
		PipelineProgress::report(options, QStringLiteral("Embedding coarse transcript windows..."), 5);
		SemanticCoarseRetriever retriever;
		QElapsedTimer coarseRetrievalTimer;
		coarseRetrievalTimer.start();
		const QVector<SemanticCoarseRegion> regions = retriever.retrieve(
			index, coarseContextFromOptions(options), options.coarseRetrieval, options.embeddingProvider);
		blog(LOG_INFO,
		     "[clip-cropper] Semantic coarse retrieval stage finished. video=%s regions=%d elapsedMs=%lld",
		     options.videoPath.toUtf8().constData(), static_cast<int>(regions.size()),
		     static_cast<long long>(coarseRetrievalTimer.elapsed()));
		if (PipelineProgress::stopIfCanceled(options, result,
						     QStringLiteral("canceled_during_coarse_retrieval")))
			return result;
		PipelineProgress::report(
			options,
			QStringLiteral("Coarse semantic regions selected: %1").arg(static_cast<int>(regions.size())),
			18);
		if (!regions.isEmpty()) {
			CandidateSourceBuilder sourceBuilder;
			candidates = sourceBuilder.fromSemanticCoarseRegions(
				index, regions, candidateSourceOptionsFromOptions(options));
		}
		PipelineProgress::report(options,
					 QStringLiteral("Generated candidate marker windows: %1")
						 .arg(static_cast<int>(candidates.size())),
					 28);
	} else {
		CandidateSourceBuilder sourceBuilder;
		candidates = sourceBuilder.fromLocalHeuristics(index, candidateSourceOptionsFromOptions(options));
		PipelineProgress::report(options,
					 QStringLiteral("Generated local heuristic candidate marker windows: %1")
						 .arg(static_cast<int>(candidates.size())),
					 28);
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
		candidates =
			beamExpander.expand(index, std::move(candidates), beamExpansionOptionsFromOptions(options));
		PipelineProgress::report(
			options,
			QStringLiteral("Expanded candidate beam variants: %1").arg(static_cast<int>(candidates.size())),
			37);
	}
	if (candidates.isEmpty()) {
		result.summary =
			semanticBackendAvailable
				? QStringLiteral(
					  "no_candidates: semantic_coarse_structural_ranking_found_no_viable_ranges")
				: QStringLiteral("no_candidates: pre_semantic_ranking_found_no_viable_ranges");
		return result;
	}

	if (semanticBackendAvailable) {
		const int embeddingLimit = std::max(1, options.budget.maxCandidatesBeforeEmbedding);
		candidates = CandidateStageLimiter::limit(
			std::move(candidates), embeddingLimit,
			QStringLiteral("embedding_top_candidate_stage_limit:%1").arg(embeddingLimit));
	}

	const int semanticTotal = static_cast<int>(candidates.size());
	if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_before_semantic_scoring")))
		return result;
	PipelineProgress::report(options,
				 QStringLiteral("Embedding/scoring %1 marker candidates...").arg(semanticTotal), 40);
	QElapsedTimer semanticStageTimer;
	semanticStageTimer.start();
	candidates = semanticScoreCandidates(index, options, std::move(candidates));
	blog(LOG_INFO,
	     "[clip-cropper] Semantic embedding/scoring stage finished. video=%s candidates=%d elapsedMs=%lld",
	     options.videoPath.toUtf8().constData(), semanticTotal,
	     static_cast<long long>(semanticStageTimer.elapsed()));
	if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_during_semantic_scoring")))
		return result;
	PipelineProgress::report(
		options, QStringLiteral("Semantic scoring finished for %1 marker candidates").arg(semanticTotal), 66);

	if (feedbackMemory.loaded) {
		FeedbackConsistencyGate feedbackGate;
		FeedbackConsistencyGateOptions feedbackGateOptions;
		feedbackGateOptions.viewerMessagePreset = viewerPreset;
		candidates = feedbackGate.apply(std::move(candidates), index, feedbackMemory, feedbackGateOptions);
		FeedbackTrainedRanker trainedRanker;
		candidates = trainedRanker.apply(std::move(candidates), index, feedbackMemory, options.scoring.presetId,
						 options.videoPath);
	}

	candidates = CandidateBuilder::enforceSemanticAvailability(
		std::move(candidates), options.budget.requireSemanticScoringWhenEmbeddingProviderEnabled,
		options.embeddingProvider != nullptr);
	candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
					[](const ClipCandidate &candidate) { return candidate.rejectedByQualityGate; }),
			 candidates.end());
	if (candidates.isEmpty()) {
		result.summary = QStringLiteral("no_candidates: semantic_embedding_required_but_unavailable_or_failed");
		return result;
	}

	if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_before_reranking")))
		return result;

	const bool rerankerWasAvailable = options.rerankerOptions.enabled && options.reranker &&
					  options.reranker->isAvailable();
	if (rerankerWasAvailable) {
		const int rerankerLimit = CandidateStageLimiter::rerankerLimit(options);
		candidates = CandidateStageLimiter::limit(
			std::move(candidates), rerankerLimit,
			QStringLiteral("reranker_top_candidate_stage_limit:%1").arg(rerankerLimit));
	}

	SemanticRerankerStage rerankerStage;
	PipelineProgress::report(
		options,
		QStringLiteral("Reranking top %1 marker candidates...").arg(static_cast<int>(candidates.size())), 72);
	QElapsedTimer rerankerStageTimer;
	rerankerStageTimer.start();
	const int rerankerCandidateCount = static_cast<int>(candidates.size());
	candidates = rerankerStage.apply(std::move(candidates), rerankerContextFromOptions(options),
					 options.rerankerOptions, options.reranker);
	blog(LOG_INFO, "[clip-cropper] Reranker stage finished. video=%s candidates=%d elapsedMs=%lld",
	     options.videoPath.toUtf8().constData(), rerankerCandidateCount,
	     static_cast<long long>(rerankerStageTimer.elapsed()));
	if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_during_reranking")))
		return result;
	PipelineProgress::report(
		options,
		QStringLiteral("Reranking completed for %1 marker candidates").arg(static_cast<int>(candidates.size())),
		80);

	if (semanticBackendAvailable && options.embeddingProvider && options.embeddingProvider->isAvailable()) {
		const int boundaryLimit = CandidateStageLimiter::boundaryDpLimit(options);
		candidates = CandidateStageLimiter::limit(
			std::move(candidates), boundaryLimit,
			QStringLiteral("boundary_dp_top_candidate_stage_limit:%1").arg(boundaryLimit));
		if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_before_boundary_dp")))
			return result;
		PipelineProgress::report(options,
					 QStringLiteral("Refining top %1 marker candidates with boundary DP...")
						 .arg(static_cast<int>(candidates.size())),
					 82);
		BoundaryRefinementStage refinementStage;
		QElapsedTimer boundaryStageTimer;
		boundaryStageTimer.start();
		const int boundaryCandidateCount = static_cast<int>(candidates.size());
		candidates = refinementStage.apply(index, std::move(candidates),
						   boundaryRefinementOptionsFromOptions(options));
		blog(LOG_INFO, "[clip-cropper] Boundary DP stage finished. video=%s candidates=%d elapsedMs=%lld",
		     options.videoPath.toUtf8().constData(), boundaryCandidateCount,
		     static_cast<long long>(boundaryStageTimer.elapsed()));
		PipelineProgress::report(options,
					 QStringLiteral("Boundary DP refinement finished for %1 marker candidates")
						 .arg(static_cast<int>(candidates.size())),
					 85);
	}

	if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_before_quality_gate")))
		return result;

	ViewerArcGate viewerArcGate;
	const ViewerArcGateOptions arcGateOptions = viewerArcGateOptionsFromOptions(options);
	viewerArcGate.recoverMissingOpenings(index, candidates, arcGateOptions);

	CandidateQualityGate qualityGate;
	PipelineProgress::report(options, QStringLiteral("Applying semantic quality gate..."), 86);
	candidates =
		qualityGate.apply(std::move(candidates), qualityGateOptionsFromOptions(options, rerankerWasAvailable));

	if (feedbackMemory.loaded) {
		FeedbackConsistencyGate feedbackGate;
		FeedbackConsistencyGateOptions feedbackGateOptions;
		feedbackGateOptions.viewerMessagePreset = viewerPreset;
		candidates = feedbackGate.apply(std::move(candidates), index, feedbackMemory, feedbackGateOptions);
		FeedbackTrainedRanker trainedRanker;
		candidates = trainedRanker.apply(std::move(candidates), index, feedbackMemory, options.scoring.presetId,
						 options.videoPath);
	}
	candidates = viewerArcGate.apply(std::move(candidates), arcGateOptions);
	if (viewerPreset) {
		blog(LOG_INFO,
		     "[clip-cropper] Feedback-guided candidate gate summary. video=%s total=%d usable=%d rejected=%d positive=%d learned=%d",
		     options.videoPath.toUtf8().constData(), static_cast<int>(candidates.size()),
		     usableCandidateCount(candidates), rejectedCandidateCount(candidates),
		     positiveFeedbackCandidateCount(candidates), learnedFeedbackAcceptedCandidateCount(candidates));
		blog(LOG_INFO, "[clip-cropper] Feedback-guided candidate rejection diagnostics. video=%s %s",
		     options.videoPath.toUtf8().constData(),
		     feedbackGateRejectionDiagnostics(candidates).toUtf8().constData());
	}
	const QVector<ClipCandidate> candidatesBeforeRejectedErase = candidates;
	const QVector<ClipCandidate> rejectedAfterArcGate =
		topRejectedCandidatesForDiagnostics(candidates, 12, feedbackMemory.loaded ? &feedbackMemory : nullptr);
	const QVector<ClipCandidate> relaxedRejectedAfterArcGate =
		viewerPreset && feedbackMemory.loaded
			? topRejectedCandidatesForDiagnostics(candidates, 12, &feedbackMemory,
							      DiagnosticReviewedRangeMode::RelaxedExploration)
			: rejectedAfterArcGate;
	candidates.erase(std::remove_if(candidates.begin(), candidates.end(), isRejectedCandidate), candidates.end());
	const int suppressedExistingExactSeeds =
		suppressExistingReviewRangeExactSeeds(candidates, options.existingReviewRanges);
	if (suppressedExistingExactSeeds > 0) {
		blog(LOG_INFO,
		     "[clip-cropper] Suppressed already-existing exact feedback seed markers before final selection. video=%s suppressed=%d",
		     options.videoPath.toUtf8().constData(), suppressedExistingExactSeeds);
		candidates.erase(std::remove_if(candidates.begin(), candidates.end(), isRejectedCandidate),
				 candidates.end());
	}
	int suppressedReviewedExactSeeds = 0;
	int suppressedReviewedFeedbackCandidates = 0;
	if (viewerPreset && feedbackMemory.loaded) {
		suppressedReviewedExactSeeds = suppressAlreadyReviewedExactPositiveSeeds(candidates, feedbackMemory);
		if (suppressedReviewedExactSeeds > 0) {
			blog(LOG_INFO,
			     "[clip-cropper] Suppressed already-reviewed exact positive feedback seeds before final selection. video=%s suppressed=%d",
			     options.videoPath.toUtf8().constData(), suppressedReviewedExactSeeds);
			candidates.erase(std::remove_if(candidates.begin(), candidates.end(), isRejectedCandidate),
					 candidates.end());
		}
		suppressedReviewedFeedbackCandidates =
			suppressAlreadyReviewedFeedbackCandidates(candidates, feedbackMemory);
		if (suppressedReviewedFeedbackCandidates > 0) {
			blog(LOG_INFO,
			     "[clip-cropper] Suppressed already-reviewed feedback candidates before final selection. video=%s suppressed=%d",
			     options.videoPath.toUtf8().constData(), suppressedReviewedFeedbackCandidates);
			candidates.erase(std::remove_if(candidates.begin(), candidates.end(), isRejectedCandidate),
					 candidates.end());
		}
	}

	if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_before_final_ranking")))
		return result;
	PipelineProgress::report(options, QStringLiteral("Selecting final marker suggestions..."), 92);

	if (candidates.isEmpty() && viewerPreset && feedbackMemory.loaded &&
	    (suppressedExistingExactSeeds > 0 || suppressedReviewedExactSeeds > 0 ||
	     suppressedReviewedFeedbackCandidates > 0)) {
		blog(LOG_INFO,
		     "[clip-cropper] Positive feedback replay skipped after suppressing reviewed/existing seeds. video=%s existingSuppressed=%d reviewedSuppressed=%d reviewedFeedbackSuppressed=%d",
		     options.videoPath.toUtf8().constData(), suppressedExistingExactSeeds, suppressedReviewedExactSeeds,
		     suppressedReviewedFeedbackCandidates);
	}

	if (candidates.isEmpty() && viewerPreset && feedbackMemory.loaded && rejectedAfterArcGate.isEmpty() &&
	    suppressedExistingExactSeeds <= 0 && suppressedReviewedExactSeeds <= 0 &&
	    suppressedReviewedFeedbackCandidates <= 0) {
		// Positive feedback ranges are ground-truth examples, not fresh suggestions. Replaying
		// them into the timeline after an empty gate result made "suggest new clips" overwrite
		// good markers with already-reviewed ranges and let the user re-rate the same clips.
		// Keep them only as memory/calibration signals; surface new rejected/diagnostic probes
		// below instead of restoring exact positives as upload markers.
		blog(LOG_INFO,
		     "[clip-cropper] Positive feedback replay disabled after empty gate result. video=%s positives=%d",
		     options.videoPath.toUtf8().constData(), static_cast<int>(feedbackMemory.positiveRanges.size()));
	}

	if (candidates.isEmpty()) {
		QVector<ClipCandidate> diagnostics = rejectedAfterArcGate;
		appendUniqueDiagnosticCandidates(diagnostics, relaxedRejectedAfterArcGate, 12);
		if (diagnostics.isEmpty() && rejectedCandidateCount(candidatesBeforeRejectedErase) > 0) {
			diagnostics = topRejectedCandidatesForDiagnostics(
				candidatesBeforeRejectedErase, 12, feedbackMemory.loaded ? &feedbackMemory : nullptr,
				DiagnosticReviewedRangeMode::RelaxedExploration);
			if (!diagnostics.isEmpty())
				blog(LOG_INFO,
				     "[clip-cropper] Recovered unreviewed diagnostics from rejected gate pool. video=%s rejected=%d diagnostics=%d",
				     options.videoPath.toUtf8().constData(),
				     rejectedCandidateCount(candidatesBeforeRejectedErase),
				     static_cast<int>(diagnostics.size()));
			else
				blog(LOG_INFO,
				     "[clip-cropper] Rejected gate pool contained only reviewed/ignored diagnostic neighborhoods. video=%s rejected=%d",
				     options.videoPath.toUtf8().constData(),
				     rejectedCandidateCount(candidatesBeforeRejectedErase));
		}
		const int feedbackSuppressedAfterArcGate =
			feedbackSuppressedCandidateCount(candidatesBeforeRejectedErase);
		const int rejectedAfterArcGateCount = rejectedCandidateCount(candidatesBeforeRejectedErase);
		if (viewerPreset && feedbackMemory.loaded && diagnostics.size() < 4 &&
		    (feedbackSuppressedAfterArcGate > 0 || rejectedAfterArcGateCount > 0 ||
		     suppressedExistingExactSeeds > 0 || suppressedReviewedExactSeeds > 0)) {
			PipelineProgress::report(
				options,
				QStringLiteral(
					"Exploring new diagnostic candidates outside rejected feedback ranges..."),
				93);
			QElapsedTimer noveltyTimer;
			noveltyTimer.start();
			NoveltyExplorationBuildStats noveltyStats;
			QVector<ClipCandidate> exploration =
				buildNoveltyExplorationCandidates(index, options, feedbackMemory, &noveltyStats);
			blog(LOG_INFO,
			     "[clip-cropper] Starting novelty exploration fallback. video=%s suppressed=%d raw=%d reviewedSuppressed=%d localAccepted=%d filtered=%d workers=%d generationMs=%lld localScoreMs=%lld diversityMs=%lld negative=%d positive=%d semanticPositive=%d",
			     options.videoPath.toUtf8().constData(), feedbackSuppressedAfterArcGate,
			     noveltyStats.rawCandidates, noveltyStats.reviewedSuppressed, noveltyStats.localAccepted,
			     static_cast<int>(exploration.size()), noveltyStats.workerCount, noveltyStats.generationMs,
			     noveltyStats.localScoreMs, noveltyStats.diversityMs,
			     static_cast<int>(feedbackMemory.negativeRanges.size()),
			     static_cast<int>(feedbackMemory.positiveRanges.size()),
			     semanticPrototypePositiveRangeCount(feedbackMemory));

			long long semanticMs = 0;
			long long rerankMs = 0;
			long long boundaryMs = 0;
			long long gateMs = 0;
			QVector<ClipCandidate> diagnosticFallbackPool = exploration;
			QVector<ClipCandidate> diagnosticPreGatePool;
			if (!exploration.isEmpty()) {
				QElapsedTimer stageTimer;
				if (semanticBackendAvailable && options.embeddingProvider &&
				    options.embeddingProvider->isAvailable()) {
					stageTimer.start();
					exploration = semanticScoreCandidates(index, options, std::move(exploration));
					semanticMs = stageTimer.elapsed();
				}
				exploration = CandidateBuilder::enforceSemanticAvailability(
					std::move(exploration),
					options.budget.requireSemanticScoringWhenEmbeddingProviderEnabled,
					options.embeddingProvider != nullptr);
				exploration.erase(
					std::remove_if(exploration.begin(), exploration.end(),
						       [](const ClipCandidate &candidate) {
							       return candidate.rejectedByQualityGate &&
								      candidate.rejectionReason ==
									      QStringLiteral(
										      "semantic_embedding_unavailable");
						       }),
					exploration.end());
				SemanticRerankerStage explorationReranker;
				stageTimer.restart();
				exploration = explorationReranker.apply(std::move(exploration),
									rerankerContextFromOptions(options),
									options.rerankerOptions, options.reranker);
				rerankMs = stageTimer.elapsed();
				if (semanticBackendAvailable && options.embeddingProvider &&
				    options.embeddingProvider->isAvailable()) {
					BoundaryRefinementStage explorationRefinement;
					stageTimer.restart();
					exploration = explorationRefinement.apply(
						index, std::move(exploration),
						boundaryRefinementOptionsFromOptions(options));
					boundaryMs = stageTimer.elapsed();
				}
				diagnosticPreGatePool = exploration;
				stageTimer.restart();
				CandidateQualityGate explorationQualityGate;
				exploration = explorationQualityGate.apply(
					std::move(exploration),
					qualityGateOptionsFromOptions(options, rerankerWasAvailable));
				FeedbackConsistencyGate explorationFeedbackGate;
				FeedbackConsistencyGateOptions explorationFeedbackOptions;
				explorationFeedbackOptions.viewerMessagePreset = true;
				exploration = explorationFeedbackGate.apply(std::move(exploration), index,
									    feedbackMemory, explorationFeedbackOptions);
				FeedbackTrainedRanker explorationTrainedRanker;
				exploration = explorationTrainedRanker.apply(std::move(exploration), index,
									     feedbackMemory, options.scoring.presetId,
									     options.videoPath);
				ViewerArcGate explorationArcGate;
				explorationArcGate.recoverMissingOpenings(index, exploration, arcGateOptions);
				exploration = explorationArcGate.apply(std::move(exploration), arcGateOptions);
				gateMs = stageTimer.elapsed();
				QVector<ClipCandidate> promotedNovelty = noveltyTimelineCandidatesFromCandidates(
					exploration, feedbackMemory, options.ranking.maxCandidates);
				if (!promotedNovelty.isEmpty()) {
					blog(LOG_INFO,
					     "[clip-cropper] Novelty exploration promoted candidates to timeline. video=%s promoted=%d",
					     options.videoPath.toUtf8().constData(),
					     static_cast<int>(promotedNovelty.size()));
					candidates = std::move(promotedNovelty);
				}
				if (candidates.isEmpty())
					appendUniqueDiagnosticCandidates(
						diagnostics,
						explorationDiagnosticsFromCandidates(exploration, feedbackMemory), 12);
				if (candidates.isEmpty() && diagnostics.size() < 4) {
					const int beforeDiagnostics = diagnostics.size();
					appendUniqueDiagnosticCandidates(diagnostics,
									 explorationDiagnosticsFromCandidates(
										 diagnosticPreGatePool, feedbackMemory),
									 12);
					if (diagnostics.size() > beforeDiagnostics)
						blog(LOG_INFO,
						     "[clip-cropper] Novelty exploration diagnostics recovered from pre-gate pool. video=%s count=%d",
						     options.videoPath.toUtf8().constData(),
						     static_cast<int>(diagnostics.size()));
				}
				if (candidates.isEmpty() && diagnostics.size() < 4) {
					const int beforeDiagnostics = diagnostics.size();
					appendUniqueDiagnosticCandidates(
						diagnostics,
						explorationDiagnosticsFromCandidates(diagnosticFallbackPool,
										     feedbackMemory),
						12);
					if (diagnostics.size() > beforeDiagnostics)
						blog(LOG_INFO,
						     "[clip-cropper] Novelty exploration diagnostics recovered from local candidate pool. video=%s count=%d",
						     options.videoPath.toUtf8().constData(),
						     static_cast<int>(diagnostics.size()));
				}
			}

			blog(LOG_INFO,
			     "[clip-cropper] Novelty exploration fallback finished. video=%s diagnostics=%d semanticMs=%lld rerankMs=%lld boundaryMs=%lld gateMs=%lld elapsedMs=%lld",
			     options.videoPath.toUtf8().constData(), static_cast<int>(diagnostics.size()), semanticMs,
			     rerankMs, boundaryMs, gateMs, static_cast<long long>(noveltyTimer.elapsed()));
		}

		if (candidates.isEmpty() && diagnostics.size() < 6 && viewerPreset && feedbackMemory.loaded) {
			const int beforeCoverageDiagnostics = diagnostics.size();
			QVector<ClipCandidate> coverageDiagnostics =
				buildExhaustionCoverageDiagnostics(index, options, feedbackMemory, 6);
			appendUniqueDiagnosticCandidates(diagnostics, coverageDiagnostics, 6);
			if (diagnostics.size() > beforeCoverageDiagnostics)
				blog(LOG_INFO,
				     "[clip-cropper] Coverage exhaustion diagnostics supplemented review pool after reviewed/ignored neighborhoods. video=%s before=%d diagnostics=%d",
				     options.videoPath.toUtf8().constData(), beforeCoverageDiagnostics,
				     static_cast<int>(diagnostics.size()));
		}

		if (candidates.isEmpty()) {
			// Incomplete-arc candidates are useful for human review, but applying them as
			// timeline markers makes the UI look like it found a good clip even when the
			// arc gate explicitly says the viewer message/origin is missing. Preserve them
			// as diagnostics so the user can ignore, reject, or adjust them deliberately.
			result.rejectedCandidateDiagnostics = diagnostics;
			const QString reason =
				diagnostics.isEmpty()
					? QStringLiteral("feedback_guided_scoring_found_no_consistent_complete_arcs")
					: QStringLiteral("feedback_novelty_exploration_candidates_require_review");
			result.summary = ClipScoringPipelineSummary::noCandidate(reason, diagnostics);
			logRejectedCandidateDiagnostics(options.videoPath,
							diagnostics.isEmpty() ? QStringLiteral("viewer_arc_gate")
									      : QStringLiteral("novelty_exploration"),
							diagnostics);
			PipelineProgress::report(options, QStringLiteral("Marker suggestion analysis finished."), 100);
			return result;
		}
	}

	ClipRankerOptions finalRanking = options.ranking;
	bool feedbackDpSelectedFinalPool = false;
	int feedbackDpSelectedCount = 0;
	QVector<ClipCandidate> feedbackDpSelectedCandidates;
	QVector<ClipCandidate> rejectedAfterFinalFeedbackGate;
	if (viewerPreset && feedbackMemory.loaded) {
		FeedbackConsistencyGate feedbackGate;
		FeedbackConsistencyGateOptions feedbackGateOptions;
		feedbackGateOptions.viewerMessagePreset = true;
		candidates = feedbackGate.apply(std::move(candidates), index, feedbackMemory, feedbackGateOptions);
		FeedbackTrainedRanker trainedRanker;
		candidates = trainedRanker.apply(std::move(candidates), index, feedbackMemory, options.scoring.presetId,
						 options.videoPath);
		rejectedAfterFinalFeedbackGate = topRejectedCandidatesForDiagnostics(candidates, 12, &feedbackMemory);
		candidates.erase(std::remove_if(candidates.begin(), candidates.end(), isRejectedCandidate),
				 candidates.end());

		QVector<ClipCandidate> demotedUnsafeDirectPositive;
		auto unsafeEnd =
			std::remove_if(candidates.begin(), candidates.end(),
				       [&demotedUnsafeDirectPositive](const ClipCandidate &candidate) {
					       if (!isUnsafeArcBypass(candidate))
						       return false;
					       demotedUnsafeDirectPositive.append(demoteUnsafeArcBypass(candidate));
					       return true;
				       });
		candidates.erase(unsafeEnd, candidates.end());
		if (!demotedUnsafeDirectPositive.isEmpty()) {
			appendUniqueDiagnosticCandidates(rejectedAfterFinalFeedbackGate, demotedUnsafeDirectPositive,
							 12);
			blog(LOG_INFO,
			     "[clip-cropper] Demoted unsafe direct-positive arc bypass candidates to diagnostics. video=%s demoted=%d",
			     options.videoPath.toUtf8().constData(),
			     static_cast<int>(demotedUnsafeDirectPositive.size()));
		}

		if (candidates.isEmpty()) {
			result.rejectedCandidateDiagnostics = rejectedAfterFinalFeedbackGate;
			result.summary = ClipScoringPipelineSummary::noCandidate(
				QStringLiteral("feedback_final_selection_found_no_consistent_ranges"),
				rejectedAfterFinalFeedbackGate);
			logRejectedCandidateDiagnostics(options.videoPath, QStringLiteral("final_feedback_gate"),
							rejectedAfterFinalFeedbackGate);
			PipelineProgress::report(options, QStringLiteral("Marker suggestion analysis finished."), 100);
			return result;
		}
		FeedbackAwareIntervalSelector feedbackSelector;
		QVector<ClipCandidate> selectedByFeedbackDp =
			feedbackSelector.select(candidates, index, feedbackMemory, finalRanking);
		if (!selectedByFeedbackDp.isEmpty()) {
			blog(LOG_INFO,
			     "[clip-cropper] Feedback-aware interval DP selected marker pool. video=%s before=%d after=%d",
			     options.videoPath.toUtf8().constData(), static_cast<int>(candidates.size()),
			     static_cast<int>(selectedByFeedbackDp.size()));
			candidates = std::move(selectedByFeedbackDp);
			feedbackDpSelectedFinalPool = true;
			feedbackDpSelectedCount = static_cast<int>(candidates.size());
			feedbackDpSelectedCandidates = candidates;
		}
	}

	if (viewerPreset && feedbackMemory.loaded) {
		QVector<ClipCandidate> diagnostics = rejectedAfterArcGate;
		appendUniqueDiagnosticCandidates(diagnostics, relaxedRejectedAfterArcGate, 12);
		for (const ClipCandidate &candidate : rejectedAfterFinalFeedbackGate) {
			if (!hasSimilarDiagnosticRange(diagnostics, candidate.range))
				diagnostics.append(candidate);
		}
		if (!diagnostics.isEmpty()) {
			logRejectedCandidateDiagnostics(
				options.videoPath, QStringLiteral("selected_with_rejected_review_pool"), diagnostics);
			result.rejectedCandidateDiagnostics = diagnostics;
		}
	}

	result.candidates = ranker.rank(candidates, finalRanking);
	const int finalRankedCount = static_cast<int>(result.candidates.size());
	if (feedbackDpSelectedFinalPool && feedbackDpSelectedCount > 1 &&
	    result.candidates.size() < std::min(feedbackDpSelectedCount, 3)) {
		QVector<ClipCandidate> restored = feedbackDpSelectedCandidates;
		std::sort(restored.begin(), restored.end(), [](const ClipCandidate &left, const ClipCandidate &right) {
			if (std::fabs(left.scores.final - right.scores.final) > 0.0001)
				return left.scores.final > right.scores.final;
			return left.range.startSec < right.range.startSec;
		});
		const int limit = std::min(finalRanking.maxCandidates, static_cast<int>(restored.size()));
		result.candidates.clear();
		result.candidates.reserve(limit);
		for (int i = 0; i < limit; ++i) {
			ClipCandidate selected = restored.at(i);
			selected.selectedRank = i + 1;
			selected.evidence.append(QStringLiteral("feedback_aware_interval_dp_final_pool_preserved"));
			selected.evidence.append(QStringLiteral("selected_rank:%1").arg(selected.selectedRank));
			selected.evidence.removeDuplicates();
			result.candidates.append(selected);
		}
		blog(LOG_INFO,
		     "[clip-cropper] Preserved feedback-aware DP marker pool after final ranker collapse. video=%s finalRanked=%d restored=%d",
		     options.videoPath.toUtf8().constData(), finalRankedCount,
		     static_cast<int>(result.candidates.size()));
	}
	if (result.candidates.isEmpty())
		result.summary = ClipScoringPipelineSummary::noCandidate(
			QStringLiteral("final_ranking_found_no_viable_ranges"), candidates);
	else {
		for (int i = 0; i < static_cast<int>(result.candidates.size()); ++i)
			PipelineProgress::report(
				options,
				PipelineProgress::candidateMessage(QStringLiteral("Selected marker"),
								   result.candidates.at(i), i + 1,
								   static_cast<int>(result.candidates.size())),
				94 + ((i * 5) / std::max(1, static_cast<int>(result.candidates.size()))));
		result.summary = ClipScoringPipelineSummary::build(result.candidates);
	}
	PipelineProgress::report(options, QStringLiteral("Marker suggestion analysis finished."), 100);
	return result;
}

SemanticCoarseRetrievalContext
ClipScoringPipeline::coarseContextFromOptions(const ClipScoringPipelineOptions &options) const
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

ClipRankerOptions
ClipScoringPipeline::preSemanticRankingOptionsFromOptions(const ClipScoringPipelineOptions &options) const
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

CandidateQualityGateOptions
ClipScoringPipeline::qualityGateOptionsFromOptions(const ClipScoringPipelineOptions &options,
						   bool rerankerWasAvailable) const
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
								    const ClipScoringPipelineOptions &options,
								    QVector<ClipCandidate> candidates) const
{
	if (candidates.isEmpty())
		return candidates;

	SemanticClipScorer semanticScorer;
	QVector<ClipCandidate> scored = semanticScorer.scoreBatch(
		index, candidates, semanticContextFromOptions(options), options.semantic, options.embeddingProvider);
	for (ClipCandidate &candidate : scored)
		candidate.evidence.append(QStringLiteral("semantic_embedding_batch_stage"));
	return scored;
}
