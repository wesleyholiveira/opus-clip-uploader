#include "opus/opus-clip-client.hpp"

#include "curation/curation-preset.hpp"

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QStringList>

#include <algorithm>
#include <utility>

static constexpr const char *OPUS_API_BASE_URL = "https://api.opus.pro";

static QString obsText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

static QJsonArray clipDurationsForBounds(double minSec, double maxSec)
{
	QJsonArray bounds;
	bounds.append(minSec);
	bounds.append(maxSec);

	QJsonArray clipDurations;
	clipDurations.append(bounds);
	return clipDurations;
}

OpusClipClient::OpusClipClient(QString apiKey, QString brandTemplateId, QString sourceLang,
			       CurationSettings curationSettings, QObject *parent)
	: QObject(parent),
	  apiKey(std::move(apiKey)),
	  brandTemplateId(std::move(brandTemplateId)),
	  sourceLang(std::move(sourceLang)),
	  curationSettings(std::move(curationSettings))
{
	if (this->sourceLang.trimmed().isEmpty())
		this->sourceLang = "auto";
}

void OpusClipClient::cancel()
{
	cancelRequested = true;

	if (currentUploadFile)
		currentUploadFile->close();

	if (currentReply) {
		currentReply->abort();
		return;
	}

	emitCanceledIfNeeded();
}

void OpusClipClient::emitCanceledIfNeeded()
{
	if (terminalSignalEmitted)
		return;

	terminalSignalEmitted = true;
	OpusUploadResult result;
	result.error.message = obsText("Message.UploadCanceled").toStdString();
	emit uploadFailed(result);
}

void OpusClipClient::fail(const QString &message, int code, long httpStatus, const QByteArray &body)
{
	Q_UNUSED(code);

	if (terminalSignalEmitted)
		return;

	terminalSignalEmitted = true;
	OpusUploadResult result;
	result.httpStatus = httpStatus;
	QString finalMessage = message;
	if (!body.isEmpty())
		finalMessage += QStringLiteral(" - ") + QString::fromUtf8(body);
	result.error.message = finalMessage.toStdString();
	emit uploadFailed(result);
}

void OpusClipClient::uploadFileResumableAndCreateProjectAsync(const QString &filePath, const QString &fileName,
							      const QString &mimeType)
{
	createdProjectIds.clear();
	cancelRequested = false;
	terminalSignalEmitted = false;

	Q_UNUSED(fileName);
	Q_UNUSED(mimeType);

	if (apiKey.trimmed().isEmpty()) {
		fail(QStringLiteral("Opus Clip API key is empty"));
		return;
	}

	QFileInfo info(filePath);

	if (!info.exists() || !info.isFile()) {
		fail(QStringLiteral("Invalid video file path"));
		return;
	}

	createUploadLink(filePath);
}

void OpusClipClient::createUploadLink(const QString &filePath)
{
	QNetworkRequest request{QUrl(QString("%1/api/upload-links").arg(OPUS_API_BASE_URL))};
	request.setRawHeader("Accept", "application/json");
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	request.setRawHeader("Authorization", QString("Bearer %1").arg(apiKey).toUtf8());

	QJsonObject video;
	video.insert("usecase", "LocalUpload");

	QJsonObject payload;
	payload.insert("video", video);

	const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);

	QNetworkReply *reply = network.post(request, body);
	currentReply = reply;

	connect(reply, &QNetworkReply::finished, this, [this, reply, filePath]() {
		const QByteArray response = reply->readAll();
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QNetworkReply::NetworkError error = reply->error();

		if (currentReply == reply)
			currentReply = nullptr;

		if (cancelRequested || error == QNetworkReply::OperationCanceledError) {
			reply->deleteLater();
			emitCanceledIfNeeded();
			return;
		}

		reply->deleteLater();

		if (error != QNetworkReply::NoError || status < 200 || status >= 300) {
			OpusUploadResult result;
			result.httpStatus = status;
			result.error.message = QString("Failed to create Opus upload link: %1 - %2")
						       .arg(reply->errorString(), QString::fromUtf8(response))
						       .toStdString();

			emit uploadFailed(result);
			return;
		}

		const QJsonDocument doc = QJsonDocument::fromJson(response);
		const QJsonObject root = doc.object();

		const QString uploadUrl = root.value("url").toString();
		const QString uploadId = root.value("uploadId").toString();

		if (uploadUrl.trimmed().isEmpty() || uploadId.trimmed().isEmpty()) {
			OpusUploadResult result;
			result.httpStatus = status;
			result.error.message = QString("Invalid Opus upload-links response: %1")
						       .arg(QString::fromUtf8(response))
						       .toStdString();

			emit uploadFailed(result);
			return;
		}

		startResumableSession(filePath, uploadUrl, uploadId);
	});
}

