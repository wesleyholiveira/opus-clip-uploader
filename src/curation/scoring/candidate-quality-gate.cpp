#include "curation/scoring/candidate-quality-gate.hpp"

#include <algorithm>
#include <cmath>

using namespace Curation::Scoring;

QVector<ClipCandidate> CandidateQualityGate::apply(QVector<ClipCandidate> candidates,
						   const CandidateQualityGateOptions &options) const
{
	for (ClipCandidate &candidate : candidates)
		candidate = apply(candidate, options);
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
		if (hasStrongContrastSemanticEvidence(gated, options))
			gated.evidence.append(QStringLiteral("quality_gate_strong_contrast"));
		if (hasSafeBorderlineSemanticEvidence(gated, options))
			gated.evidence.append(QStringLiteral("quality_gate_safe_borderline"));
	}

	gated.evidence.removeDuplicates();
	return gated;
}

QString CandidateQualityGate::rejectionReason(const ClipCandidate &candidate,
						      const CandidateQualityGateOptions &options) const
{
	const double durationSec = candidate.range.endSec - candidate.range.startSec;
	if (durationSec < options.minDurationSec)
		return QStringLiteral("too_short");
	if (candidate.text.trimmed().size() < options.minTextChars)
		return QStringLiteral("not_enough_text");
	if (candidate.rejectedAsNoise || candidate.scores.noise >= options.maxNoiseScore)
		return QStringLiteral("noise_or_stream_management");

	const bool strongPositive = hasStrongPositiveSemanticEvidence(candidate, options);
	const bool strongContrast = hasStrongContrastSemanticEvidence(candidate, options);
	const bool safeBorderline = hasSafeBorderlineSemanticEvidence(candidate, options);
	// Strong contrast is useful to avoid false negatives in the reranker stage.
	// Safe-borderline evidence is narrower: it only handles calibration fuzz around
	// otherwise good candidates, such as value=0.69 versus a 0.70 threshold, and
	// still requires a strong good-vs-bad reranker margin plus non-hard boundaries.
	const bool rerankerGrace = strongPositive || strongContrast || safeBorderline;
	const bool semanticGrace = strongPositive || safeBorderline;

	if (options.requireSemanticTargetWhenAvailable && options.reliableMainTarget && candidate.semanticScoringAvailable &&
	    candidate.scores.semanticTarget < options.minSemanticTargetWhenAvailable && !semanticGrace) {
		return QStringLiteral("semantic_target_mismatch");
	}

	if (options.requireRerankerWhenAvailable && options.rejectInvalidRerankerWhenRequired &&
	    candidate.rerankerAttempted && !candidate.rerankerAvailable) {
		if (candidate.rerankerFailureReason.trimmed().isEmpty())
			return QStringLiteral("reranker_unavailable");
		return QStringLiteral("reranker_%1").arg(candidate.rerankerFailureReason.left(80));
	}

	if (options.requireRerankerWhenAvailable && candidate.rerankerFailed) {
		if (candidate.rerankerFailureReason.trimmed().isEmpty())
			return QStringLiteral("reranker_failed");
		return QStringLiteral("reranker_%1").arg(candidate.rerankerFailureReason.left(80));
	}

	if (options.requireRerankerWhenAvailable && candidate.rerankerAvailable) {
		const bool rawIsStrong = candidate.scores.rerankerRaw >= options.minStrongRerankerRawScoreWhenAvailable;
		const bool rawIsConditional = candidate.scores.rerankerRaw >= options.minConditionalRerankerRawScoreWhenAvailable;
		const bool normalizedIsAcceptable = candidate.scores.reranker >= options.minRerankerScoreWhenAvailable;
		const bool contextIsStrong = candidate.scores.semanticTarget >= options.minConditionalSemanticTargetWhenAvailable &&
			candidate.scores.boundary >= options.minConditionalBoundaryWhenAvailable;
		const bool hasBadClipScore = candidate.scores.rerankerBadClip > 0.0;
		const bool badClipTooStrong = hasBadClipScore &&
			candidate.scores.rerankerBadClip >= options.maxRerankerBadClipWhenAvailable &&
			candidate.scores.rerankerClipQualityMargin < options.minRerankerGoodBadMarginWhenAvailable;

		if (badClipTooStrong)
			return QStringLiteral("reranker_bad_clip_margin");

		// The raw Qwen3 reranker score is more stable across batches than the relative normalized score,
		// but it must now beat a separate semantic bad-clip query.
		if (!normalizedIsAcceptable && !rawIsStrong && !(rawIsConditional && contextIsStrong && rerankerGrace) && !strongContrast)
			return QStringLiteral("weak_reranker_score");

		if (candidate.scores.rerankerRaw < options.minRerankerRawScoreWhenAvailable && !rerankerGrace)
			return QStringLiteral("weak_reranker_raw_score");

		if (!rawIsStrong) {
			const bool strongEnoughByContext = (rawIsConditional && contextIsStrong && rerankerGrace) || strongContrast;
			if (!strongEnoughByContext)
				return QStringLiteral("weak_reranker_clipability");
		}
	}

	if (candidate.semanticScoringAvailable) {
		const double semanticNoise = std::max(candidate.scores.semanticNoise, candidate.scores.noise);
		const double valueMargin = semanticValueMargin(candidate);
		const bool openingIsMostlyMeta = candidate.scores.semanticOpeningMetaNoise >= options.maxOpeningMetaNoiseWhenAvailable &&
			candidate.scores.semanticOpeningHook < candidate.scores.semanticOpeningMetaNoise + 0.06;
		const bool endingIsMeta = candidate.scores.semanticEndingMetaNoise >= options.maxEndingMetaNoiseWhenAvailable &&
			candidate.scores.semanticEndingResolution < candidate.scores.semanticEndingMetaNoise + 0.04;
		const bool endingChangedTopic = candidate.scores.semanticEndingTopicShift >= options.maxEndingTopicShiftWhenAvailable &&
			candidate.scores.semanticEndingResolution < candidate.scores.semanticEndingTopicShift + 0.04;

		const bool openingMetaHardVeto = candidate.scores.semanticOpeningMetaNoise >= options.strongContrastHardMaxOpeningMetaNoise ||
			candidate.scores.semanticOpeningHook < 0.48;
		const bool endingMetaHardVeto = candidate.scores.semanticEndingMetaNoise >= options.strongContrastHardMaxEndingMetaNoise ||
			candidate.scores.semanticEndingResolution < 0.48;
		const bool endingShiftHardVeto = candidate.scores.semanticEndingTopicShift >= options.strongContrastHardMaxEndingTopicShift ||
			candidate.scores.semanticEndingResolution < 0.48;

		if (openingIsMostlyMeta && ((!strongContrast && !safeBorderline) || openingMetaHardVeto))
			return QStringLiteral("semantic_opening_meta_noise");
		if (endingIsMeta && ((!strongContrast && !safeBorderline) || endingMetaHardVeto))
			return QStringLiteral("semantic_ending_meta_noise");
		if (endingChangedTopic && ((!strongContrast && !safeBorderline) || endingShiftHardVeto))
			return QStringLiteral("semantic_ending_topic_shift");
		if (candidate.scores.semanticMetaNoise >= options.hardMaxSemanticMetaNoiseWhenAvailable &&
		    valueMargin < options.semanticNoiseMarginTolerance)
			return QStringLiteral("semantic_meta_noise");
		if (candidate.scores.semanticMetaNoise >= options.maxSemanticMetaNoiseWhenAvailable &&
		    !semanticGrace && valueMargin < options.semanticNoiseMarginTolerance)
			return QStringLiteral("semantic_meta_noise");
		if (semanticNoise >= options.maxSemanticNoiseWhenAvailable &&
		    !semanticGrace && valueMargin < options.semanticNoiseMarginTolerance)
			return QStringLiteral("semantic_noise");
	}

	if (options.presetId == QStringLiteral("viewer_message_response")) {
		if (candidate.scores.viewerResponse < options.minViewerResponseForViewerPreset && !semanticGrace)
			return QStringLiteral("weak_viewer_response");

		if (candidate.semanticScoringAvailable) {
			if (candidate.scores.semanticClipValue < options.minClipValueForViewerPreset && !semanticGrace)
				return QStringLiteral("weak_clip_value");
			if (candidate.scores.semanticHook < options.minHookForViewerPreset && !semanticGrace)
				return QStringLiteral("weak_hook");
			if (candidate.scores.semanticOpeningHook < options.minOpeningHookForViewerPreset && !semanticGrace)
				return QStringLiteral("weak_opening_hook");
			if (candidate.scores.semanticResolution < options.minResolutionForViewerPreset && !semanticGrace)
				return QStringLiteral("weak_resolution");
			if (candidate.scores.semanticEndingResolution < options.minEndingResolutionForViewerPreset && !semanticGrace)
				return QStringLiteral("weak_ending_resolution");
			if (candidate.scores.topicContinuity > 0.0 &&
			    candidate.scores.topicContinuity < options.minTopicContinuityForViewerPreset && !semanticGrace)
				return QStringLiteral("weak_topic_continuity");
		}
	}

	if (candidate.semanticScoringAvailable && durationSec >= options.longCandidateDurationSec &&
	    candidate.scores.topicContinuity > 0.0 &&
	    candidate.scores.topicContinuity < options.minTopicContinuityForLongCandidate && !semanticGrace) {
		return QStringLiteral("weak_topic_continuity");
	}

	if (candidate.scores.final < options.minFinalScore && !semanticGrace)
		return QStringLiteral("low_final_score");

	return {};
}

