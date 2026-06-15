#include "transcription/realtime-transcription-service.hpp"

#include "transcription/transcript-store.hpp"

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDateTime>
#include <QCoreApplication>
#include <QProcess>

#include <memory>

#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>
#include <utility>

#include <whisper.h>

#ifndef CLIP_CROPPER_WHISPER_CUDA_REQUESTED
#define CLIP_CROPPER_WHISPER_CUDA_REQUESTED 0
#endif

namespace {
constexpr int TargetSampleRate = 16000;
constexpr int ChunkSeconds = 20;
constexpr double WhisperTimeUnitToSeconds = 0.01;

const char *boolText(bool value)
{
	return value ? "true" : "false";
}

bool transcriptionCanceled(const TranscriptionCancelCallback &cancelCallback)
{
	return cancelCallback && cancelCallback();
}

void reportTranscriptionProgress(const TranscriptionProgressCallback &progressCallback, int progress,
				 const QString &message)
{
	if (!progressCallback)
		return;

	progressCallback(qBound(0, progress, 100), message);
}

QVector<float> resampleLinear(const QVector<float> &input, int sourceRate, int targetRate)
{
	if (input.isEmpty() || sourceRate <= 0 || targetRate <= 0)
		return {};

	if (sourceRate == targetRate)
		return input;

	const double ratio = static_cast<double>(sourceRate) / static_cast<double>(targetRate);
	const int outputSize = std::max(1, static_cast<int>(input.size() / ratio));

	QVector<float> output;
	output.resize(outputSize);

	for (int i = 0; i < outputSize; ++i) {
		const double sourceIndex = i * ratio;
		const qsizetype maxIndex = input.size() - 1;

		const int index0 = static_cast<int>(
			std::clamp<qsizetype>(static_cast<qsizetype>(std::floor(sourceIndex)), 0, maxIndex));

		const int index1 =
			static_cast<int>(std::clamp<qsizetype>(static_cast<qsizetype>(index0 + 1), 0, maxIndex));
		const float frac = static_cast<float>(sourceIndex - index0);
		output[i] = input[index0] + (input[index1] - input[index0]) * frac;
	}

	return output;
}

int whisperThreadCount()
{
	const unsigned int hardwareThreads = std::thread::hardware_concurrency();
	if (hardwareThreads <= 1)
		return 1;

	// Post-recording transcription still runs inside the OBS process. Keep it conservative so
	// the review dialog/player and OBS UI are less likely to freeze after recording stops.
	return static_cast<int>(std::min<unsigned int>(hardwareThreads - 1, 4));
}

QString transcriptionTempDirectory()
{
	QString baseDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	if (baseDir.trimmed().isEmpty())
		baseDir = QDir::tempPath() + QStringLiteral("/clip-cropper");

	const QString dir = QDir(baseDir).filePath(QStringLiteral("transcription-cache"));
	QDir().mkpath(dir);
	return dir;
}

QString makeCaptureFilePath()
{
	return QDir(transcriptionTempDirectory())
		.filePath(QStringLiteral("capture-%1.f32le").arg(QDateTime::currentMSecsSinceEpoch()));
}

QString obsModuleFilePath(const QString &relativePath)
{
	const QByteArray relativePathBytes = relativePath.toUtf8();
	using ObsCharPtr = std::unique_ptr<char, decltype(&bfree)>;
	ObsCharPtr modulePath(obs_module_file(relativePathBytes.constData()), bfree);

	if (!modulePath || !modulePath.get() || modulePath.get()[0] == '\0')
		return {};

	return QString::fromUtf8(modulePath.get());
}

QStringList ffmpegExecutableCandidates()
{
	QStringList candidates;

	const QString configuredPath = QString::fromLocal8Bit(qgetenv("CLIP_CROPPER_FFMPEG_PATH")).trimmed();
	if (!configuredPath.isEmpty())
		candidates.append(configuredPath);

#ifdef Q_OS_WIN
	candidates.append(obsModuleFilePath(QStringLiteral("ffmpeg/ffmpeg.exe")));
	candidates.append(obsModuleFilePath(QStringLiteral("ffmpeg.exe")));

	const QString appDir = QCoreApplication::applicationDirPath();
	candidates.append(QDir(appDir).filePath(QStringLiteral("ffmpeg.exe")));
	candidates.append(QDir(appDir).filePath(QStringLiteral("../bin/ffmpeg.exe")));
	candidates.append(QStringLiteral("ffmpeg.exe"));
#else
	candidates.append(obsModuleFilePath(QStringLiteral("ffmpeg/ffmpeg")));
	candidates.append(obsModuleFilePath(QStringLiteral("ffmpeg")));
#endif
	candidates.append(QStringLiteral("ffmpeg"));

	candidates.removeAll(QString());
	candidates.removeDuplicates();
	return candidates;
}

QString resolveFfmpegExecutable()
{
	for (const QString &candidate : ffmpegExecutableCandidates()) {
		if (candidate.trimmed().isEmpty())
			continue;

		QProcess process;
		process.setProgram(candidate);
		process.setArguments(QStringList{QStringLiteral("-version")});
		process.setProcessChannelMode(QProcess::MergedChannels);
		process.start();

		if (!process.waitForStarted(1500))
			continue;

		if (!process.waitForFinished(3000)) {
			process.kill();
			process.waitForFinished(1000);
			continue;
		}

		if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0)
			return candidate;
	}

