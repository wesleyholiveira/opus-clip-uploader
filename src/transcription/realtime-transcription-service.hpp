#pragma once

#include "models/transcript.hpp"
#include "models/curation-settings.hpp"

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
	RecordingTranscript transcribeVideoFile(const QString &videoPath, const QString &whisperModelPath,
						const QString &transcriptionLanguage,
						TranscriptionProgressCallback progressCallback = {},
						TranscriptionCancelCallback cancelCallback = {});
	RecordingTranscript alignTranscriptWithWhisperX(const QString &videoPath, const QString &transcriptionLanguage,
							 const RecordingTranscript &baseTranscript,
							 TranscriptionProgressCallback progressCallback = {},
							 TranscriptionCancelCallback cancelCallback = {});
	RecordingTranscript transcribeVideoRanges(const QString &videoPath, const QString &whisperModelPath,
						  const QString &transcriptionLanguage,
						  const QVector<ClipDuration> &ranges,
						  TranscriptionProgressCallback progressCallback = {},
						  TranscriptionCancelCallback cancelCallback = {});
};

RealtimeTranscriptionService *global_realtime_transcription_service();