void OpusClipClient::startResumableSession(const QString &filePath, const QString &uploadUrl, const QString &uploadId)
{
	QNetworkRequest request{QUrl(uploadUrl)};
	request.setRawHeader("x-goog-resumable", "start");
	request.setHeader(QNetworkRequest::ContentLengthHeader, 0);

	QNetworkReply *reply = network.post(request, QByteArray());
	currentReply = reply;

	connect(reply, &QNetworkReply::finished, this, [this, reply, filePath, uploadId]() {
		const QByteArray response = reply->readAll();
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QNetworkReply::NetworkError error = reply->error();

		const QString location = QString::fromUtf8(reply->rawHeader("location"));

		if (currentReply == reply)
			currentReply = nullptr;

		if (cancelRequested || error == QNetworkReply::OperationCanceledError) {
			reply->deleteLater();
			emitCanceledIfNeeded();
			return;
		}

		reply->deleteLater();

		if (error != QNetworkReply::NoError || status < 200 || status >= 300) {
			OpusUploadResult result;
			result.httpStatus = status;
			result.error.message = QString("Failed to start GCS resumable session: %1 - %2")
						       .arg(reply->errorString(), QString::fromUtf8(response))
						       .toStdString();

			emit uploadFailed(result);
			return;
		}

		if (location.trimmed().isEmpty()) {
			OpusUploadResult result;
			result.httpStatus = status;
			result.error.message = "GCS resumable session did not return location header";

			emit uploadFailed(result);
			return;
		}

		uploadFileToResumableLocation(filePath, location, uploadId);
	});
}

void OpusClipClient::uploadFileToResumableLocation(const QString &filePath, const QString &location,
						   const QString &uploadId)
{
	auto *file = new QFile(filePath, this);

	if (!file->open(QIODevice::ReadOnly)) {
		OpusUploadResult result;
		result.error.message =
			QString("Failed to open video file for upload: %1").arg(file->errorString()).toStdString();

		file->deleteLater();

		emit uploadFailed(result);
		return;
	}

	QNetworkRequest request{QUrl(location)};
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");

	QNetworkReply *reply = network.put(request, file);
	currentReply = reply;
	currentUploadFile = file;

	connect(reply, &QNetworkReply::uploadProgress, this, [this](qint64 bytesSent, qint64 bytesTotal) {
		if (bytesTotal <= 0)
			return;

		const int uploadProgress = static_cast<int>((bytesSent * 50) / bytesTotal);
		emit progressChanged(qBound(0, uploadProgress, 50), obsText("Status.UploadingVideo"));
	});

	connect(reply, &QNetworkReply::finished, this, [this, reply, file, uploadId]() {
		const QByteArray response = reply->readAll();
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QNetworkReply::NetworkError error = reply->error();

		file->close();
		if (currentUploadFile == file)
			currentUploadFile = nullptr;
		if (currentReply == reply)
			currentReply = nullptr;

		if (cancelRequested || error == QNetworkReply::OperationCanceledError) {
			file->deleteLater();
			reply->deleteLater();
			emitCanceledIfNeeded();
			return;
		}

		file->deleteLater();
		reply->deleteLater();

		if (error != QNetworkReply::NoError || status < 200 || status >= 300) {
			OpusUploadResult result;
			result.httpStatus = status;
			result.error.message = QString("Failed to upload video to GCS resumable location: %1 - %2")
						       .arg(reply->errorString(), QString::fromUtf8(response))
						       .toStdString();

			emit uploadFailed(result);
			return;
		}

		emit progressChanged(50, obsText("Status.UploadCompleteCreatingProjects"));
		createNextClipProject(uploadId, 0);
	});
}