bool CandidateQualityGate::hasStrongPositiveSemanticEvidence(const ClipCandidate &candidate,
							      const CandidateQualityGateOptions &options) const
{
	if (!candidate.semanticScoringAvailable)
		return false;

	const double resolution = std::max({candidate.scores.semanticResolution, candidate.scores.semanticEndingResolution,
		candidate.scores.topicContinuity});
	const double hook = candidate.scores.semanticOpeningHook;
	const double valueMargin = semanticValueMargin(candidate);
	const bool strongReranker = !options.requireRerankerWhenAvailable ||
		((candidate.scores.rerankerRaw >= options.strongPositiveRerankerRaw ||
		  candidate.scores.reranker >= options.strongPositiveRerankerRaw) &&
		 (!candidate.rerankerAvailable ||
		  candidate.scores.rerankerClipQualityMargin >= options.minRerankerGoodBadMarginWhenAvailable));
	const bool strongValue = candidate.scores.semanticClipValue >= options.strongPositiveClipValue;
	const bool usefulShape = hook >= options.strongPositiveHook &&
		resolution >= options.strongPositiveResolution && candidate.scores.boundary >= 0.86 &&
		candidate.scores.semanticOpeningMetaNoise < options.maxOpeningMetaNoiseWhenAvailable;

	return strongReranker && strongValue && usefulShape && valueMargin >= -options.semanticNoiseMarginTolerance;
}


