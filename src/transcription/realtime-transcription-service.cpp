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
#include <QRegularExpression>

#include <memory>

#include <algorithm>
#include <cmath>
#include <cstring>
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
constexpr const char *AutoLanguage = "auto";

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

QString normalizeWhisperLanguage(QString language)
{
	language = language.trimmed().toLower();

	if (language.isEmpty() || language == QStringLiteral("auto"))
		return QStringLiteral("auto");

	if (language == QStringLiteral("pt-br") || language == QStringLiteral("portuguese"))
		return QStringLiteral("pt");

	if (language == QStringLiteral("en-us") || language == QStringLiteral("english"))
		return QStringLiteral("en");

	return language;
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

int detectAudioStreamCount(const QString &videoPath)
{
	const QString ffmpeg = resolveFfmpegExecutable();
	if (ffmpeg.trimmed().isEmpty())
		return 1;

	QProcess process;
	process.setProgram(ffmpeg);
	process.setArguments(QStringList{
		QStringLiteral("-hide_banner"),
		QStringLiteral("-i"),
		videoPath,
	});
	process.setProcessChannelMode(QProcess::MergedChannels);
	process.start();

	if (!process.waitForStarted(3000))
		return 1;

	if (!process.waitForFinished(5000)) {
		process.kill();
		process.waitForFinished(1000);
	}

	const QString output = QString::fromUtf8(process.readAll());
	const QRegularExpression streamRegex(QStringLiteral(R"(Stream #0:\d+(?:\[[^\]]+\])?(?:\([^)]*\))?: Audio:)"));
	QRegularExpressionMatchIterator it = streamRegex.globalMatch(output);
	int count = 0;
	while (it.hasNext()) {
		it.next();
		++count;
	}

	if (count <= 0)
		count = 1;

	blog(LOG_INFO, "[clip-cropper] Detected audio stream count for transcription. video=%s audioStreams=%d",
	     videoPath.toUtf8().constData(), count);
	return count;
}

bool pcmContainsAudibleSignal(const QString &pcmPath)
{
	QFile file(pcmPath);
	if (!file.open(QIODevice::ReadOnly))
		return true;

	constexpr qint64 MaxBytesToInspect = 8 * 1024 * 1024;
	const QByteArray bytes = file.read(MaxBytesToInspect);
	file.close();

	const qsizetype sampleCount = bytes.size() / static_cast<qsizetype>(sizeof(float));
	if (sampleCount <= 0)
		return false;

	double sumSquares = 0.0;
	double maxAbs = 0.0;
	for (qsizetype i = 0; i < sampleCount; ++i) {
		float sample = 0.0f;
		std::memcpy(&sample, bytes.constData() + i * static_cast<qsizetype>(sizeof(float)), sizeof(float));
		const double value = static_cast<double>(sample);
		if (!std::isfinite(value))
			continue;

		const double absValue = std::abs(value);
		maxAbs = std::max(maxAbs, absValue);
		sumSquares += value * value;
	}

	const double rms = std::sqrt(sumSquares / static_cast<double>(sampleCount));
	const bool audible = maxAbs > 0.00001 || rms > 0.000001;
	blog(LOG_INFO,
	     "[clip-cropper] PCM signal check for transcription. pcm=%s samples=%lld maxAbs=%.8f rms=%.8f audible=%s",
	     pcmPath.toUtf8().constData(), static_cast<long long>(sampleCount), maxAbs, rms, boolText(audible));
	return audible;
}

bool extractVideoAudioToPcm16k(const QString &videoPath, const QString &pcmPath, int audioStreamIndex,
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
		QStringLiteral("0:a:%1").arg(std::max(0, audioStreamIndex)),
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

	blog(LOG_INFO,
	     "[clip-cropper] Extracting video audio for on-demand transcription. ffmpeg=%s video=%s audioStream=%d pcm=%s",
	     ffmpeg.toUtf8().constData(), videoPath.toUtf8().constData(), std::max(0, audioStreamIndex),
	     pcmPath.toUtf8().constData());

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

bool extractVideoAudioRangeToPcm16k(const QString &videoPath, const QString &pcmPath, double startSec,
				    double durationSec, int audioStreamIndex,
				    const TranscriptionProgressCallback &progressCallback,
				    const TranscriptionCancelCallback &cancelCallback)
{
	const QString ffmpeg = resolveFfmpegExecutable();
	if (ffmpeg.trimmed().isEmpty()) {
		blog(LOG_ERROR,
		     "[clip-cropper] ffmpeg executable was not found. Install ffmpeg or set CLIP_CROPPER_FFMPEG_PATH to enable on-demand video transcription.");
		return false;
	}

	if (durationSec <= 0.0)
		return false;

	if (transcriptionCanceled(cancelCallback)) {
		blog(LOG_INFO, "[clip-cropper] Audio range extraction canceled before ffmpeg start.");
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
		QStringLiteral("-ss"),
		QString::number(std::max(0.0, startSec), 'f', 3),
		QStringLiteral("-t"),
		QString::number(std::max(0.0, durationSec), 'f', 3),
		QStringLiteral("-i"),
		videoPath,
		QStringLiteral("-map"),
		QStringLiteral("0:a:%1").arg(std::max(0, audioStreamIndex)),
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

	blog(LOG_INFO,
	     "[clip-cropper] Extracting selected video audio range for on-demand transcription. ffmpeg=%s video=%s audioStream=%d startSec=%.3f durationSec=%.3f pcm=%s",
	     ffmpeg.toUtf8().constData(), videoPath.toUtf8().constData(), std::max(0, audioStreamIndex), startSec,
	     durationSec, pcmPath.toUtf8().constData());

	process.start();
	if (!process.waitForStarted(5000)) {
		blog(LOG_ERROR, "[clip-cropper] Failed to start ffmpeg for selected range transcription. ffmpeg=%s",
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
			blog(LOG_INFO, "[clip-cropper] Audio range extraction canceled. Killing ffmpeg process.");
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

	if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
		blog(LOG_ERROR,
		     "[clip-cropper] ffmpeg failed while extracting selected audio range. exitCode=%d output=%s",
		     process.exitCode(), output.constData());
		QFile::remove(pcmPath);
		return false;
	}

	const QFileInfo pcmInfo(pcmPath);
	if (!pcmInfo.isFile() || pcmInfo.size() <= 0) {
		blog(LOG_ERROR,
		     "[clip-cropper] ffmpeg finished but no PCM audio was produced for selected range transcription. pcm=%s",
		     pcmPath.toUtf8().constData());
		QFile::remove(pcmPath);
		return false;
	}

	blog(LOG_INFO,
	     "[clip-cropper] Selected video audio range extracted for on-demand transcription. pcm=%s sizeBytes=%lld",
	     pcmPath.toUtf8().constData(), static_cast<long long>(pcmInfo.size()));
	return true;
}
} // namespace

class PostRecordingTranscriptionJob {
public:
	PostRecordingTranscriptionJob(QString modelPath, QString capturePath, int sourceSampleRate,
				      QStringList videoPaths,
				      QString transcriptionLanguage = QString::fromLatin1(AutoLanguage),
				      TranscriptionProgressCallback progressCallback = {},
				      TranscriptionCancelCallback cancelCallback = {})
		: modelPath(std::move(modelPath)),
		  capturePath(std::move(capturePath)),
		  sourceSampleRate(sourceSampleRate),
		  videoPaths(std::move(videoPaths)),
		  transcriptionLanguage(normalizeWhisperLanguage(std::move(transcriptionLanguage))),
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

		while (processedSourceSamples < totalSourceSamples) {
			if (transcriptionCanceled(cancelCallback)) {
				blog(LOG_INFO, "[clip-cropper] Transcription canceled before next Whisper chunk.");
				break;
			}

			if (!captureFile.seek(processedSourceSamples * static_cast<qint64>(sizeof(float)))) {
				blog(LOG_WARNING,
				     "[clip-cropper] Failed to seek transcription capture. sampleOffset=%lld",
				     static_cast<long long>(processedSourceSamples));
				break;
			}

			QVector<float> chunk;
			const qint64 remainingSamples = totalSourceSamples - processedSourceSamples;
			const int samplesToRead = static_cast<int>(std::min<qint64>(chunkSamples, remainingSamples));
			chunk.resize(samplesToRead);

			const qint64 requestedBytes =
				static_cast<qint64>(samplesToRead) * static_cast<qint64>(sizeof(float));
			const qint64 readBytes =
				captureFile.read(reinterpret_cast<char *>(chunk.data()), requestedBytes);
			if (readBytes <= 0) {
				blog(LOG_WARNING,
				     "[clip-cropper] Transcription capture read returned no data. requestedBytes=%lld sampleOffset=%lld fileSize=%lld",
				     static_cast<long long>(requestedBytes),
				     static_cast<long long>(processedSourceSamples),
				     static_cast<long long>(captureFile.size()));
				break;
			}

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
			TranscriptStore::saveForVideoPath(videoPath, transcriptionLanguage, transcriptForVideo);
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

		const QString whisperLanguage = transcriptionLanguage;
		const QByteArray whisperLanguageBytes =
			whisperLanguage == QStringLiteral("auto") ? QByteArray() : whisperLanguage.toUtf8();
		params.language = whisperLanguageBytes.isEmpty() ? nullptr : whisperLanguageBytes.constData();
		params.detect_language = whisperLanguageBytes.isEmpty();
		params.n_threads = whisperThreadCount();

		const int sampleCount =
			static_cast<int>(std::min<qsizetype>(pcm16k.size(), std::numeric_limits<int>::max()));

		blog(LOG_INFO,
		     "[clip-cropper] Whisper processing chunk. sourceSamples=%lld pcm16kSamples=%d startSec=%.2f language=%s threads=%d",
		     static_cast<long long>(chunk.size()), sampleCount, chunkStartSec,
		     whisperLanguage.toUtf8().constData(), params.n_threads);

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
	QString transcriptionLanguage = QString::fromLatin1(AutoLanguage);
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
	return transcribeVideoFile(videoPath, whisperModelPath, QString::fromLatin1(AutoLanguage), progressCallback,
				   cancelCallback);
}

RecordingTranscript RealtimeTranscriptionService::transcribeVideoFile(const QString &videoPath,
								      const QString &whisperModelPath,
								      const QString &transcriptionLanguage,
								      TranscriptionProgressCallback progressCallback,
								      TranscriptionCancelCallback cancelCallback)
{
	const QString normalizedLanguage = normalizeWhisperLanguage(transcriptionLanguage);
	RecordingTranscript cached = TranscriptStore::loadForVideoPath(videoPath, normalizedLanguage);
	if (!cached.segments.isEmpty()) {
		reportTranscriptionProgress(progressCallback, 100,
					    QString::fromUtf8(obs_module_text("Status.TranscriptionComplete")));
		blog(LOG_INFO,
		     "[clip-cropper] Transcript cache hit before on-demand transcription. Skipping Whisper/GPU transcription. video=%s segments=%d language=%s cacheKey=%s",
		     videoPath.toUtf8().constData(), static_cast<int>(cached.segments.size()),
		     normalizedLanguage.toUtf8().constData(),
		     TranscriptStore::keyForVideoPath(videoPath, normalizedLanguage).toUtf8().constData());
		return cached;
	}

	blog(LOG_INFO,
	     "[clip-cropper] Transcript cache miss. Whisper/GPU transcription will run. video=%s language=%s cacheKey=%s",
	     videoPath.toUtf8().constData(), normalizedLanguage.toUtf8().constData(),
	     TranscriptStore::keyForVideoPath(videoPath, normalizedLanguage).toUtf8().constData());

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

	RecordingTranscript transcript;
	const int audioStreamCount = detectAudioStreamCount(videoPath);
	for (int audioStreamIndex = 0; audioStreamIndex < audioStreamCount; ++audioStreamIndex) {
		if (transcriptionCanceled(cancelCallback)) {
			blog(LOG_INFO, "[clip-cropper] On-demand transcription canceled for %s",
			     videoPath.toUtf8().constData());
			return {};
		}

		const QString pcmPath = makeCaptureFilePath();
		if (!extractVideoAudioToPcm16k(videoPath, pcmPath, audioStreamIndex, progressCallback,
					       cancelCallback)) {
			QFile::remove(pcmPath);
			continue;
		}

		if (!pcmContainsAudibleSignal(pcmPath)) {
			blog(LOG_WARNING,
			     "[clip-cropper] Extracted audio stream appears silent. Trying next audio stream if available. video=%s audioStream=%d",
			     videoPath.toUtf8().constData(), audioStreamIndex);
			QFile::remove(pcmPath);
			continue;
		}

		PostRecordingTranscriptionJob job(whisperModelPath, pcmPath, TargetSampleRate, QStringList{videoPath},
						  normalizedLanguage, progressCallback, cancelCallback);
		transcript = job.transcribe();
		QFile::remove(pcmPath);

		if (transcriptionCanceled(cancelCallback)) {
			blog(LOG_INFO, "[clip-cropper] On-demand transcription canceled for %s",
			     videoPath.toUtf8().constData());
			return {};
		}

		if (!transcript.segments.isEmpty()) {
			blog(LOG_INFO,
			     "[clip-cropper] On-demand transcription selected audio stream. video=%s audioStream=%d segments=%d",
			     videoPath.toUtf8().constData(), audioStreamIndex,
			     static_cast<int>(transcript.segments.size()));
			break;
		}

		blog(LOG_WARNING,
		     "[clip-cropper] On-demand transcription produced no segments for audio stream. video=%s audioStream=%d/%d",
		     videoPath.toUtf8().constData(), audioStreamIndex + 1, audioStreamCount);
	}

	if (transcript.segments.isEmpty()) {
		blog(LOG_WARNING,
		     "[clip-cropper] On-demand transcription produced no segments for any audio stream: %s",
		     videoPath.toUtf8().constData());
		return {};
	}

	transcript.videoPath = videoPath;
	transcript.videoFileName = videoInfo.fileName();
	reportTranscriptionProgress(progressCallback, 95,
				    QString::fromUtf8(obs_module_text("Status.SavingTranscript")));
	TranscriptStore::saveForVideoPath(videoPath, normalizedLanguage, transcript);
	reportTranscriptionProgress(progressCallback, 100,
				    QString::fromUtf8(obs_module_text("Status.TranscriptionComplete")));
	blog(LOG_INFO, "[clip-cropper] On-demand transcript saved for %s with %d segment(s). language=%s cacheKey=%s",
	     videoPath.toUtf8().constData(), static_cast<int>(transcript.segments.size()),
	     normalizedLanguage.toUtf8().constData(),
	     TranscriptStore::keyForVideoPath(videoPath, normalizedLanguage).toUtf8().constData());
	return transcript;
}

RecordingTranscript RealtimeTranscriptionService::transcribeVideoRanges(const QString &videoPath,
									const QString &whisperModelPath,
									const QString &transcriptionLanguage,
									const QVector<ClipDuration> &ranges,
									TranscriptionProgressCallback progressCallback,
									TranscriptionCancelCallback cancelCallback)
{
	RecordingTranscript combined;
	combined.videoPath = videoPath;
	combined.videoFileName = QFileInfo(videoPath).fileName();

	const QString normalizedLanguage = normalizeWhisperLanguage(transcriptionLanguage);
	const QFileInfo videoInfo(videoPath);
	if (!videoInfo.isFile()) {
		blog(LOG_ERROR, "[clip-cropper] Cannot transcribe selected ranges because video file is missing: %s",
		     videoPath.toUtf8().constData());
		return combined;
	}

	if (whisperModelPath.trimmed().isEmpty() || !QFileInfo(whisperModelPath).isFile()) {
		blog(LOG_ERROR, "[clip-cropper] Cannot transcribe selected ranges because Whisper model is missing: %s",
		     whisperModelPath.toUtf8().constData());
		return combined;
	}

	QVector<ClipDuration> validRanges;
	for (const ClipDuration &range : ranges) {
		const double startSec = std::max(0.0, range.startSec);
		const double endSec = std::max(startSec, range.endSec);
		if (endSec > startSec)
			validRanges.append({startSec, endSec});
	}

	if (validRanges.isEmpty())
		return transcribeVideoFile(videoPath, whisperModelPath, normalizedLanguage, progressCallback,
					   cancelCallback);

	blog(LOG_INFO, "[clip-cropper] Starting selected-range transcription. video=%s ranges=%d language=%s",
	     videoPath.toUtf8().constData(), static_cast<int>(validRanges.size()),
	     normalizedLanguage.toUtf8().constData());

	const int audioStreamCount = detectAudioStreamCount(videoPath);

	for (qsizetype index = 0; index < validRanges.size(); ++index) {
		if (transcriptionCanceled(cancelCallback)) {
			blog(LOG_INFO, "[clip-cropper] Selected-range transcription canceled before range %d.",
			     static_cast<int>(index + 1));
			return {};
		}

		const ClipDuration range = validRanges.at(index);
		const double durationSec = range.endSec - range.startSec;

		auto rangeProgress = [progressCallback, index, count = validRanges.size()](int progress,
											   const QString &message) {
			if (!progressCallback)
				return;

			const int base = static_cast<int>((index * 100) / std::max<qsizetype>(1, count));
			const int span = static_cast<int>(100 / std::max<qsizetype>(1, count));
			progressCallback(qBound(0, base + (progress * span) / 100, 100), message);
		};

		RecordingTranscript rangeTranscript;
		int selectedAudioStream = -1;
		for (int audioStreamIndex = 0; audioStreamIndex < audioStreamCount; ++audioStreamIndex) {
			if (transcriptionCanceled(cancelCallback)) {
				blog(LOG_INFO,
				     "[clip-cropper] Selected-range transcription canceled before audio stream %d for range %d.",
				     audioStreamIndex + 1, static_cast<int>(index + 1));
				return {};
			}

			const QString pcmPath = makeCaptureFilePath();
			if (!extractVideoAudioRangeToPcm16k(videoPath, pcmPath, range.startSec, durationSec,
							    audioStreamIndex, rangeProgress, cancelCallback)) {
				QFile::remove(pcmPath);
				continue;
			}

			if (!pcmContainsAudibleSignal(pcmPath)) {
				blog(LOG_WARNING,
				     "[clip-cropper] Extracted selected-range audio stream appears silent. Trying next audio stream if available. video=%s range=%d/%d audioStream=%d",
				     videoPath.toUtf8().constData(), static_cast<int>(index + 1),
				     static_cast<int>(validRanges.size()), audioStreamIndex);
				QFile::remove(pcmPath);
				continue;
			}

			PostRecordingTranscriptionJob job(whisperModelPath, pcmPath, TargetSampleRate,
							  QStringList{videoPath}, normalizedLanguage, rangeProgress,
							  cancelCallback);
			rangeTranscript = job.transcribe();
			QFile::remove(pcmPath);

			if (!rangeTranscript.segments.isEmpty()) {
				selectedAudioStream = audioStreamIndex;
				break;
			}

			blog(LOG_WARNING,
			     "[clip-cropper] Selected range transcription produced no segments for audio stream. range=%d/%d audioStream=%d/%d startSec=%.2f endSec=%.2f",
			     static_cast<int>(index + 1), static_cast<int>(validRanges.size()), audioStreamIndex + 1,
			     audioStreamCount, range.startSec, range.endSec);
		}

		for (TranscriptSegment segment : rangeTranscript.segments) {
			segment.startSec += range.startSec;
			segment.endSec += range.startSec;
			combined.segments.append(segment);
		}

		blog(LOG_INFO,
		     "[clip-cropper] Selected range transcription completed. range=%d/%d startSec=%.2f endSec=%.2f audioStream=%d segments=%d",
		     static_cast<int>(index + 1), static_cast<int>(validRanges.size()), range.startSec, range.endSec,
		     selectedAudioStream, static_cast<int>(rangeTranscript.segments.size()));
	}

	if (!combined.segments.isEmpty())
		reportTranscriptionProgress(progressCallback, 100,
					    QString::fromUtf8(obs_module_text("Status.TranscriptionComplete")));

	blog(LOG_INFO, "[clip-cropper] Selected-range transcription finalized with %d segment(s).",
	     static_cast<int>(combined.segments.size()));
	return combined;
}

RealtimeTranscriptionService *global_realtime_transcription_service()
{
	static RealtimeTranscriptionService service;
	return &service;
}
