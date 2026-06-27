#include "curation/scoring/candidate-quality-gate.hpp"
#include "curation/scoring/candidate-quality-rules.hpp"

#include <algorithm>
#include <cmath>

using namespace Curation::Scoring;
using namespace Curation::Scoring::CandidateQualityRules;

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
		const double leftConfidence =
			left.scores.final + (left.scores.rerankerRaw * 0.18) +
			(left.scores.rerankerClipQualityMargin * 0.12) + (left.scores.semanticEndingResolution * 0.08) +
			(left.scores.semanticEmpathy * 0.08) + (left.scores.semanticDirectAnswer * 0.05) +
			(left.scores.semanticViewerMessage * 0.05) - (left.scores.rerankerBadClip * 0.08) -
			(isSocialMetaDominated(left, options) ? 0.20 : 0.0) -
			(evidenceContainsFragment(left, QStringLiteral("coarse_noise_penalty")) ? 0.06 : 0.0);
		const double rightConfidence =
			right.scores.final + (right.scores.rerankerRaw * 0.18) +
			(right.scores.rerankerClipQualityMargin * 0.12) +
			(right.scores.semanticEndingResolution * 0.08) + (right.scores.semanticEmpathy * 0.08) +
			(right.scores.semanticDirectAnswer * 0.05) + (right.scores.semanticViewerMessage * 0.05) -
			(right.scores.rerankerBadClip * 0.08) - (isSocialMetaDominated(right, options) ? 0.20 : 0.0) -
			(evidenceContainsFragment(right, QStringLiteral("coarse_noise_penalty")) ? 0.06 : 0.0);
		if (std::fabs(leftConfidence - rightConfidence) > 0.0001)
			return leftConfidence > rightConfidence;
		return left.range.startSec < right.range.startSec;
	});

	const int recoverLimit =
		std::min(options.maxFailsafeRecoveredCandidates, static_cast<int>(recoverableIndices.size()));
	for (int i = 0; i < recoverLimit; ++i) {
		ClipCandidate &candidate = candidates[recoverableIndices.at(i)];
		const QString previousReason = candidate.rejectionReason;
		const bool collapsedRoleRecovery = isCollapsedRoleRecoverable(candidate, options);
		const bool lastResort = !collapsedRoleRecovery && !isFailsafeRecoverable(candidate, options) &&
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
			candidate.evidence.append(
				QStringLiteral("quality_gate_failsafe_recovered_from:%1").arg(previousReason.left(80)));
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
	if (requiresStrictContextualArc(options) && !hasCompleteContextualArc(candidate))
		return false;
	if (candidate.rejectionReason == QStringLiteral("reranker_failed") ||
	    candidate.rejectionReason == QStringLiteral("reranker_unavailable") ||
	    candidate.rejectionReason == QStringLiteral("too_short") ||
	    candidate.rejectionReason == QStringLiteral("not_enough_text") ||
	    candidate.rejectionReason == QStringLiteral("noise_or_stream_management") ||
	    candidate.rejectionReason == QStringLiteral("weak_social_opening") ||
	    candidate.rejectionReason == QStringLiteral("exchange_arc_degenerate_roles") ||
	    candidate.rejectionReason == QStringLiteral("missing_contextual_arc") ||
	    candidate.rejectionReason == QStringLiteral("invalid_contextual_arc") ||
	    candidate.rejectionReason == QStringLiteral("implicit_opening_not_allowed") ||
	    candidate.rejectionReason == QStringLiteral("resolution_plateau_without_opening") ||
	    candidate.rejectionReason == QStringLiteral("semantic_target_not_specific") ||
	    candidate.rejectionReason == QStringLiteral("semantic_target_mismatch") ||
	    candidate.rejectionReason == QStringLiteral("overlong_viewer_arc") ||
	    candidate.rejectionReason == QStringLiteral("overextended_tail_after_resolution"))
		return false;
	if (hasUnresolvedInternalPause(candidate, options) || hasDegenerateArcShape(candidate, options) ||
	    hasWeakSocialOpening(candidate, options) || hasOverextendedTail(candidate, options))
		return false;
	if (isSocialMetaDominated(candidate, options))
		return false;

	const double openingHook = std::max(candidate.scores.semanticOpeningHook, candidate.scores.arcOpening);
	const double endingResolution =
		std::max(candidate.scores.semanticEndingResolution,
			 std::max(candidate.scores.semanticResolution, candidate.scores.arcConclusion));
	const bool targetBackedClipValue = candidate.hasReliableMainTarget && candidate.scores.semanticTarget >= 0.62 &&
					   candidate.scores.semanticClipValue >= 0.60;
	const bool strongPositive =
		candidate.scores.final >= options.minFailsafeFinalScore &&
		(candidate.scores.semanticClipValue >= options.minFailsafeClipValue || targetBackedClipValue) &&
		openingHook >= options.minFailsafeOpeningHook &&
		endingResolution >= options.minFailsafeEndingResolution &&
		candidate.scores.rerankerRaw >= options.minStrongGoodRawForBadClipOverride &&
		candidate.scores.rerankerClipQualityMargin >= options.minFailsafeRerankerMargin;
	const bool weakConclusionOnly = isWeakConclusionReason(candidate.rejectionReason);
	const bool weakConclusionFailsafe = weakConclusionOnly && hasWeakConclusionFailsafe(candidate, options);
	if (!strongPositive && !weakConclusionFailsafe)
		return false;

	const bool openingWasTrimmed =
		hasEvidence(candidate, QStringLiteral("exchange_arc_opening_meta_trimmed")) ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_weak_opening_prelude_trimmed"));
	const bool rerankerSaysOpeningClean = !candidate.rerankerAvailable ||
					      candidate.scores.rerankerOpeningDefect <= 0.38 ||
					      candidate.scores.rerankerClipQualityMargin >= 0.42;
	const bool semanticOpeningIsUseful = candidate.scores.semanticOpeningHook >= options.minFailsafeOpeningHook &&
					     candidate.scores.semanticOpeningHook >=
						     candidate.scores.semanticOpeningMetaNoise - 0.02;
	const bool arcOpeningTooWeak = candidate.scores.arcCompleteness > 0.0 &&
				       candidate.scores.arcOpening < options.minFailsafeArcOpening &&
				       !openingWasTrimmed && !(semanticOpeningIsUseful && rerankerSaysOpeningClean);
	const bool semanticOpeningStillMeta =
		candidate.semanticScoringAvailable &&
		candidate.scores.semanticOpeningMetaNoise >= options.maxOpeningMetaNoiseWhenAvailable &&
		candidate.scores.semanticOpeningHook <=
			candidate.scores.semanticOpeningMetaNoise + options.maxFailsafeOpeningMetaAdvantage &&
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
	    candidate.scores.semanticOpeningHook <
		    candidate.scores.semanticOpeningMetaNoise + options.cleanBoundaryOpeningMargin)
		return false;
	if (candidate.scores.rerankerEndingDefect > 0.70 &&
	    candidate.scores.semanticEndingResolution <
		    std::max(candidate.scores.semanticEndingMetaNoise, candidate.scores.semanticEndingTopicShift) +
			    options.cleanBoundaryEndingMargin)
		return false;
	if (candidate.scores.rerankerStructureDefect > 0.70 &&
	    candidate.scores.arcCompleteness < options.minArcCompletenessForViewerPreset - 0.05)
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

	return hasCleanOpening(candidate, options) &&
	       (hasCleanEnding(candidate, options) || hasReliableConclusionSupport(candidate, options));
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
	if (!hasCompleteContextualArc(candidate))
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

	const bool allDefectsHigh = candidate.rerankerAvailable && candidate.scores.rerankerOpeningDefect >= 0.74 &&
				    candidate.scores.rerankerEndingDefect >= 0.74 &&
				    candidate.scores.rerankerStructureDefect >= 0.74;
	if (allDefectsHigh)
		return false;
	if (candidate.rerankerAvailable && candidate.scores.rerankerStructureDefect > 0.58 &&
	    candidate.scores.rerankerClipQualityMargin < 0.36)
		return false;
	if (candidate.rerankerAvailable && candidate.scores.rerankerOpeningDefect > 0.64 &&
	    candidate.scores.semanticOpeningHook <
		    candidate.scores.semanticOpeningMetaNoise + options.cleanBoundaryOpeningMargin)
		return false;
	if (candidate.rerankerAvailable && candidate.scores.rerankerEndingDefect > 0.72 &&
	    candidate.scores.rerankerClipQualityMargin < 0.36)
		return false;

	const double opening = std::max(candidate.scores.semanticOpeningHook, candidate.scores.semanticHook);
	const double resolution =
		std::max(candidate.scores.semanticEndingResolution, candidate.scores.semanticResolution);
	const double endingDefect =
		std::max(candidate.scores.semanticEndingMetaNoise, candidate.scores.semanticEndingTopicShift);
	const bool usefulOpening = opening >= 0.60 && opening >= candidate.scores.semanticOpeningMetaNoise - 0.10;
	const bool usefulBody = candidate.scores.semanticClipValue >= 0.60 ||
				(candidate.hasReliableMainTarget && candidate.scores.semanticTarget >= 0.60 &&
				 candidate.scores.semanticClipValue >= 0.56);
	const bool usefulResolution = resolution >= 0.60 && resolution >= endingDefect - 0.14;
	const bool rerankerSupports =
		!candidate.rerankerAvailable ||
		(candidate.scores.rerankerRaw >= 0.94 && candidate.scores.rerankerClipQualityMargin >= 0.24);
	const bool tailNotObvious = candidate.scores.arcTailRisk <= options.maxArcTailRiskForViewerPreset + 0.04;
	const bool finalNotTiny = candidate.scores.final >= 0.46;

	return finalNotTiny && usefulOpening && usefulBody && usefulResolution && rerankerSupports && tailNotObvious;
}

