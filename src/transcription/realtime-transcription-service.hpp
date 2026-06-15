#pragma once

#include "models/transcript.hpp"

#include <functional>

#include <QObject>
#include <QString>
#include <QStringList>

using TranscriptionProgressCallback = std::function<void(int progress, const QString &message)>;
using TranscriptionCancelCallback = std::function<bool()>;

class RealtimeTranscriptionService : public QObject {
public:
	explicit RealtimeTranscriptionService(QObject *parent = nullptr);
	~RealtimeTranscriptionService() override;

	RecordingTranscript transcribeVideoFile(const QString &videoPath, const QString &whisperModelPath,
						TranscriptionProgressCallback progressCallback = {},
						TranscriptionCancelCallback cancelCallback = {});
};

RealtimeTranscriptionService *global_realtime_transcription_service();
