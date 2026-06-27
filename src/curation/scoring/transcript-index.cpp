#include "curation/scoring/transcript-index.hpp"

#include "curation/scoring/word-boundary-snapper.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace Curation::Scoring {

TranscriptIndex::TranscriptIndex(const RecordingTranscript &transcript_) : transcript(transcript_)
{
	if (!transcript.segments.isEmpty()) {
		transcriptStartSec = transcript.segments.first().startSec;
		transcriptEndSec = transcript.segments.last().endSec;
	}

	nonEmptySegmentIndices.reserve(transcript.segments.size());
	nonEmptyStartSecs.reserve(transcript.segments.size());
	nonEmptyEndSecs.reserve(transcript.segments.size());
	for (int i = 0; i < static_cast<int>(transcript.segments.size()); ++i) {
		const TranscriptSegment &segment = transcript.segments.at(i);
		if (segment.text.trimmed().isEmpty())
			continue;
		nonEmptySegmentIndices.append(i);
		nonEmptyStartSecs.append(segment.startSec);
		nonEmptyEndSecs.append(segment.endSec);
	}
}

bool TranscriptIndex::isEmpty() const
{
	return nonEmptySegmentIndices.isEmpty();
}

int TranscriptIndex::size() const
{
	return static_cast<int>(transcript.segments.size());
}

double TranscriptIndex::durationSec() const
{
	return transcriptEndSec > transcriptStartSec ? transcriptEndSec - transcriptStartSec : 0.0;
}

const RecordingTranscript &TranscriptIndex::recording() const
{
	return transcript;
}

const TranscriptSegment *TranscriptIndex::segmentAt(int index) const
{
	if (index < 0 || index >= static_cast<int>(transcript.segments.size()))
		return nullptr;
	return &transcript.segments.at(index);
}

bool TranscriptIndex::segmentOverlapsRange(const TranscriptSegment &segment, const ClipDuration &range)
{
	return std::min(segment.endSec, range.endSec) > std::max(segment.startSec, range.startSec);
}

int TranscriptIndex::firstSegmentIndexOverlapping(const ClipDuration &range) const
{
	if (nonEmptySegmentIndices.isEmpty() || range.endSec <= range.startSec)
		return -1;

	const auto it = std::upper_bound(nonEmptyEndSecs.cbegin(), nonEmptyEndSecs.cend(), range.startSec);
	for (int pos = static_cast<int>(std::distance(nonEmptyEndSecs.cbegin(), it));
	     pos < static_cast<int>(nonEmptySegmentIndices.size()); ++pos) {
		const int index = nonEmptySegmentIndices.at(pos);
		const TranscriptSegment &segment = transcript.segments.at(index);
		if (segment.startSec >= range.endSec)
			break;
		if (segmentOverlapsRange(segment, range))
			return index;
	}
	return -1;
}

int TranscriptIndex::lastSegmentIndexOverlapping(const ClipDuration &range) const
{
	if (nonEmptySegmentIndices.isEmpty() || range.endSec <= range.startSec)
		return -1;

	const auto it = std::lower_bound(nonEmptyStartSecs.cbegin(), nonEmptyStartSecs.cend(), range.endSec);
	for (int pos = static_cast<int>(std::distance(nonEmptyStartSecs.cbegin(), it)) - 1; pos >= 0; --pos) {
		const int index = nonEmptySegmentIndices.at(pos);
		const TranscriptSegment &segment = transcript.segments.at(index);
		if (segment.endSec <= range.startSec)
			break;
		if (segmentOverlapsRange(segment, range))
			return index;
	}
	return -1;
}

QVector<int> TranscriptIndex::segmentIndicesForRange(const ClipDuration &range) const
{
	QVector<int> result;
	if (nonEmptySegmentIndices.isEmpty() || range.endSec <= range.startSec)
		return result;

	const auto it = std::upper_bound(nonEmptyEndSecs.cbegin(), nonEmptyEndSecs.cend(), range.startSec);
	const int firstPos = static_cast<int>(std::distance(nonEmptyEndSecs.cbegin(), it));
	for (int pos = firstPos; pos < static_cast<int>(nonEmptySegmentIndices.size()); ++pos) {
		const int index = nonEmptySegmentIndices.at(pos);
		const TranscriptSegment &segment = transcript.segments.at(index);
		if (segment.startSec >= range.endSec)
			break;
		if (segmentOverlapsRange(segment, range))
			result.append(index);
	}
	return result;
}

QString TranscriptIndex::textForRange(const ClipDuration &range) const
{
	const int first = firstSegmentIndexOverlapping(range);
	const int last = lastSegmentIndexOverlapping(range);
	return textForSegmentWindow(first, last);
}

QString TranscriptIndex::textForSegmentWindow(int firstIndex, int lastIndex) const
{
	if (firstIndex < 0 || lastIndex < firstIndex)
		return {};

	QString text;
	const int safeLast = std::min(lastIndex, static_cast<int>(transcript.segments.size()) - 1);
	for (int i = firstIndex; i <= safeLast; ++i) {
		const QString segmentText = transcript.segments.at(i).text.trimmed();
		if (segmentText.isEmpty())
			continue;
		if (!text.isEmpty())
			text += QLatin1Char(' ');
		text += segmentText;
	}
	return text.simplified();
}

