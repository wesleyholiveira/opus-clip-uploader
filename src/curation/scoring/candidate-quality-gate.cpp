#include "curation/scoring/candidate-quality-gate.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

using namespace Curation::Scoring;

namespace {

static double boundedScore(double value)
{
	return std::clamp(value, 0.0, 1.0);
}

static double durationSec(const ClipCandidate &candidate)
{
	return candidate.range.endSec - candidate.range.startSec;
}

static bool hasEvidence(const ClipCandidate &candidate, const QString &value)
{
	return candidate.evidence.contains(value);
}

static bool evidenceContainsFragment(const ClipCandidate &candidate, const QString &fragment)
{
	for (const QString &evidence : candidate.evidence) {
		if (evidence.contains(fragment))
			return true;
	}
	return false;
}

static bool hasArcRole(const ClipCandidate &candidate, const QString &role)
{
	return evidenceContainsFragment(candidate, QStringLiteral(":%1").arg(role));
}

static bool arcRolesStartWith(const ClipCandidate &candidate, const QString &role)
{
	return evidenceContainsFragment(candidate, QStringLiteral("exchange_arc_roles:0:%1").arg(role));
}

static bool hasResolutionOnlyArcRoles(const ClipCandidate &candidate)
{
	return arcRolesStartWith(candidate, QStringLiteral("resolution")) &&
		!hasArcRole(candidate, QStringLiteral("opening")) &&
		!hasArcRole(candidate, QStringLiteral("development"));
}

static bool hasDevelopmentOnlyArcRoles(const ClipCandidate &candidate)
{
	return arcRolesStartWith(candidate, QStringLiteral("development")) &&
		!hasArcRole(candidate, QStringLiteral("opening")) &&
		!hasArcRole(candidate, QStringLiteral("resolution"));
}

static bool hasStrongHumanContext(const ClipCandidate &candidate)
{
	const double strongestContext = std::max({candidate.scores.semanticViewerMessage,
		candidate.scores.semanticDirectAnswer, candidate.scores.semanticTarget});
	const double strongestMeta = std::max({candidate.scores.semanticMetaNoise,
		candidate.scores.semanticOpeningMetaNoise, candidate.scores.semanticEndingMetaNoise});
	const bool explicitContext = strongestContext >= 0.66;
	const bool empathyClearlyWins = candidate.scores.semanticEmpathy >= 0.66 &&
		candidate.scores.semanticEmpathy >= strongestMeta + 0.08 &&
		candidate.scores.semanticClipValue >= candidate.scores.semanticMetaNoise + 0.03;
	return explicitContext || empathyClearlyWins;
}

static bool semanticSupportsCollapsedArcRoles(const ClipCandidate &candidate)
{
	if (!candidate.semanticScoringAvailable)
		return false;

	// A collapsed role sequence (all resolution/all development) is common with very
	// short WhisperX-aligned chunks, but it must not become a free pass. Only soften
	// it when independent semantic evidence says this is a real human-context clip
	// with a clean hook/payoff, not stream setup, social check-in, gift thanks or
	// background-game chatter that happens to look semantically "complete".
	const double opening = std::max(candidate.scores.semanticOpeningHook, candidate.scores.semanticHook);
	const double resolution = std::max(candidate.scores.semanticEndingResolution, candidate.scores.semanticResolution);
	const double endingDefect = std::max(candidate.scores.semanticEndingMetaNoise, candidate.scores.semanticEndingTopicShift);
	const bool strongHumanContext = hasStrongHumanContext(candidate);
	const bool contextBoundaryWasImproved = hasEvidence(candidate, QStringLiteral("exchange_arc_context_start_extended")) ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_essential_context_prepended"));
	const bool openingBeatsMeta = opening >= candidate.scores.semanticOpeningMetaNoise +
		(strongHumanContext ? 0.00 : 0.035);
	const bool bodyBeatsMeta = candidate.scores.semanticClipValue >= candidate.scores.semanticMetaNoise +
		(strongHumanContext ? -0.01 : 0.025);
	const bool usefulSemanticArc = candidate.scores.semanticClipValue >= 0.66 &&
		opening >= 0.64 &&
		resolution >= 0.64 &&
		resolution >= endingDefect - 0.06 &&
		openingBeatsMeta &&
		bodyBeatsMeta &&
		(candidate.scores.topicContinuity <= 0.0 || candidate.scores.topicContinuity >= 0.64);
	const bool arcHasSomeOpeningEvidence = candidate.scores.arcOpening >= 0.42 ||
		(contextBoundaryWasImproved && candidate.scores.arcOpening >= 0.38);
	const bool rerankerDoesNotDisagree = !candidate.rerankerAvailable ||
		(candidate.scores.rerankerRaw >= 0.94 && candidate.scores.rerankerClipQualityMargin >= 0.30 &&
		 candidate.scores.rerankerStructureDefect <= 0.62);
	return usefulSemanticArc && arcHasSomeOpeningEvidence && rerankerDoesNotDisagree && strongHumanContext;
}

static bool isWeakConclusionReason(const QString &reason)
{
	return reason == QStringLiteral("exchange_arc_weak_conclusion") ||
		reason == QStringLiteral("exchange_arc_incomplete");
}

} // namespace

