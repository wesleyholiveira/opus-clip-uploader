#include "curation/scoring/clip-scoring-pipeline-detail.hpp"

#include "curation/feedback/curation-feedback-store.hpp"
#include "curation/scoring/candidate-builder.hpp"
#include "curation/scoring/feedback-similarity-scorer.hpp"
#include "curation/scoring/semantic-coarse-retriever.hpp"

#include <QStringList>

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

#include <algorithm>
#include <cmath>

namespace Curation::Scoring::ClipScoringPipelineDetail {

bool positiveSignalCanReplayAsTimelineMarker(const Curation::Feedback::FeedbackRangeSignal &signal)
{
	return signal.semanticPrototypeEligible || signal.decision == QStringLiteral("accepted") ||
	       signal.decision == QStringLiteral("approved_adjusted") ||
	       signal.decision == QStringLiteral("added_by_user");
}

bool hasSimilarTimelineCandidateRange(const QVector<ClipCandidate> &candidates, const ClipDuration &range)
{
	for (const ClipCandidate &candidate : candidates) {
		if (FeedbackSimilarityScorer::rangeSimilarity(candidate.range, range) >= 0.72)
			return true;
		const double boundaryDistance = std::fabs(candidate.range.startSec - range.startSec) +
						std::fabs(candidate.range.endSec - range.endSec);
		if (boundaryDistance <= 8.0)
			return true;
	}
	return false;
}

bool rangeMatchesExistingReviewRange(const ClipDuration &range, const QVector<ClipDuration> &existingRanges)
{
	if (!validMemoryRange(range) || existingRanges.isEmpty())
		return false;
	for (const ClipDuration &existing : existingRanges) {
		if (!validMemoryRange(existing))
			continue;
		if (FeedbackSimilarityScorer::rangeSimilarity(existing, range) >= 0.76)
			return true;
		const double boundaryDistance =
			std::fabs(existing.startSec - range.startSec) + std::fabs(existing.endSec - range.endSec);
		if (boundaryDistance <= 10.0)
			return true;
		const double overlap = rangeOverlapSec(existing, range);
		const double minDuration = std::min(existing.endSec - existing.startSec, range.endSec - range.startSec);
		if (minDuration > 0.0 && overlap >= minDuration * 0.86)
			return true;
	}
	return false;
}

bool isStrictExactPositiveFeedbackSeedCandidate(const ClipCandidate &candidate)
{
	return candidate.source.contains(QStringLiteral("feedback_positive_exact_seed")) ||
	       candidateHasEvidence(candidate, QStringLiteral("feedback_positive_exact_seed_preserved"));
}

bool isExactUserFeedbackSeedCandidate(const ClipCandidate &candidate)
{
	return isStrictExactPositiveFeedbackSeedCandidate(candidate) ||
	       candidateHasEvidence(candidate, QStringLiteral("complete_viewer_arc_gate_passed_by_user_feedback"));
}

bool exactSeedMatchesPositiveMemory(const ClipCandidate &candidate,
				    const Curation::Feedback::FeedbackRangeMemory &memory)
{
	if (!memory.loaded || !isStrictExactPositiveFeedbackSeedCandidate(candidate))
		return false;
	for (const Curation::Feedback::FeedbackRangeSignal &signal : memory.positiveRanges) {
		if (rangeMatchesReviewedSignal(candidate.range, signal))
			return true;
	}
	return false;
}

int suppressAlreadyReviewedExactPositiveSeeds(QVector<ClipCandidate> &candidates,
					      const Curation::Feedback::FeedbackRangeMemory &memory)
{
	if (!memory.loaded || candidates.isEmpty())
		return 0;
	int suppressed = 0;
	for (ClipCandidate &candidate : candidates) {
		if (candidate.rejectedByQualityGate || candidate.rejectedAsNoise)
			continue;
		if (!exactSeedMatchesPositiveMemory(candidate, memory))
			continue;
		candidate.rejectedByQualityGate = true;
		candidate.rejectionReason = QStringLiteral("already_reviewed_positive_feedback_seed");
		candidate.evidence.append(QStringLiteral("feedback_positive_exact_seed_suppressed_already_reviewed"));
		candidate.evidence.append(QStringLiteral("feedback_positive_seed_kept_for_guidance_not_auto_replay"));
		candidate.evidence.removeDuplicates();
		++suppressed;
	}
	return suppressed;
}

int suppressAlreadyReviewedFeedbackCandidates(QVector<ClipCandidate> &candidates,
					      const Curation::Feedback::FeedbackRangeMemory &memory)
{
	if (!memory.loaded || candidates.isEmpty())
		return 0;
	int suppressed = 0;
	for (ClipCandidate &candidate : candidates) {
		if (candidate.rejectedByQualityGate || candidate.rejectedAsNoise)
			continue;
		if (!rangeWasAlreadyReviewedByFeedback(candidate.range, memory))
			continue;
		candidate.rejectedByQualityGate = true;
		candidate.rejectionReason = QStringLiteral("already_reviewed_feedback_candidate");
		candidate.evidence.append(QStringLiteral("feedback_candidate_suppressed_already_reviewed"));
		candidate.evidence.append(QStringLiteral("reviewed_feedback_not_asked_again"));
		candidate.evidence.removeDuplicates();
		++suppressed;
	}
	return suppressed;
}

int suppressExistingReviewRangeExactSeeds(QVector<ClipCandidate> &candidates,
					  const QVector<ClipDuration> &existingRanges)
{
	if (existingRanges.isEmpty() || candidates.isEmpty())
		return 0;
	int suppressed = 0;
	for (ClipCandidate &candidate : candidates) {
		if (candidate.rejectedByQualityGate || candidate.rejectedAsNoise)
			continue;
		if (!isExactUserFeedbackSeedCandidate(candidate))
			continue;
		if (!rangeMatchesExistingReviewRange(candidate.range, existingRanges))
			continue;
		candidate.rejectedByQualityGate = true;
		candidate.rejectionReason = QStringLiteral("already_existing_review_marker");
		candidate.evidence.append(QStringLiteral("feedback_positive_exact_seed_suppressed_existing_marker"));
		candidate.evidence.removeDuplicates();
		++suppressed;
	}
	return suppressed;
}

QVector<ClipCandidate> replayPositiveFeedbackTimelineCandidates(const TranscriptIndex &index,
								const ClipScoringPipelineOptions &options,
								const Curation::Feedback::FeedbackRangeMemory &memory,
								int limit)
{
	QVector<ClipCandidate> replay;
	if (!memory.loaded || memory.positiveRanges.isEmpty() || limit <= 0)
		return replay;

	QVector<Curation::Feedback::FeedbackRangeSignal> positives = memory.positiveRanges;
	std::sort(positives.begin(), positives.end(), [](const auto &left, const auto &right) {
		if (left.semanticPrototypeEligible != right.semanticPrototypeEligible)
			return left.semanticPrototypeEligible;
		if (left.sequence != right.sequence)
			return left.sequence > right.sequence;
		return left.weight > right.weight;
	});

	SemanticCoarseRegion replayRegion;
	replayRegion.score = 0.86;
	replayRegion.evidence.append(QStringLiteral("feedback_positive_replay_region"));

	FeedbackSimilarityScorer scorer;
	for (const Curation::Feedback::FeedbackRangeSignal &signal : positives) {
		if (static_cast<int>(replay.size()) >= limit)
			break;
		if (!positiveSignalCanReplayAsTimelineMarker(signal) || !validMemoryRange(signal.range))
			continue;

		ClipDuration range = index.clampRange(signal.range, options.generation.searchRange);
		if (!validMemoryRange(range))
			continue;
		if (rangeMatchesExistingReviewRange(range, options.existingReviewRanges))
			continue;
		const double durationSec = range.endSec - range.startSec;
		if (durationSec < options.generation.minDurationSec ||
		    durationSec > options.generation.maxDurationSec + 1.0)
			continue;
		if (hasSimilarTimelineCandidateRange(replay, range))
			continue;

		const QString text = index.textForRange(range).simplified();
		if (text.trimmed().isEmpty())
			continue;
		const FeedbackSimilarityFeatures features = scorer.scoreRange(range, text, index, memory);
		const bool contradictedByNewerNegative = features.negativeRangeContamination &&
							 !features.explainedByPositiveRange &&
							 features.negativeScore >= features.positiveScore + 0.16;
		const bool ambiguousConflict = features.negativeRangeContamination && features.negativeScore >= 0.72 &&
					       features.margin <= 0.08 && features.positiveScore < 0.88;
		if (contradictedByNewerNegative || ambiguousConflict)
			continue;

		ClipCandidate candidate = CandidateBuilder::buildForRange(index, options.generation, options.scoring,
									  options.qualityGate, range, replayRegion);
		if (candidate.text.trimmed().isEmpty() ||
		    !CandidateBuilder::isStructurallyViable(candidate, options.generation, options.qualityGate))
			continue;
		candidate.source = QStringLiteral("feedback_positive_exact_seed");
		candidate.startsNearViewerCue = true;
		candidate.rejectedAsNoise = false;
		candidate.rejectedByQualityGate = false;
		candidate.rejectionReason.clear();
		candidate.qualityGateChecked = true;
		candidate.scores.final = std::max(candidate.scores.final, 0.88);
		candidate.scores.qualityGate = std::max(candidate.scores.qualityGate, 0.92);
		candidate.scores.semanticTarget = std::max(candidate.scores.semanticTarget, 0.70);
		candidate.scores.semanticClipValue = std::max(candidate.scores.semanticClipValue, 0.72);
		candidate.scores.semanticViewerMessage = std::max(candidate.scores.semanticViewerMessage, 0.66);
		candidate.scores.semanticDirectAnswer = std::max(candidate.scores.semanticDirectAnswer, 0.64);
		candidate.scores.semanticOpeningHook = std::max(candidate.scores.semanticOpeningHook, 0.62);
		candidate.scores.semanticResolution = std::max(candidate.scores.semanticResolution, 0.62);
		candidate.scores.semanticEndingResolution = std::max(candidate.scores.semanticEndingResolution, 0.62);
		candidate.scores.arcOpening = std::max(candidate.scores.arcOpening, 0.48);
		candidate.scores.arcDevelopment = std::max(candidate.scores.arcDevelopment, 0.52);
		candidate.scores.arcConclusion = std::max(candidate.scores.arcConclusion, 0.52);
		candidate.scores.arcBoundaryCleanliness = std::max(candidate.scores.arcBoundaryCleanliness, 0.46);
		candidate.scores.arcCompleteness = std::max(candidate.scores.arcCompleteness, 0.52);
		candidate.evidence.append(QStringLiteral("feedback_positive_exact_seed"));
		candidate.evidence.append(QStringLiteral("feedback_positive_exact_seed_preserved"));
		candidate.evidence.append(QStringLiteral("feedback_positive_replayed_to_timeline"));
		candidate.evidence.append(QStringLiteral("complete_viewer_arc_gate_passed_by_user_feedback"));
		candidate.evidence.append(
			QStringLiteral("feedback_positive_decision:%1").arg(signal.decision.left(32)));
		if (!signal.reason.trimmed().isEmpty())
			candidate.evidence.append(
				QStringLiteral("feedback_positive_reason:%1").arg(signal.reason.left(96)));
		candidate.evidence.append(features.evidence);
		candidate.evidence.removeDuplicates();
		replay.append(candidate);
	}
	return replay;
}
} // namespace Curation::Scoring::ClipScoringPipelineDetail
