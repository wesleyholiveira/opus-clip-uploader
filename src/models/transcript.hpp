#pragma once

#include <QString>
#include <QVector>
#include <QMetaType>

struct WordTiming {
	double startSec = 0.0;
	double endSec = 0.0;
	QString word;
	double score = 0.0;
};

struct TranscriptSegment {
	double startSec = 0.0;
	double endSec = 0.0;
	QString text;
	QVector<WordTiming> words;

	bool hasWordTimings() const { return !words.isEmpty(); }
};

struct RecordingTranscript {
	QString videoFileName;
	QString videoPath;
	QVector<TranscriptSegment> segments;
	bool wordAligned = false;
	QString alignmentBackend;

	bool isEmpty() const { return segments.isEmpty(); }
	bool hasWordTimings() const
	{
		for (const TranscriptSegment &segment : segments) {
			if (!segment.words.isEmpty())
				return true;
		}
		return false;
	}
};

Q_DECLARE_METATYPE(WordTiming)
Q_DECLARE_METATYPE(TranscriptSegment)
Q_DECLARE_METATYPE(RecordingTranscript)
