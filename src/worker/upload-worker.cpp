#include "worker/upload-worker.hpp"

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

#include "opus/opus-clip-client.hpp"
#include "transcription/transcript-store.hpp"
#include "utils/config.hpp"

#include <QCoreApplication>
#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QProcess>
#include <QPointer>
#include <QTextStream>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace {
constexpr const char *ConfigUploadResampleThresholdPercent = "upload_resample_threshold_percent";
constexpr double DefaultResampleThresholdPercent = 60.0;

static QString obsText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
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

double configuredResampleThresholdPercent()
{
	bool ok = false;
	double threshold = PluginConfig::getValue(QString::fromLatin1(ConfigUploadResampleThresholdPercent),
						  QString::number(DefaultResampleThresholdPercent))
				   .trimmed()
				   .toDouble(&ok);

	if (!ok || !std::isfinite(threshold))
		threshold = DefaultResampleThresholdPercent;

	return std::clamp(threshold, 0.0, 100.0);
}

QVector<ClipDuration> validRanges(const CurationSettings &settings)
{
	QVector<ClipDuration> ranges = settings.clipDurations;

	if (ranges.isEmpty() && settings.rangeEndSec > settings.rangeStartSec)
		ranges.append({settings.rangeStartSec, settings.rangeEndSec});

	QVector<ClipDuration> output;
	output.reserve(ranges.size());

	for (ClipDuration range : ranges) {
		if (!std::isfinite(range.startSec) || !std::isfinite(range.endSec))
			continue;

		range.startSec = std::max(0.0, range.startSec);
		range.endSec = std::max(range.startSec, range.endSec);
		if (range.endSec > range.startSec)
			output.append(range);
	}

	return output;
}

double totalRangeDuration(const QVector<ClipDuration> &ranges)
{
	double total = 0.0;
	for (const ClipDuration &range : ranges)
		total += std::max(0.0, range.endSec - range.startSec);
	return total;
}

QString uniqueResampledVideoPath(const QString &sourcePath)
{
	const QFileInfo sourceInfo(sourcePath);
	const QString baseName = sourceInfo.completeBaseName().trimmed().isEmpty()
					 ? QStringLiteral("video")
					 : sourceInfo.completeBaseName().trimmed();
	const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
	const QDir dir = sourceInfo.dir();

	for (int i = 0; i < 100; ++i) {
		const QString suffix = i == 0 ? QString{} : QStringLiteral("-%1").arg(i + 1);
		const QString candidate = dir.filePath(
			QStringLiteral("%1.clip-cropper-selected-%2%3.mp4").arg(baseName, timestamp, suffix));
		if (!QFileInfo::exists(candidate))
			return candidate;
	}

	return dir.filePath(QStringLiteral("%1.clip-cropper-selected-%2-%3.mp4")
				    .arg(baseName, timestamp)
				    .arg(QDateTime::currentMSecsSinceEpoch()));
}

QString concatFileLinePath(QString path)
{
	path = QDir::fromNativeSeparators(path);
	path.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
	return QStringLiteral("file '%1'\n").arg(path);
}

CurationSettings adjustedSettingsForResampledVideo(const CurationSettings &settings,
						   const QVector<ClipDuration> &ranges)
{
	CurationSettings adjusted = settings;
	adjusted.clipDurations.clear();

	double cursor = 0.0;
	for (const ClipDuration &range : ranges) {
		const double duration = std::max(0.0, range.endSec - range.startSec);
		if (duration <= 0.0)
			continue;

		adjusted.clipDurations.append({cursor, cursor + duration});
		cursor += duration;
	}

	adjusted.rangeStartSec = adjusted.clipDurations.isEmpty() ? 0.0 : adjusted.clipDurations.first().startSec;
	adjusted.rangeEndSec = adjusted.clipDurations.isEmpty() ? 0.0 : adjusted.clipDurations.last().endSec;
	adjusted.originalVideoDurationSec = cursor;
	return adjusted;
}