QVector<ClipCandidate> CandidateQualityGate::apply(QVector<ClipCandidate> candidates,
	const CandidateQualityGateOptions &options) const
{
	for (ClipCandidate &candidate : candidates)
		candidate = apply(candidate, options);

	const bool allRejected = !candidates.isEmpty() && std::all_of(candidates.constBegin(), candidates.constEnd(),
		[](const ClipCandidate &candidate) { return candidate.rejectedByQualityGate; });
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
		if ((hasResolutionOnlyArcRoles(gated) || hasDevelopmentOnlyArcRoles(gated)) &&
		    !hasDegenerateArcShape(gated, options))
			gated.evidence.append(QStringLiteral("quality_gate_collapsed_arc_roles_softened"));
		if (hasUsefulExchangeArc(gated, options))
			gated.evidence.append(QStringLiteral("quality_gate_exchange_arc_validated"));
		if (hasReliableConclusionFallback(gated, options))
			gated.evidence.append(QStringLiteral("quality_gate_conclusion_semantic_fallback"));
		if (hasWeakConclusionFailsafe(gated, options))
			gated.evidence.append(QStringLiteral("quality_gate_weak_conclusion_failsafe"));
		if (hasSemanticBackedArcFallback(gated, options))
			gated.evidence.append(QStringLiteral("quality_gate_semantic_backed_arc_fallback"));
		if (hasCuriosityArcFallback(gated, options))
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

	if (options.requireRerankerWhenAvailable && options.rejectInvalidRerankerWhenRequired &&
	    candidate.rerankerAttempted && !candidate.rerankerAvailable) {
		const QString failure = candidate.rerankerFailureReason.trimmed();
		return failure.isEmpty() ? QStringLiteral("reranker_unavailable")
					     : QStringLiteral("reranker_%1").arg(failure.left(80));
	}
	if (options.requireRerankerWhenAvailable && candidate.rerankerFailed) {
		const QString failure = candidate.rerankerFailureReason.trimmed();
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

	if (options.requireSemanticTargetWhenAvailable && options.reliableMainTarget && candidate.semanticScoringAvailable &&
	    candidate.scores.semanticTarget < options.minSemanticTargetWhenAvailable && !usefulArc)
		return QStringLiteral("semantic_target_mismatch");

	if (candidate.semanticScoringAvailable) {
		// Embedding scores are similarities, not calibrated classifiers. Treat meta/noise as a hard
		// boundary defect only when it clearly beats the useful opening/ending signal. Ties are
		// common with multilingual embeddings and should be resolved by the arc refiner/reranker,
		// not by a direct veto.
		const bool openingMetaClearlyWins = candidate.scores.semanticOpeningMetaNoise >=
			options.maxOpeningMetaNoiseWhenAvailable &&
			candidate.scores.semanticOpeningMetaNoise >= candidate.scores.semanticOpeningHook +
				options.hardSemanticBoundaryDefectMargin;
		const bool openingMetaVeryHigh = candidate.scores.semanticOpeningMetaNoise >=
			options.hardOpeningMetaNoiseWhenAvailable &&
			candidate.scores.semanticOpeningHook < candidate.scores.semanticOpeningMetaNoise +
				options.cleanBoundaryOpeningMargin;
		const bool openingIsMeta = openingMetaClearlyWins || openingMetaVeryHigh;
		const bool endingMetaClearlyWins = candidate.scores.semanticEndingMetaNoise >=
			options.maxEndingMetaNoiseWhenAvailable &&
			candidate.scores.semanticEndingMetaNoise >= candidate.scores.semanticEndingResolution +
				options.hardSemanticBoundaryDefectMargin;
		const bool endingMetaVeryHigh = candidate.scores.semanticEndingMetaNoise >=
			options.hardEndingMetaNoiseWhenAvailable &&
			candidate.scores.semanticEndingResolution < candidate.scores.semanticEndingMetaNoise +
				options.cleanBoundaryEndingMargin;
		const bool endingIsMeta = endingMetaClearlyWins || endingMetaVeryHigh;
		const bool endingChangedTopic = candidate.scores.semanticEndingTopicShift >= options.maxEndingTopicShiftWhenAvailable &&
			candidate.scores.semanticEndingTopicShift >= candidate.scores.semanticEndingResolution +
				options.hardSemanticBoundaryDefectMargin;

		if (openingIsMeta && !cleanOpening)
			return QStringLiteral("semantic_opening_meta_noise");
		if (endingIsMeta && !cleanEnding)
			return QStringLiteral("semantic_ending_meta_noise");
		if (endingChangedTopic && !cleanEnding)
			return QStringLiteral("semantic_ending_topic_shift");
		if (candidate.scores.semanticMetaNoise >= options.hardMaxSemanticMetaNoiseWhenAvailable && semanticMargin < 0.0)
			return QStringLiteral("semantic_meta_noise");
		if (candidate.scores.semanticNoise >= options.maxSemanticNoiseWhenAvailable && semanticMargin < -options.semanticNoiseMarginTolerance)
			return QStringLiteral("semantic_noise");
	}

	if (candidate.rerankerAvailable) {
		const bool positiveRerankerStrong = candidate.scores.rerankerRaw >= options.minStrongGoodRawForBadClipOverride;
		const bool defectBatchSaturated = hasEvidence(candidate, QStringLiteral("reranker_defect_batch_saturated"));
		const bool hardDefect = candidate.scores.rerankerBadClip >= options.maxHardRerankerBadClipWhenAvailable &&
			candidate.scores.rerankerClipQualityMargin < -0.12;
		const bool localOpeningFailure = !cleanOpening &&
			candidate.scores.semanticOpeningMetaNoise >= candidate.scores.semanticOpeningHook +
				options.hardSemanticBoundaryDefectMargin;
		const bool localEndingFailure = !cleanEnding &&
			std::max(candidate.scores.semanticEndingMetaNoise, candidate.scores.semanticEndingTopicShift) >=
				candidate.scores.semanticEndingResolution + options.hardSemanticBoundaryDefectMargin;
		const bool localArcFailure = candidate.scores.arcCompleteness > 0.0 &&
			(candidate.scores.arcTailRisk > options.maxArcTailRiskForViewerPreset + 0.08 ||
			 candidate.scores.arcBoundaryCleanliness < options.minArcBoundaryCleanlinessForViewerPreset - 0.10 ||
			 candidate.scores.arcCompleteness < options.minArcCompletenessForViewerPreset - 0.10);
		const bool shapeDisagrees = localOpeningFailure || localEndingFailure || localArcFailure || semanticMargin < -0.12;
		const bool badDefectOnlyWinsWhenShapeAlsoFails = !defectBatchSaturated &&
			candidate.scores.rerankerBadClip >= options.maxRerankerBadClipWhenAvailable &&
			candidate.scores.rerankerClipQualityMargin < options.minRerankerGoodBadMarginWhenAvailable && shapeDisagrees;

		if (!defectBatchSaturated && hardDefect && shapeDisagrees)
			return QStringLiteral("reranker_structural_defect");
		if (badDefectOnlyWinsWhenShapeAlsoFails)
			return QStringLiteral("reranker_defect_confirmed_by_shape");
		if (candidate.scores.rerankerRaw < options.minRerankerRawScoreWhenAvailable && !usefulArc && !positiveRerankerStrong)
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
			const bool semanticBackedArc = hasSemanticBackedArcFallback(candidate, options);
			const bool curiosityArc = hasCuriosityArcFallback(candidate, options);
			if (candidate.scores.arcTailRisk > options.maxArcTailRiskForViewerPreset)
				return QStringLiteral("exchange_arc_tail_risk");
			if (candidate.scores.arcOpening < options.minArcOpeningForViewerPreset && !semanticBackedArc && !curiosityArc)
				return QStringLiteral("exchange_arc_weak_opening");
			if (candidate.scores.arcDevelopment < options.minArcDevelopmentForViewerPreset && !semanticBackedArc && !curiosityArc)
				return QStringLiteral("exchange_arc_weak_development");
			if (candidate.scores.arcConclusion < options.minArcConclusionForViewerPreset &&
			    !hasReliableConclusionFallback(candidate, options) && !hasWeakConclusionFailsafe(candidate, options) &&
			    !semanticBackedArc && !curiosityArc)
				return QStringLiteral("exchange_arc_weak_conclusion");
			if (candidate.scores.arcBoundaryCleanliness < options.minArcBoundaryCleanlinessForViewerPreset && !semanticBackedArc && !curiosityArc)
				return QStringLiteral("exchange_arc_dirty_boundary");
			if (candidate.scores.arcCompleteness < options.minArcCompletenessForViewerPreset &&
			    !semanticBackedArc && !curiosityArc && !hasWeakConclusionFailsafe(candidate, options))
				return QStringLiteral("exchange_arc_incomplete");
		} else if (!candidate.semanticScoringAvailable) {
			return QStringLiteral("exchange_arc_not_available");
		}

		const bool semanticBackedArc = hasSemanticBackedArcFallback(candidate, options);
		const bool curiosityArc = hasCuriosityArcFallback(candidate, options);
		if (isSocialMetaDominated(candidate, options) && !curiosityArc)
			return QStringLiteral("social_meta_dominated");
		if (!cleanOpening && !semanticBackedArc && !curiosityArc)
			return QStringLiteral("dirty_opening_boundary");
		if (!cleanEnding && !semanticBackedArc && !curiosityArc)
			return QStringLiteral("dirty_ending_boundary");
		if (candidate.scores.viewerResponse < options.minViewerResponseForViewerPreset && !usefulArc && !curiosityArc)
			return QStringLiteral("weak_viewer_response");

		if (candidate.semanticScoringAvailable && !usefulArc && !curiosityArc) {
			const double hook = std::max(candidate.scores.semanticHook, candidate.scores.semanticOpeningHook);
			const double resolution = std::max(candidate.scores.semanticResolution, candidate.scores.semanticEndingResolution);
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
	    candidate.scores.topicContinuity > 0.0 && candidate.scores.topicContinuity < options.minTopicContinuityForLongCandidate &&
	    !usefulArc)
		return QStringLiteral("weak_topic_continuity");

	if (candidate.scores.final < options.minFinalScore && !usefulArc)
		return QStringLiteral("low_final_score");

	return {};
}

QVector<ClipCandidate> CandidateQualityGate::recoverFailsafeCandidates(QVector<ClipCandidate> candidates,
	const CandidateQualityGateOptions &options) const
{
	if (options.maxFailsafeRecoveredCandidates <= 0)
		return candidates;

	QVector<int> recoverableIndices;
	for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
		if (isFailsafeRecoverable(candidates.at(i), options) ||
		    isLastResortRecoverable(candidates.at(i), options) ||
		    isCollapsedRoleRecoverable(candidates.at(i), options))
			recoverableIndices.append(i);
	}

	std::sort(recoverableIndices.begin(), recoverableIndices.end(), [&](int leftIndex, int rightIndex) {
		const ClipCandidate &left = candidates.at(leftIndex);
		const ClipCandidate &right = candidates.at(rightIndex);
		const double leftConfidence = left.scores.final + (left.scores.rerankerRaw * 0.18) +
			(left.scores.rerankerClipQualityMargin * 0.12) + (left.scores.semanticEndingResolution * 0.08) +
			(left.scores.semanticEmpathy * 0.08) + (left.scores.semanticDirectAnswer * 0.05) +
			(left.scores.semanticViewerMessage * 0.05) - (left.scores.rerankerBadClip * 0.08) -
			(isSocialMetaDominated(left, options) ? 0.20 : 0.0) -
			(evidenceContainsFragment(left, QStringLiteral("coarse_noise_penalty")) ? 0.06 : 0.0);
		const double rightConfidence = right.scores.final + (right.scores.rerankerRaw * 0.18) +
			(right.scores.rerankerClipQualityMargin * 0.12) + (right.scores.semanticEndingResolution * 0.08) +
			(right.scores.semanticEmpathy * 0.08) + (right.scores.semanticDirectAnswer * 0.05) +
			(right.scores.semanticViewerMessage * 0.05) - (right.scores.rerankerBadClip * 0.08) -
			(isSocialMetaDominated(right, options) ? 0.20 : 0.0) -
			(evidenceContainsFragment(right, QStringLiteral("coarse_noise_penalty")) ? 0.06 : 0.0);
		if (std::fabs(leftConfidence - rightConfidence) > 0.0001)
			return leftConfidence > rightConfidence;
		return left.range.startSec < right.range.startSec;
	});

	const int recoverLimit = std::min(options.maxFailsafeRecoveredCandidates, static_cast<int>(recoverableIndices.size()));
	for (int i = 0; i < recoverLimit; ++i) {
		ClipCandidate &candidate = candidates[recoverableIndices.at(i)];
		const QString previousReason = candidate.rejectionReason;
		const bool collapsedRoleRecovery = isCollapsedRoleRecoverable(candidate, options);
		const bool lastResort = !isFailsafeRecoverable(candidate, options) &&
			isLastResortRecoverable(candidate, options);
		candidate.rejectedByQualityGate = false;
		candidate.rejectionReason.clear();
		candidate.scores.qualityGate = lastResort ? 0.58 : 0.72;
		candidate.evidence.append(QStringLiteral("quality_gate_failsafe_recovered"));
		if (lastResort)
			candidate.evidence.append(QStringLiteral("quality_gate_last_resort_safe_recovered"));
		if (collapsedRoleRecovery)
			candidate.evidence.append(QStringLiteral("quality_gate_collapsed_arc_roles_recovered"));
		if (isWeakConclusionReason(previousReason))
			candidate.evidence.append(QStringLiteral("quality_gate_weak_conclusion_failsafe_recovered"));
		if (!previousReason.trimmed().isEmpty())
			candidate.evidence.append(QStringLiteral("quality_gate_failsafe_recovered_from:%1").arg(previousReason.left(80)));
		candidate.evidence.removeDuplicates();
	}
	return candidates;
}