QVector<ClipDuration> OpusClipClient::projectRanges() const
{
	QVector<ClipDuration> ranges = curationSettings.clipDurations;

	if (ranges.isEmpty() && curationSettings.rangeEndSec > curationSettings.rangeStartSec)
		ranges.append({curationSettings.rangeStartSec, curationSettings.rangeEndSec});

	QVector<ClipDuration> validRanges;
	validRanges.reserve(ranges.size());

	for (const auto &range : ranges) {
		if (range.endSec > range.startSec)
			validRanges.append(range);
	}

	return validRanges;
}

void OpusClipClient::createNextClipProject(const QString &uploadId, int projectIndex)
{
	const QVector<ClipDuration> ranges = projectRanges();

	if (ranges.isEmpty()) {
		OpusUploadResult result;
		result.error.message = "No valid clip ranges were selected";
		emit uploadFailed(result);
		return;
	}

	if (projectIndex >= ranges.size()) {
		if (terminalSignalEmitted)
			return;

		terminalSignalEmitted = true;
		OpusUploadResult result;
		result.ok = true;
		result.httpStatus = 200;
		result.projectId = QStringList(createdProjectIds).join(", ").toStdString();
		emit progressChanged(100, obsText("Status.AllClipProjectsCreated"));
		emit uploadFinished(result);
		return;
	}

	createClipProject(uploadId, ranges[projectIndex], projectIndex, ranges.size());
}

