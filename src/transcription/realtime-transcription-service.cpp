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

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

#include <whisper.h>

namespace {
constexpr int TargetSampleRate = 16000;
constexpr int ChunkSeconds = 20;
constexpr int MaxQueuedCaptureSeconds = 30;
constexpr double WhisperTimeUnitToSeconds = 0.01;
constexpr unsigned long long AudioCallbackLogInterval = 100;

const char *boolText(bool value)
{
	return value ? "true" : "false";
}

QVector<float> downmixFirstPlaneToMono(const struct audio_data *data)
{
	QVector<float> samples;
	if (!data || !data->data[0] || data->frames == 0)
		return samples;

	const auto frameCount =
		static_cast<int>(std::min<size_t>(data->frames, static_cast<size_t>(std::numeric_limits<int>::max())));
	samples.resize(frameCount);
	const float *source = reinterpret_cast<const float *>(data->data[0]);
	std::copy(source, source + frameCount, samples.begin());
	return samples;
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
} // namespace

class AudioCaptureWriter {
public:
	AudioCaptureWriter(QString capturePath, int sourceSampleRate)
		: capturePath(std::move(capturePath)),
		  sourceSampleRate(sourceSampleRate)
	{
	}

	~AudioCaptureWriter()
	{
		requestStop();
		join();
	}

	bool start()
	{
		file.setFileName(capturePath);
		if (!file.open(QIODevice::WriteOnly)) {
			blog(LOG_ERROR, "[clip-cropper] Failed to open transcription capture file: %s",
			     capturePath.toUtf8().constData());
			return false;
		}

		thread = std::thread(&AudioCaptureWriter::run, this);
		return true;
	}

	void appendSamples(QVector<float> samples)
	{
		if (samples.isEmpty())
			return;

		{
			std::lock_guard<std::mutex> lock(mutex);
			if (stopping)
				return;

			queuedSamples.fetch_add(samples.size());
			queue.push_back(std::move(samples));

			const qint64 maxQueuedSamples = static_cast<qint64>(sourceSampleRate) * MaxQueuedCaptureSeconds;
			while (pendingSamplesNoLock() > maxQueuedSamples && !queue.empty()) {
				droppedSamples.fetch_add(queue.front().size());
				queue.pop_front();
			}
		}

		condition.notify_one();
	}

	void requestStop()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
		}
		condition.notify_one();
	}

	void join()
	{
		if (thread.joinable())
			thread.join();
	}

	QString path() const { return capturePath; }
	qint64 writtenSampleCount() const { return writtenSamples.load(); }
	qint64 droppedSampleCount() const { return droppedSamples.load(); }

private:
	qint64 pendingSamplesNoLock() const
	{
		qint64 total = 0;
		for (const QVector<float> &chunk : queue)
			total += chunk.size();
		return total;
	}

	void run()
	{
		while (true) {
			QVector<float> samples;

			{
				std::unique_lock<std::mutex> lock(mutex);
				condition.wait(lock, [this]() { return stopping || !queue.empty(); });

				if (queue.empty()) {
					if (stopping)
						break;
					continue;
				}

				samples = std::move(queue.front());
				queue.pop_front();
			}

			if (!samples.isEmpty()) {
				const auto bytes = reinterpret_cast<const char *>(samples.constData());
				const qint64 byteCount =
					static_cast<qint64>(samples.size()) * static_cast<qint64>(sizeof(float));
				const qint64 written = file.write(bytes, byteCount);
				if (written != byteCount) {
					blog(LOG_WARNING,
					     "[clip-cropper] Incomplete transcription capture write. expectedBytes=%lld writtenBytes=%lld",
					     static_cast<long long>(byteCount), static_cast<long long>(written));
				}
				writtenSamples.fetch_add(samples.size());
			}
		}

		file.flush();
		file.close();
		blog(LOG_INFO,
		     "[clip-cropper] Audio capture writer stopped. path=%s writtenSamples=%lld droppedSamples=%lld",
		     capturePath.toUtf8().constData(), static_cast<long long>(writtenSamples.load()),
		     static_cast<long long>(droppedSamples.load()));
	}

	QString capturePath;
	int sourceSampleRate = 48000;
	QFile file;
	std::thread thread;
	std::mutex mutex;
	std::condition_variable condition;
	std::deque<QVector<float>> queue;
	bool stopping = false;
	std::atomic<qint64> queuedSamples{0};
	std::atomic<qint64> writtenSamples{0};
	std::atomic<qint64> droppedSamples{0};
};

