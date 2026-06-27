#include "curation/scoring/candidate-quality-gate.hpp"
#include "curation/scoring/candidate-quality-rules.hpp"

#include <algorithm>
#include <utility>

using namespace Curation::Scoring;

using namespace Curation::Scoring::CandidateQualityRules;

QVector<ClipCandidate> CandidateQualityGate::apply(QVector<ClipCandidate> candidates,
						   const CandidateQualityGateOptions &options) const
{
	for (ClipCandidate &candidate : candidates)
		candidate = apply(candidate, options);

	const bool allRejected = !candidates.isEmpty() && std::all_of(candidates.constBegin(), candidates.constEnd(),
								      [](const ClipCandidate &candidate) {
									      return candidate.rejectedByQualityGate;
								      });
	if (allRejected)
		return recoverFailsafeCandidates(std::move(candidates), options);

	return candidates;
}

ClipCandidate CandidateQualityGate::apply(const ClipCandidate &candidate,
					  const CandidateQualityGateOptions &options) const
{
	ClipCandidate gated = candidate;
	gated.qualityGateChecked = true;
	gated.scores.qualityGate = 1.0;

	const QString reason = rejectionReason(gated, options);
	if (!reason.isEmpty()) {
		gated.rejectedByQualityGate = true;
		gated.rejectionReason = reason;
		gated.scores.qualityGate = 0.0;
		gated.evidence.append(QStringLiteral("quality_rejected:%1").arg(reason));
	} else {
		gated.evidence.append(QStringLiteral("quality_gate_passed"));
		if (hasDevelopmentOnlyArcRoles(gated) && !hasResolutionOnlyArcRoles(gated) &&
		    !hasDegenerateArcShape(gated, options))
			gated.evidence.append(QStringLiteral("quality_gate_collapsed_arc_roles_softened"));
		if (hasStrongRepairedResolutionOnlyArc(gated))
			gated.evidence.append(QStringLiteral("quality_gate_resolution_only_arc_semantic_repaired"));
		if (hasUsefulExchangeArc(gated, options))
			gated.evidence.append(QStringLiteral("quality_gate_exchange_arc_validated"));
		if (hasValidContextualStateMachine(gated))
			gated.evidence.append(QStringLiteral("quality_gate_contextual_state_machine_validated"));
		if (hasReliableConclusionSupport(gated, options))
			gated.evidence.append(QStringLiteral("quality_gate_conclusion_semantic_support"));
		if (hasWeakConclusionFailsafe(gated, options))
			gated.evidence.append(QStringLiteral("quality_gate_weak_conclusion_failsafe"));
		if (hasSemanticBackedArcSupport(gated, options))
			gated.evidence.append(QStringLiteral("quality_gate_semantic_backed_arc_support"));
		if (hasCuriosityArcSupport(gated, options))
			gated.evidence.append(QStringLiteral("quality_gate_curiosity_arc_validated"));
		if (isEmpathyBacked(gated, options))
			gated.evidence.append(QStringLiteral("quality_gate_empathy_backed"));
	}

	gated.evidence.removeDuplicates();
	return gated;
}

