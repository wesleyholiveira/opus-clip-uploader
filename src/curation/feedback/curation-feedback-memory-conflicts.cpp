#include "curation/feedback/curation-feedback-detail.hpp"

#include <algorithm>
#include <cmath>

namespace Curation::Feedback::Detail {

namespace {

bool feedbackSignalsDuplicate(const FeedbackRangeSignal &left, const FeedbackRangeSignal &right)
{
	const double leftDuration = feedbackRangeDuration(left.range);
	const double rightDuration = feedbackRangeDuration(right.range);
	if (leftDuration <= 0.0 || rightDuration <= 0.0)
		return false;
	const double overlap = feedbackRangeOverlap(left.range, right.range);
	if (overlap <= 0.0)
		return false;
	const double shorterCoverage = overlap / std::min(leftDuration, rightDuration);
	const double longerCoverage = overlap / std::max(leftDuration, rightDuration);
	const bool closeBoundaries = std::fabs(left.range.startSec - right.range.startSec) <= 3.0 &&
				     std::fabs(left.range.endSec - right.range.endSec) <= 5.0;
	return closeBoundaries || (shorterCoverage >= 0.88 && longerCoverage >= 0.62);
}

QVector<FeedbackRangeSignal> collapseDuplicateFeedbackSignals(const QVector<FeedbackRangeSignal> &results)
{
	QVector<FeedbackRangeSignal> newestFirst;
	newestFirst.reserve(results.size());
	for (int i = results.size() - 1; i >= 0; --i) {
		const FeedbackRangeSignal &signal = results.at(i);
		bool duplicate = false;
		for (FeedbackRangeSignal &kept : newestFirst) {
			if (!feedbackSignalsDuplicate(signal, kept))
				continue;
			kept.weight = std::min(4.0, std::max(kept.weight, signal.weight) + 0.10);
			kept.semanticPrototypeEligible = kept.semanticPrototypeEligible ||
							 signal.semanticPrototypeEligible;
			kept.weakNegative = kept.weakNegative && signal.weakNegative;
			kept.ignoreForTraining = kept.ignoreForTraining && signal.ignoreForTraining;
			duplicate = true;
			break;
		}
		if (!duplicate)
			newestFirst.append(signal);
	}
	std::reverse(newestFirst.begin(), newestFirst.end());
	return newestFirst;
}

} // namespace

void resolvePositiveNegativeConflicts(FeedbackRangeMemory &memory)
{
	memory.positiveRanges = collapseDuplicateFeedbackSignals(memory.positiveRanges);
	memory.negativeRanges = collapseDuplicateFeedbackSignals(memory.negativeRanges);
	if (memory.positiveRanges.isEmpty() || memory.negativeRanges.isEmpty())
		return;

	QVector<FeedbackRangeSignal> positives;
	positives.reserve(memory.positiveRanges.size());
	for (const FeedbackRangeSignal &positive : memory.positiveRanges) {
		bool keep = true;
		for (const FeedbackRangeSignal &negative : memory.negativeRanges) {
			if (negative.weakNegative || negative.ignoreForTraining)
				continue;
			if (!feedbackSignalsConflict(positive, negative))
				continue;
			if (sameCorrectedFeedbackPair(negative, positive))
				continue;
			// When the same range was liked and later disliked, the newest explicit
			// feedback is the source of truth. If ordering is tied, keep the negative
			// because it is safer to avoid a known bad marker than to resurrect it.
			if (negative.sequence >= positive.sequence) {
				keep = false;
				break;
			}
		}
		if (keep)
			positives.append(positive);
	}

	QVector<FeedbackRangeSignal> negatives;
	negatives.reserve(memory.negativeRanges.size());
	for (const FeedbackRangeSignal &negative : memory.negativeRanges) {
		bool keep = true;
		for (const FeedbackRangeSignal &positive : memory.positiveRanges) {
			if (!feedbackSignalsConflict(negative, positive))
				continue;
			if (sameCorrectedFeedbackPair(negative, positive))
				continue;
			if (positive.sequence > negative.sequence) {
				keep = false;
				break;
			}
		}
		if (keep)
			negatives.append(negative);
	}

	memory.positiveRanges = positives;
	memory.negativeRanges = negatives;
}
} // namespace Curation::Feedback::Detail