class PostRecordingTranscriptionJob {
public:
	PostRecordingTranscriptionJob(QString modelPath, QString capturePath, int sourceSampleRate,
				      QStringList videoPaths)
		: modelPath(std::move(modelPath)),
		  capturePath(std::move(capturePath)),
		  sourceSampleRate(sourceSampleRate),
		  videoPaths(std::move(videoPaths))
	{
	}

	void run()
	{
		if (videoPaths.isEmpty()) {
			blog(LOG_INFO,
			     "[clip-cropper] Post-recording transcription skipped because there are no video paths.");
			QFile::remove(capturePath);
			return;
		}

		if (modelPath.trimmed().isEmpty()) {
			blog(LOG_INFO,
			     "[clip-cropper] Whisper model path is empty; post-recording transcription skipped.");
			QFile::remove(capturePath);
			return;
		}

		QFile captureFile(capturePath);
		if (!captureFile.open(QIODevice::ReadOnly)) {
			blog(LOG_ERROR, "[clip-cropper] Failed to open transcription capture for reading: %s",
			     capturePath.toUtf8().constData());
			QFile::remove(capturePath);
			return;
		}

		const QFileInfo modelInfo(modelPath);
		blog(LOG_INFO,
		     "[clip-cropper] Loading Whisper model for post-recording transcription. path=%s exists=%s sizeBytes=%lld cudaRequested=%s",
		     modelPath.toUtf8().constData(), boolText(modelInfo.isFile()),
		     static_cast<long long>(modelInfo.size()),
#if CLIP_CROPPER_WHISPER_CUDA_REQUESTED
		     "true"
#else
		     "false"
#endif
		);

		whisper_context_params contextParams = whisper_context_default_params();
#if CLIP_CROPPER_WHISPER_CUDA_REQUESTED
		contextParams.use_gpu = true;
#else
		contextParams.use_gpu = false;
#endif

		whisper_context *ctx =
			whisper_init_from_file_with_params(modelPath.toUtf8().constData(), contextParams);
		if (!ctx) {
			blog(LOG_ERROR,
			     "[clip-cropper] Failed to load Whisper model for post-recording transcription. path=%s use_gpu=%s",
			     modelPath.toUtf8().constData(), boolText(contextParams.use_gpu));
			QFile::remove(capturePath);
			return;
		}

		RecordingTranscript transcript;
		const int chunkSamples = sourceSampleRate * ChunkSeconds;
		qint64 processedSourceSamples = 0;

		while (!captureFile.atEnd()) {
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

			processChunk(ctx, chunk, processedSourceSamples, transcript);
			processedSourceSamples += readSamples;
		}

		captureFile.close();
		whisper_free(ctx);

		blog(LOG_INFO,
		     "[clip-cropper] Post-recording transcription finalized with %lld segment(s) for %d video path(s).",
		     static_cast<long long>(transcript.segments.size()), static_cast<long long>(videoPaths.size()));

		for (const QString &videoPath : videoPaths) {
			if (videoPath.trimmed().isEmpty() || transcript.segments.isEmpty())
				continue;

			RecordingTranscript transcriptForVideo = transcript;
			transcriptForVideo.videoPath = videoPath;
			transcriptForVideo.videoFileName = QFileInfo(videoPath).fileName();
			TranscriptStore::saveForVideoPath(videoPath, transcriptForVideo);
			blog(LOG_INFO, "[clip-cropper] Transcript saved for %s with %d segment(s).",
			     videoPath.toUtf8().constData(), transcriptForVideo.segments.size());
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
			     "[clip-cropper] Whisper post-recording chunk skipped because resampled PCM is empty. inputSamples=%lld sourceRate=%d",
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
		     "[clip-cropper] Whisper post-recording processing chunk. sourceSamples=%d pcm16kSamples=%lld startSec=%.2f threads=%d",
		     static_cast<long long>(chunk.size()), sampleCount, chunkStartSec, params.n_threads);

		const int result = whisper_full(ctx, params, pcm16k.constData(), sampleCount);
		if (result != 0) {
			blog(LOG_WARNING,
			     "[clip-cropper] Whisper post-recording transcription failed for chunk. result=%d", result);
			return;
		}

		const int segmentCount = whisper_full_n_segments(ctx);
		blog(LOG_INFO, "[clip-cropper] Whisper post-recording chunk completed. rawSegmentCount=%d",
		     segmentCount);
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
};

RealtimeTranscriptionService::RealtimeTranscriptionService(QObject *parent) : QObject(parent) {}

RealtimeTranscriptionService::~RealtimeTranscriptionService()
{
	if (running)
		stopAndSaveForVideos({});
}

void RealtimeTranscriptionService::start(const QString &whisperModelPath)
{
	if (running)
		return;

	modelPath = whisperModelPath;
	if (modelPath.trimmed().isEmpty()) {
		blog(LOG_INFO, "[clip-cropper] Whisper model path is empty; audio capture skipped.");
		return;
	}

	audio_t *audio = obs_get_audio();
	if (audio)
		sampleRate = static_cast<int>(audio_output_get_sample_rate(audio));
	if (sampleRate <= 0)
		sampleRate = 48000;

	capturePath = makeCaptureFilePath();
	writer = new AudioCaptureWriter(capturePath, sampleRate);
	if (!writer->start()) {
		delete writer;
		writer = nullptr;
		capturePath.clear();
		return;
	}

	obs_add_raw_audio_callback(0, nullptr, &RealtimeTranscriptionService::rawAudioCallback, this);
	running = true;
	rawAudioCallbackCount = 0;
	queuedRawFrames = 0;

	blog(LOG_INFO,
	     "[clip-cropper] Post-recording transcription capture started. modelPath=%s sampleRate=%d capturePath=%s",
	     modelPath.toUtf8().constData(), sampleRate, capturePath.toUtf8().constData());
}

void RealtimeTranscriptionService::stopAndSaveForVideos(const QStringList &videoPaths)
{
	if (!running)
		return;

	obs_remove_raw_audio_callback(0, &RealtimeTranscriptionService::rawAudioCallback, this);
	running = false;

	auto *finishedWriter = writer;
	writer = nullptr;
	if (!finishedWriter)
		return;

	finishedWriter->requestStop();
	finishedWriter->join();

	const QString finishedCapturePath = finishedWriter->path();
	const qint64 writtenSamples = finishedWriter->writtenSampleCount();
	const qint64 droppedSamples = finishedWriter->droppedSampleCount();
	delete finishedWriter;

	blog(LOG_INFO,
	     "[clip-cropper] Audio capture finalized. writtenSamples=%lld droppedSamples=%lld videoPaths=%lld. Starting post-recording transcription.",
	     static_cast<long long>(writtenSamples), static_cast<long long>(droppedSamples),
	     static_cast<long long>(videoPaths.size()));

	if (writtenSamples <= 0) {
		blog(LOG_WARNING,
		     "[clip-cropper] Post-recording transcription skipped because no audio samples were captured.");
		QFile::remove(finishedCapturePath);
		return;
	}

	const QString finishedModelPath = modelPath;
	const int finishedSampleRate = sampleRate;
	std::thread([finishedModelPath, finishedCapturePath, finishedSampleRate, videoPaths]() {
		PostRecordingTranscriptionJob job(finishedModelPath, finishedCapturePath, finishedSampleRate,
						  videoPaths);
		job.run();
	}).detach();
}

void RealtimeTranscriptionService::rawAudioCallback(void *param, size_t mixIdx, struct audio_data *data)
{
	auto *service = static_cast<RealtimeTranscriptionService *>(param);
	if (service)
		service->handleRawAudio(mixIdx, data);
}

void RealtimeTranscriptionService::handleRawAudio(size_t mixIdx, struct audio_data *data)
{
	if (!running || !writer || !data)
		return;

	++rawAudioCallbackCount;
	queuedRawFrames += data->frames;

	if (rawAudioCallbackCount == 1 || rawAudioCallbackCount % AudioCallbackLogInterval == 0) {
		blog(LOG_INFO,
		     "[clip-cropper] Raw audio captured for post-recording transcription. count=%llu mixIdx=%llu frames=%u totalFrames=%llu plane0=%s",
		     static_cast<unsigned long long>(rawAudioCallbackCount), static_cast<unsigned long long>(mixIdx),
		     data->frames, static_cast<unsigned long long>(queuedRawFrames),
		     boolText(data->data[0] != nullptr));
	}

	QVector<float> monoSamples = downmixFirstPlaneToMono(data);
	if (monoSamples.isEmpty()) {
		if (rawAudioCallbackCount == 1 || rawAudioCallbackCount % AudioCallbackLogInterval == 0) {
			blog(LOG_WARNING,
			     "[clip-cropper] Raw audio callback had no usable mono samples. frames=%u plane0=%s",
			     data->frames, boolText(data->data[0] != nullptr));
		}
		return;
	}

	writer->appendSamples(std::move(monoSamples));
}

RealtimeTranscriptionService *global_realtime_transcription_service()
{
	static RealtimeTranscriptionService service;
	return &service;
}
