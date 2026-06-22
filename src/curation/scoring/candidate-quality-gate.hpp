#pragma once

#include "curation/scoring/clip-candidate.hpp"

#include <QString>
#include <QVector>

namespace Curation::Scoring {

struct CandidateQualityGateOptions {
	QString presetId;
	QString mainTarget;
	bool reliableMainTarget = false;
	double minDurationSec = 12.0;
	int minTextChars = 40;
	double minFinalScore = 0.34;
	double maxNoiseScore = 0.82;
	double minSemanticTargetWhenAvailable = 0.54;
	double minViewerResponseForViewerPreset = 0.32;
	double minClipValueForViewerPreset = 0.70;
	double minHookForViewerPreset = 0.58;
	double minOpeningHookForViewerPreset = 0.60;
	double minResolutionForViewerPreset = 0.58;
	double minEndingResolutionForViewerPreset = 0.58;
	double minTopicContinuityForViewerPreset = 0.58;
	double maxSemanticMetaNoiseWhenAvailable = 0.70;
	double hardMaxSemanticMetaNoiseWhenAvailable = 0.80;
	double maxSemanticNoiseWhenAvailable = 0.76;
	double maxOpeningMetaNoiseWhenAvailable = 0.66;
	double maxEndingMetaNoiseWhenAvailable = 0.70;
	double maxEndingTopicShiftWhenAvailable = 0.68;
	double hardOpeningMetaNoiseWhenAvailable = 0.80;
	double hardEndingMetaNoiseWhenAvailable = 0.82;
	double hardSemanticBoundaryDefectMargin = 0.055;
	double maxRerankerBadClipWhenAvailable = 0.64;
	double maxHardRerankerBadClipWhenAvailable = 0.92;
	double minRerankerGoodBadMarginWhenAvailable = 0.18;
	double minStrongGoodRawForBadClipOverride = 0.90;
	double minStrongSemanticValueForBadClipOverride = 0.66;
	double minSemanticMarginForBadClipOverride = 0.08;
	double semanticNoiseMarginTolerance = 0.02;
	double strongPositiveRerankerRaw = 0.90;
	double strongPositiveClipValue = 0.74;
	double strongPositiveHook = 0.60;
	double strongPositiveResolution = 0.58;
	double strongContrastRerankerRaw = 0.82;
	double strongContrastMaxBadClip = 0.40;
	double strongContrastMinMargin = 0.50;
	double strongContrastMinFinal = 0.60;
	double strongContrastMinClipValue = 0.65;
	double strongContrastMinHook = 0.60;
	double strongContrastMinResolution = 0.62;
	double strongContrastMinBoundary = 0.50;
	double strongContrastHardMaxOpeningMetaNoise = 0.78;
	double strongContrastHardMaxEndingMetaNoise = 0.80;
	double strongContrastHardMaxEndingTopicShift = 0.80;
	bool requireSemanticTargetWhenAvailable = true;
	bool requireRerankerWhenAvailable = false;
	bool rejectInvalidRerankerWhenRequired = true;
	double minRerankerScoreWhenAvailable = 0.66;
	double minRerankerRawScoreWhenAvailable = 0.72;
	double minStrongRerankerRawScoreWhenAvailable = 0.86;
	double minConditionalRerankerRawScoreWhenAvailable = 0.76;
	double minConditionalSemanticTargetWhenAvailable = 0.72;
	double minConditionalBoundaryWhenAvailable = 0.90;
	double longCandidateDurationSec = 35.0;
	double minTopicContinuityForLongCandidate = 0.58;
	double probableTopicBreakPauseSec = 2.0;
	double cleanBoundaryOpeningMargin = 0.025;
	double cleanBoundaryEndingMargin = 0.015;
	double minArcOpeningForViewerPreset = 0.30;
	double minArcDevelopmentForViewerPreset = 0.38;
	double minArcConclusionForViewerPreset = 0.30;
	double minArcCompletenessForViewerPreset = 0.40;
	double minArcBoundaryCleanlinessForViewerPreset = 0.34;
	double maxArcTailRiskForViewerPreset = 0.72;
	double minFailsafeFinalScore = 0.50;
	double minFailsafeClipValue = 0.66;
	double minFailsafeOpeningHook = 0.62;
	double minFailsafeEndingResolution = 0.64;
	double minFailsafeArcOpening = 0.48;
	double maxFailsafeOpeningMetaAdvantage = 0.015;
	double maxFailsafeDefectScore = 0.62;
	double unresolvedInternalPauseSec = 4.0;
	double strictSemanticFallbackMinArcOpening = 0.38;
	double strictSemanticFallbackMinArcCleanliness = 0.40;
	double minFailsafeRerankerMargin = 0.28;
	double minEmpathyArcScore = 0.60;
	double maxSocialMetaDominanceWithoutEmpathy = 0.015;
	int maxFailsafeRecoveredCandidates = 2;
	int maxSoftRecoveredCandidates = 0;
};

class CandidateQualityGate {
public:
	QVector<ClipCandidate> apply(QVector<ClipCandidate> candidates, const CandidateQualityGateOptions &options) const;
	ClipCandidate apply(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;

private:
	QString rejectionReason(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;
	QVector<ClipCandidate> recoverFailsafeCandidates(QVector<ClipCandidate> candidates, const CandidateQualityGateOptions &options) const;
	bool isFailsafeRecoverable(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;
	bool isLastResortRecoverable(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;
	bool isCollapsedRoleRecoverable(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;
	bool hasReliableConclusionFallback(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;
	bool hasWeakConclusionFailsafe(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;
	bool hasSemanticBackedArcFallback(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;
	bool hasCuriosityArcFallback(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;
	bool hasUsefulExchangeArc(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;
	bool hasCleanOpening(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;
	bool hasCleanEnding(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;
	bool hasUntrimmedPauseTail(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;
	bool hasUnresolvedInternalPause(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;
	bool hasDegenerateArcShape(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;
	bool hasWeakSocialOpening(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;
	bool hasOverextendedTail(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;
	bool isEmpathyBacked(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;
	bool isSocialMetaDominated(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;
	double semanticPositiveScore(const ClipCandidate &candidate) const;
	double semanticNegativeScore(const ClipCandidate &candidate) const;
};

} // namespace Curation::Scoring
