#include "opus/opus-clip-client.hpp"

#include "curation/curation-preset.hpp"
#include "opus/opus-api-types.hpp"
#include "opus/opus-curation-payload-builder.hpp"

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QStringList>
#include <QVariant>

#include <algorithm>
#include <utility>

namespace {
constexpr int OpusProjectStatusPollDelayMs = 30000;
constexpr int OpusProjectStatusMaxAttempts = 240;
}

static QString obsText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
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

void OpusClipClient::setWaitForProjectCompletionBeforeFinish(bool enabled)
{
	waitForProjectCompletionBeforeFinish = enabled;
}

void OpusClipClient::releaseCurrentUploadPayload()
{
	QByteArray emptyPayload;
	currentUploadPayload.swap(emptyPayload);
}

void OpusClipClient::cancel()
{
	cancelRequested = true;

	if (currentUploadFile)
		currentUploadFile->close();
	if (currentUploadBuffer)
		currentUploadBuffer->close();
	releaseCurrentUploadPayload();

	if (currentReply) {
		currentReply->abort();
		return;
	}

	emitCanceledIfNeeded();
}

void OpusClipClient::emitCanceledIfNeeded()
{
	if (currentUploadBuffer)
		currentUploadBuffer->close();
	releaseCurrentUploadPayload();

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

	if (currentUploadBuffer)
		currentUploadBuffer->close();
	releaseCurrentUploadPayload();

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

void OpusClipClient::uploadDataResumableAndCreateProjectAsync(QByteArray data, const QString &fileName,
							      const QString &mimeType)
{
	createdProjectIds.clear();
	cancelRequested = false;
	terminalSignalEmitted = false;
	releaseCurrentUploadPayload();

	Q_UNUSED(fileName);
	Q_UNUSED(mimeType);

	if (apiKey.trimmed().isEmpty()) {
		fail(QStringLiteral("Opus Clip API key is empty"));
		return;
	}

	if (data.isEmpty()) {
		fail(QStringLiteral("Invalid in-memory video payload"));
		return;
	}

	currentUploadPayload = std::move(data);
	createUploadLinkForData();
}

void OpusClipClient::createUploadLink(const QString &filePath)
{
	QNetworkRequest request{QUrl(QString("%1/api/upload-links").arg(OpusApi::BaseUrl))};
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

void OpusClipClient::createUploadLinkForData()
{
	QNetworkRequest request{QUrl(QString("%1/api/upload-links").arg(OpusApi::BaseUrl))};
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

	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
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
			fail(QString("Failed to create Opus upload link: %1 - %2")
				     .arg(reply->errorString(), QString::fromUtf8(response)),
			     -1, status);
			return;
		}

		const QJsonDocument doc = QJsonDocument::fromJson(response);
		const QJsonObject root = doc.object();

		const QString uploadUrl = root.value("url").toString();
		const QString uploadId = root.value("uploadId").toString();

		if (uploadUrl.trimmed().isEmpty() || uploadId.trimmed().isEmpty()) {
			fail(QString("Invalid Opus upload-links response: %1").arg(QString::fromUtf8(response)), -1, status);
			return;
		}

		startResumableSessionForData(uploadUrl, uploadId);
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

void OpusClipClient::startResumableSessionForData(const QString &uploadUrl, const QString &uploadId)
{
	QNetworkRequest request{QUrl(uploadUrl)};
	request.setRawHeader("x-goog-resumable", "start");
	request.setHeader(QNetworkRequest::ContentLengthHeader, 0);

	QNetworkReply *reply = network.post(request, QByteArray());
	currentReply = reply;

	connect(reply, &QNetworkReply::finished, this, [this, reply, uploadId]() {
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
			fail(QString("Failed to start GCS resumable session: %1 - %2")
				     .arg(reply->errorString(), QString::fromUtf8(response)),
			     -1, status);
			return;
		}

		if (location.trimmed().isEmpty()) {
			fail(QStringLiteral("GCS resumable session did not return location header"), -1, status);
			return;
		}

		uploadDataToResumableLocation(location, uploadId);
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

void OpusClipClient::uploadDataToResumableLocation(const QString &location, const QString &uploadId)
{
	auto *buffer = new QBuffer(&currentUploadPayload, this);

	if (!buffer->open(QIODevice::ReadOnly)) {
		buffer->deleteLater();
		releaseCurrentUploadPayload();
		fail(QStringLiteral("Failed to open in-memory video payload for upload"));
		return;
	}

	QNetworkRequest request{QUrl(location)};
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
	request.setHeader(QNetworkRequest::ContentLengthHeader, static_cast<qint64>(currentUploadPayload.size()));

	QNetworkReply *reply = network.put(request, buffer);
	currentReply = reply;
	currentUploadBuffer = buffer;

	connect(reply, &QNetworkReply::uploadProgress, this, [this](qint64 bytesSent, qint64 bytesTotal) {
		if (bytesTotal <= 0)
			return;

		const int uploadProgress = static_cast<int>((bytesSent * 50) / bytesTotal);
		emit progressChanged(qBound(0, uploadProgress, 50), obsText("Status.UploadingVideo"));
	});

	connect(reply, &QNetworkReply::finished, this, [this, reply, buffer, uploadId]() {
		const QByteArray response = reply->readAll();
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QNetworkReply::NetworkError error = reply->error();

		buffer->close();
		if (currentUploadBuffer == buffer)
			currentUploadBuffer = nullptr;
		if (currentReply == reply)
			currentReply = nullptr;

		if (cancelRequested || error == QNetworkReply::OperationCanceledError) {
			buffer->deleteLater();
			releaseCurrentUploadPayload();
			reply->deleteLater();
			emitCanceledIfNeeded();
			return;
		}

		buffer->deleteLater();
		releaseCurrentUploadPayload();
		reply->deleteLater();

		if (error != QNetworkReply::NoError || status < 200 || status >= 300) {
			fail(QString("Failed to upload video to GCS resumable location: %1 - %2")
				     .arg(reply->errorString(), QString::fromUtf8(response)),
			     -1, status);
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


QString OpusClipClient::projectStageFromResponse(const QByteArray &body) const
{
	const QJsonDocument doc = QJsonDocument::fromJson(body);
	if (!doc.isObject())
		return {};

	const QJsonObject root = doc.object();
	const QString directStage = root.value(QStringLiteral("stage")).toString().trimmed();
	if (!directStage.isEmpty())
		return directStage.toUpper();

	const QJsonObject project = root.value(QStringLiteral("project")).toObject();
	const QString projectStage = project.value(QStringLiteral("stage")).toString().trimmed();
	if (!projectStage.isEmpty())
		return projectStage.toUpper();

	const QJsonObject data = root.value(QStringLiteral("data")).toObject();
	const QString dataStage = data.value(QStringLiteral("stage")).toString().trimmed();
	if (!dataStage.isEmpty())
		return dataStage.toUpper();

	return {};
}

bool OpusClipClient::isTerminalProjectStage(const QString &stage) const
{
	const QString value = stage.trimmed().toUpper();
	return value == QStringLiteral("COMPLETE") || value == QStringLiteral("STALLED") ||
	       value == QStringLiteral("FAILED") || value == QStringLiteral("ERROR") ||
	       value == QStringLiteral("CANCELED") || value == QStringLiteral("CANCELLED") ||
	       value == QStringLiteral("DELETED") || value == QStringLiteral("PURGED");
}

bool OpusClipClient::isSuccessfulProjectStage(const QString &stage) const
{
	return stage.trimmed().toUpper() == QStringLiteral("COMPLETE");
}

void OpusClipClient::pollProjectUntilTerminal(const QString &uploadId, const QString &projectId, int projectIndex,
						      int totalProjects, int attempt)
{
	if (terminalSignalEmitted || cancelRequested) {
		emitCanceledIfNeeded();
		return;
	}

	if (attempt >= OpusProjectStatusMaxAttempts) {
		blog(LOG_WARNING,
		     "[clip-cropper] Timed out waiting for Opus project processing to finish. projectId=%s attempts=%d",
		     projectId.toUtf8().constData(), attempt);
		createNextClipProject(uploadId, projectIndex + 1);
		return;
	}

	emit progressChanged(50 + static_cast<int>(((projectIndex + 1) * 50.0) / std::max(1, totalProjects)),
			     QStringLiteral("Waiting for Opus project %1/%2 to finish processing...")
				     .arg(projectIndex + 1)
				     .arg(totalProjects));

	QUrl url(QStringLiteral("%1/api/clip-projects/%2")
			 .arg(OpusApi::BaseUrl, QString::fromUtf8(QUrl::toPercentEncoding(projectId))));
	QNetworkRequest request{url};
	request.setRawHeader("Accept", "application/json");
	request.setRawHeader("Authorization", QString("Bearer %1").arg(apiKey).toUtf8());

	QNetworkReply *reply = network.get(request);
	currentReply = reply;

	connect(reply, &QNetworkReply::finished, this,
		[this, reply, uploadId, projectId, projectIndex, totalProjects, attempt]() {
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
				blog(LOG_WARNING,
				     "[clip-cropper] Could not poll Opus project status. projectId=%s status=%d error=%s response=%s attempt=%d/%d",
				     projectId.toUtf8().constData(), status, errorString.toUtf8().constData(),
				     response.constData(), attempt + 1, OpusProjectStatusMaxAttempts);
				QTimer::singleShot(OpusProjectStatusPollDelayMs, this,
						   [this, uploadId, projectId, projectIndex, totalProjects, attempt]() {
							   pollProjectUntilTerminal(uploadId, projectId, projectIndex, totalProjects,
											    attempt + 1);
						   });
				return;
			}

			const QString stage = projectStageFromResponse(response);
			blog(LOG_INFO, "[clip-cropper] Polled Opus project status. projectId=%s stage=%s attempt=%d/%d",
			     projectId.toUtf8().constData(), stage.toUtf8().constData(), attempt + 1,
			     OpusProjectStatusMaxAttempts);

			if (isTerminalProjectStage(stage)) {
				if (!isSuccessfulProjectStage(stage)) {
					blog(LOG_WARNING,
					     "[clip-cropper] Opus project reached non-success terminal stage. projectId=%s stage=%s. Continuing candidate queue.",
					     projectId.toUtf8().constData(), stage.toUtf8().constData());
				}
				createNextClipProject(uploadId, projectIndex + 1);
				return;
			}

			QTimer::singleShot(OpusProjectStatusPollDelayMs, this,
					   [this, uploadId, projectId, projectIndex, totalProjects, attempt]() {
						   pollProjectUntilTerminal(uploadId, projectId, projectIndex, totalProjects,
									    attempt + 1);
					   });
		});
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

	QNetworkRequest request{QUrl(QString("%1/api/clip-projects").arg(OpusApi::BaseUrl))};
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

	const QJsonObject curationPref = OpusCurationPayloadBuilder{}.build(rangeValue, curationSettings);

	const double rangeDurationSec = std::max(0.0, rangeValue.endSec - rangeValue.startSec);
	const bool createFixedClip = curationSettings.skipCurate;
	const CurationPreset::ClipLengthBounds clipLengthBounds =
		CurationPreset::clipLengthBoundsForSettings(curationSettings);
	const bool hasPreferredClipLength = !createFixedClip && clipLengthBounds.enabled;

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

		if (waitForProjectCompletionBeforeFinish) {
			pollProjectUntilTerminal(uploadId, projectId, projectIndex, totalProjects, 0);
			return;
		}

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