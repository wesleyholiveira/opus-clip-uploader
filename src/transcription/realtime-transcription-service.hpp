#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#ifdef __cplusplus
extern "C" {
#endif
#include <media-io/audio-io.h>
#ifdef __cplusplus
}
#endif

class AudioCaptureWriter;

class RealtimeTranscriptionService : public QObject {
public:
	explicit RealtimeTranscriptionService(QObject *parent = nullptr);
	~RealtimeTranscriptionService() override;

	// Starts only lightweight audio capture. Whisper/model/CUDA are loaded later, after recording stops.
	void start(const QString &whisperModelPath);
	void stopAndSaveForVideos(const QStringList &videoPaths);
	bool isRunning() const { return running; }

private:
	static void rawAudioCallback(void *param, size_t mixIdx, struct audio_data *data);
	void handleRawAudio(size_t mixIdx, struct audio_data *data);

	AudioCaptureWriter *writer = nullptr;
	bool running = false;
	int sampleRate = 48000;
	QString modelPath;
	QString capturePath;
	unsigned long long rawAudioCallbackCount = 0;
	unsigned long long queuedRawFrames = 0;
};

RealtimeTranscriptionService *global_realtime_transcription_service();
