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
	double maxRerankerBadClipWhenAvailable = 0.64;
	double minRerankerGoodBadMarginWhenAvailable = 0.18;
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
};

class CandidateQualityGate {
public:
	QVector<ClipCandidate> apply(QVector<ClipCandidate> candidates, const CandidateQualityGateOptions &options) const;
	ClipCandidate apply(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;

private:
	QString rejectionReason(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;
	bool hasStrongPositiveSemanticEvidence(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;
	bool hasStrongContrastSemanticEvidence(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;
	bool hasSafeBorderlineSemanticEvidence(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;
	double semanticValueMargin(const ClipCandidate &candidate) const;
};

} // namespace Curation::Scoring
