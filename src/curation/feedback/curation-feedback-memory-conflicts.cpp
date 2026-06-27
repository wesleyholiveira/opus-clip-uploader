#include "curation/feedback/curation-feedback-detail.hpp"

namespace Curation::Feedback::Detail {

void resolvePositiveNegativeConflicts(FeedbackRangeMemory &memory)
{
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