	return {};
}

bool extractVideoAudioToPcm16k(const QString &videoPath, const QString &pcmPath,
			       const TranscriptionProgressCallback &progressCallback,
			       const TranscriptionCancelCallback &cancelCallback)
{
	const QString ffmpeg = resolveFfmpegExecutable();
	if (ffmpeg.trimmed().isEmpty()) {
		blog(LOG_ERROR,
		     "[clip-cropper] ffmpeg executable was not found. Install ffmpeg or set CLIP_CROPPER_FFMPEG_PATH to enable on-demand video transcription.");
		return false;
	}

	if (transcriptionCanceled(cancelCallback)) {
		blog(LOG_INFO, "[clip-cropper] Audio extraction canceled before ffmpeg start.");
		return false;
	}

	QFile::remove(pcmPath);
	reportTranscriptionProgress(progressCallback, 5, QString::fromUtf8(obs_module_text("Status.ExtractingAudio")));

	QProcess process;
	process.setProgram(ffmpeg);
	process.setArguments(QStringList{
		QStringLiteral("-hide_banner"),
		QStringLiteral("-nostdin"),
		QStringLiteral("-y"),
		QStringLiteral("-i"),
		videoPath,
		QStringLiteral("-map"),
		QStringLiteral("0:a:0"),
		QStringLiteral("-vn"),
		QStringLiteral("-sn"),
		QStringLiteral("-dn"),
		QStringLiteral("-ac"),
		QStringLiteral("1"),
		QStringLiteral("-ar"),
		QStringLiteral("16000"),
		QStringLiteral("-acodec"),
		QStringLiteral("pcm_f32le"),
		QStringLiteral("-f"),
		QStringLiteral("f32le"),
		pcmPath,
	});
	process.setProcessChannelMode(QProcess::MergedChannels);

	blog(LOG_INFO, "[clip-cropper] Extracting video audio for on-demand transcription. ffmpeg=%s video=%s pcm=%s",
	     ffmpeg.toUtf8().constData(), videoPath.toUtf8().constData(), pcmPath.toUtf8().constData());

	process.start();
	if (!process.waitForStarted(5000)) {
		blog(LOG_ERROR, "[clip-cropper] Failed to start ffmpeg for on-demand transcription. ffmpeg=%s",
		     ffmpeg.toUtf8().constData());
		return false;
	}

	QByteArray output;
	int progressTick = 5;
	while (!process.waitForFinished(200)) {
		output.append(process.readAll());
		if (output.size() > 8192)
			output.remove(0, output.size() - 8192);

		if (transcriptionCanceled(cancelCallback)) {
			blog(LOG_INFO, "[clip-cropper] Audio extraction canceled. Killing ffmpeg process.");
			process.kill();
			process.waitForFinished(3000);
			QFile::remove(pcmPath);
			return false;
		}

		progressTick = std::min(progressTick + 1, 25);
		reportTranscriptionProgress(progressCallback, progressTick,
					    QString::fromUtf8(obs_module_text("Status.ExtractingAudio")));
	}
	output.append(process.readAll());
	if (output.size() > 8192)
		output.remove(0, output.size() - 8192);

	if (transcriptionCanceled(cancelCallback)) {
		blog(LOG_INFO, "[clip-cropper] Audio extraction canceled after ffmpeg finished.");
		QFile::remove(pcmPath);
		return false;
	}

	if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
		blog(LOG_ERROR,
		     "[clip-cropper] ffmpeg failed while extracting audio for on-demand transcription. exitCode=%d output=%s",
		     process.exitCode(), output.constData());
		QFile::remove(pcmPath);
		return false;
	}

	const QFileInfo pcmInfo(pcmPath);
	if (!pcmInfo.isFile() || pcmInfo.size() <= 0) {
		blog(LOG_ERROR,
		     "[clip-cropper] ffmpeg finished but no PCM audio was produced for on-demand transcription. pcm=%s",
		     pcmPath.toUtf8().constData());
		QFile::remove(pcmPath);
		return false;
	}

	reportTranscriptionProgress(progressCallback, 30,
				    QString::fromUtf8(obs_module_text("Status.TranscribingAudio")));

	blog(LOG_INFO, "[clip-cropper] Video audio extracted for on-demand transcription. pcm=%s sizeBytes=%lld",
	     pcmPath.toUtf8().constData(), static_cast<long long>(pcmInfo.size()));
	return true;
}
} // namespace

