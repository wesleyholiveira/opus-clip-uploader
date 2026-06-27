#include "curation/scoring/candidate-quality-gate.hpp"
#include "curation/scoring/candidate-quality-rules.hpp"

#include <algorithm>

using namespace Curation::Scoring;
using namespace Curation::Scoring::CandidateQualityRules;

bool CandidateQualityGate::hasReliableConclusionSupport(const ClipCandidate &candidate,
							const CandidateQualityGateOptions &options) const
{
	if (!candidate.semanticScoringAvailable)
		return false;
	if (hasImplicitContextualOpening(candidate) || hasResolutionPlateauWithoutOpening(candidate))
		return false;
	const double semanticResolution =
		std::max(candidate.scores.semanticEndingResolution, candidate.scores.semanticResolution);
	const double endingDefect =
		std::max(candidate.scores.semanticEndingMetaNoise, candidate.scores.semanticEndingTopicShift);
	const bool semanticConclusion = semanticResolution >= options.minFailsafeEndingResolution - 0.03 &&
					semanticResolution >= endingDefect - 0.08;
	const bool rerankerAllowsEnding =
		!candidate.rerankerAvailable || candidate.scores.rerankerEndingDefect <= 0.60 ||
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
	if (hasImplicitContextualOpening(candidate) || hasResolutionPlateauWithoutOpening(candidate))
		return false;
	if (options.reliableMainTarget && !options.mainTarget.trimmed().isEmpty() &&
	    (candidate.scores.arcCompleteness <= 0.0 || !hasStrictReliableTargetSupport(candidate, options)))
		return false;
	if (isSocialMetaDominated(candidate, options))
		return false;

	const double openingHook = std::max(candidate.scores.semanticOpeningHook, candidate.scores.semanticHook);
	const double resolution =
		std::max(candidate.scores.semanticEndingResolution, candidate.scores.semanticResolution);
	const double endingDefect =
		std::max(candidate.scores.semanticEndingMetaNoise, candidate.scores.semanticEndingTopicShift);
	const bool targetBackedClipValue = candidate.hasReliableMainTarget && candidate.scores.semanticTarget >= 0.60 &&
					   candidate.scores.semanticClipValue >= 0.58;
	const bool empathyBacked = isEmpathyBacked(candidate, options);
	const bool usefulBody = candidate.scores.semanticClipValue >= 0.62 || targetBackedClipValue || empathyBacked;
	const bool usefulOpening = openingHook >= (empathyBacked ? 0.57 : 0.60) &&
				   openingHook >=
					   candidate.scores.semanticOpeningMetaNoise - (empathyBacked ? 0.12 : 0.05) &&
				   (!candidate.rerankerAvailable || candidate.scores.rerankerOpeningDefect <= 0.68 ||
				    candidate.scores.rerankerClipQualityMargin >= 0.38);
	const bool usefulConclusion = resolution >= (empathyBacked ? 0.58 : 0.61) &&
				      resolution >= endingDefect - (empathyBacked ? 0.14 : 0.10) &&
				      (!candidate.rerankerAvailable || candidate.scores.rerankerEndingDefect <= 0.64 ||
				       candidate.scores.rerankerClipQualityMargin >= 0.36);
	const bool rerankerStrong =
		!candidate.rerankerAvailable ||
		(candidate.scores.rerankerRaw >= 0.96 && candidate.scores.rerankerClipQualityMargin >= 0.34);
	const bool structureNotBad = !candidate.rerankerAvailable || candidate.scores.rerankerStructureDefect <= 0.62 ||
				     candidate.scores.rerankerClipQualityMargin >= 0.42;
	const bool notTail = candidate.scores.arcTailRisk <= options.maxArcTailRiskForViewerPreset + 0.08;
	const bool noUnresolvedPause = !hasUnresolvedInternalPause(candidate, options);
	const bool notAllDefectsHigh =
		!candidate.rerankerAvailable ||
		!(candidate.scores.rerankerOpeningDefect >= 0.78 && candidate.scores.rerankerEndingDefect >= 0.78 &&
		  candidate.scores.rerankerStructureDefect >= 0.78);

	return usefulBody && usefulOpening && usefulConclusion && rerankerStrong && structureNotBad && notTail &&
	       noUnresolvedPause && notAllDefectsHigh;
}