bool CandidateQualityGate::isFailsafeRecoverable(const ClipCandidate &candidate,
	const CandidateQualityGateOptions &options) const
{
	if (!candidate.rejectedByQualityGate)
		return false;
	if (durationSec(candidate) < options.minDurationSec || candidate.text.trimmed().size() < options.minTextChars)
		return false;
	if (candidate.rejectedAsNoise || candidate.scores.noise >= options.maxNoiseScore)
		return false;
	if (candidate.rejectionReason == QStringLiteral("reranker_failed") ||
	    candidate.rejectionReason == QStringLiteral("reranker_unavailable") ||
	    candidate.rejectionReason == QStringLiteral("too_short") ||
	    candidate.rejectionReason == QStringLiteral("not_enough_text") ||
	    candidate.rejectionReason == QStringLiteral("noise_or_stream_management") ||
	    candidate.rejectionReason == QStringLiteral("weak_social_opening") ||
	    candidate.rejectionReason == QStringLiteral("exchange_arc_degenerate_roles") ||
	    candidate.rejectionReason == QStringLiteral("overextended_tail_after_resolution"))
		return false;
	if (hasUnresolvedInternalPause(candidate, options) || hasDegenerateArcShape(candidate, options) ||
	    hasWeakSocialOpening(candidate, options) || hasOverextendedTail(candidate, options))
		return false;
	if (isSocialMetaDominated(candidate, options))
		return false;

	const double openingHook = std::max(candidate.scores.semanticOpeningHook, candidate.scores.arcOpening);
	const double endingResolution = std::max(candidate.scores.semanticEndingResolution,
		std::max(candidate.scores.semanticResolution, candidate.scores.arcConclusion));
	const bool targetBackedClipValue = candidate.hasReliableMainTarget && candidate.scores.semanticTarget >= 0.62 &&
		candidate.scores.semanticClipValue >= 0.60;
	const bool strongPositive = candidate.scores.final >= options.minFailsafeFinalScore &&
		(candidate.scores.semanticClipValue >= options.minFailsafeClipValue || targetBackedClipValue) &&
		openingHook >= options.minFailsafeOpeningHook &&
		endingResolution >= options.minFailsafeEndingResolution &&
		candidate.scores.rerankerRaw >= options.minStrongGoodRawForBadClipOverride &&
		candidate.scores.rerankerClipQualityMargin >= options.minFailsafeRerankerMargin;
	const bool weakConclusionOnly = isWeakConclusionReason(candidate.rejectionReason);
	const bool weakConclusionFailsafe = weakConclusionOnly && hasWeakConclusionFailsafe(candidate, options);
	if (!strongPositive && !weakConclusionFailsafe)
		return false;

	const bool openingWasTrimmed = hasEvidence(candidate, QStringLiteral("exchange_arc_opening_meta_trimmed")) ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_weak_opening_prelude_trimmed"));
	const bool rerankerSaysOpeningClean = !candidate.rerankerAvailable || candidate.scores.rerankerOpeningDefect <= 0.38 ||
		candidate.scores.rerankerClipQualityMargin >= 0.42;
	const bool semanticOpeningIsUseful = candidate.scores.semanticOpeningHook >= options.minFailsafeOpeningHook &&
		candidate.scores.semanticOpeningHook >= candidate.scores.semanticOpeningMetaNoise - 0.02;
	const bool arcOpeningTooWeak = candidate.scores.arcCompleteness > 0.0 &&
		candidate.scores.arcOpening < options.minFailsafeArcOpening && !openingWasTrimmed &&
		!(semanticOpeningIsUseful && rerankerSaysOpeningClean);
	const bool semanticOpeningStillMeta = candidate.semanticScoringAvailable &&
		candidate.scores.semanticOpeningMetaNoise >= options.maxOpeningMetaNoiseWhenAvailable &&
		candidate.scores.semanticOpeningHook <= candidate.scores.semanticOpeningMetaNoise + options.maxFailsafeOpeningMetaAdvantage &&
		!openingWasTrimmed;
	if ((arcOpeningTooWeak || semanticOpeningStillMeta) && !weakConclusionFailsafe)
		return false;

	const bool allDefectsHigh = candidate.scores.rerankerOpeningDefect >= 0.78 &&
		candidate.scores.rerankerEndingDefect >= 0.78 &&
		candidate.scores.rerankerStructureDefect >= 0.78;
	if (allDefectsHigh)
		return false;
	if (candidate.scores.rerankerBadClip > options.maxFailsafeDefectScore &&
	    candidate.scores.rerankerClipQualityMargin < 0.42)
		return false;
	if (candidate.scores.rerankerOpeningDefect > 0.68 &&
	    candidate.scores.semanticOpeningHook < candidate.scores.semanticOpeningMetaNoise + options.cleanBoundaryOpeningMargin)
		return false;
	if (candidate.scores.rerankerEndingDefect > 0.70 &&
	    candidate.scores.semanticEndingResolution <
		std::max(candidate.scores.semanticEndingMetaNoise, candidate.scores.semanticEndingTopicShift) + options.cleanBoundaryEndingMargin)
		return false;
	if (candidate.scores.rerankerStructureDefect > 0.70 && candidate.scores.arcCompleteness < options.minArcCompletenessForViewerPreset - 0.05)
		return false;
	if (candidate.scores.arcTailRisk > options.maxArcTailRiskForViewerPreset + 0.06)
		return false;

	// Weak-conclusion false negatives are the common zero-marker failure mode for long,
	// conversational videos. At this point we already verified that the candidate is not
	// text/noise invalid, has strong positive semantic/reranker support, does not have
	// all reranker defects high, and is not an obvious tail. Do not require the local
	// arc opening/conclusion scores to be clean again here; that would reintroduce the
	// same false negative this failsafe is meant to handle.
	if (weakConclusionFailsafe)
		return true;

	return hasCleanOpening(candidate, options) && (hasCleanEnding(candidate, options) || hasReliableConclusionFallback(candidate, options));
}

