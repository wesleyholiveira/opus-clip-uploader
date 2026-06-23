#pragma once

#include "models/curation-settings.hpp"
#include "models/transcript.hpp"

#include <QString>
#include <QVector>

namespace Curation::Scoring {

class TranscriptIndex {
public:
	explicit TranscriptIndex(const RecordingTranscript &transcript);

	bool isEmpty() const;
	int size() const;
	double durationSec() const;
	const RecordingTranscript &recording() const;
	const TranscriptSegment *segmentAt(int index) const;

	int firstSegmentIndexOverlapping(const ClipDuration &range) const;
	int lastSegmentIndexOverlapping(const ClipDuration &range) const;
	QVector<int> segmentIndicesForRange(const ClipDuration &range) const;
	QString textForRange(const ClipDuration &range) const;
	QString textForSegmentWindow(int firstIndex, int lastIndex) const;
	QString timedTextForRange(const ClipDuration &range, double pauseMarkerThresholdSec = 1.2) const;
	QString timedTextForSegmentWindow(int firstIndex, int lastIndex, double pauseMarkerThresholdSec = 1.2) const;
	bool hasWordTimings() const;
	QVector<WordTiming> wordsForRange(const ClipDuration &range) const;
	ClipDuration snapRangeToWordBoundaries(const ClipDuration &range, const ClipDuration &bounds) const;

	double silenceBeforeSegment(int index) const;
	double silenceAfterSegment(int index) const;
	double silenceBeforeRange(const ClipDuration &range) const;
	double silenceAfterRange(const ClipDuration &range) const;
	double maxInternalSilenceInRange(const ClipDuration &range) const;
	ClipDuration clampRange(const ClipDuration &range, const ClipDuration &bounds) const;

	static bool segmentOverlapsRange(const TranscriptSegment &segment, const ClipDuration &range);

private:
	const RecordingTranscript &transcript;
	QVector<int> nonEmptySegmentIndices;
	QVector<double> nonEmptyStartSecs;
	QVector<double> nonEmptyEndSecs;
	double transcriptStartSec = 0.0;
	double transcriptEndSec = 0.0;
};

} // namespace Curation::Scoring