bool CandidateQualityGate::hasSemanticBackedArcSupport(const ClipCandidate &candidate,
						       const CandidateQualityGateOptions &options) const
{
	if (options.presetId != QStringLiteral("viewer_message_response") || !candidate.semanticScoringAvailable)
		return false;
	if (candidate.scores.arcCompleteness <= 0.0)
		return false;
	if (hasImplicitContextualOpening(candidate) || hasResolutionPlateauWithoutOpening(candidate))
		return false;
	if (options.reliableMainTarget && !options.mainTarget.trimmed().isEmpty() &&
	    (!hasValidContextualStateMachine(candidate) || !hasStrictReliableTargetSupport(candidate, options)))
		return false;
	if (isSocialMetaDominated(candidate, options) || hasDegenerateArcShape(candidate, options) ||
	    hasWeakSocialOpening(candidate, options) || hasOverextendedTail(candidate, options))
		return false;
	if (hasUnresolvedInternalPause(candidate, options))
		return false;

	const bool openingWasTrimmed =
		hasEvidence(candidate, QStringLiteral("exchange_arc_opening_meta_trimmed")) ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_weak_opening_prelude_trimmed"));
	const bool endingWasTrimmed =
		hasEvidence(candidate, QStringLiteral("exchange_arc_hard_topic_break_trimmed")) ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_pause_tail_trimmed")) ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_first_resolution_tail_trimmed"));
	if (candidate.scores.arcCompleteness > 0.0) {
		const bool openingArcTooWeak =
			candidate.scores.arcOpening < options.strictSemanticFallbackMinArcOpening && !openingWasTrimmed;
		const bool boundaryArcTooWeak = candidate.scores.arcBoundaryCleanliness <
							options.strictSemanticFallbackMinArcCleanliness &&
						!openingWasTrimmed && !endingWasTrimmed;
		const bool conclusionArcTooWeak = candidate.scores.arcConclusion <
							  options.minArcConclusionForViewerPreset - 0.08 &&
						  candidate.scores.rerankerEndingDefect >= 0.62 && !endingWasTrimmed;
		if (openingArcTooWeak || boundaryArcTooWeak || conclusionArcTooWeak)
			return false;
	}

	const double openingHook = std::max(candidate.scores.semanticOpeningHook, candidate.scores.semanticHook);
	const double endingResolution =
		std::max(candidate.scores.semanticEndingResolution, candidate.scores.semanticResolution);
	const double endingDefect =
		std::max(candidate.scores.semanticEndingMetaNoise, candidate.scores.semanticEndingTopicShift);
	const bool openingLooksClean =
		openingHook >= options.minOpeningHookForViewerPreset &&
		(candidate.scores.semanticOpeningMetaNoise <= options.maxOpeningMetaNoiseWhenAvailable + 0.04 ||
		 openingHook >= candidate.scores.semanticOpeningMetaNoise - 0.02 ||
		 candidate.scores.rerankerOpeningDefect <= 0.38);
	const bool endingLooksComplete =
		endingResolution >= options.minEndingResolutionForViewerPreset &&
		(endingResolution >= endingDefect - 0.04 || candidate.scores.rerankerEndingDefect <= 0.58 ||
		 candidate.scores.rerankerClipQualityMargin >= options.minFailsafeRerankerMargin);
	const bool bodyLooksUseful = candidate.scores.semanticClipValue >= 0.66 ||
				     candidate.scores.semanticEmpathy >= options.minEmpathyArcScore ||
				     (candidate.scores.semanticClipValue >= 0.60 &&
				      candidate.scores.semanticTarget >= 0.62 && candidate.hasReliableMainTarget);
	const bool rerankerSupports =
		!candidate.rerankerAvailable ||
		(candidate.scores.rerankerRaw >= 0.92 && candidate.scores.rerankerClipQualityMargin >= 0.18);
	const bool structuralDefectAcceptable = !candidate.rerankerAvailable ||
						candidate.scores.rerankerStructureDefect <= 0.72 ||
						candidate.scores.rerankerClipQualityMargin >= 0.42;
	const bool notObviousTail = candidate.scores.arcTailRisk <= options.maxArcTailRiskForViewerPreset + 0.06;
	return openingLooksClean && endingLooksComplete && bodyLooksUseful && rerankerSupports &&
	       structuralDefectAcceptable && notObviousTail;
}