class PostRecordingTranscriptionJob {
public:
	PostRecordingTranscriptionJob(QString modelPath, QString capturePath, int sourceSampleRate,
				      QStringList videoPaths, TranscriptionProgressCallback progressCallback = {},
				      TranscriptionCancelCallback cancelCallback = {})
		: modelPath(std::move(modelPath)),
		  capturePath(std::move(capturePath)),
		  sourceSampleRate(sourceSampleRate),
		  videoPaths(std::move(videoPaths)),
		  progressCallback(std::move(progressCallback)),
		  cancelCallback(std::move(cancelCallback))
	{
	}

	RecordingTranscript transcribe()
	{
		RecordingTranscript transcript;

		if (transcriptionCanceled(cancelCallback)) {
			blog(LOG_INFO, "[clip-cropper] Transcription canceled before Whisper model load.");
			return transcript;
		}

		if (modelPath.trimmed().isEmpty()) {
			blog(LOG_INFO, "[clip-cropper] Whisper model path is empty; transcription skipped.");
			return transcript;
		}

		QFile captureFile(capturePath);
		if (!captureFile.open(QIODevice::ReadOnly)) {
			blog(LOG_ERROR, "[clip-cropper] Failed to open transcription capture for reading: %s",
			     capturePath.toUtf8().constData());
			return transcript;
		}

		const QFileInfo modelInfo(modelPath);
		blog(LOG_INFO,
		     "[clip-cropper] Loading Whisper model for transcription. path=%s exists=%s sizeBytes=%lld cudaRequested=%s",
		     modelPath.toUtf8().constData(), boolText(modelInfo.isFile()),
		     static_cast<long long>(modelInfo.size()), boolText(CLIP_CROPPER_WHISPER_CUDA_REQUESTED != 0));

		const char *systemInfo = whisper_print_system_info();
		blog(LOG_INFO, "[clip-cropper] Whisper system info: %s", systemInfo ? systemInfo : "<unavailable>");
		blog(LOG_INFO, "[clip-cropper] Whisper CUDA compile flag CLIP_CROPPER_WHISPER_CUDA_REQUESTED=%s",
		     boolText(CLIP_CROPPER_WHISPER_CUDA_REQUESTED != 0));

		whisper_context_params contextParams = whisper_context_default_params();
#if CLIP_CROPPER_WHISPER_CUDA_REQUESTED
		contextParams.use_gpu = true;
#else
		contextParams.use_gpu = false;
#endif

		blog(LOG_INFO, "[clip-cropper] Whisper context params prepared. use_gpu=%s",
		     boolText(contextParams.use_gpu));

		using WhisperContextPtr = std::unique_ptr<whisper_context, decltype(&whisper_free)>;
		WhisperContextPtr ctx(whisper_init_from_file_with_params(modelPath.toUtf8().constData(), contextParams),
				      &whisper_free);
		if (!ctx) {
			blog(LOG_ERROR,
			     "[clip-cropper] Failed to load Whisper model for transcription. path=%s use_gpu=%s cudaRequested=%s",
			     modelPath.toUtf8().constData(), boolText(contextParams.use_gpu),
			     boolText(CLIP_CROPPER_WHISPER_CUDA_REQUESTED != 0));
			return transcript;
		}

		blog(LOG_INFO, "[clip-cropper] Whisper model loaded successfully. use_gpu=%s cudaRequested=%s",
		     boolText(contextParams.use_gpu), boolText(CLIP_CROPPER_WHISPER_CUDA_REQUESTED != 0));

		const int chunkSamples = sourceSampleRate * ChunkSeconds;
		const qint64 totalSourceSamples =
			std::max<qint64>(1, captureFile.size() / static_cast<qint64>(sizeof(float)));
		qint64 processedSourceSamples = 0;

		reportTranscriptionProgress(progressCallback, 30,
					    QString::fromUtf8(obs_module_text("Status.TranscribingAudio")));

		while (!captureFile.atEnd()) {
			if (transcriptionCanceled(cancelCallback)) {
				blog(LOG_INFO, "[clip-cropper] Transcription canceled before next Whisper chunk.");
				break;
			}
			QVector<float> chunk;
			chunk.resize(chunkSamples);

			const qint64 requestedBytes =
				static_cast<qint64>(chunkSamples) * static_cast<qint64>(sizeof(float));
			const qint64 readBytes =
				captureFile.read(reinterpret_cast<char *>(chunk.data()), requestedBytes);
			if (readBytes <= 0)
				break;

			const int readSamples = static_cast<int>(readBytes / static_cast<qint64>(sizeof(float)));
			chunk.resize(readSamples);

			processChunk(ctx.get(), chunk, processedSourceSamples, transcript);
			processedSourceSamples += readSamples;

			const int progress = 30 + static_cast<int>((processedSourceSamples * 60) / totalSourceSamples);
			reportTranscriptionProgress(progressCallback, progress,
						    QString::fromUtf8(obs_module_text("Status.TranscribingAudio")));
		}

		captureFile.close();

		if (!transcriptionCanceled(cancelCallback))
			reportTranscriptionProgress(progressCallback, 90,
						    QString::fromUtf8(obs_module_text("Status.TranscriptionComplete")));

		blog(LOG_INFO, "[clip-cropper] Transcription finalized with %lld segment(s). canceled=%s",
		     static_cast<long long>(transcript.segments.size()),
		     boolText(transcriptionCanceled(cancelCallback)));
		return transcript;
	}