QString CandidateQualityGate::rejectionReason(const ClipCandidate &candidate,
					      const CandidateQualityGateOptions &options) const
{
	if (durationSec(candidate) < options.minDurationSec)
		return QStringLiteral("too_short");
	if (candidate.text.trimmed().size() < options.minTextChars)
		return QStringLiteral("not_enough_text");
	if (candidate.rejectedAsNoise || candidate.scores.noise >= options.maxNoiseScore)
		return QStringLiteral("noise_or_stream_management");

	// Explicit user positives are ground truth for timeline application.
	// They may still look "contextually incomplete" to the automatic arc classifier
	// because the classifier can miss the viewer-message cue, but a liked/approved
	// diagnostic range should be allowed to become an applied marker on the next run
	// unless it is contaminated by explicit negative feedback.
	if (isFeedbackPositiveGroundTruth(candidate) && !isFeedbackNegativeBlocked(candidate))
		return {};

	if (hasHardLearnedContextBlocker(candidate))
		return QStringLiteral("hard_context_blocker");

	if (options.requireRerankerWhenAvailable && options.rejectInvalidRerankerWhenRequired &&
	    candidate.rerankerAttempted && !candidate.rerankerAvailable) {
		const QString failure = candidate.rerankerFailureReason.trimmed();
		if (!isNeutralRerankerFailureReason(failure))
			return failure.isEmpty() ? QStringLiteral("reranker_unavailable")
						 : QStringLiteral("reranker_%1").arg(failure.left(80));
	}
	if (options.requireRerankerWhenAvailable && candidate.rerankerFailed) {
		const QString failure = candidate.rerankerFailureReason.trimmed();
		if (!isNeutralRerankerFailureReason(failure))
			return failure.isEmpty() ? QStringLiteral("reranker_failed")
						 : QStringLiteral("reranker_%1").arg(failure.left(80));
	}

	const bool viewerPreset = options.presetId == QStringLiteral("viewer_message_response");
	const bool usefulArc = hasUsefulExchangeArc(candidate, options);
	const bool cleanOpening = hasCleanOpening(candidate, options);
	const bool cleanEnding = hasCleanEnding(candidate, options);
	const bool untrimmedPauseTail = hasUntrimmedPauseTail(candidate, options);
	const double positive = semanticPositiveScore(candidate);
	const double negative = semanticNegativeScore(candidate);
	const double semanticMargin = positive - negative;
	const bool reliableTargetMode = viewerPreset && options.reliableMainTarget &&
					hasSpecificMainTarget(options.mainTarget);

	if (requiresStrictContextualArc(options) && candidate.semanticScoringAvailable &&
	    !hasCompleteContextualArc(candidate))
		return candidate.scores.arcCompleteness <= 0.0 ? QStringLiteral("missing_contextual_arc")
							       : QStringLiteral("invalid_contextual_arc");
	if (viewerPreset && durationSec(candidate) >= 75.0 &&
	    (candidate.scores.arcCompleteness < 0.70 || candidate.scores.maxInternalPauseSec >= 4.0 ||
	     (candidate.scores.topicContinuity > 0.0 && candidate.scores.topicContinuity < 0.84)))
		return QStringLiteral("overlong_viewer_arc");
	if (viewerPreset && hasImplicitContextualOpening(candidate))
		return QStringLiteral("implicit_opening_not_allowed");
	if (viewerPreset && hasResolutionPlateauWithoutOpening(candidate) &&
	    !hasStrongRecoveredContextualDpArc(candidate))
		return QStringLiteral("resolution_plateau_without_opening");
	if (reliableTargetMode && candidate.semanticScoringAvailable &&
	    !hasStrictReliableTargetSupport(candidate, options))
		return QStringLiteral("semantic_target_not_specific");

	if (options.requireSemanticTargetWhenAvailable && options.reliableMainTarget &&
	    candidate.semanticScoringAvailable &&
	    candidate.scores.semanticTarget < options.minSemanticTargetWhenAvailable && !usefulArc)
		return QStringLiteral("semantic_target_mismatch");

	if (candidate.semanticScoringAvailable) {
		// Embedding scores are similarities, not calibrated classifiers. Treat meta/noise as a hard
		// boundary defect only when it clearly beats the useful opening/ending signal. Ties are
		// common with multilingual embeddings and should be resolved by the arc refiner/reranker,
		// not by a direct veto.
		const bool openingMetaClearlyWins =
			candidate.scores.semanticOpeningMetaNoise >= options.maxOpeningMetaNoiseWhenAvailable &&
			candidate.scores.semanticOpeningMetaNoise >=
				candidate.scores.semanticOpeningHook + options.hardSemanticBoundaryDefectMargin;
		const bool openingMetaVeryHigh =
			candidate.scores.semanticOpeningMetaNoise >= options.hardOpeningMetaNoiseWhenAvailable &&
			candidate.scores.semanticOpeningHook <
				candidate.scores.semanticOpeningMetaNoise + options.cleanBoundaryOpeningMargin;
		const bool openingIsMeta = openingMetaClearlyWins || openingMetaVeryHigh;
		const bool endingMetaClearlyWins =
			candidate.scores.semanticEndingMetaNoise >= options.maxEndingMetaNoiseWhenAvailable &&
			candidate.scores.semanticEndingMetaNoise >=
				candidate.scores.semanticEndingResolution + options.hardSemanticBoundaryDefectMargin;
		const bool endingMetaVeryHigh =
			candidate.scores.semanticEndingMetaNoise >= options.hardEndingMetaNoiseWhenAvailable &&
			candidate.scores.semanticEndingResolution <
				candidate.scores.semanticEndingMetaNoise + options.cleanBoundaryEndingMargin;
		const bool endingIsMeta = endingMetaClearlyWins || endingMetaVeryHigh;
		const bool endingChangedTopic =
			candidate.scores.semanticEndingTopicShift >= options.maxEndingTopicShiftWhenAvailable &&
			candidate.scores.semanticEndingTopicShift >=
				candidate.scores.semanticEndingResolution + options.hardSemanticBoundaryDefectMargin;

		if (openingIsMeta && !cleanOpening)
			return QStringLiteral("semantic_opening_meta_noise");
		if (endingIsMeta && !cleanEnding)
			return QStringLiteral("semantic_ending_meta_noise");
		if (endingChangedTopic && !cleanEnding)
			return QStringLiteral("semantic_ending_topic_shift");
		if (candidate.scores.semanticMetaNoise >= options.hardMaxSemanticMetaNoiseWhenAvailable &&
		    semanticMargin < 0.0)
			return QStringLiteral("semantic_meta_noise");
		if (candidate.scores.semanticNoise >= options.maxSemanticNoiseWhenAvailable &&
		    semanticMargin < -options.semanticNoiseMarginTolerance)
			return QStringLiteral("semantic_noise");
	}

	if (candidate.rerankerAvailable) {
		const bool positiveRerankerStrong = candidate.scores.rerankerRaw >=
						    options.minStrongGoodRawForBadClipOverride;
		const bool defectBatchSaturated =
			hasEvidence(candidate, QStringLiteral("reranker_defect_batch_saturated"));
		const bool hardDefect = candidate.scores.rerankerBadClip >=
						options.maxHardRerankerBadClipWhenAvailable &&
					candidate.scores.rerankerClipQualityMargin < -0.12;
		const bool localOpeningFailure =
			!cleanOpening &&
			candidate.scores.semanticOpeningMetaNoise >=
				candidate.scores.semanticOpeningHook + options.hardSemanticBoundaryDefectMargin;
		const bool localEndingFailure =
			!cleanEnding &&
			std::max(candidate.scores.semanticEndingMetaNoise, candidate.scores.semanticEndingTopicShift) >=
				candidate.scores.semanticEndingResolution + options.hardSemanticBoundaryDefectMargin;
		const bool localArcFailure =
			candidate.scores.arcCompleteness > 0.0 &&
			(candidate.scores.arcTailRisk > options.maxArcTailRiskForViewerPreset + 0.08 ||
			 candidate.scores.arcBoundaryCleanliness <
				 options.minArcBoundaryCleanlinessForViewerPreset - 0.10 ||
			 candidate.scores.arcCompleteness < options.minArcCompletenessForViewerPreset - 0.10);
		const bool shapeDisagrees = localOpeningFailure || localEndingFailure || localArcFailure ||
					    semanticMargin < -0.12;
		const bool badDefectOnlyWinsWhenShapeAlsoFails =
			!defectBatchSaturated &&
			candidate.scores.rerankerBadClip >= options.maxRerankerBadClipWhenAvailable &&
			candidate.scores.rerankerClipQualityMargin < options.minRerankerGoodBadMarginWhenAvailable &&
			shapeDisagrees;

		if (!defectBatchSaturated && hardDefect && shapeDisagrees)
			return QStringLiteral("reranker_structural_defect");
		if (badDefectOnlyWinsWhenShapeAlsoFails)
			return QStringLiteral("reranker_defect_confirmed_by_shape");
		if (candidate.scores.rerankerRaw < options.minRerankerRawScoreWhenAvailable && !usefulArc &&
		    !positiveRerankerStrong)
			return QStringLiteral("weak_reranker_raw_score");
	}

	if (viewerPreset) {
		if (untrimmedPauseTail)
			return QStringLiteral("untrimmed_long_pause_tail");
		if (hasDegenerateArcShape(candidate, options))
			return QStringLiteral("exchange_arc_degenerate_roles");
		if (hasWeakSocialOpening(candidate, options))
			return QStringLiteral("weak_social_opening");
		if (hasOverextendedTail(candidate, options))
			return QStringLiteral("overextended_tail_after_resolution");

		if (candidate.scores.arcCompleteness > 0.0) {
			const bool semanticBackedArc = hasSemanticBackedArcSupport(candidate, options);
			const bool curiosityArc = hasCuriosityArcSupport(candidate, options);
			if (candidate.scores.arcTailRisk > options.maxArcTailRiskForViewerPreset)
				return QStringLiteral("exchange_arc_tail_risk");
			if (candidate.scores.arcOpening < options.minArcOpeningForViewerPreset && !semanticBackedArc &&
			    !curiosityArc)
				return QStringLiteral("exchange_arc_weak_opening");
			if (candidate.scores.arcDevelopment < options.minArcDevelopmentForViewerPreset &&
			    !semanticBackedArc && !curiosityArc)
				return QStringLiteral("exchange_arc_weak_development");
			if (candidate.scores.arcConclusion < options.minArcConclusionForViewerPreset &&
			    !hasReliableConclusionSupport(candidate, options) &&
			    !hasWeakConclusionFailsafe(candidate, options) && !semanticBackedArc && !curiosityArc)
				return QStringLiteral("exchange_arc_weak_conclusion");
			if (candidate.scores.arcBoundaryCleanliness <
				    options.minArcBoundaryCleanlinessForViewerPreset &&
			    !semanticBackedArc && !curiosityArc)
				return QStringLiteral("exchange_arc_dirty_boundary");
			if (candidate.scores.arcCompleteness < options.minArcCompletenessForViewerPreset &&
			    !semanticBackedArc && !curiosityArc && !hasWeakConclusionFailsafe(candidate, options))
				return QStringLiteral("exchange_arc_incomplete");
		} else if (!candidate.semanticScoringAvailable) {
			return QStringLiteral("exchange_arc_not_available");
		}

		const bool semanticBackedArc = hasSemanticBackedArcSupport(candidate, options);
		const bool curiosityArc = hasCuriosityArcSupport(candidate, options);
		if (isSocialMetaDominated(candidate, options) && !curiosityArc)
			return QStringLiteral("social_meta_dominated");
		if (!cleanOpening && !semanticBackedArc && !curiosityArc)
			return QStringLiteral("dirty_opening_boundary");
		if (!cleanEnding && !semanticBackedArc && !curiosityArc)
			return QStringLiteral("dirty_ending_boundary");
		if (candidate.scores.viewerResponse < options.minViewerResponseForViewerPreset && !usefulArc &&
		    !curiosityArc)
			return QStringLiteral("weak_viewer_response");

		if (candidate.semanticScoringAvailable && !usefulArc && !curiosityArc) {
			const double hook =
				std::max(candidate.scores.semanticHook, candidate.scores.semanticOpeningHook);
			const double resolution = std::max(candidate.scores.semanticResolution,
							   candidate.scores.semanticEndingResolution);
			if (candidate.scores.semanticClipValue < options.minClipValueForViewerPreset)
				return QStringLiteral("weak_clip_value");
			if (hook < options.minHookForViewerPreset)
				return QStringLiteral("weak_hook");
			if (candidate.scores.semanticOpeningHook < options.minOpeningHookForViewerPreset)
				return QStringLiteral("weak_opening_hook");
			if (resolution < options.minResolutionForViewerPreset)
				return QStringLiteral("weak_resolution");
			if (candidate.scores.semanticEndingResolution < options.minEndingResolutionForViewerPreset)
				return QStringLiteral("weak_ending_resolution");
			if (candidate.scores.topicContinuity > 0.0 &&
			    candidate.scores.topicContinuity < options.minTopicContinuityForViewerPreset)
				return QStringLiteral("weak_topic_continuity");
		}
	}

	if (candidate.semanticScoringAvailable && durationSec(candidate) >= options.longCandidateDurationSec &&
	    candidate.scores.topicContinuity > 0.0 &&
	    candidate.scores.topicContinuity < options.minTopicContinuityForLongCandidate && !usefulArc)
		return QStringLiteral("weak_topic_continuity");

	if (candidate.scores.final < options.minFinalScore && !usefulArc)
		return QStringLiteral("low_final_score");

	return {};
}
