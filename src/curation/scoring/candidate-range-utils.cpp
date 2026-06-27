#include "curation/scoring/candidate-range-utils.hpp"

#include <algorithm>
#include <cmath>

namespace Curation::Scoring::CandidateRangeUtils {

QString rangeKey(const ClipDuration &range)
{
	return QStringLiteral("%1:%2")
		.arg(QString::number(range.startSec, 'f', 1))
		.arg(QString::number(range.endSec, 'f', 1));
}

double rangeCenterSec(const ClipDuration &range)
{
	return range.startSec + ((range.endSec - range.startSec) * 0.5);
}

double overlapSec(const ClipDuration &left, const ClipDuration &right)
{
	return std::max(0.0, std::min(left.endSec, right.endSec) - std::max(left.startSec, right.startSec));
}

bool substantiallyOverlapsFocus(const ClipDuration &candidate, const ClipDuration &focus)
{
	const double focusDuration = std::max(0.0, focus.endSec - focus.startSec);
	if (focusDuration <= 0.0)
		return true;
	const double overlap = overlapSec(candidate, focus);
	return overlap >= std::min(8.0, focusDuration * 0.35) ||
	       (candidate.startSec <= rangeCenterSec(focus) && candidate.endSec >= rangeCenterSec(focus));
}

void appendUniqueStart(QVector<double> &starts, double startSec)
{
	if (!std::isfinite(startSec))
		return;
	for (double existing : starts) {
		if (std::fabs(existing - startSec) < 0.75)
			return;
	}
	starts.append(startSec);
}

void appendUniqueValue(QVector<double> &values, double value, double tolerance)
{
	if (!std::isfinite(value))
		return;
	for (const double existing : values) {
		if (std::fabs(existing - value) <= tolerance)
			return;
	}
	values.append(value);
}

void appendUniqueRange(QVector<ClipDuration> &ranges, const ClipDuration &range, double toleranceSec)
{
	if (!std::isfinite(range.startSec) || !std::isfinite(range.endSec) || range.endSec <= range.startSec)
		return;
	for (const ClipDuration &existing : ranges) {
		if (std::fabs(existing.startSec - range.startSec) < toleranceSec &&
		    std::fabs(existing.endSec - range.endSec) < toleranceSec)
			return;
	}
	ranges.append(range);
}

bool rangeIsWithinDurationLimits(const ClipDuration &range, const CandidateGenerationOptions &options)
{
	const double durationSec = range.endSec - range.startSec;
	return durationSec >= options.minDurationSec && durationSec <= options.maxDurationSec + 0.50;
}

ClipDuration normalizedVariantRange(const TranscriptIndex &index, const CandidateGenerationOptions &options,
				    const ClipDuration &candidateRange, const ClipDuration &localSearchRange)
{
	ClipDuration range = candidateRange;
	if (range.startSec < localSearchRange.startSec) {
		const double shift = localSearchRange.startSec - range.startSec;
		range.startSec += shift;
		range.endSec += shift;
	}
	if (range.endSec > localSearchRange.endSec) {
		const double shift = range.endSec - localSearchRange.endSec;
		range.startSec -= shift;
		range.endSec -= shift;
	}
	range = index.clampRange(range, localSearchRange);
	if (!rangeIsWithinDurationLimits(range, options))
		return {};
	return range;
}

bool hasSimilarRange(const QVector<ClipCandidate> &candidates, const ClipDuration &range, double toleranceSec)
{
	for (const ClipCandidate &candidate : candidates) {
		if (std::fabs(candidate.range.startSec - range.startSec) <= toleranceSec &&
		    std::fabs(candidate.range.endSec - range.endSec) <= toleranceSec)
			return true;
	}
	return false;
}

} // namespace Curation::Scoring::CandidateRangeUtils
