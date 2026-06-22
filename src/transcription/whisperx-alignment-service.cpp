#include "transcription/whisperx-alignment-service.hpp"

#include "transcription/transcript-store.hpp"

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextStream>
#include <QStringConverter>

#include <algorithm>
#include <cmath>
#include <memory>

namespace Transcription {
namespace {

bool transcriptionCanceled(const TranscriptionCancelCallback &cancelCallback)
{
	return cancelCallback && cancelCallback();
}

void reportProgress(const TranscriptionProgressCallback &progressCallback, int progress, const QString &message)
{
	if (progressCallback)
		progressCallback(qBound(0, progress, 100), message);
}

QString obsModuleFilePath(const QString &relativePath)
{
	const QByteArray relativePathBytes = relativePath.toUtf8();
	char *modulePath = obs_module_file(relativePathBytes.constData());
	if (!modulePath)
		return {};
	const QString result = QString::fromUtf8(modulePath);
	bfree(modulePath);
	return result;
}

QString ffmpegExecutableName()
{
#ifdef _WIN32
	return QStringLiteral("ffmpeg.exe");
#else
	return QStringLiteral("ffmpeg");
#endif
}

QStringList ffmpegExecutableCandidates(const WhisperXSettings &settings)
{
	QStringList candidates;
	const QString configuredPath = settings.ffmpegPath.trimmed();
	if (!configuredPath.isEmpty())
		candidates.append(configuredPath);

	const QString envPath = QString::fromLocal8Bit(qgetenv("CLIP_CROPPER_FFMPEG_PATH")).trimmed();
	if (!envPath.isEmpty())
		candidates.append(envPath);

	const QFileInfo workerInfo(settings.workerPath);
	if (workerInfo.exists()) {
		const QDir toolsDir(workerInfo.absolutePath());
		const QString projectDir = QDir::cleanPath(toolsDir.filePath(QStringLiteral("..")));
		candidates.append(QDir(projectDir).filePath(QStringLiteral(".ffmpeg-runtime/%1").arg(ffmpegExecutableName())));
		candidates.append(QDir(projectDir).filePath(QStringLiteral("ffmpeg/%1").arg(ffmpegExecutableName())));
	}

#ifdef _WIN32
	candidates.append(obsModuleFilePath(QStringLiteral("ffmpeg/ffmpeg.exe")));
	candidates.append(obsModuleFilePath(QStringLiteral("ffmpeg.exe")));
	const QString appDir = QCoreApplication::applicationDirPath();
	candidates.append(QDir(appDir).filePath(QStringLiteral("ffmpeg.exe")));
	candidates.append(QDir(appDir).filePath(QStringLiteral("ffmpeg/ffmpeg.exe")));
	candidates.append(QDir(appDir).filePath(QStringLiteral("../data/obs-plugins/clip-cropper/ffmpeg/ffmpeg.exe")));
	candidates.append(QStringLiteral("ffmpeg.exe"));
#else
	candidates.append(obsModuleFilePath(QStringLiteral("ffmpeg/ffmpeg")));
	candidates.append(obsModuleFilePath(QStringLiteral("ffmpeg")));
	candidates.append(QStringLiteral("ffmpeg"));
#endif
	return candidates;
}

QString resolveFfmpegExecutableForWhisperX(const WhisperXSettings &settings)
{
	for (const QString &candidate : ffmpegExecutableCandidates(settings)) {
		const QString trimmed = candidate.trimmed();
		if (trimmed.isEmpty())
			continue;

		const QFileInfo info(trimmed);
		if (info.isDir()) {
			const QString executable = QDir(info.absoluteFilePath()).filePath(ffmpegExecutableName());
			if (QFileInfo::exists(executable))
				return executable;
			continue;
		}

		if (info.isFile())
			return trimmed;

		if (trimmed == ffmpegExecutableName() || trimmed == QStringLiteral("ffmpeg")) {
			const QString resolved = QStandardPaths::findExecutable(trimmed);
			if (!resolved.trimmed().isEmpty())
				return resolved;
		}

	}
	return {};
}

void addExecutableDirectoryToPath(QProcessEnvironment &env, const QString &executableOrDirectory)
{
	const QFileInfo info(executableOrDirectory);
	const QString dir = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
	if (dir.trimmed().isEmpty())
		return;
	env.insert(QStringLiteral("PATH"), dir + QDir::listSeparator() + env.value(QStringLiteral("PATH")));
}

QString cacheWorkDirectory()
{
	QString baseDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	if (baseDir.trimmed().isEmpty())
		baseDir = QDir::tempPath() + QStringLiteral("/clip-cropper");
	const QString dir = QDir(baseDir).filePath(QStringLiteral("transcription-cache/whisperx-work"));
	QDir().mkpath(dir);
	return dir;
}

QString tempPath(const QString &suffix)
{
	return QDir(cacheWorkDirectory()).filePath(
		QStringLiteral("whisperx-%1-%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(suffix));
}

bool writeSegmentsJsonl(const QString &path, const RecordingTranscript &transcript)
{
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
		blog(LOG_ERROR, "[clip-cropper] Failed to open WhisperX segment JSONL for writing: %s",
		     path.toUtf8().constData());
		return false;
	}

	QTextStream stream(&file);
	stream.setEncoding(QStringConverter::Utf8);
	for (int i = 0; i < static_cast<int>(transcript.segments.size()); ++i) {
		const TranscriptSegment &segment = transcript.segments.at(i);
		const QString text = segment.text.simplified();
		if (text.isEmpty() || segment.endSec <= segment.startSec)
			continue;

		QJsonObject item;
		item.insert(QStringLiteral("type"), QStringLiteral("segment"));
		item.insert(QStringLiteral("segment_index"), i);
		item.insert(QStringLiteral("start"), segment.startSec);
		item.insert(QStringLiteral("end"), segment.endSec);
		item.insert(QStringLiteral("text"), text);
		stream << QString::fromUtf8(QJsonDocument(item).toJson(QJsonDocument::Compact)) << '\n';
	}

	if (!file.commit()) {
		blog(LOG_ERROR, "[clip-cropper] Failed to commit WhisperX segment JSONL: %s",
		     path.toUtf8().constData());
		return false;
	}
	return true;
}

bool parseAlignedJsonl(const QString &path, RecordingTranscript &transcript)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		blog(LOG_ERROR, "[clip-cropper] Failed to open WhisperX aligned JSONL: %s", path.toUtf8().constData());
		return false;
	}