void OpusClipClient::createClipProject(const QString &uploadId, const ClipDuration &rangeValue, int projectIndex,
				       int totalProjects)
{
	emit progressChanged(50 + static_cast<int>((projectIndex * 50.0) / std::max(1, totalProjects)),
			     obsText("Status.CreatingClipProject").arg(projectIndex + 1).arg(totalProjects));

	QNetworkRequest request{QUrl(QString("%1/api/clip-projects").arg(OPUS_API_BASE_URL))};
	request.setRawHeader("Accept", "application/json");
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	request.setRawHeader("Authorization", QString("Bearer %1").arg(apiKey).toUtf8());

	QJsonObject importPref;
	importPref.insert("sourceLang", sourceLang.trimmed().isEmpty() ? "auto" : sourceLang.trimmed());

	QJsonObject payload;
	payload.insert("videoUrl", uploadId);
	payload.insert("importPref", importPref);

	if (!brandTemplateId.trimmed().isEmpty())
		payload.insert("brandTemplateId", brandTemplateId.trimmed());

	QJsonObject curationPref;

	QJsonObject range;
	range.insert("startSec", rangeValue.startSec);
	range.insert("endSec", rangeValue.endSec);
	curationPref.insert("range", range);

	const double rangeDurationSec = std::max(0.0, rangeValue.endSec - rangeValue.startSec);
	const bool createFixedClip = curationSettings.skipCurate;
	const CurationPreset::ClipLengthBounds clipLengthBounds =
		CurationPreset::clipLengthBoundsForSettings(curationSettings);
	const bool hasPreferredClipLength = !createFixedClip && clipLengthBounds.enabled;

	if (createFixedClip) {
		QJsonArray clipDurations;
		QJsonArray item;
		item.append(rangeValue.startSec);
		item.append(rangeValue.endSec);
		clipDurations.append(item);
		curationPref.insert("clipDurations", clipDurations);

		QJsonArray clipStarts;
		clipStarts.append(rangeValue.startSec);
		curationPref.insert("clip_start", clipStarts);

		QJsonArray clipDurationSeconds;
		clipDurationSeconds.append(rangeDurationSec);
		curationPref.insert("clip_duration", clipDurationSeconds);
	} else if (hasPreferredClipLength) {
		curationPref.insert("clipDurations",
				    clipDurationsForBounds(clipLengthBounds.minSec, clipLengthBounds.maxSec));
	}

	QJsonArray topicKeywords;
	for (const QString &keyword : curationSettings.topicKeywords)
		topicKeywords.append(keyword);

	curationPref.insert("model", curationSettings.model.trimmed().isEmpty() ? "ClipAnything"
										: curationSettings.model.trimmed());
	curationPref.insert("topicKeywords", topicKeywords);
	curationPref.insert("genre", curationSettings.genre.trimmed().isEmpty() ? "Auto" : curationSettings.genre);
	curationPref.insert("skipCurate", curationSettings.skipCurate);

	if (!curationSettings.aiPrompt.trimmed().isEmpty()) {
		curationPref.insert("prompt", curationSettings.aiPrompt.trimmed());
		curationPref.insert("userPrompt", curationSettings.aiPrompt.trimmed());
	}

	const QString clipLengthBoundsLog = hasPreferredClipLength ? QStringLiteral("%1-%2")
									     .arg(clipLengthBounds.minSec, 0, 'f', 0)
									     .arg(clipLengthBounds.maxSec, 0, 'f', 0)
								   : QStringLiteral("auto");
	const QString effectivePresetId = CurationPreset::resolveId(curationSettings, curationSettings.aiPrompt);

	blog(LOG_INFO,
	     "[clip-cropper] Creating Opus Clip project %d/%d. mode=%s startSec=%.2f endSec=%.2f durationSec=%.2f "
	     "skipCurate=%s clipLengthPreset=%s curationPreset=%s clipLengthBounds=%s clipLengthBoundsSource=%s",
	     projectIndex + 1, totalProjects, createFixedClip ? "fixed-clip" : "curation-window", rangeValue.startSec,
	     rangeValue.endSec, rangeDurationSec, curationSettings.skipCurate ? "true" : "false",
	     curationSettings.clipLengthPreset.toUtf8().constData(), effectivePresetId.toUtf8().constData(),
	     clipLengthBoundsLog.toUtf8().constData(), clipLengthBounds.source.toUtf8().constData());

	payload.insert("curationPref", curationPref);

	const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);

	QNetworkReply *reply = network.post(request, body);
	currentReply = reply;

	connect(reply, &QNetworkReply::finished, this, [this, reply, uploadId, projectIndex, totalProjects]() {
		const QByteArray response = reply->readAll();
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QNetworkReply::NetworkError error = reply->error();
		const QString errorString = reply->errorString();

		if (currentReply == reply)
			currentReply = nullptr;

		if (cancelRequested || error == QNetworkReply::OperationCanceledError) {
			reply->deleteLater();
			emitCanceledIfNeeded();
			return;
		}

		reply->deleteLater();

		if (error != QNetworkReply::NoError || status < 200 || status >= 300) {
			OpusUploadResult result;
			result.httpStatus = status;
			result.error.message = QString("Failed to create Opus Clip project %1/%2: %3 - %4")
						       .arg(projectIndex + 1)
						       .arg(totalProjects)
						       .arg(errorString, QString::fromUtf8(response))
						       .toStdString();

			emit uploadFailed(result);
			return;
		}

		const QString projectId = extractProjectId(response, uploadId);
		createdProjectIds.append(projectId);

		emit progressChanged(50 + static_cast<int>(((projectIndex + 1) * 50.0) / std::max(1, totalProjects)),
				     obsText("Status.CreatedClipProject").arg(projectIndex + 1).arg(totalProjects));

		createNextClipProject(uploadId, projectIndex + 1);
	});
}

QString OpusClipClient::extractProjectId(const QByteArray &response, const QString &fallbackUploadId) const
{
	const QJsonDocument doc = QJsonDocument::fromJson(response);

	if (!doc.isObject())
		return fallbackUploadId;

	const QJsonObject root = doc.object();

	const QString directId = root.value("id").toString();

	if (!directId.trimmed().isEmpty())
		return directId;

	const QString projectId = root.value("projectId").toString();

	if (!projectId.trimmed().isEmpty())
		return projectId;

	const QJsonObject project = root.value("project").toObject();
	const QString nestedId = project.value("id").toString();

	if (!nestedId.trimmed().isEmpty())
		return nestedId;

	return fallbackUploadId;
}