bool CandidateQualityGate::hasStrongContrastSemanticEvidence(const ClipCandidate &candidate,
							      const CandidateQualityGateOptions &options) const
{
	if (!candidate.semanticScoringAvailable || !candidate.rerankerAvailable)
		return false;

	const double resolution = std::max(candidate.scores.semanticResolution, candidate.scores.semanticEndingResolution);
	const double hook = std::max(candidate.scores.semanticHook, candidate.scores.semanticOpeningHook);
	const bool rerankerStrong = candidate.scores.rerankerRaw >= options.strongContrastRerankerRaw &&
		candidate.scores.rerankerBadClip <= options.strongContrastMaxBadClip &&
		candidate.scores.rerankerClipQualityMargin >= options.strongContrastMinMargin;
	const bool openingBeatsMeta = candidate.scores.semanticOpeningHook >= candidate.scores.semanticOpeningMetaNoise + 0.04;
	const bool endingBeatsMeta = candidate.scores.semanticEndingResolution >= candidate.scores.semanticEndingMetaNoise + 0.02 &&
		candidate.scores.semanticEndingResolution >= candidate.scores.semanticEndingTopicShift + 0.02;
	const bool shapeIsAcceptable = candidate.scores.final >= options.strongContrastMinFinal &&
		candidate.scores.semanticClipValue >= options.strongContrastMinClipValue &&
		hook >= options.strongContrastMinHook &&
		resolution >= options.strongContrastMinResolution &&
		openingBeatsMeta && endingBeatsMeta &&
		candidate.scores.boundary >= options.strongContrastMinBoundary;
	const bool notHardMeta = candidate.scores.semanticOpeningMetaNoise < options.strongContrastHardMaxOpeningMetaNoise &&
		candidate.scores.semanticEndingMetaNoise < options.strongContrastHardMaxEndingMetaNoise &&
		candidate.scores.semanticEndingTopicShift < options.strongContrastHardMaxEndingTopicShift;

	return rerankerStrong && shapeIsAcceptable && notHardMeta;
}


