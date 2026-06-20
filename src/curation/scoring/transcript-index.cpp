#include "curation/scoring/transcript-index.hpp"

#include <algorithm>
#include <cmath>

namespace Curation::Scoring {

TranscriptIndex::TranscriptIndex(const RecordingTranscript &transcript_) : transcript(transcript_)
{
	if (!transcript.segments.isEmpty()) {
		transcriptStartSec = transcript.segments.first().startSec;
		transcriptEndSec = transcript.segments.last().endSec;
	}

	nonEmptySegmentIndices.reserve(transcript.segments.size());
	for (int i = 0; i < static_cast<int>(transcript.segments.size()); ++i) {
		if (!transcript.segments.at(i).text.trimmed().isEmpty())
			nonEmptySegmentIndices.append(i);
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
	for (const int index : nonEmptySegmentIndices) {
		const TranscriptSegment &segment = transcript.segments.at(index);
		if (segmentOverlapsRange(segment, range))
			return index;
	}
	return -1;
}

int TranscriptIndex::lastSegmentIndexOverlapping(const ClipDuration &range) const
{
	for (int i = static_cast<int>(nonEmptySegmentIndices.size()) - 1; i >= 0; --i) {
		const int index = nonEmptySegmentIndices.at(i);
		const TranscriptSegment &segment = transcript.segments.at(index);
		if (segmentOverlapsRange(segment, range))
			return index;
	}
	return -1;
}

QVector<int> TranscriptIndex::segmentIndicesForRange(const ClipDuration &range) const
{
	QVector<int> result;
	for (const int index : nonEmptySegmentIndices) {
		const TranscriptSegment &segment = transcript.segments.at(index);
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