bool CandidateQualityGate::isLastResortRecoverable(const ClipCandidate &candidate,
	const CandidateQualityGateOptions &options) const
{
	// This path exists only to avoid an empty result set when the stricter arc
	// classifier under-detects conclusion on conversational material. It is not a
	// general soft recovery: hard invalid candidates, unresolved long pauses and
	// saturated reranker defects remain blocked.
	if (!candidate.rejectedByQualityGate || options.presetId != QStringLiteral("viewer_message_response"))
		return false;
	if (!isWeakConclusionReason(candidate.rejectionReason) &&
	    candidate.rejectionReason != QStringLiteral("weak_clip_value"))
		return false;
	if (durationSec(candidate) < options.minDurationSec || candidate.text.trimmed().size() < options.minTextChars)
		return false;
	if (candidate.rejectedAsNoise || candidate.scores.noise >= options.maxNoiseScore)
		return false;
	if (hasUnresolvedInternalPause(candidate, options) || hasDegenerateArcShape(candidate, options) ||
	    hasWeakSocialOpening(candidate, options) || hasOverextendedTail(candidate, options))
		return false;
	if (isSocialMetaDominated(candidate, options))
		return false;

	const bool allDefectsHigh = candidate.rerankerAvailable &&
		candidate.scores.rerankerOpeningDefect >= 0.74 &&
		candidate.scores.rerankerEndingDefect >= 0.74 &&
		candidate.scores.rerankerStructureDefect >= 0.74;
	if (allDefectsHigh)
		return false;
	if (candidate.rerankerAvailable && candidate.scores.rerankerStructureDefect > 0.58 &&
	    candidate.scores.rerankerClipQualityMargin < 0.36)
		return false;
	if (candidate.rerankerAvailable && candidate.scores.rerankerOpeningDefect > 0.64 &&
	    candidate.scores.semanticOpeningHook < candidate.scores.semanticOpeningMetaNoise + options.cleanBoundaryOpeningMargin)
		return false;
	if (candidate.rerankerAvailable && candidate.scores.rerankerEndingDefect > 0.72 &&
	    candidate.scores.rerankerClipQualityMargin < 0.36)
		return false;

	const double opening = std::max(candidate.scores.semanticOpeningHook, candidate.scores.semanticHook);
	const double resolution = std::max(candidate.scores.semanticEndingResolution, candidate.scores.semanticResolution);
	const double endingDefect = std::max(candidate.scores.semanticEndingMetaNoise, candidate.scores.semanticEndingTopicShift);
	const bool usefulOpening = opening >= 0.60 &&
		opening >= candidate.scores.semanticOpeningMetaNoise - 0.10;
	const bool usefulBody = candidate.scores.semanticClipValue >= 0.60 ||
		(candidate.hasReliableMainTarget && candidate.scores.semanticTarget >= 0.60 && candidate.scores.semanticClipValue >= 0.56);
	const bool usefulResolution = resolution >= 0.60 && resolution >= endingDefect - 0.14;
	const bool rerankerSupports = !candidate.rerankerAvailable ||
		(candidate.scores.rerankerRaw >= 0.94 && candidate.scores.rerankerClipQualityMargin >= 0.24);
	const bool tailNotObvious = candidate.scores.arcTailRisk <= options.maxArcTailRiskForViewerPreset + 0.04;
	const bool finalNotTiny = candidate.scores.final >= 0.46;

	return finalNotTiny && usefulOpening && usefulBody && usefulResolution && rerankerSupports && tailNotObvious;
}


