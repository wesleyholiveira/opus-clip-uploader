#include "curation/scoring/candidate-quality-gate.hpp"
#include "curation/scoring/candidate-quality-rules.hpp"

#include <algorithm>

using namespace Curation::Scoring;
using namespace Curation::Scoring::CandidateQualityRules;

bool CandidateQualityGate::hasCleanOpening(const ClipCandidate &candidate,
					   const CandidateQualityGateOptions &options) const
{
	const bool openingWasTrimmed =
		hasEvidence(candidate, QStringLiteral("exchange_arc_opening_meta_trimmed")) ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_weak_opening_prelude_trimmed"));
	if (candidate.scores.arcCompleteness > 0.0) {
		const bool arcClean = candidate.scores.arcOpening >= options.minArcOpeningForViewerPreset &&
				      candidate.scores.arcBoundaryCleanliness >=
					      options.minArcBoundaryCleanlinessForViewerPreset;
		if (!arcClean)
			return false;
		if (candidate.semanticScoringAvailable &&
		    candidate.scores.semanticOpeningMetaNoise >= options.maxOpeningMetaNoiseWhenAvailable &&
		    candidate.scores.semanticOpeningMetaNoise >=
			    candidate.scores.semanticOpeningHook + options.hardSemanticBoundaryDefectMargin &&
		    !openingWasTrimmed)
			return false;
		return true;
	}
	if (!candidate.semanticScoringAvailable)
		return true;
	return candidate.scores.semanticOpeningHook + options.hardSemanticBoundaryDefectMargin >=
		       candidate.scores.semanticOpeningMetaNoise ||
	       candidate.scores.semanticOpeningMetaNoise <= options.maxOpeningMetaNoiseWhenAvailable - 0.08 ||
	       openingWasTrimmed;
}

bool CandidateQualityGate::hasCleanEnding(const ClipCandidate &candidate,
					  const CandidateQualityGateOptions &options) const
{
	const bool endingWasTrimmed =
		hasEvidence(candidate, QStringLiteral("exchange_arc_hard_topic_break_trimmed")) ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_pause_tail_trimmed")) ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_first_resolution_tail_trimmed"));
	if (candidate.scores.arcCompleteness > 0.0)
		return (candidate.scores.arcConclusion >= options.minArcConclusionForViewerPreset ||
			hasReliableConclusionSupport(candidate, options) || endingWasTrimmed) &&
		       candidate.scores.arcBoundaryCleanliness >= options.minArcBoundaryCleanlinessForViewerPreset &&
		       candidate.scores.arcTailRisk <= options.maxArcTailRiskForViewerPreset;
	if (!candidate.semanticScoringAvailable)
		return true;
	const bool resolutionBeatsNoise =
		candidate.scores.semanticEndingResolution + options.hardSemanticBoundaryDefectMargin >=
		std::max(candidate.scores.semanticEndingMetaNoise, candidate.scores.semanticEndingTopicShift);
	const bool lowNoise =
		candidate.scores.semanticEndingMetaNoise <= options.maxEndingMetaNoiseWhenAvailable - 0.08 &&
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
	const bool weakArcShape =
		candidate.scores.arcCompleteness > 0.0 &&
		(candidate.scores.arcOpening < options.minArcOpeningForViewerPreset + 0.08 ||
		 candidate.scores.arcConclusion < options.minArcConclusionForViewerPreset ||
		 candidate.scores.arcBoundaryCleanliness < options.minArcBoundaryCleanlinessForViewerPreset + 0.06);
	const bool endingNotClearlyResolved =
		candidate.semanticScoringAvailable &&
		candidate.scores.semanticEndingResolution <
			std::max(candidate.scores.semanticEndingMetaNoise, candidate.scores.semanticEndingTopicShift) +
				options.cleanBoundaryEndingMargin;
	return extendedAcrossTail || weakArcShape || endingNotClearlyResolved;
}

bool CandidateQualityGate::hasDegenerateArcShape(const ClipCandidate &candidate,
						 const CandidateQualityGateOptions &options) const
{
	if (options.presetId != QStringLiteral("viewer_message_response") || candidate.scores.arcCompleteness <= 0.0)
		return false;
	if (hasInvalidContextualStateMachine(candidate) || hasImplicitContextualOpening(candidate) ||
	    hasResolutionPlateauWithoutOpening(candidate))
		return true;

	// The local arc-role classifier is intentionally cheap and can collapse when
	// WhisperX word-aligned segments are shorter/more fragmented. A flat
	// "all resolution" sequence is not a complete viewer-message arc: it usually
	// means the clip starts inside the answer and lost the viewer setup/context.
	// Allow semantic/reranker support only for development-only cases.
	const bool semanticBackedCollapsedRoles = semanticSupportsCollapsedArcRoles(candidate);
	const bool resolutionOnly = hasResolutionOnlyArcRoles(candidate);
	const bool repairedResolutionOnly = resolutionOnly && hasStrongRepairedResolutionOnlyArc(candidate);
	const bool developmentOnlyWithoutPayoff =
		hasDevelopmentOnlyArcRoles(candidate) && !semanticBackedCollapsedRoles &&
		candidate.scores.arcConclusion < options.minArcConclusionForViewerPreset - 0.06 &&
		candidate.scores.semanticEndingResolution < options.minEndingResolutionForViewerPreset + 0.04;
	const bool hookOnlyWithoutBody =
		candidate.scores.semanticOpeningHook >= 0.68 &&
		candidate.scores.arcDevelopment < options.minArcDevelopmentForViewerPreset + 0.04 &&
		candidate.scores.arcConclusion < options.minArcConclusionForViewerPreset + 0.02 &&
		!semanticBackedCollapsedRoles;

	const bool startsWithPriorOrMeta = arcRolesStartWith(candidate, QStringLiteral("previous_conclusion")) ||
					   arcRolesStartWith(candidate, QStringLiteral("meta_prelude")) ||
					   arcRolesStartWith(candidate, QStringLiteral("tail_or_new_turn"));

	return startsWithPriorOrMeta || (resolutionOnly && !repairedResolutionOnly) || developmentOnlyWithoutPayoff ||
	       hookOnlyWithoutBody;
}

