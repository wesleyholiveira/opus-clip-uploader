#pragma once

#include <QString>
#include <QVector>
#include <QMetaType>

struct TranscriptSegment {
	double startSec = 0.0;
	double endSec = 0.0;
	QString text;
};

struct RecordingTranscript {
	QString videoFileName;
	QString videoPath;
	QVector<TranscriptSegment> segments;

	bool isEmpty() const { return segments.isEmpty(); }
};

Q_DECLARE_METATYPE(TranscriptSegment)
Q_DECLARE_METATYPE(RecordingTranscript)