bool CandidateQualityGate::isCollapsedRoleRecoverable(const ClipCandidate &candidate,
	const CandidateQualityGateOptions &options) const
{
	// WhisperX word-level cache can fragment the transcript enough for the cheap
	// role classifier to collapse into resolution-only/development-only. Recover
	// at most a couple of such candidates only when the independent semantic and
	// reranker signals indicate a useful human-context arc, while setup/social/meta
	// material remains blocked.
	if (!candidate.rejectedByQualityGate || options.presetId != QStringLiteral("viewer_message_response"))
		return false;
	if (candidate.rejectionReason != QStringLiteral("exchange_arc_degenerate_roles"))
		return false;
	if (!candidate.semanticScoringAvailable)
		return false;
	if (durationSec(candidate) < options.minDurationSec || candidate.text.trimmed().size() < options.minTextChars)
		return false;
	if (candidate.rejectedAsNoise || candidate.scores.noise >= options.maxNoiseScore)
		return false;
	if (hasUnresolvedInternalPause(candidate, options) || hasWeakSocialOpening(candidate, options) ||
	    hasOverextendedTail(candidate, options) || isSocialMetaDominated(candidate, options))
		return false;

	const double opening = std::max(candidate.scores.semanticOpeningHook, candidate.scores.semanticHook);
	const double resolution = std::max(candidate.scores.semanticEndingResolution, candidate.scores.semanticResolution);
	const double meta = std::max({candidate.scores.semanticMetaNoise, candidate.scores.semanticOpeningMetaNoise,
		candidate.scores.semanticEndingMetaNoise});
	const double endingDefect = std::max(candidate.scores.semanticEndingMetaNoise, candidate.scores.semanticEndingTopicShift);
	const double humanContext = std::max({candidate.scores.semanticDirectAnswer, candidate.scores.semanticViewerMessage,
		candidate.scores.semanticTarget, candidate.scores.semanticEmpathy});

	const bool usefulSemantics = candidate.scores.semanticClipValue >= 0.66 && opening >= 0.63 &&
		resolution >= 0.63 && candidate.scores.topicContinuity >= 0.62;
	const bool socialDoesNotDominate = candidate.scores.semanticClipValue >= meta - 0.005 &&
		opening >= candidate.scores.semanticOpeningMetaNoise - 0.015 &&
		resolution >= endingDefect - 0.06;
	const bool cleanRerankerOpeningBacked = candidate.rerankerAvailable &&
		candidate.scores.rerankerOpeningDefect <= 0.24 &&
		candidate.scores.rerankerClipQualityMargin >= 0.38 &&
		candidate.scores.semanticClipValue >= meta + 0.01;
	const bool humanBacked = humanContext >= 0.68 ||
		(candidate.scores.semanticEmpathy >= 0.66 && candidate.scores.semanticEmpathy >= meta + 0.03) ||
		cleanRerankerOpeningBacked;
	const bool rerankerSupports = !candidate.rerankerAvailable ||
		(candidate.scores.rerankerRaw >= 0.96 && candidate.scores.rerankerClipQualityMargin >= 0.34 &&
		 candidate.scores.rerankerStructureDefect <= 0.62 && candidate.scores.rerankerOpeningDefect <= 0.62 &&
		 candidate.scores.rerankerEndingDefect <= 0.68);
	const bool notAllRerankerDefectsHigh = !candidate.rerankerAvailable ||
		!(candidate.scores.rerankerOpeningDefect >= 0.74 && candidate.scores.rerankerEndingDefect >= 0.74 &&
		  candidate.scores.rerankerStructureDefect >= 0.74);
	const bool notCoarseNoiseUnlessStrongHuman = !evidenceContainsFragment(candidate, QStringLiteral("coarse_noise_penalty")) ||
		humanContext >= 0.72 || candidate.scores.semanticClipValue >= meta + 0.045;

	return usefulSemantics && socialDoesNotDominate && humanBacked && rerankerSupports &&
		notAllRerankerDefectsHigh && notCoarseNoiseUnlessStrongHuman &&
		candidate.scores.final >= 0.50;
}

bool CandidateQualityGate::hasReliableConclusionFallback(const ClipCandidate &candidate,
	const CandidateQualityGateOptions &options) const
{
	if (!candidate.semanticScoringAvailable)
		return false;
	const double semanticResolution = std::max(candidate.scores.semanticEndingResolution, candidate.scores.semanticResolution);
	const double endingDefect = std::max(candidate.scores.semanticEndingMetaNoise, candidate.scores.semanticEndingTopicShift);
	const bool semanticConclusion = semanticResolution >= options.minFailsafeEndingResolution - 0.03 &&
		semanticResolution >= endingDefect - 0.08;
	const bool rerankerAllowsEnding = !candidate.rerankerAvailable ||
		candidate.scores.rerankerEndingDefect <= 0.60 ||
		candidate.scores.rerankerClipQualityMargin >= options.minFailsafeRerankerMargin ||
		(candidate.scores.rerankerRaw >= 0.96 && candidate.scores.rerankerClipQualityMargin >= 0.36 &&
		 candidate.scores.rerankerStructureDefect <= 0.58);
	const bool notObviousTail = candidate.scores.arcTailRisk <= options.maxArcTailRiskForViewerPreset + 0.06;
	return semanticConclusion && rerankerAllowsEnding && notObviousTail;
}

bool CandidateQualityGate::hasWeakConclusionFailsafe(const ClipCandidate &candidate,
	const CandidateQualityGateOptions &options) const
{
	if (options.presetId != QStringLiteral("viewer_message_response") || !candidate.semanticScoringAvailable)
		return false;
	if (isSocialMetaDominated(candidate, options))
		return false;

	const double openingHook = std::max(candidate.scores.semanticOpeningHook, candidate.scores.semanticHook);
	const double resolution = std::max(candidate.scores.semanticEndingResolution, candidate.scores.semanticResolution);
	const double endingDefect = std::max(candidate.scores.semanticEndingMetaNoise, candidate.scores.semanticEndingTopicShift);
	const bool targetBackedClipValue = candidate.hasReliableMainTarget && candidate.scores.semanticTarget >= 0.60 &&
		candidate.scores.semanticClipValue >= 0.58;
	const bool empathyBacked = isEmpathyBacked(candidate, options);
	const bool usefulBody = candidate.scores.semanticClipValue >= 0.62 || targetBackedClipValue || empathyBacked;
	const bool usefulOpening = openingHook >= (empathyBacked ? 0.57 : 0.60) &&
		openingHook >= candidate.scores.semanticOpeningMetaNoise - (empathyBacked ? 0.12 : 0.05) &&
		(!candidate.rerankerAvailable || candidate.scores.rerankerOpeningDefect <= 0.68 ||
		 candidate.scores.rerankerClipQualityMargin >= 0.38);
	const bool usefulConclusion = resolution >= (empathyBacked ? 0.58 : 0.61) &&
		resolution >= endingDefect - (empathyBacked ? 0.14 : 0.10) &&
		(!candidate.rerankerAvailable || candidate.scores.rerankerEndingDefect <= 0.64 ||
		 candidate.scores.rerankerClipQualityMargin >= 0.36);
	const bool rerankerStrong = !candidate.rerankerAvailable ||
		(candidate.scores.rerankerRaw >= 0.96 && candidate.scores.rerankerClipQualityMargin >= 0.34);
	const bool structureNotBad = !candidate.rerankerAvailable || candidate.scores.rerankerStructureDefect <= 0.62 ||
		candidate.scores.rerankerClipQualityMargin >= 0.42;
	const bool notTail = candidate.scores.arcTailRisk <= options.maxArcTailRiskForViewerPreset + 0.08;
	const bool noUnresolvedPause = !hasUnresolvedInternalPause(candidate, options);
	const bool notAllDefectsHigh = !candidate.rerankerAvailable ||
		!(candidate.scores.rerankerOpeningDefect >= 0.78 && candidate.scores.rerankerEndingDefect >= 0.78 &&
		  candidate.scores.rerankerStructureDefect >= 0.78);

	return usefulBody && usefulOpening && usefulConclusion && rerankerStrong && structureNotBad && notTail &&
		noUnresolvedPause && notAllDefectsHigh;
}