	QTextStream stream(&file);
	stream.setEncoding(QStringConverter::Utf8);
	int alignedSegments = 0;
	int alignedWords = 0;
	while (!stream.atEnd()) {
		const QString line = stream.readLine().trimmed();
		if (line.isEmpty())
			continue;

		QJsonParseError error;
		const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8(), &error);
		if (error.error != QJsonParseError::NoError || !doc.isObject()) {
			blog(LOG_WARNING, "[clip-cropper] Ignoring malformed WhisperX JSONL line: %s",
			     error.errorString().toUtf8().constData());
			continue;
		}

		const QJsonObject item = doc.object();
		if (item.value(QStringLiteral("type")).toString() != QStringLiteral("segment"))
			continue;

		const int index = item.value(QStringLiteral("segment_index")).toInt(-1);
		if (index < 0)
			continue;
		while (index >= static_cast<int>(transcript.segments.size()))
			transcript.segments.append(TranscriptSegment{});

		TranscriptSegment &segment = transcript.segments[index];
		const double alignedStart = item.value(QStringLiteral("start")).toDouble(segment.startSec);
		const double alignedEnd = item.value(QStringLiteral("end")).toDouble(segment.endSec);
		if (alignedEnd > alignedStart) {
			segment.startSec = alignedStart;
			segment.endSec = alignedEnd;
		}

		const QString alignedText = item.value(QStringLiteral("text")).toString().simplified();
		if (!alignedText.isEmpty())
			segment.text = alignedText;