bool CandidateQualityGate::hasSafeBorderlineSemanticEvidence(const ClipCandidate &candidate,
								     const CandidateQualityGateOptions &options) const
{
	if (!candidate.semanticScoringAvailable || !candidate.rerankerAvailable)
		return false;

	const double resolution = std::max(candidate.scores.semanticResolution, candidate.scores.semanticEndingResolution);
	const double hook = std::max(candidate.scores.semanticHook, candidate.scores.semanticOpeningHook);
	const double positive = std::max({candidate.scores.semanticClipValue, hook, resolution, candidate.scores.topicContinuity});
	const double negative = std::max({candidate.scores.semanticMetaNoise, candidate.scores.semanticNoise,
		candidate.scores.semanticOpeningMetaNoise, candidate.scores.semanticEndingMetaNoise,
		candidate.scores.semanticEndingTopicShift});
	const double valueMargin = positive - negative;

	const bool rerankerVeryConfident = candidate.scores.rerankerRaw >= std::max(0.86, options.strongContrastRerankerRaw) &&
		candidate.scores.rerankerBadClip <= std::min(0.28, options.strongContrastMaxBadClip) &&
		candidate.scores.rerankerClipQualityMargin >= std::max(0.64, options.strongContrastMinMargin);

	const bool nearSemanticShape = candidate.scores.final >= std::max(0.64, options.strongContrastMinFinal) &&
		candidate.scores.semanticClipValue >= options.minClipValueForViewerPreset - 0.035 &&
		hook >= options.minHookForViewerPreset - 0.025 &&
		candidate.scores.semanticOpeningHook >= options.minOpeningHookForViewerPreset - 0.055 &&
		resolution >= options.minResolutionForViewerPreset - 0.045 &&
		candidate.scores.semanticEndingResolution >= options.minEndingResolutionForViewerPreset - 0.045 &&
		candidate.scores.boundary >= 0.58 &&
		(candidate.scores.topicContinuity <= 0.0 ||
		 candidate.scores.topicContinuity >= options.minTopicContinuityForViewerPreset - 0.10);

	const bool notHardMeta = candidate.scores.semanticOpeningMetaNoise < std::min(0.76, options.strongContrastHardMaxOpeningMetaNoise) &&
		candidate.scores.semanticEndingMetaNoise < std::min(0.78, options.strongContrastHardMaxEndingMetaNoise) &&
		candidate.scores.semanticEndingTopicShift < std::min(0.78, options.strongContrastHardMaxEndingTopicShift) &&
		negative <= positive + 0.08 && valueMargin >= -0.08;

	return rerankerVeryConfident && nearSemanticShape && notHardMeta;
}

double CandidateQualityGate::semanticValueMargin(const ClipCandidate &candidate) const
{
	const double positive = std::max({candidate.scores.semanticClipValue,
		candidate.scores.semanticHook,
		candidate.scores.semanticOpeningHook,
		candidate.scores.semanticResolution,
		candidate.scores.semanticEndingResolution,
		candidate.scores.topicContinuity});
	const double negative = std::max({candidate.scores.semanticMetaNoise, candidate.scores.semanticNoise,
		candidate.scores.semanticOpeningMetaNoise, candidate.scores.semanticEndingMetaNoise,
		candidate.scores.semanticEndingTopicShift});
	return positive - negative;
}