bool CandidateQualityGate::hasSemanticBackedArcFallback(const ClipCandidate &candidate,
	const CandidateQualityGateOptions &options) const
{
	if (options.presetId != QStringLiteral("viewer_message_response") || !candidate.semanticScoringAvailable)
		return false;
	if (isSocialMetaDominated(candidate, options) || hasDegenerateArcShape(candidate, options) ||
	    hasWeakSocialOpening(candidate, options) || hasOverextendedTail(candidate, options))
		return false;
	if (hasUnresolvedInternalPause(candidate, options))
		return false;

	const bool openingWasTrimmed = hasEvidence(candidate, QStringLiteral("exchange_arc_opening_meta_trimmed")) ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_weak_opening_prelude_trimmed"));
	const bool endingWasTrimmed = hasEvidence(candidate, QStringLiteral("exchange_arc_hard_topic_break_trimmed")) ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_pause_tail_trimmed")) ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_first_resolution_tail_trimmed"));
	if (candidate.scores.arcCompleteness > 0.0) {
		const bool openingArcTooWeak = candidate.scores.arcOpening < options.strictSemanticFallbackMinArcOpening &&
			!openingWasTrimmed;
		const bool boundaryArcTooWeak = candidate.scores.arcBoundaryCleanliness <
			options.strictSemanticFallbackMinArcCleanliness && !openingWasTrimmed && !endingWasTrimmed;
		const bool conclusionArcTooWeak = candidate.scores.arcConclusion < options.minArcConclusionForViewerPreset - 0.08 &&
			candidate.scores.rerankerEndingDefect >= 0.62 && !endingWasTrimmed;
		if (openingArcTooWeak || boundaryArcTooWeak || conclusionArcTooWeak)
			return false;
	}

	const double openingHook = std::max(candidate.scores.semanticOpeningHook, candidate.scores.semanticHook);
	const double endingResolution = std::max(candidate.scores.semanticEndingResolution, candidate.scores.semanticResolution);
	const double endingDefect = std::max(candidate.scores.semanticEndingMetaNoise, candidate.scores.semanticEndingTopicShift);
	const bool openingLooksClean = openingHook >= options.minOpeningHookForViewerPreset &&
		(candidate.scores.semanticOpeningMetaNoise <= options.maxOpeningMetaNoiseWhenAvailable + 0.04 ||
		 openingHook >= candidate.scores.semanticOpeningMetaNoise - 0.02 ||
		 candidate.scores.rerankerOpeningDefect <= 0.38);
	const bool endingLooksComplete = endingResolution >= options.minEndingResolutionForViewerPreset &&
		(endingResolution >= endingDefect - 0.04 || candidate.scores.rerankerEndingDefect <= 0.58 ||
		 candidate.scores.rerankerClipQualityMargin >= options.minFailsafeRerankerMargin);
	const bool bodyLooksUseful = candidate.scores.semanticClipValue >= 0.66 ||
		candidate.scores.semanticEmpathy >= options.minEmpathyArcScore ||
		(candidate.scores.semanticClipValue >= 0.60 && candidate.scores.semanticTarget >= 0.62 && candidate.hasReliableMainTarget);
	const bool rerankerSupports = !candidate.rerankerAvailable ||
		(candidate.scores.rerankerRaw >= 0.92 && candidate.scores.rerankerClipQualityMargin >= 0.18);
	const bool structuralDefectAcceptable = !candidate.rerankerAvailable || candidate.scores.rerankerStructureDefect <= 0.72 ||
		candidate.scores.rerankerClipQualityMargin >= 0.42;
	const bool notObviousTail = candidate.scores.arcTailRisk <= options.maxArcTailRiskForViewerPreset + 0.06;
	return openingLooksClean && endingLooksComplete && bodyLooksUseful && rerankerSupports &&
		structuralDefectAcceptable && notObviousTail;
}


bool CandidateQualityGate::hasCuriosityArcFallback(const ClipCandidate &candidate,
	const CandidateQualityGateOptions &options) const
{
	// Viewer-message mode is the default live/chat preset, but not every valuable
	// live clip is explicit Q&A. Accept a focused curiosity/story/opinion arc when
	// the semantic signals show a self-contained hook, body and local payoff, while
	// still blocking meta/social openings, unresolved pause tails and saturated
	// reranker defects. This turns good non-Q&A moments into first-class candidates
	// instead of only rescuing them through the last-resort recovery path.
	if (options.presetId != QStringLiteral("viewer_message_response") || !candidate.semanticScoringAvailable)
		return false;
	if (hasUnresolvedInternalPause(candidate, options) || hasDegenerateArcShape(candidate, options) ||
	    hasWeakSocialOpening(candidate, options) || hasOverextendedTail(candidate, options))
		return false;

	const double opening = std::max(candidate.scores.semanticOpeningHook, candidate.scores.semanticHook);
	const double resolution = std::max(candidate.scores.semanticEndingResolution, candidate.scores.semanticResolution);
	const double endingDefect = std::max(candidate.scores.semanticEndingMetaNoise, candidate.scores.semanticEndingTopicShift);
	const double openingMeta = std::max(candidate.scores.semanticOpeningMetaNoise, candidate.scores.semanticMetaNoise * 0.90);
	const bool empathyBacked = isEmpathyBacked(candidate, options);
	const bool meaningfulHook = opening >= (empathyBacked ? 0.57 : 0.61) &&
		opening >= openingMeta - (empathyBacked ? 0.12 : 0.02);
	const bool meaningfulBody = (candidate.scores.semanticClipValue >= 0.62 || empathyBacked) &&
		(candidate.scores.topicContinuity <= 0.0 || candidate.scores.topicContinuity >= 0.60);
	const bool meaningfulPayoff = resolution >= (empathyBacked ? 0.58 : 0.61) &&
		resolution >= endingDefect - (empathyBacked ? 0.14 : 0.08);
	const bool arcNotObviouslyBroken = candidate.scores.arcTailRisk <= options.maxArcTailRiskForViewerPreset + 0.04 &&
		candidate.scores.arcBoundaryCleanliness >= 0.34 && candidate.scores.arcDevelopment >= 0.48;
	const bool rerankerAllows = !candidate.rerankerAvailable ||
		(candidate.scores.rerankerRaw >= 0.94 && candidate.scores.rerankerClipQualityMargin >= 0.24 &&
		 candidate.scores.rerankerStructureDefect <= 0.58 && candidate.scores.rerankerOpeningDefect <= 0.58 &&
		 candidate.scores.rerankerEndingDefect <= 0.72);
	const bool notMostlyMeta = !isSocialMetaDominated(candidate, options) &&
		(candidate.scores.semanticMetaNoise <= 0.72 ||
		 candidate.scores.semanticClipValue >= candidate.scores.semanticMetaNoise + (empathyBacked ? -0.04 : 0.01));
	const bool notAllDefectsHigh = !candidate.rerankerAvailable ||
		!(candidate.scores.rerankerOpeningDefect >= 0.74 && candidate.scores.rerankerEndingDefect >= 0.74 &&
		  candidate.scores.rerankerStructureDefect >= 0.74);

	return meaningfulHook && meaningfulBody && meaningfulPayoff && arcNotObviouslyBroken && rerankerAllows &&
		notMostlyMeta && notAllDefectsHigh && candidate.scores.final >= 0.46;
}