		segment.words.clear();
		const QJsonArray words = item.value(QStringLiteral("words")).toArray();
		segment.words.reserve(words.size());
		for (const QJsonValue &wordValue : words) {
			const QJsonObject wordObject = wordValue.toObject();
			WordTiming word;
			word.word = wordObject.value(QStringLiteral("word")).toString().trimmed();
			word.startSec = wordObject.value(QStringLiteral("start")).toDouble();
			word.endSec = wordObject.value(QStringLiteral("end")).toDouble();
			word.score = wordObject.value(QStringLiteral("score")).toDouble(0.0);
			if (!word.word.isEmpty() && word.endSec >= word.startSec)
				segment.words.append(word);
		}
		if (!segment.words.isEmpty()) {
			segment.startSec = std::min(segment.startSec, segment.words.first().startSec);
			segment.endSec = std::max(segment.endSec, segment.words.last().endSec);
		}

		++alignedSegments;
		alignedWords += static_cast<int>(segment.words.size());
	}

	transcript.wordAligned = alignedWords > 0;
	transcript.alignmentBackend = transcript.wordAligned ? QStringLiteral("whisperx") : QString();
	blog(LOG_INFO, "[clip-cropper] Parsed WhisperX aligned JSONL. path=%s alignedSegments=%d alignedWords=%d",
	     path.toUtf8().constData(), alignedSegments, alignedWords);
	return transcript.wordAligned;
}

} // namespace

RecordingTranscript WhisperXAlignmentService::transcribeVideo(const QString &videoPath, const QString &language,
	const WhisperXSettings &settings, const TranscriptionProgressCallback &progressCallback,
	const TranscriptionCancelCallback &cancelCallback) const
{
	RecordingTranscript transcript;
	transcript.videoPath = videoPath;
	transcript.videoFileName = QFileInfo(videoPath).fileName();

	if (!settings.primaryTranscription())
		return transcript;
	if (transcriptionCanceled(cancelCallback))
		return transcript;

	const QFileInfo pythonInfo(settings.pythonPath);
	const QFileInfo workerInfo(settings.workerPath);
	const QFileInfo videoInfo(videoPath);
	if (!videoInfo.isFile()) {
		blog(LOG_ERROR, "[clip-cropper] WhisperX primary transcription skipped because video is missing: %s",
		     videoPath.toUtf8().constData());
		return transcript;
	}
	if (!pythonInfo.isFile() || !workerInfo.isFile()) {
		blog(LOG_WARNING,
		     "[clip-cropper] WhisperX primary transcription skipped because python or worker path is invalid. python=%s worker=%s",
		     settings.pythonPath.toUtf8().constData(), settings.workerPath.toUtf8().constData());
		return transcript;
	}

	const QString outputPath = tempPath(QStringLiteral("primary-transcript.jsonl"));
	reportProgress(progressCallback, 5, QString::fromUtf8(obs_module_text("Status.WhisperXTranscribing")));

	QProcess process;
	process.setProgram(settings.pythonPath);
	const QString ffmpegPath = resolveFfmpegExecutableForWhisperX(settings);
	QStringList args{
		settings.workerPath,
		QStringLiteral("--mode"), QStringLiteral("transcribe"),
		QStringLiteral("--audio"), videoPath,
		QStringLiteral("--output-jsonl"), outputPath,
		QStringLiteral("--language"), language.trimmed().isEmpty() ? QStringLiteral("auto") : language,
		QStringLiteral("--device"), settings.device,
		QStringLiteral("--model"), settings.model,
		QStringLiteral("--compute-type"), settings.computeType,
		QStringLiteral("--batch-size"), QString::number(settings.batchSize),
	};
	if (!ffmpegPath.trimmed().isEmpty())
		args << QStringLiteral("--ffmpeg-path") << ffmpegPath;
	process.setArguments(args);
	process.setProcessChannelMode(QProcess::MergedChannels);

	QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
	if (!ffmpegPath.trimmed().isEmpty()) {
		addExecutableDirectoryToPath(env, ffmpegPath);
		env.insert(QStringLiteral("CLIP_CROPPER_FFMPEG_PATH"), ffmpegPath);
	}
	process.setProcessEnvironment(env);

	blog(LOG_INFO,
	     "[clip-cropper] Starting WhisperX primary transcription. python=%s worker=%s model=%s device=%s compute=%s language=%s ffmpeg=%s video=%s",
	     settings.pythonPath.toUtf8().constData(), settings.workerPath.toUtf8().constData(),
	     settings.model.toUtf8().constData(), settings.device.toUtf8().constData(),
	     settings.computeType.toUtf8().constData(), language.toUtf8().constData(), ffmpegPath.toUtf8().constData(),
	     videoPath.toUtf8().constData());

	process.start();
	if (!process.waitForStarted(5000)) {
		blog(LOG_ERROR, "[clip-cropper] Failed to start WhisperX primary transcription worker. python=%s",
		     settings.pythonPath.toUtf8().constData());
		QFile::remove(outputPath);
		return transcript;
	}

	QByteArray outputTail;
	int tick = 8;
	while (!process.waitForFinished(500)) {
		outputTail.append(process.readAll());
		if (outputTail.size() > 32768)
			outputTail.remove(0, outputTail.size() - 32768);
		if (transcriptionCanceled(cancelCallback)) {
			blog(LOG_INFO, "[clip-cropper] WhisperX primary transcription canceled. Killing worker process.");
			process.kill();
			process.waitForFinished(3000);
			QFile::remove(outputPath);
			return {};
		}
		tick = std::min(tick + 1, 97);
		reportProgress(progressCallback, tick, QString::fromUtf8(obs_module_text("Status.WhisperXTranscribing")));
	}
	outputTail.append(process.readAll());
	if (outputTail.size() > 32768)
		outputTail.remove(0, outputTail.size() - 32768);

	if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
		blog(LOG_ERROR, "[clip-cropper] WhisperX primary transcription failed. exitCode=%d output=%s",
		     process.exitCode(), outputTail.constData());
		QFile::remove(outputPath);
		return transcript;
	}

	RecordingTranscript parsed;
	parsed.videoPath = videoPath;
	parsed.videoFileName = QFileInfo(videoPath).fileName();
	if (parseAlignedJsonl(outputPath, parsed) && !parsed.segments.isEmpty()) {
		parsed.wordAligned = parsed.hasWordTimings();
		parsed.alignmentBackend = QStringLiteral("whisperx-primary");
		TranscriptStore::saveAlignedForVideoPath(videoPath, language, parsed);
		reportProgress(progressCallback, 98, QString::fromUtf8(obs_module_text("Status.SavingTranscript")));
		blog(LOG_INFO,
		     "[clip-cropper] WhisperX primary transcript saved. video=%s segments=%d cacheKey=%s wordAligned=%s",
		     videoPath.toUtf8().constData(), static_cast<int>(parsed.segments.size()),
		     TranscriptStore::keyForAlignedVideoPath(videoPath, language).toUtf8().constData(),
		     parsed.hasWordTimings() ? "true" : "false");
		QFile::remove(outputPath);
		return parsed;
	}

	QFile::remove(outputPath);
	return transcript;
}

