#include "curation/scoring/candidate-quality-gate.hpp"

#include <algorithm>

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

	if (options.requireSemanticTargetWhenAvailable && options.reliableMainTarget && candidate.semanticScoringAvailable &&
	    candidate.scores.semanticTarget < options.minSemanticTargetWhenAvailable) {
		return QStringLiteral("semantic_target_mismatch");
	}

	if (options.presetId == QStringLiteral("viewer_message_response") &&
	    candidate.scores.viewerResponse < options.minViewerResponseForViewerPreset) {
		return QStringLiteral("weak_viewer_response");
	}

	if (candidate.scores.final < options.minFinalScore)
		return QStringLiteral("low_final_score");

	return {};
}