bool CandidateQualityGate::hasUsefulExchangeArc(const ClipCandidate &candidate,
	const CandidateQualityGateOptions &options) const
{
	if (hasSemanticBackedArcFallback(candidate, options) || hasCuriosityArcFallback(candidate, options))
		return true;
	if (candidate.scores.arcCompleteness <= 0.0)
		return false;
	return candidate.scores.arcOpening >= options.minArcOpeningForViewerPreset &&
		candidate.scores.arcDevelopment >= options.minArcDevelopmentForViewerPreset &&
		(candidate.scores.arcConclusion >= options.minArcConclusionForViewerPreset ||
		 hasReliableConclusionFallback(candidate, options)) &&
		candidate.scores.arcBoundaryCleanliness >= options.minArcBoundaryCleanlinessForViewerPreset &&
		candidate.scores.arcCompleteness >= options.minArcCompletenessForViewerPreset &&
		candidate.scores.arcTailRisk <= options.maxArcTailRiskForViewerPreset;
}

bool CandidateQualityGate::hasCleanOpening(const ClipCandidate &candidate,
	const CandidateQualityGateOptions &options) const
{
	const bool openingWasTrimmed = hasEvidence(candidate, QStringLiteral("exchange_arc_opening_meta_trimmed")) ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_weak_opening_prelude_trimmed"));
	if (candidate.scores.arcCompleteness > 0.0) {
		const bool arcClean = candidate.scores.arcOpening >= options.minArcOpeningForViewerPreset &&
			candidate.scores.arcBoundaryCleanliness >= options.minArcBoundaryCleanlinessForViewerPreset;
		if (!arcClean)
			return false;
		if (candidate.semanticScoringAvailable &&
		    candidate.scores.semanticOpeningMetaNoise >= options.maxOpeningMetaNoiseWhenAvailable &&
		    candidate.scores.semanticOpeningMetaNoise >= candidate.scores.semanticOpeningHook + options.hardSemanticBoundaryDefectMargin &&
		    !openingWasTrimmed)
			return false;
		return true;
	}
	if (!candidate.semanticScoringAvailable)
		return true;
	return candidate.scores.semanticOpeningHook + options.hardSemanticBoundaryDefectMargin >=
		candidate.scores.semanticOpeningMetaNoise ||
		candidate.scores.semanticOpeningMetaNoise <= options.maxOpeningMetaNoiseWhenAvailable - 0.08 || openingWasTrimmed;
}

bool CandidateQualityGate::hasCleanEnding(const ClipCandidate &candidate,
	const CandidateQualityGateOptions &options) const
{
	const bool endingWasTrimmed = hasEvidence(candidate, QStringLiteral("exchange_arc_hard_topic_break_trimmed")) ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_pause_tail_trimmed")) ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_first_resolution_tail_trimmed"));
	if (candidate.scores.arcCompleteness > 0.0)
		return (candidate.scores.arcConclusion >= options.minArcConclusionForViewerPreset ||
			hasReliableConclusionFallback(candidate, options) || endingWasTrimmed) &&
			candidate.scores.arcBoundaryCleanliness >= options.minArcBoundaryCleanlinessForViewerPreset &&
			candidate.scores.arcTailRisk <= options.maxArcTailRiskForViewerPreset;
	if (!candidate.semanticScoringAvailable)
		return true;
	const bool resolutionBeatsNoise = candidate.scores.semanticEndingResolution + options.hardSemanticBoundaryDefectMargin >=
		std::max(candidate.scores.semanticEndingMetaNoise, candidate.scores.semanticEndingTopicShift);
	const bool lowNoise = candidate.scores.semanticEndingMetaNoise <= options.maxEndingMetaNoiseWhenAvailable - 0.08 &&
		candidate.scores.semanticEndingTopicShift <= options.maxEndingTopicShiftWhenAvailable - 0.08;
	return resolutionBeatsNoise || lowNoise ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_hard_topic_break_trimmed")) ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_pause_tail_trimmed")) ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_first_resolution_tail_trimmed"));
}

bool CandidateQualityGate::hasUntrimmedPauseTail(const ClipCandidate &candidate,
	const CandidateQualityGateOptions &options) const
{
	if (options.presetId != QStringLiteral("viewer_message_response") ||
	    candidate.scores.maxInternalPauseSec < options.probableTopicBreakPauseSec)
		return false;
	if (hasEvidence(candidate, QStringLiteral("exchange_arc_pause_tail_trimmed")) ||
	    hasEvidence(candidate, QStringLiteral("exchange_arc_hard_topic_break_trimmed")) ||
	    hasEvidence(candidate, QStringLiteral("exchange_arc_first_resolution_tail_trimmed")))
		return false;
	if (hasUnresolvedInternalPause(candidate, options))
		return true;
	if (candidate.scores.arcCompleteness > 0.0)
		return candidate.scores.arcTailRisk > options.maxArcTailRiskForViewerPreset - 0.08;
	return !hasCleanEnding(candidate, options);
}

bool CandidateQualityGate::hasUnresolvedInternalPause(const ClipCandidate &candidate,
	const CandidateQualityGateOptions &options) const
{
	if (options.presetId != QStringLiteral("viewer_message_response"))
		return false;
	if (candidate.scores.maxInternalPauseSec < options.unresolvedInternalPauseSec)
		return false;
	if (hasEvidence(candidate, QStringLiteral("exchange_arc_pause_tail_trimmed")) ||
	    hasEvidence(candidate, QStringLiteral("exchange_arc_hard_topic_break_trimmed")) ||
	    hasEvidence(candidate, QStringLiteral("exchange_arc_first_resolution_tail_trimmed")))
		return false;

	const bool extendedAcrossTail = hasEvidence(candidate, QStringLiteral("exchange_arc_tail_continuation"));
	const bool weakArcShape = candidate.scores.arcCompleteness > 0.0 &&
		(candidate.scores.arcOpening < options.minArcOpeningForViewerPreset + 0.08 ||
		 candidate.scores.arcConclusion < options.minArcConclusionForViewerPreset ||
		 candidate.scores.arcBoundaryCleanliness < options.minArcBoundaryCleanlinessForViewerPreset + 0.06);
	const bool endingNotClearlyResolved = candidate.semanticScoringAvailable &&
		candidate.scores.semanticEndingResolution <
			std::max(candidate.scores.semanticEndingMetaNoise, candidate.scores.semanticEndingTopicShift) +
			options.cleanBoundaryEndingMargin;
	return extendedAcrossTail || weakArcShape || endingNotClearlyResolved;
}

bool CandidateQualityGate::hasDegenerateArcShape(const ClipCandidate &candidate,
	const CandidateQualityGateOptions &options) const
{
	if (options.presetId != QStringLiteral("viewer_message_response") ||
	    candidate.scores.arcCompleteness <= 0.0)
		return false;

	// The local arc-role classifier is intentionally cheap and can collapse when
	// WhisperX word-aligned segments are shorter/more fragmented. Treat a flat
	// "all resolution" or "all development" role sequence as a hard defect only
	// when the independent semantic/reranker signals do not support a complete
	// hook/body/payoff arc. Otherwise the gate should keep the candidate and let
	// the semantic/reranker score order it.
	const bool semanticBackedCollapsedRoles = semanticSupportsCollapsedArcRoles(candidate);
	const bool resolutionOnly = hasResolutionOnlyArcRoles(candidate) && !semanticBackedCollapsedRoles;
	const bool developmentOnlyWithoutPayoff = hasDevelopmentOnlyArcRoles(candidate) &&
		!semanticBackedCollapsedRoles &&
		candidate.scores.arcConclusion < options.minArcConclusionForViewerPreset - 0.06 &&
		candidate.scores.semanticEndingResolution < options.minEndingResolutionForViewerPreset + 0.04;
	const bool hookOnlyWithoutBody = candidate.scores.semanticOpeningHook >= 0.68 &&
		candidate.scores.arcDevelopment < options.minArcDevelopmentForViewerPreset + 0.04 &&
		candidate.scores.arcConclusion < options.minArcConclusionForViewerPreset + 0.02 &&
		!semanticBackedCollapsedRoles;

	return resolutionOnly || developmentOnlyWithoutPayoff || hookOnlyWithoutBody;
}

