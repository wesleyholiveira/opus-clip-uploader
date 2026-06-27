#include "curation/scoring/word-boundary-snapper.hpp"

#include <algorithm>

namespace Curation::Scoring {

namespace {

static bool isBoundarySafe(const WordTiming &word)
{
	const double durationSec = word.endSec - word.startSec;
	return !word.word.trimmed().isEmpty() && durationSec >= 0.015 && durationSec <= 1.85;
}

static ClipDuration clampRange(const ClipDuration &range, const ClipDuration &bounds)
{
	ClipDuration clamped;
	clamped.startSec = std::clamp(range.startSec, bounds.startSec, bounds.endSec);
	clamped.endSec = std::clamp(range.endSec, bounds.startSec, bounds.endSec);
	if (clamped.endSec < clamped.startSec)
		std::swap(clamped.startSec, clamped.endSec);
	return clamped;
}

} // namespace

ClipDuration WordBoundarySnapper::snap(const ClipDuration &range, const ClipDuration &bounds,
				       const QVector<WordTiming> &words) const
{
	if (words.isEmpty())
		return clampRange(range, bounds);

	double start = range.startSec;
	for (const WordTiming &word : words) {
		if (word.endSec < range.startSec - 0.55)
			continue;
		if (word.startSec > range.startSec + 1.25)
			break;
		if (isBoundarySafe(word)) {
			start = word.startSec;
			break;
		}
	}

	double end = range.endSec;
	for (int i = static_cast<int>(words.size()) - 1; i >= 0; --i) {
		const WordTiming &word = words.at(i);
		if (word.startSec > range.endSec + 0.80)
			continue;
		if (word.endSec < range.endSec - 1.75)
			break;
		if (isBoundarySafe(word)) {
			end = word.endSec;
			break;
		}
	}

	return clampRange({start, end}, bounds);
}

} // namespace Curation::Scoring