QString TranscriptIndex::timedTextForRange(const ClipDuration &range, double pauseMarkerThresholdSec) const
{
	const int first = firstSegmentIndexOverlapping(range);
	const int last = lastSegmentIndexOverlapping(range);
	return timedTextForSegmentWindow(first, last, pauseMarkerThresholdSec);
}

QString TranscriptIndex::timedTextForSegmentWindow(int firstIndex, int lastIndex, double pauseMarkerThresholdSec) const
{
	if (firstIndex < 0 || lastIndex < firstIndex)
		return {};

	QString text;
	const int safeLast = std::min(lastIndex, static_cast<int>(transcript.segments.size()) - 1);
	int previousNonEmpty = -1;
	for (int i = firstIndex; i <= safeLast; ++i) {
		const TranscriptSegment &segment = transcript.segments.at(i);
		const QString segmentText = segment.text.trimmed();
		if (segmentText.isEmpty())
			continue;

		if (previousNonEmpty >= 0) {
			const double pauseSec =
				std::max(0.0, segment.startSec - transcript.segments.at(previousNonEmpty).endSec);
			if (pauseSec >= pauseMarkerThresholdSec) {
				if (!text.isEmpty())
					text += QLatin1Char(' ');
				text += QStringLiteral("[PAUSA %1s]").arg(QString::number(pauseSec, 'f', 1));
			}
		}

		if (!text.isEmpty())
			text += QLatin1Char(' ');
		text += QStringLiteral("[%1-%2] %3")
				.arg(QString::number(segment.startSec, 'f', 2), QString::number(segment.endSec, 'f', 2),
				     segmentText);
		previousNonEmpty = i;
	}
	return text.simplified();
}

bool TranscriptIndex::hasWordTimings() const
{
	return transcript.hasWordTimings();
}

QVector<WordTiming> TranscriptIndex::wordsForRange(const ClipDuration &range) const
{
	QVector<WordTiming> words;
	const QVector<int> indices = segmentIndicesForRange(range);
	for (const int index : indices) {
		const TranscriptSegment &segment = transcript.segments.at(index);
		if (segment.words.isEmpty())
			continue;
		for (const WordTiming &word : segment.words) {
			if (std::min(word.endSec, range.endSec) > std::max(word.startSec, range.startSec))
				words.append(word);
		}
	}
	std::sort(words.begin(), words.end(),
		  [](const WordTiming &left, const WordTiming &right) { return left.startSec < right.startSec; });
	return words;
}

ClipDuration TranscriptIndex::snapRangeToWordBoundaries(const ClipDuration &range, const ClipDuration &bounds) const
{
	if (!hasWordTimings())
		return clampRange(range, bounds);

	const ClipDuration search{std::max(bounds.startSec, range.startSec - 0.55),
				  std::min(bounds.endSec, range.endSec + 0.80)};
	const QVector<WordTiming> words = wordsForRange(search);
	WordBoundarySnapper snapper;
	return snapper.snap(range, bounds, words);
}

double TranscriptIndex::silenceBeforeSegment(int index) const
{
	if (index <= 0 || index >= static_cast<int>(transcript.segments.size()))
		return 0.0;

	return std::max(0.0, transcript.segments.at(index).startSec - transcript.segments.at(index - 1).endSec);
}

double TranscriptIndex::silenceAfterSegment(int index) const
{
	if (index < 0 || index >= static_cast<int>(transcript.segments.size()) - 1)
		return 0.0;

	return std::max(0.0, transcript.segments.at(index + 1).startSec - transcript.segments.at(index).endSec);
}

double TranscriptIndex::silenceBeforeRange(const ClipDuration &range) const
{
	const int first = firstSegmentIndexOverlapping(range);
	return first >= 0 ? silenceBeforeSegment(first) : 0.0;
}

double TranscriptIndex::silenceAfterRange(const ClipDuration &range) const
{
	const int last = lastSegmentIndexOverlapping(range);
	return last >= 0 ? silenceAfterSegment(last) : 0.0;
}

double TranscriptIndex::maxInternalSilenceInRange(const ClipDuration &range) const
{
	const QVector<int> indices = segmentIndicesForRange(range);
	if (indices.size() < 2)
		return 0.0;

	double maxSilence = 0.0;
	for (int i = 1; i < static_cast<int>(indices.size()); ++i) {
		const TranscriptSegment *previous = segmentAt(indices.at(i - 1));
		const TranscriptSegment *current = segmentAt(indices.at(i));
		if (!previous || !current)
			continue;
		maxSilence = std::max(maxSilence, current->startSec - previous->endSec);
	}
	return std::max(0.0, maxSilence);
}

ClipDuration TranscriptIndex::clampRange(const ClipDuration &range, const ClipDuration &bounds) const
{
	ClipDuration clamped;
	clamped.startSec = std::clamp(range.startSec, bounds.startSec, bounds.endSec);
	clamped.endSec = std::clamp(range.endSec, bounds.startSec, bounds.endSec);
	if (clamped.endSec < clamped.startSec)
		clamped.endSec = clamped.startSec;
	return clamped;
}

} // namespace Curation::Scoring