	void run()
	{
		if (videoPaths.isEmpty()) {
			blog(LOG_INFO,
			     "[clip-cropper] Post-recording transcription skipped because there are no video paths.");
			QFile::remove(capturePath);
			return;
		}

		RecordingTranscript transcript = transcribe();

		for (const QString &videoPath : videoPaths) {
			if (videoPath.trimmed().isEmpty() || transcript.segments.isEmpty())
				continue;

			RecordingTranscript transcriptForVideo = transcript;
			transcriptForVideo.videoPath = videoPath;
			transcriptForVideo.videoFileName = QFileInfo(videoPath).fileName();
			TranscriptStore::saveForVideoPath(videoPath, transcriptForVideo);
			blog(LOG_INFO, "[clip-cropper] Transcript saved for %s with %d segment(s).",
			     videoPath.toUtf8().constData(), static_cast<int>(transcriptForVideo.segments.size()));
		}

		QFile::remove(capturePath);
	}

private:
	void processChunk(whisper_context *ctx, const QVector<float> &chunk, qint64 chunkStartSourceSample,
			  RecordingTranscript &transcript)
	{
		if (!ctx || chunk.isEmpty())
			return;

		const double chunkStartSec = chunkStartSourceSample / static_cast<double>(sourceSampleRate);
		QVector<float> pcm16k = resampleLinear(chunk, sourceSampleRate, TargetSampleRate);
		if (pcm16k.isEmpty()) {
			blog(LOG_WARNING,
			     "[clip-cropper] Whisper chunk skipped because resampled PCM is empty. inputSamples=%lld sourceRate=%d",
			     static_cast<long long>(chunk.size()), sourceSampleRate);
			return;
		}

		whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
		params.print_progress = false;
		params.print_realtime = false;
		params.print_timestamps = true;
		params.translate = false;
		params.language = "pt";
		params.n_threads = whisperThreadCount();

		const int sampleCount =
			static_cast<int>(std::min<qsizetype>(pcm16k.size(), std::numeric_limits<int>::max()));

		blog(LOG_INFO,
		     "[clip-cropper] Whisper processing chunk. sourceSamples=%lld pcm16kSamples=%d startSec=%.2f threads=%d",
		     static_cast<long long>(chunk.size()), sampleCount, chunkStartSec, params.n_threads);

		const int result = whisper_full(ctx, params, pcm16k.constData(), sampleCount);
		if (result != 0) {
			blog(LOG_WARNING, "[clip-cropper] Whisper transcription failed for chunk. result=%d", result);
			return;
		}

		const int segmentCount = whisper_full_n_segments(ctx);
		blog(LOG_INFO, "[clip-cropper] Whisper chunk completed. rawSegmentCount=%d", segmentCount);
		for (int i = 0; i < segmentCount; ++i) {
			const char *rawText = whisper_full_get_segment_text(ctx, i);
			const QString text = QString::fromUtf8(rawText ? rawText : "").trimmed();
			if (text.isEmpty())
				continue;

			TranscriptSegment segment;
			segment.startSec =
				chunkStartSec + whisper_full_get_segment_t0(ctx, i) * WhisperTimeUnitToSeconds;
			segment.endSec = chunkStartSec + whisper_full_get_segment_t1(ctx, i) * WhisperTimeUnitToSeconds;
			segment.text = text;
			transcript.segments.append(segment);
		}
	}