RecordingTranscript WhisperXAlignmentService::alignVideoTranscript(const QString &videoPath, const QString &language,
	const RecordingTranscript &baseTranscript, const WhisperXSettings &settings,
	const TranscriptionProgressCallback &progressCallback, const TranscriptionCancelCallback &cancelCallback) const
{
	RecordingTranscript transcript = baseTranscript;
	transcript.videoPath = videoPath;
	transcript.videoFileName = QFileInfo(videoPath).fileName();

	if (!settings.alignmentOnly() || transcript.segments.isEmpty())
		return transcript;
	if (transcript.hasWordTimings()) {
		transcript.wordAligned = true;
		return transcript;
	}
	if (transcriptionCanceled(cancelCallback))
		return transcript;

	const QFileInfo pythonInfo(settings.pythonPath);
	const QFileInfo workerInfo(settings.workerPath);
	if (!pythonInfo.isFile() || !workerInfo.isFile()) {
		blog(LOG_WARNING,
		     "[clip-cropper] WhisperX alignment skipped because python or worker path is invalid. python=%s worker=%s",
		     settings.pythonPath.toUtf8().constData(), settings.workerPath.toUtf8().constData());
		return transcript;
	}

	const QString segmentsPath = tempPath(QStringLiteral("segments.jsonl"));
	const QString outputPath = tempPath(QStringLiteral("aligned.jsonl"));
	if (!writeSegmentsJsonl(segmentsPath, transcript))
		return transcript;

	reportProgress(progressCallback, 90, QString::fromUtf8(obs_module_text("Status.WhisperXAligning")));

	QProcess process;
	process.setProgram(settings.pythonPath);
	const QString ffmpegPath = resolveFfmpegExecutableForWhisperX(settings);
	QStringList args{
		settings.workerPath,
		QStringLiteral("--audio"), videoPath,
		QStringLiteral("--segments-jsonl"), segmentsPath,
		QStringLiteral("--output-jsonl"), outputPath,
		QStringLiteral("--language"), language.trimmed().isEmpty() ? QStringLiteral("auto") : language,
		QStringLiteral("--device"), settings.device,
	};
	if (!ffmpegPath.trimmed().isEmpty())
		args << QStringLiteral("--ffmpeg-path") << ffmpegPath;
	process.setArguments(args);
	process.setProcessChannelMode(QProcess::MergedChannels);

	QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
	if (!ffmpegPath.trimmed().isEmpty()) {
		addExecutableDirectoryToPath(env, ffmpegPath);
		env.insert(QStringLiteral("CLIP_CROPPER_FFMPEG_PATH"), ffmpegPath);
	}
	process.setProcessEnvironment(env);

	blog(LOG_INFO,
	     "[clip-cropper] Starting WhisperX word alignment. python=%s worker=%s device=%s language=%s ffmpeg=%s video=%s segments=%d",
	     settings.pythonPath.toUtf8().constData(), settings.workerPath.toUtf8().constData(),
	     settings.device.toUtf8().constData(), language.toUtf8().constData(), ffmpegPath.toUtf8().constData(),
	     videoPath.toUtf8().constData(), static_cast<int>(transcript.segments.size()));

	process.start();
	if (!process.waitForStarted(5000)) {
		blog(LOG_ERROR, "[clip-cropper] Failed to start WhisperX alignment worker. python=%s",
		     settings.pythonPath.toUtf8().constData());
		QFile::remove(segmentsPath);
		QFile::remove(outputPath);
		return transcript;
	}

	QByteArray outputTail;
	int tick = 90;
	while (!process.waitForFinished(250)) {
		outputTail.append(process.readAll());
		if (outputTail.size() > 16384)
			outputTail.remove(0, outputTail.size() - 16384);
		if (transcriptionCanceled(cancelCallback)) {
			blog(LOG_INFO, "[clip-cropper] WhisperX alignment canceled. Killing worker process.");
			process.kill();
			process.waitForFinished(3000);
			QFile::remove(segmentsPath);
			QFile::remove(outputPath);
			return transcript;
		}
		tick = std::min(tick + 1, 97);
		reportProgress(progressCallback, tick, QString::fromUtf8(obs_module_text("Status.WhisperXAligning")));
	}
	outputTail.append(process.readAll());
	if (outputTail.size() > 16384)
		outputTail.remove(0, outputTail.size() - 16384);

	if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
		blog(LOG_ERROR, "[clip-cropper] WhisperX alignment failed. exitCode=%d output=%s",
		     process.exitCode(), outputTail.constData());
		QFile::remove(segmentsPath);
		QFile::remove(outputPath);
		return transcript;
	}

	RecordingTranscript aligned = transcript;
	if (parseAlignedJsonl(outputPath, aligned)) {
		TranscriptStore::saveAlignedForVideoPath(videoPath, language, aligned);
		reportProgress(progressCallback, 98, QString::fromUtf8(obs_module_text("Status.SavingTranscript")));
		blog(LOG_INFO, "[clip-cropper] WhisperX word-aligned transcript saved. video=%s segments=%d cacheKey=%s",
		     videoPath.toUtf8().constData(), static_cast<int>(aligned.segments.size()),
		     TranscriptStore::keyForAlignedVideoPath(videoPath, language).toUtf8().constData());
		QFile::remove(segmentsPath);
		QFile::remove(outputPath);
		return aligned;
	}

	QFile::remove(segmentsPath);
	QFile::remove(outputPath);
	return transcript;
}

} // namespace Transcription
