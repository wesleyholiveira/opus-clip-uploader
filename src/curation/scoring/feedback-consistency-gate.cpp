#include "curation/scoring/feedback-consistency-gate.hpp"

#include <algorithm>

namespace Curation::Scoring {

namespace {

static double boundedScore(double value)
{
	return std::clamp(value, 0.0, 1.0);
}

static bool hasEvidence(const ClipCandidate &candidate, const QString &needle)
{
	for (const QString &evidence : candidate.evidence) {
		if (evidence == needle || evidence.contains(needle))
			return true;
	}
	return false;
}

static bool isUserAcceptedSeed(const ClipCandidate &candidate)
{
	return candidate.source == QStringLiteral("feedback_positive_seed") ||
	       candidate.source == QStringLiteral("feedback_positive_exact_seed") ||
	       candidate.source == QStringLiteral("feedback_positive_exact_seed_beam") ||
	       hasEvidence(candidate, QStringLiteral("feedback_positive_seed")) ||
	       hasEvidence(candidate, QStringLiteral("feedback_positive_exact_seed_preserved")) ||
	       hasEvidence(candidate, QStringLiteral("complete_viewer_arc_gate_passed_by_user_feedback"));
}

} // namespace

bool FeedbackConsistencyGate::shouldReject(const ClipCandidate &candidate, const FeedbackSimilarityFeatures &features,
					   const FeedbackConsistencyGateOptions &options) const
{
	if (candidate.range.endSec <= candidate.range.startSec || candidate.text.trimmed().isEmpty())
		return true;

	if (isUserAcceptedSeed(candidate)) {
		// A positive seed is ground truth only when it is not contradicted by an
		// overlapping explicit negative. Conflicting like/dislike on the same range
		// must be resolved before selection; otherwise the system resurrects markers
		// the user just rejected.
		if (features.negativeRangeContamination && features.negativeScore >= 0.72 && features.margin <= 0.08)
			return true;
		return features.negativeRangeContamination && !features.explainedByPositiveRange &&
		       features.negativeScore >= features.positiveScore + 0.16;
	}

	if (features.negativeRangeContamination && !features.explainedByPositiveRange)
		return true;
	if (features.negativeScore >= features.positiveScore + options.hardNegativeMargin &&
	    features.negativeScore >= 0.46)
		return true;

	if (options.viewerMessagePreset) {
		const bool hasUsefulFeedbackSupport = features.positiveScore >= 0.38 &&
						      features.margin >= options.minPositiveMargin;
		const bool hasArcSupport = candidate.scores.arcCompleteness >= 0.20 ||
					   hasEvidence(candidate, QStringLiteral("complete_viewer_arc_gate_passed"));
		const bool hasSemanticSupport =
			std::max({candidate.scores.semanticClipValue, candidate.scores.semanticDirectAnswer,
				  candidate.scores.semanticViewerMessage, candidate.scores.semanticTarget,
				  candidate.scores.viewerResponse}) >= 0.58;
		if (!hasUsefulFeedbackSupport && !hasArcSupport && !hasSemanticSupport)
			return true;
	}

	return false;
}

void FeedbackConsistencyGate::applyPositiveBoost(ClipCandidate &candidate, const FeedbackSimilarityFeatures &features,
						 const FeedbackConsistencyGateOptions &options) const
{
	if (features.positiveScore <= 0.0 && features.negativeScore <= 0.0)
		return;

	candidate.evidence.append(features.evidence);
	if (features.positiveScore >= 0.32) {
		candidate.scores.final = boundedScore(candidate.scores.final + (features.positiveScore * 0.14) +
						      (std::max(0.0, features.margin) * 0.16));
		candidate.scores.qualityGate =
			std::max(candidate.scores.qualityGate, 0.42 + (features.positiveScore * 0.32));
		candidate.evidence.append(QStringLiteral("feedback_positive_guided_score:%1")
						  .arg(QString::number(features.positiveScore, 'f', 2)));
		if (!features.positiveReason.trimmed().isEmpty())
			candidate.evidence.append(
				QStringLiteral("feedback_positive_reason:%1").arg(features.positiveReason.left(96)));
	}
	if (features.negativeScore >= 0.28) {
		candidate.scores.final = boundedScore(candidate.scores.final - (features.negativeScore * 0.10));
		candidate.evidence.append(QStringLiteral("feedback_negative_guided_score:%1")
						  .arg(QString::number(features.negativeScore, 'f', 2)));
		if (!features.negativeReason.trimmed().isEmpty())
			candidate.evidence.append(
				QStringLiteral("feedback_negative_reason:%1").arg(features.negativeReason.left(96)));
	}
	if (options.viewerMessagePreset && features.positiveScore >= 0.46 &&
	    features.margin >= options.minPositiveMargin) {
		candidate.scores.semanticClipValue =
			std::max(candidate.scores.semanticClipValue, 0.64 + features.positiveScore * 0.18);
		candidate.scores.semanticTarget =
			std::max(candidate.scores.semanticTarget, 0.60 + features.positiveScore * 0.16);
		candidate.evidence.append(QStringLiteral("feedback_consistency_positive_margin"));
	}
}

QVector<ClipCandidate> FeedbackConsistencyGate::apply(QVector<ClipCandidate> candidates, const TranscriptIndex &index,
						      const Curation::Feedback::FeedbackRangeMemory &memory,
						      const FeedbackConsistencyGateOptions &options) const
{
	if (!memory.loaded || (memory.positiveRanges.isEmpty() && memory.negativeRanges.isEmpty()))
		return candidates;

	FeedbackSimilarityScorer scorer;
	for (ClipCandidate &candidate : candidates) {
		const FeedbackSimilarityFeatures features = scorer.score(candidate, index, memory);
		applyPositiveBoost(candidate, features, options);
		if (!shouldReject(candidate, features, options)) {
			if (isUserAcceptedSeed(candidate)) {
				candidate.rejectedByQualityGate = false;
				candidate.rejectedAsNoise = false;
				candidate.qualityGateChecked = true;
				candidate.rejectionReason.clear();
				candidate.scores.qualityGate = std::max(candidate.scores.qualityGate, 0.82);
				candidate.scores.final = std::max(candidate.scores.final, 0.84);
				candidate.evidence.append(
					QStringLiteral("feedback_positive_seed_kept_as_ground_truth"));
			}
			candidate.evidence.removeDuplicates();
			continue;
		}
		candidate.rejectedByQualityGate = true;
		candidate.rejectedAsNoise = true;
		candidate.qualityGateChecked = true;
		candidate.rejectionReason = features.negativeRangeContamination
						    ? QStringLiteral("feedback_negative_range_contamination")
						    : QStringLiteral("feedback_consistency_rejected");
		candidate.scores.qualityGate = std::min(candidate.scores.qualityGate, 0.04);
		candidate.scores.final = std::min(candidate.scores.final, 0.04);
		candidate.evidence.append(QStringLiteral("feedback_consistency_rejected"));
		candidate.evidence.removeDuplicates();
	}
	return candidates;
}

} // namespace Curation::Scoring