bool CandidateQualityGate::hasCuriosityArcSupport(const ClipCandidate &candidate,
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
	if (options.reliableMainTarget && !options.mainTarget.trimmed().isEmpty())
		return false;
	if (candidate.scores.arcCompleteness <= 0.0)
		return false;
	if (hasImplicitContextualOpening(candidate) || hasResolutionPlateauWithoutOpening(candidate))
		return false;
	if (hasUnresolvedInternalPause(candidate, options) || hasDegenerateArcShape(candidate, options) ||
	    hasWeakSocialOpening(candidate, options) || hasOverextendedTail(candidate, options))
		return false;

	const double opening = std::max(candidate.scores.semanticOpeningHook, candidate.scores.semanticHook);
	const double resolution =
		std::max(candidate.scores.semanticEndingResolution, candidate.scores.semanticResolution);
	const double endingDefect =
		std::max(candidate.scores.semanticEndingMetaNoise, candidate.scores.semanticEndingTopicShift);
	const double openingMeta =
		std::max(candidate.scores.semanticOpeningMetaNoise, candidate.scores.semanticMetaNoise * 0.90);
	const bool empathyBacked = isEmpathyBacked(candidate, options);
	const bool meaningfulHook = opening >= (empathyBacked ? 0.57 : 0.61) &&
				    opening >= openingMeta - (empathyBacked ? 0.12 : 0.02);
	const bool meaningfulBody =
		(candidate.scores.semanticClipValue >= 0.62 || empathyBacked) &&
		(candidate.scores.topicContinuity <= 0.0 || candidate.scores.topicContinuity >= 0.60);
	const bool meaningfulPayoff = resolution >= (empathyBacked ? 0.58 : 0.61) &&
				      resolution >= endingDefect - (empathyBacked ? 0.14 : 0.08);
	const bool arcNotObviouslyBroken =
		candidate.scores.arcTailRisk <= options.maxArcTailRiskForViewerPreset + 0.04 &&
		candidate.scores.arcBoundaryCleanliness >= 0.34 && candidate.scores.arcDevelopment >= 0.48;
	const bool rerankerAllows =
		!candidate.rerankerAvailable ||
		(candidate.scores.rerankerRaw >= 0.94 && candidate.scores.rerankerClipQualityMargin >= 0.24 &&
		 candidate.scores.rerankerStructureDefect <= 0.58 && candidate.scores.rerankerOpeningDefect <= 0.58 &&
		 candidate.scores.rerankerEndingDefect <= 0.72);
	const bool notMostlyMeta = !isSocialMetaDominated(candidate, options) &&
				   (candidate.scores.semanticMetaNoise <= 0.72 ||
				    candidate.scores.semanticClipValue >=
					    candidate.scores.semanticMetaNoise + (empathyBacked ? -0.04 : 0.01));
	const bool notAllDefectsHigh =
		!candidate.rerankerAvailable ||
		!(candidate.scores.rerankerOpeningDefect >= 0.74 && candidate.scores.rerankerEndingDefect >= 0.74 &&
		  candidate.scores.rerankerStructureDefect >= 0.74);

	return meaningfulHook && meaningfulBody && meaningfulPayoff && arcNotObviouslyBroken && rerankerAllows &&
	       notMostlyMeta && notAllDefectsHigh && candidate.scores.final >= 0.46;
}

bool CandidateQualityGate::hasUsefulExchangeArc(const ClipCandidate &candidate,
						const CandidateQualityGateOptions &options) const
{
	if (candidate.scores.arcCompleteness <= 0.0)
		return false;
	if (hasInvalidContextualStateMachine(candidate) || hasImplicitContextualOpening(candidate) ||
	    hasResolutionPlateauWithoutOpening(candidate))
		return false;
	if (hasSemanticBackedArcSupport(candidate, options) || hasCuriosityArcSupport(candidate, options))
		return true;
	return candidate.scores.arcOpening >= options.minArcOpeningForViewerPreset &&
	       candidate.scores.arcDevelopment >= options.minArcDevelopmentForViewerPreset &&
	       (candidate.scores.arcConclusion >= options.minArcConclusionForViewerPreset ||
		hasReliableConclusionSupport(candidate, options)) &&
	       candidate.scores.arcBoundaryCleanliness >= options.minArcBoundaryCleanlinessForViewerPreset &&
	       candidate.scores.arcCompleteness >= options.minArcCompletenessForViewerPreset &&
	       candidate.scores.arcTailRisk <= options.maxArcTailRiskForViewerPreset;
}