bool CandidateQualityGate::isCollapsedRoleRecoverable(const ClipCandidate &candidate,
						      const CandidateQualityGateOptions &options) const
{
	// WhisperX word-level cache can fragment the transcript enough for the cheap
	// role classifier to collapse. Recover collapsed roles only when independent
	// semantic/reranker signals indicate a useful human-context arc. Resolution-only
	// spans remain blocked unless the boundary refiner produced a strong repaired
	// opening/body/payoff signal; otherwise they are usually mid-answer clips.
	if (!candidate.rejectedByQualityGate || options.presetId != QStringLiteral("viewer_message_response"))
		return false;
	if (!hasCompleteContextualArc(candidate))
		return false;
	if (candidate.rejectionReason != QStringLiteral("exchange_arc_degenerate_roles"))
		return false;
	if (hasResolutionOnlyArcRoles(candidate) && !hasStrongRepairedResolutionOnlyArc(candidate))
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
	const double resolution =
		std::max(candidate.scores.semanticEndingResolution, candidate.scores.semanticResolution);
	const double meta = std::max({candidate.scores.semanticMetaNoise, candidate.scores.semanticOpeningMetaNoise,
				      candidate.scores.semanticEndingMetaNoise});
	const double endingDefect =
		std::max(candidate.scores.semanticEndingMetaNoise, candidate.scores.semanticEndingTopicShift);
	const double humanContext =
		std::max({candidate.scores.semanticDirectAnswer, candidate.scores.semanticViewerMessage,
			  candidate.scores.semanticTarget, candidate.scores.semanticEmpathy});

	const bool usefulSemantics = candidate.scores.semanticClipValue >= 0.66 && opening >= 0.63 &&
				     resolution >= 0.63 && candidate.scores.topicContinuity >= 0.62;
	const bool socialDoesNotDominate = candidate.scores.semanticClipValue >= meta - 0.005 &&
					   opening >= candidate.scores.semanticOpeningMetaNoise - 0.015 &&
					   resolution >= endingDefect - 0.06;
	const bool cleanRerankerOpeningBacked =
		candidate.rerankerAvailable && candidate.scores.rerankerOpeningDefect <= 0.24 &&
		candidate.scores.rerankerClipQualityMargin >= 0.38 && candidate.scores.semanticClipValue >= meta + 0.01;
	const bool humanBacked =
		humanContext >= 0.68 ||
		(candidate.scores.semanticEmpathy >= 0.66 && candidate.scores.semanticEmpathy >= meta + 0.03) ||
		cleanRerankerOpeningBacked;
	const bool rerankerSupports =
		!candidate.rerankerAvailable ||
		(candidate.scores.rerankerRaw >= 0.96 && candidate.scores.rerankerClipQualityMargin >= 0.34 &&
		 candidate.scores.rerankerStructureDefect <= 0.62 && candidate.scores.rerankerOpeningDefect <= 0.62 &&
		 candidate.scores.rerankerEndingDefect <= 0.68);
	const bool notAllRerankerDefectsHigh =
		!candidate.rerankerAvailable ||
		!(candidate.scores.rerankerOpeningDefect >= 0.74 && candidate.scores.rerankerEndingDefect >= 0.74 &&
		  candidate.scores.rerankerStructureDefect >= 0.74);
	const bool notCoarseNoiseUnlessStrongHuman =
		!evidenceContainsFragment(candidate, QStringLiteral("coarse_noise_penalty")) || humanContext >= 0.72 ||
		candidate.scores.semanticClipValue >= meta + 0.045;

	return usefulSemantics && socialDoesNotDominate && humanBacked && rerankerSupports &&
	       notAllRerankerDefectsHigh && notCoarseNoiseUnlessStrongHuman && candidate.scores.final >= 0.50;
}