QString normalizeTranscriptionLanguage(QString language)
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

QString safeFileNameKey(const QString &videoPath)
{
	QString fileName = QFileInfo(videoPath).fileName().trimmed();
	if (fileName.isEmpty())
		fileName = videoPath.trimmed();

	if (fileName.isEmpty())
		return QStringLiteral("unknown");

	return QString::fromLatin1(
		fileName.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QString markerConfigKeyForVideoPath(const QString &videoPath)
{
	const QString safeFileName = safeFileNameKey(videoPath);
	return safeFileName.isEmpty() ? QString{} : QStringLiteral("video_markers.%1").arg(safeFileName);
}

QString reviewTimeToken(double seconds)
{
	const qint64 milliseconds = static_cast<qint64>(std::max(0.0, std::round(seconds * 1000.0)));
	return QString::number(milliseconds);
}

QString reviewRangeKeyFromRanges(const QVector<ClipDuration> &ranges)
{
	if (ranges.isEmpty())
		return QStringLiteral("no_markers");

	QStringList values;
	values.reserve(ranges.size());
	for (const ClipDuration &range : ranges) {
		if (!std::isfinite(range.startSec) || !std::isfinite(range.endSec))
			continue;

		values.append(
			QStringLiteral("%1-%2").arg(reviewTimeToken(range.startSec), reviewTimeToken(range.endSec)));
	}

	return values.isEmpty() ? QStringLiteral("no_markers") : values.join(QStringLiteral("_"));
}

QString reviewSettingsConfigKey(const QString &videoPath, const QVector<ClipDuration> &ranges)
{
	return QStringLiteral("review.settings.%1.%2").arg(safeFileNameKey(videoPath), reviewRangeKeyFromRanges(ranges));
}

QVector<double> markerPositionsForRanges(const QVector<ClipDuration> &ranges)
{
	QVector<double> markers;
	markers.reserve(ranges.size() * 2);

	for (const ClipDuration &range : ranges) {
		const double startSec = std::max(0.0, range.startSec);
		const double endSec = std::max(startSec, range.endSec);
		if (endSec <= startSec)
			continue;

		markers.append(startSec);
		markers.append(endSec);
	}

	return markers;
}

void saveMarkersForVideoPath(const QString &videoPath, const QVector<ClipDuration> &ranges)
{
	const QString key = markerConfigKeyForVideoPath(videoPath);
	if (key.isEmpty())
		return;

	const QVector<double> markers = markerPositionsForRanges(ranges);
	if (markers.isEmpty()) {
		PluginConfig::removeValue(key);
		return;
	}

	QStringList values;
	values.reserve(markers.size());
	for (double marker : markers)
		values.append(QString::number(marker, 'f', 3));

	PluginConfig::setValue(key, values.join(';'));
}

void saveReviewSettingsForVideoPath(const QString &videoPath, CurationSettings &settings)
{
	settings.reviewSettingsKey = reviewSettingsConfigKey(videoPath, settings.clipDurations);

	QJsonObject root;
	root.insert(QStringLiteral("videoFileName"), QFileInfo(videoPath).fileName());
	root.insert(QStringLiteral("videoPath"), videoPath);
	root.insert(QStringLiteral("reviewSettingsKey"), settings.reviewSettingsKey);
	root.insert(QStringLiteral("originalVideoDurationSec"), settings.originalVideoDurationSec);

	QJsonArray ranges;
	for (const ClipDuration &range : settings.clipDurations) {
		QJsonObject item;
		item.insert(QStringLiteral("start"), range.startSec);
		item.insert(QStringLiteral("end"), range.endSec);
		ranges.append(item);
	}
	root.insert(QStringLiteral("clipDurations"), ranges);

	QJsonArray topicKeywords;
	for (const QString &keyword : settings.topicKeywords) {
		const QString trimmed = keyword.trimmed();
		if (!trimmed.isEmpty())
			topicKeywords.append(trimmed);
	}
	root.insert(QStringLiteral("topicKeywords"), topicKeywords);
	root.insert(QStringLiteral("genre"), settings.genre);
	root.insert(QStringLiteral("curationPreset"), settings.curationPreset);
	root.insert(QStringLiteral("model"), settings.model);
	root.insert(QStringLiteral("clipLengthPreset"), settings.clipLengthPreset);
	root.insert(QStringLiteral("skipCurate"), settings.skipCurate);
	root.insert(QStringLiteral("sourceLanguage"), normalizeTranscriptionLanguage(settings.sourceLanguage));
	root.insert(QStringLiteral("transcriptionLanguage"),
		    normalizeTranscriptionLanguage(settings.transcriptionLanguage));
	root.insert(QStringLiteral("aiPrompt"), settings.aiPrompt.trimmed());

	PluginConfig::setValue(settings.reviewSettingsKey,
			       QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

RecordingTranscript transcriptForResampledVideo(const RecordingTranscript &sourceTranscript, const QString &outputPath,
						const QVector<ClipDuration> &sourceRanges)
{
	RecordingTranscript output;
	output.videoPath = outputPath;
	output.videoFileName = QFileInfo(outputPath).fileName();

	double cursor = 0.0;
	for (const ClipDuration &range : sourceRanges) {
		const double rangeStart = std::max(0.0, range.startSec);
		const double rangeEnd = std::max(rangeStart, range.endSec);
		const double duration = std::max(0.0, rangeEnd - rangeStart);
		if (duration <= 0.0)
			continue;

		for (const TranscriptSegment &segment : sourceTranscript.segments) {
			if (segment.text.trimmed().isEmpty())
				continue;

			const double overlapStart = std::max(segment.startSec, rangeStart);
			const double overlapEnd = std::min(segment.endSec, rangeEnd);
			if (overlapEnd <= overlapStart)
				continue;

			TranscriptSegment item = segment;
			item.startSec = cursor + (overlapStart - rangeStart);
			item.endSec = cursor + (overlapEnd - rangeStart);
			output.segments.append(item);
		}

		cursor += duration;
	}

	return output;
}

void saveTranscriptCacheForResampledVideo(const QString &sourcePath, const QString &outputPath,
					  const QVector<ClipDuration> &sourceRanges, const CurationSettings &settings)
{
	QStringList languages;
	languages << normalizeTranscriptionLanguage(settings.transcriptionLanguage) << QStringLiteral("auto")
		  << QStringLiteral("pt") << QStringLiteral("en");
	languages.removeDuplicates();

	for (const QString &language : languages) {
		const RecordingTranscript sourceTranscript = TranscriptStore::loadForVideoPath(sourcePath, language);
		if (sourceTranscript.segments.isEmpty())
			continue;

		const RecordingTranscript outputTranscript =
			transcriptForResampledVideo(sourceTranscript, outputPath, sourceRanges);
		if (outputTranscript.segments.isEmpty())
			continue;

		TranscriptStore::saveForVideoPath(outputPath, language, outputTranscript);
		blog(LOG_INFO,
		     "[clip-cropper] Saved adjusted transcript cache for resampled video. source=%s output=%s language=%s segments=%d cacheKey=%s",
		     sourcePath.toUtf8().constData(), outputPath.toUtf8().constData(), language.toUtf8().constData(),
		     static_cast<int>(outputTranscript.segments.size()),
		     TranscriptStore::keyForVideoPath(outputPath, language).toUtf8().constData());
	}
}
} // namespace

UploadWorker::UploadWorker(QString apiKey, QString filePath, QString fileName, QString mimeType,
			   QString brandTemplateId, QString sourceLang, CurationSettings curationSettings,
			   QString openAiApiKey, QString openAiModel, QObject *parent)
	: QObject(parent),
	  apiKey(std::move(apiKey)),
	  filePath(std::move(filePath)),
	  fileName(std::move(fileName)),
	  mimeType(std::move(mimeType)),
	  brandTemplateId(std::move(brandTemplateId)),
	  sourceLang(std::move(sourceLang)),
	  curationSettings(std::move(curationSettings)),
	  openAiApiKey(std::move(openAiApiKey)),
	  openAiModel(std::move(openAiModel))
{
	if (this->sourceLang.trimmed().isEmpty())
		this->sourceLang = "auto";
}

void UploadWorker::run()
{
	cancelRequested.store(false);

	const ResampleResult prepared = prepareUploadVideo();
	if (cancelRequested.load()) {
		emit failed(obsText("Message.UploadCanceled"));
		return;
	}

	if (prepared.filePath.trimmed().isEmpty())
		return;

	startOpusUpload(prepared.filePath, prepared.fileName, prepared.mimeType, prepared.curationSettings,
			prepared.usedResampledVideo);
}

void UploadWorker::cancel()
{
	cancelRequested.store(true);

	if (client) {
		OpusClipClient *target = client;
		QMetaObject::invokeMethod(target, [target]() { target->cancel(); }, Qt::QueuedConnection);
	}
}

bool UploadWorker::runProcess(const QString &program, const QStringList &arguments, int progressStart, int progressEnd,
			      const QString &message, QString *lastOutput)
{
	QProcess process;
	currentProcess = &process;
	process.setProgram(program);
	process.setArguments(arguments);
	process.setProcessChannelMode(QProcess::MergedChannels);

	emit progressChanged(progressStart, message);
	process.start();
	if (!process.waitForStarted(5000)) {
		currentProcess = nullptr;
		return false;
	}

	QByteArray output;
	int progress = progressStart;
	const int cappedProgressEnd = std::max(progressStart, progressEnd - 1);

	while (!process.waitForFinished(250)) {
		output.append(process.readAll());
		if (output.size() > 12000)
			output.remove(0, output.size() - 12000);

		if (cancelRequested.load()) {
			process.kill();
			process.waitForFinished(3000);
			currentProcess = nullptr;
			return false;
		}

		if (progress < cappedProgressEnd) {
			++progress;
			emit progressChanged(progress, message);
		}
	}

	output.append(process.readAll());
	if (output.size() > 12000)
		output.remove(0, output.size() - 12000);

	currentProcess = nullptr;
	if (lastOutput)
		*lastOutput = QString::fromUtf8(output);

	if (cancelRequested.load())
		return false;

	if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
		return false;

	emit progressChanged(progressEnd, message);
	return true;
}

UploadWorker::ResampleResult UploadWorker::prepareUploadVideo()
{
	ResampleResult result;
	result.filePath = filePath;
	result.fileName = fileName;
	result.mimeType = mimeType;
	result.curationSettings = curationSettings;

	const QVector<ClipDuration> ranges = validRanges(curationSettings);
	const double originalDurationSec = curationSettings.originalVideoDurationSec;
	const double selectedDurationSec = totalRangeDuration(ranges);
	const double thresholdPercent = configuredResampleThresholdPercent();

	if (ranges.isEmpty() || originalDurationSec <= 0.0 || selectedDurationSec <= 0.0 ||
	    selectedDurationSec >= originalDurationSec) {
		return result;
	}

	const double differencePercent = ((originalDurationSec - selectedDurationSec) / originalDurationSec) * 100.0;
	if (differencePercent < thresholdPercent) {
		blog(LOG_INFO,
		     "[clip-cropper] Upload resample skipped. originalSec=%.2f selectedSec=%.2f differencePercent=%.2f thresholdPercent=%.2f",
		     originalDurationSec, selectedDurationSec, differencePercent, thresholdPercent);
		return result;
	}

	const QString ffmpeg = resolveFfmpegExecutable();
	if (ffmpeg.trimmed().isEmpty()) {
		emit failed(QStringLiteral(
			"ffmpeg executable was not found. Set CLIP_CROPPER_FFMPEG_PATH or install the bundled runtime."));
		return {};
	}

	const QString outputPath = uniqueResampledVideoPath(filePath);
	const QFileInfo outputInfo(outputPath);
	const QDir outputDir = outputInfo.dir();
	const QString workDirPath = outputDir.filePath(
		QStringLiteral(".clip-cropper-resample-%1").arg(QDateTime::currentMSecsSinceEpoch()));
	QDir().mkpath(workDirPath);
	QDir workDir(workDirPath);

	blog(LOG_INFO,
	     "[clip-cropper] Upload resample enabled. originalSec=%.2f selectedSec=%.2f differencePercent=%.2f thresholdPercent=%.2f ranges=%d output=%s",
	     originalDurationSec, selectedDurationSec, differencePercent, thresholdPercent,
	     static_cast<int>(ranges.size()), outputPath.toUtf8().constData());

	QStringList segmentPaths;
	segmentPaths.reserve(ranges.size());

	const int rangeCount = static_cast<int>(ranges.size());
	for (int i = 0; i < rangeCount; ++i) {
		if (cancelRequested.load())
			break;

		const ClipDuration range = ranges.at(i);
		const double durationSec = std::max(0.0, range.endSec - range.startSec);
		if (durationSec <= 0.0)
			continue;

		const QString segmentPath =
			workDir.filePath(QStringLiteral("segment-%1.mp4").arg(i, 4, 10, QChar('0')));
		segmentPaths.append(segmentPath);

		const int safeRangeCount = std::max(1, rangeCount);
		const int progressStart = static_cast<int>((i * 44.0) / safeRangeCount);
		const int progressEnd = static_cast<int>(((i + 1) * 44.0) / safeRangeCount);
		QString output;
		const bool ok = runProcess(ffmpeg,
					   QStringList{QStringLiteral("-hide_banner"),
						       QStringLiteral("-nostdin"),
						       QStringLiteral("-y"),
						       QStringLiteral("-ss"),
						       QString::number(range.startSec, 'f', 3),
						       QStringLiteral("-t"),
						       QString::number(durationSec, 'f', 3),
						       QStringLiteral("-i"),
						       filePath,
						       QStringLiteral("-map"),
						       QStringLiteral("0:v:0"),
						       QStringLiteral("-map"),
						       QStringLiteral("0:a?"),
						       QStringLiteral("-sn"),
						       QStringLiteral("-dn"),
						       QStringLiteral("-c:v"),
						       QStringLiteral("libx264"),
						       QStringLiteral("-preset"),
						       QStringLiteral("veryfast"),
						       QStringLiteral("-crf"),
						       QStringLiteral("23"),
						       QStringLiteral("-c:a"),
						       QStringLiteral("aac"),
						       QStringLiteral("-b:a"),
						       QStringLiteral("160k"),
						       QStringLiteral("-movflags"),
						       QStringLiteral("+faststart"),
						       segmentPath},
					   progressStart, progressEnd,
					   obsText("Status.ResamplingVideo").arg(i + 1).arg(ranges.size()), &output);

		if (!ok) {
			if (cancelRequested.load())
				break;

			blog(LOG_ERROR,
			     "[clip-cropper] ffmpeg failed while preparing upload resample segment. output=%s",
			     output.toUtf8().constData());
			emit failed(
				QStringLiteral("Failed to create optimized upload segment with ffmpeg: %1").arg(output));
			QDir(workDirPath).removeRecursively();
			return {};
		}
	}

	if (cancelRequested.load()) {
		QDir(workDirPath).removeRecursively();
		return {};
	}

	if (segmentPaths.isEmpty()) {
		QDir(workDirPath).removeRecursively();
		return result;
	}

	const QString concatListPath = workDir.filePath(QStringLiteral("concat-list.txt"));
	QFile concatList(concatListPath);
	if (!concatList.open(QIODevice::WriteOnly | QIODevice::Text)) {
		QDir(workDirPath).removeRecursively();
		emit failed(QStringLiteral("Failed to write ffmpeg concat list: %1").arg(concatList.errorString()));
		return {};
	}

	QTextStream stream(&concatList);
	for (const QString &segmentPath : segmentPaths)
		stream << concatFileLinePath(segmentPath);
	concatList.close();

	QFile::remove(outputPath);
	QString concatOutput;
	const bool concatOk =
		runProcess(ffmpeg,
			   QStringList{QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-y"),
				       QStringLiteral("-f"), QStringLiteral("concat"), QStringLiteral("-safe"),
				       QStringLiteral("0"), QStringLiteral("-i"), concatListPath, QStringLiteral("-c"),
				       QStringLiteral("copy"), QStringLiteral("-movflags"),
				       QStringLiteral("+faststart"), outputPath},
			   44, 50, obsText("Status.FinalizingResampledVideo"), &concatOutput);

	QDir(workDirPath).removeRecursively();

	if (cancelRequested.load())
		return {};

	if (!concatOk || !QFileInfo(outputPath).isFile() || QFileInfo(outputPath).size() <= 0) {
		QFile::remove(outputPath);
		blog(LOG_ERROR, "[clip-cropper] ffmpeg failed while concatenating optimized upload video. output=%s",
		     concatOutput.toUtf8().constData());
		emit failed(
			QStringLiteral("Failed to finalize optimized upload video with ffmpeg: %1").arg(concatOutput));
		return {};
	}

	CurationSettings adjustedSettings = adjustedSettingsForResampledVideo(curationSettings, ranges);
	saveMarkersForVideoPath(outputPath, adjustedSettings.clipDurations);
	saveReviewSettingsForVideoPath(outputPath, adjustedSettings);
	saveTranscriptCacheForResampledVideo(filePath, outputPath, ranges, curationSettings);

	result.usedResampledVideo = true;
	result.filePath = outputPath;
	result.fileName = QFileInfo(outputPath).fileName();
	result.mimeType = QStringLiteral("video/mp4");
	result.curationSettings = adjustedSettings;

	emit progressChanged(50, obsText("Status.ResampledVideoReady"));
	blog(LOG_INFO,
	     "[clip-cropper] Upload resample completed. original=%s output=%s originalSec=%.2f selectedSec=%.2f adjustedRanges=%d",
	     filePath.toUtf8().constData(), outputPath.toUtf8().constData(), originalDurationSec, selectedDurationSec,
	     result.curationSettings.clipDurations.size());
	return result;
}

void UploadWorker::startOpusUpload(const QString &uploadFilePath, const QString &uploadFileName,
				   const QString &uploadMimeType, const CurationSettings &settings,
				   bool hasResamplePhase)
{
	Q_UNUSED(uploadFileName);
	Q_UNUSED(uploadMimeType);

	auto *opusClient = new OpusClipClient(apiKey, brandTemplateId, sourceLang, settings, this);
	client = opusClient;
	const QPointer<OpusClipClient> safeClient(opusClient);

	connect(opusClient, &OpusClipClient::progressChanged, this,
		[this, hasResamplePhase](int progress, const QString &message) {
			if (hasResamplePhase) {
				emit progressChanged(50 + qBound(0, progress, 100) / 2,
						     obsText("Status.UploadPhase").arg(message));
				return;
			}

			emit progressChanged(progress, message);
		});

	connect(opusClient, &OpusClipClient::uploadFinished, this, [this, safeClient](const OpusUploadResult &result) {
		emit finished(QString::fromStdString(result.projectId));
		if (client == safeClient.data())
			client = nullptr;
		if (safeClient)
			safeClient->deleteLater();
	});

	connect(opusClient, &OpusClipClient::uploadFailed, this, [this, safeClient](const OpusUploadResult &result) {
		QString message = QString::fromUtf8(result.error.message.c_str());
		if (result.httpStatus > 0)
			message += QString(" (HTTP %1)").arg(result.httpStatus);

		emit failed(message);
		if (client == safeClient.data())
			client = nullptr;
		if (safeClient)
			safeClient->deleteLater();
	});

	opusClient->uploadFileResumableAndCreateProjectAsync(uploadFilePath, uploadFileName, uploadMimeType);
}