bool CandidateQualityGate::hasWeakSocialOpening(const ClipCandidate &candidate,
	const CandidateQualityGateOptions &options) const
{
	if (options.presetId != QStringLiteral("viewer_message_response") || !candidate.semanticScoringAvailable)
		return false;

	const bool openingWasTrimmed = hasEvidence(candidate, QStringLiteral("exchange_arc_opening_meta_trimmed")) ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_weak_opening_prelude_trimmed"));
	const double opening = std::max(candidate.scores.semanticOpeningHook, candidate.scores.semanticHook);
	const double openingMeta = std::max(candidate.scores.semanticOpeningMetaNoise, candidate.scores.semanticMetaNoise * 0.92);
	const bool weakArcOpening = candidate.scores.arcCompleteness > 0.0 &&
		candidate.scores.arcOpening < options.minArcOpeningForViewerPreset + 0.12;
	const bool socialCompetesWithHook = openingMeta >= opening - 0.02 ||
		(candidate.scores.semanticMetaNoise >= candidate.scores.semanticClipValue - 0.01 &&
		 candidate.scores.semanticDirectAnswer < 0.62 && candidate.scores.semanticViewerMessage < 0.62);
	const bool notClearlyHumanValue = !isEmpathyBacked(candidate, options) &&
		candidate.scores.semanticTarget < 0.66 && candidate.scores.semanticDirectAnswer < 0.66;

	return !openingWasTrimmed && weakArcOpening && socialCompetesWithHook && notClearlyHumanValue;
}

bool CandidateQualityGate::hasOverextendedTail(const ClipCandidate &candidate,
	const CandidateQualityGateOptions &options) const
{
	if (options.presetId != QStringLiteral("viewer_message_response") || !candidate.semanticScoringAvailable)
		return false;
	if (hasEvidence(candidate, QStringLiteral("exchange_arc_first_resolution_tail_trimmed")) ||
	    hasEvidence(candidate, QStringLiteral("exchange_arc_hard_topic_break_trimmed")) ||
	    hasEvidence(candidate, QStringLiteral("exchange_arc_pause_tail_trimmed")))
		return false;

	const double candidateDurationSec = durationSec(candidate);
	const double endingDefect = std::max(candidate.scores.semanticEndingMetaNoise,
		candidate.scores.semanticEndingTopicShift);
	const bool hasLongTailSignal = candidate.scores.pauseAfterSec >= 6.0 ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_tail_continuation")) ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_pause_tail_blocked"));
	const bool endingNotClearlyBetterThanTail = endingDefect >= candidate.scores.semanticEndingResolution - 0.06 ||
		candidate.scores.arcTailRisk >= options.maxArcTailRiskForViewerPreset - 0.46;
	return candidateDurationSec >= 30.0 && hasLongTailSignal && endingNotClearlyBetterThanTail;
}

bool CandidateQualityGate::isEmpathyBacked(const ClipCandidate &candidate,
	const CandidateQualityGateOptions &options) const
{
	if (!candidate.semanticScoringAvailable || candidate.scores.semanticEmpathy < options.minEmpathyArcScore)
		return false;

	const double meta = std::max({candidate.scores.semanticMetaNoise, candidate.scores.semanticOpeningMetaNoise,
		candidate.scores.semanticEndingMetaNoise});
	const bool answerOrViewerContext = candidate.scores.semanticDirectAnswer >= 0.64 ||
		candidate.scores.semanticViewerMessage >= 0.64 || candidate.scores.semanticTarget >= 0.64;
	const bool empathyClearlyBeatsMeta = candidate.scores.semanticEmpathy >= meta + 0.08 &&
		candidate.scores.semanticClipValue >= candidate.scores.semanticMetaNoise + 0.03;
	const bool boundariesNotSocial = candidate.scores.semanticOpeningMetaNoise <=
		std::max(candidate.scores.semanticOpeningHook, candidate.scores.semanticHook) +
		(answerOrViewerContext ? 0.03 : -0.01) &&
		candidate.scores.semanticEndingMetaNoise <=
		std::max(candidate.scores.semanticEndingResolution, candidate.scores.semanticResolution) + 0.05;
	return boundariesNotSocial && (answerOrViewerContext || empathyClearlyBeatsMeta);
}

bool CandidateQualityGate::isSocialMetaDominated(const ClipCandidate &candidate,
	const CandidateQualityGateOptions &options) const
{
	if (!candidate.semanticScoringAvailable)
		return false;

	const bool empathyBacked = isEmpathyBacked(candidate, options);
	const double opening = std::max(candidate.scores.semanticOpeningHook, candidate.scores.semanticHook);
	const double resolution = std::max(candidate.scores.semanticEndingResolution, candidate.scores.semanticResolution);
	const double tolerance = empathyBacked ? 0.07 : options.maxSocialMetaDominanceWithoutEmpathy;
	const bool metaCompetesWithOpening = candidate.scores.semanticOpeningMetaNoise >= opening - tolerance;
	const bool metaCompetesWithBody = candidate.scores.semanticMetaNoise >=
		candidate.scores.semanticClipValue - tolerance;
	const bool endingLooksMeta = candidate.scores.semanticEndingMetaNoise >= resolution - 0.05 ||
		candidate.scores.semanticEndingMetaNoise >= options.maxEndingMetaNoiseWhenAvailable + 0.02;
	const bool weakHumanValue = candidate.scores.semanticDirectAnswer < 0.68 &&
		candidate.scores.semanticTarget < 0.68;

	const bool empathyClearlyWins = empathyBacked && candidate.scores.semanticEmpathy >=
		std::max(candidate.scores.semanticMetaNoise, candidate.scores.semanticOpeningMetaNoise) + 0.04 &&
		candidate.scores.semanticEndingMetaNoise < resolution + 0.08;
	return !empathyClearlyWins && metaCompetesWithOpening && metaCompetesWithBody &&
		(endingLooksMeta || weakHumanValue);
}

double CandidateQualityGate::semanticPositiveScore(const ClipCandidate &candidate) const
{
	return boundedScore(std::max({candidate.scores.semanticClipValue, candidate.scores.semanticEmpathy,
		candidate.scores.semanticHook,
		candidate.scores.semanticOpeningHook, candidate.scores.semanticResolution,
		candidate.scores.semanticEndingResolution, candidate.scores.topicContinuity,
		candidate.scores.arcCompleteness}));
}

double CandidateQualityGate::semanticNegativeScore(const ClipCandidate &candidate) const
{
	return boundedScore(std::max({candidate.scores.semanticNoise, candidate.scores.semanticMetaNoise,
		candidate.scores.semanticOpeningMetaNoise, candidate.scores.semanticEndingMetaNoise,
		candidate.scores.semanticEndingTopicShift, candidate.scores.arcTailRisk}));
}
