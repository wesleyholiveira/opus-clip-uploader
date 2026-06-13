#pragma once

#include "models/transcript.hpp"

#include <QString>
#include <QVector>

class TranscriptStore {
public:
	static QString keyForVideoPath(const QString &videoPath);
	static void saveForVideoPath(const QString &videoPath, const RecordingTranscript &transcript);
	static RecordingTranscript loadForVideoPath(const QString &videoPath);
	static void removeForVideoPath(const QString &videoPath);

private:
	static QString safeFileKey(const QString &videoPath);
};