	QString modelPath;
	QString capturePath;
	int sourceSampleRate = 48000;
	QStringList videoPaths;
	TranscriptionProgressCallback progressCallback;
	TranscriptionCancelCallback cancelCallback;
};

RealtimeTranscriptionService::RealtimeTranscriptionService(QObject *parent) : QObject(parent) {}

RealtimeTranscriptionService::~RealtimeTranscriptionService() = default;

RecordingTranscript RealtimeTranscriptionService::transcribeVideoFile(const QString &videoPath,
								      const QString &whisperModelPath,
								      TranscriptionProgressCallback progressCallback,
								      TranscriptionCancelCallback cancelCallback)
{
	RecordingTranscript cached = TranscriptStore::loadForVideoPath(videoPath);
	if (!cached.segments.isEmpty()) {
		reportTranscriptionProgress(progressCallback, 100,
					    QString::fromUtf8(obs_module_text("Status.TranscriptionComplete")));
		blog(LOG_INFO, "[clip-cropper] Transcript cache hit before on-demand transcription: %s",
		     videoPath.toUtf8().constData());
		return cached;
	}

	const QFileInfo videoInfo(videoPath);
	if (!videoInfo.isFile()) {
		blog(LOG_ERROR, "[clip-cropper] Cannot transcribe missing video file: %s",
		     videoPath.toUtf8().constData());
		return {};
	}

	if (whisperModelPath.trimmed().isEmpty() || !QFileInfo(whisperModelPath).isFile()) {
		blog(LOG_ERROR, "[clip-cropper] Cannot transcribe video because Whisper model is missing: %s",
		     whisperModelPath.toUtf8().constData());
		return {};
	}

	const QString pcmPath = makeCaptureFilePath();
	if (!extractVideoAudioToPcm16k(videoPath, pcmPath, progressCallback, cancelCallback))
		return {};

	PostRecordingTranscriptionJob job(whisperModelPath, pcmPath, TargetSampleRate, QStringList{videoPath},
					  progressCallback, cancelCallback);
	RecordingTranscript transcript = job.transcribe();
	QFile::remove(pcmPath);

	if (transcriptionCanceled(cancelCallback)) {
		blog(LOG_INFO, "[clip-cropper] On-demand transcription canceled for %s",
		     videoPath.toUtf8().constData());
		return {};
	}

	if (transcript.segments.isEmpty()) {
		blog(LOG_WARNING, "[clip-cropper] On-demand transcription produced no segments for %s",
		     videoPath.toUtf8().constData());
		return {};
	}

	transcript.videoPath = videoPath;
	transcript.videoFileName = videoInfo.fileName();
	reportTranscriptionProgress(progressCallback, 95,
				    QString::fromUtf8(obs_module_text("Status.SavingTranscript")));
	TranscriptStore::saveForVideoPath(videoPath, transcript);
	reportTranscriptionProgress(progressCallback, 100,
				    QString::fromUtf8(obs_module_text("Status.TranscriptionComplete")));
	blog(LOG_INFO, "[clip-cropper] On-demand transcript saved for %s with %d segment(s).",
	     videoPath.toUtf8().constData(), static_cast<int>(transcript.segments.size()));
	return transcript;
}

RealtimeTranscriptionService *global_realtime_transcription_service()
{
	static RealtimeTranscriptionService service;
	return &service;
}
