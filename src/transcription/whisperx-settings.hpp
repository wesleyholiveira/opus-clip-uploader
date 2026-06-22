#pragma once

#include <QString>

namespace Transcription {

inline constexpr const char *CONFIG_WHISPERX_BACKEND = "whisperx_backend";
inline constexpr const char *CONFIG_WHISPERX_MODEL = "whisperx_model";
inline constexpr const char *CONFIG_WHISPERX_COMPUTE_TYPE = "whisperx_compute_type";
inline constexpr const char *CONFIG_WHISPERX_BATCH_SIZE = "whisperx_batch_size";
inline constexpr const char *CONFIG_WHISPERX_PYTHON_PATH = "whisperx_python_path";
inline constexpr const char *CONFIG_WHISPERX_WORKER_PATH = "whisperx_worker_path";
inline constexpr const char *CONFIG_WHISPERX_DEVICE = "whisperx_device";
inline constexpr const char *CONFIG_WHISPERX_FFMPEG_PATH = "whisperx_ffmpeg_path";
inline constexpr const char *WHISPERX_BACKEND_DISABLED = "disabled";
inline constexpr const char *WHISPERX_BACKEND_ALIGNMENT = "alignment";
inline constexpr const char *WHISPERX_BACKEND_PRIMARY = "primary";
// Backward-compatible value used by earlier experimental builds.
inline constexpr const char *WHISPERX_BACKEND_PYTHON = "python";

struct WhisperXSettings {
	QString backend;
	QString pythonPath;
	QString workerPath;
	QString device;
	QString ffmpegPath;
	QString model;
	QString computeType;
	int batchSize = 8;

	bool enabled() const;
	bool alignmentOnly() const;
	bool primaryTranscription() const;
};

WhisperXSettings whisperXSettingsFromConfig();
QString defaultWhisperXWorkerPath();
QString normalizeWhisperXDevice(QString value);
QString normalizeWhisperXComputeType(QString value);

} // namespace Transcription