bool CandidateQualityGate::hasWeakSocialOpening(const ClipCandidate &candidate,
						const CandidateQualityGateOptions &options) const
{
	if (options.presetId != QStringLiteral("viewer_message_response") || !candidate.semanticScoringAvailable)
		return false;

	const bool openingWasTrimmed =
		hasEvidence(candidate, QStringLiteral("exchange_arc_opening_meta_trimmed")) ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_weak_opening_prelude_trimmed"));
	const double opening = std::max(candidate.scores.semanticOpeningHook, candidate.scores.semanticHook);
	const double openingMeta =
		std::max(candidate.scores.semanticOpeningMetaNoise, candidate.scores.semanticMetaNoise * 0.92);
	const bool weakArcOpening = candidate.scores.arcCompleteness > 0.0 &&
				    candidate.scores.arcOpening < options.minArcOpeningForViewerPreset + 0.12;
	const bool socialCompetesWithHook =
		openingMeta >= opening - 0.02 ||
		(candidate.scores.semanticMetaNoise >= candidate.scores.semanticClipValue - 0.01 &&
		 candidate.scores.semanticDirectAnswer < 0.62 && candidate.scores.semanticViewerMessage < 0.62);
	const bool notClearlyHumanValue = !isEmpathyBacked(candidate, options) &&
					  candidate.scores.semanticTarget < 0.66 &&
					  candidate.scores.semanticDirectAnswer < 0.66;

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
	const double endingDefect =
		std::max(candidate.scores.semanticEndingMetaNoise, candidate.scores.semanticEndingTopicShift);
	const bool hasLongTailSignal = candidate.scores.pauseAfterSec >= 6.0 ||
				       hasEvidence(candidate, QStringLiteral("exchange_arc_tail_continuation")) ||
				       hasEvidence(candidate, QStringLiteral("exchange_arc_pause_tail_blocked"));
	const bool endingNotClearlyBetterThanTail = endingDefect >= candidate.scores.semanticEndingResolution - 0.06 ||
						    candidate.scores.arcTailRisk >=
							    options.maxArcTailRiskForViewerPreset - 0.46;
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
					   candidate.scores.semanticViewerMessage >= 0.64 ||
					   candidate.scores.semanticTarget >= 0.64;
	const bool empathyClearlyBeatsMeta = candidate.scores.semanticEmpathy >= meta + 0.08 &&
					     candidate.scores.semanticClipValue >=
						     candidate.scores.semanticMetaNoise + 0.03;
	const bool boundariesNotSocial =
		candidate.scores.semanticOpeningMetaNoise <=
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
	const double resolution =
		std::max(candidate.scores.semanticEndingResolution, candidate.scores.semanticResolution);
	const double tolerance = empathyBacked ? 0.07 : options.maxSocialMetaDominanceWithoutEmpathy;
	const bool metaCompetesWithOpening = candidate.scores.semanticOpeningMetaNoise >= opening - tolerance;
	const bool metaCompetesWithBody = candidate.scores.semanticMetaNoise >=
					  candidate.scores.semanticClipValue - tolerance;
	const bool endingLooksMeta = candidate.scores.semanticEndingMetaNoise >= resolution - 0.05 ||
				     candidate.scores.semanticEndingMetaNoise >=
					     options.maxEndingMetaNoiseWhenAvailable + 0.02;
	const bool weakHumanValue = candidate.scores.semanticDirectAnswer < 0.68 &&
				    candidate.scores.semanticTarget < 0.68;

	const bool empathyClearlyWins =
		empathyBacked &&
		candidate.scores.semanticEmpathy >=
			std::max(candidate.scores.semanticMetaNoise, candidate.scores.semanticOpeningMetaNoise) +
				0.04 &&
		candidate.scores.semanticEndingMetaNoise < resolution + 0.08;
	return !empathyClearlyWins && metaCompetesWithOpening && metaCompetesWithBody &&
	       (endingLooksMeta || weakHumanValue);
}

double CandidateQualityGate::semanticPositiveScore(const ClipCandidate &candidate) const
{
	return boundedScore(std::max({candidate.scores.semanticClipValue, candidate.scores.semanticEmpathy,
				      candidate.scores.semanticHook, candidate.scores.semanticOpeningHook,
				      candidate.scores.semanticResolution, candidate.scores.semanticEndingResolution,
				      candidate.scores.topicContinuity, candidate.scores.arcCompleteness}));
}

double CandidateQualityGate::semanticNegativeScore(const ClipCandidate &candidate) const
{
	return boundedScore(
		std::max({candidate.scores.semanticNoise, candidate.scores.semanticMetaNoise,
			  candidate.scores.semanticOpeningMetaNoise, candidate.scores.semanticEndingMetaNoise,
			  candidate.scores.semanticEndingTopicShift, candidate.scores.arcTailRisk}));
}
