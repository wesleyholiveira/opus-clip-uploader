#pragma once

#include "models/transcript.hpp"
#include "models/curation-settings.hpp"

#include <QString>
#include <QVector>

class TranscriptStore {
public:
	static QString keyForVideoPath(const QString &videoPath);
	static QString keyForVideoPath(const QString &videoPath, const QString &transcriptionLanguage);
	static QString keyForVideoRanges(const QString &videoPath, const QString &transcriptionLanguage,
					 const QVector<ClipDuration> &ranges);
	static void saveForVideoPath(const QString &videoPath, const RecordingTranscript &transcript);
	static void saveForVideoPath(const QString &videoPath, const QString &transcriptionLanguage,
				     const RecordingTranscript &transcript);
	static RecordingTranscript loadForVideoPath(const QString &videoPath);
	static RecordingTranscript loadForVideoPath(const QString &videoPath, const QString &transcriptionLanguage);
	static void saveForVideoRanges(const QString &videoPath, const QString &transcriptionLanguage,
				       const QVector<ClipDuration> &ranges, const RecordingTranscript &transcript);
	static RecordingTranscript loadForVideoRanges(const QString &videoPath, const QString &transcriptionLanguage,
						      const QVector<ClipDuration> &ranges);
	static void removeForVideoPath(const QString &videoPath);
	static void removeForVideoPath(const QString &videoPath, const QString &transcriptionLanguage);

private:
	static QString safeFileKey(const QString &videoPath);
};
