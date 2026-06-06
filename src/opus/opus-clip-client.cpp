#ifdef __cplusplus
extern "C" {
#endif

#include <obs-module.h>
#include "plugin-support.h"

#ifdef __cplusplus
}
#endif

#include "opus/opus-clip-client.hpp"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <utility>

OpusClipClient::OpusClipClient(QString apiKey, QObject *parent) : QObject(parent), apiKey(std::move(apiKey)) {}

void OpusClipClient::uploadFileResumableAndCreateProjectAsync(const QString &path, const QString &fileName,
							      const QString &mimeType)
{
	this->path = path;
	this->fileName = fileName;
	this->mimeType = mimeType.isEmpty() ? QStringLiteral("application/octet-stream") : mimeType;
	this->uploadLinkUrl.clear();
	this->resumableSessionUrl.clear();
	this->uploadId.clear();
	this->offset = 0;
	this->transientRetries = 0;
	this->fileSize = fileSizeOrZero(path);

	if (apiKey.trimmed().isEmpty()) {
		fail("Opus Clip API key is empty");
		return;
	}

	if (this->fileSize == 0) {
		fail(QString("Could not determine file size: %1").arg(path));
		return;
	}

	if (uploadFile.isOpen()) {
		uploadFile.close();
	}
	uploadFile.setFileName(path);
	if (!uploadFile.open(QIODevice::ReadOnly)) {
		fail(QString("Failed to open file: %1").arg(path));
		return;
	}

	emit progressChanged(0);
	generateUploadLinkAsync();
}

void OpusClipClient::generateUploadLinkAsync()
{
	QNetworkRequest request(QUrl("https://api.opus.pro/api/upload-links"));
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	request.setRawHeader("Accept", "application/json");
	request.setRawHeader("Authorization", "Bearer " + apiKey.toUtf8());
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);

	QJsonObject video;
	video.insert("usecase", "LocalUpload");

	QJsonObject body;
	body.insert("video", video);

	QNetworkReply *reply = network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));

	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		const long httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toLongLong();
		const QByteArray body = reply->readAll();

		if (reply->error() != QNetworkReply::NoError) {
			fail(reply->errorString(), reply->error(), httpStatus, body);
			return;
		}

		if (httpStatus < 200 || httpStatus >= 300) {
			fail(QString("Failed to generate Opus Clip upload link. HTTP status %1").arg(httpStatus),
			     httpStatus, httpStatus, body);
			return;
		}

		const QJsonDocument document = QJsonDocument::fromJson(body);
		if (!document.isObject()) {
			fail("Invalid Opus Clip upload link response", -1, httpStatus, body);
			return;
		}

		const QJsonObject object = document.object();
		uploadLinkUrl = object.value("url").toString();
		uploadId = object.value("uploadId").toString();

		if (uploadLinkUrl.trimmed().isEmpty() || uploadId.trimmed().isEmpty()) {
			fail("Opus Clip upload link response did not contain url/uploadId", -1, httpStatus, body);
			return;
		}

		initiateResumableUploadSessionAsync();
	});
}

void OpusClipClient::initiateResumableUploadSessionAsync()
{
	QNetworkRequest request{QUrl(uploadLinkUrl)};
	request.setRawHeader("x-goog-resumable", "start");
	request.setHeader(QNetworkRequest::ContentLengthHeader, 0);
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);

	QNetworkReply *reply = network.post(request, QByteArray());

	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		const long httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toLongLong();
		const QByteArray body = reply->readAll();

		if (reply->error() != QNetworkReply::NoError) {
			fail(reply->errorString(), reply->error(), httpStatus, body);
			return;
		}

		if (httpStatus < 200 || httpStatus >= 300) {
			fail(QString("Failed to initiate Opus Clip resumable upload session. HTTP status %1")
				     .arg(httpStatus),
			     httpStatus, httpStatus, body);
			return;
		}

		const QByteArray location = reply->rawHeader("Location");
		if (location.isEmpty()) {
			fail("Opus Clip/GCS did not return a resumable session Location header", -1, httpStatus, body);
			return;
		}

		resumableSessionUrl = QString::fromUtf8(location);
		offset = 0;
		uploadNextChunkAsync();
	});
}

void OpusClipClient::uploadNextChunkAsync()
{
	if (offset >= fileSize) {
		createClipProjectAsync();
		return;
	}

	if (!uploadFile.isOpen()) {
		uploadFile.setFileName(path);
		if (!uploadFile.open(QIODevice::ReadOnly)) {
			fail(QString("Failed to open file: %1").arg(path));
			return;
		}
	}

	if (!uploadFile.seek(static_cast<qint64>(offset))) {
		fail(QString("Failed to seek file to offset %1").arg(offset));
		return;
	}

	const std::uint64_t remaining = fileSize - offset;
	const std::uint64_t currentChunkSize = std::min(DefaultChunkSize, remaining);
	const std::uint64_t end = offset + currentChunkSize - 1;
	const QByteArray payload = uploadFile.read(static_cast<qint64>(currentChunkSize));

	if (static_cast<std::uint64_t>(payload.size()) != currentChunkSize) {
		fail(QString("Failed to read upload chunk. Expected %1 bytes, got %2")
			     .arg(currentChunkSize)
			     .arg(payload.size()));
		return;
	}

	QNetworkRequest request{QUrl(resumableSessionUrl)};
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
	request.setHeader(QNetworkRequest::ContentLengthHeader, payload.size());
	request.setRawHeader("Content-Range", QString("bytes %1-%2/%3").arg(offset).arg(end).arg(fileSize).toUtf8());
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);

	QNetworkReply *reply = network.put(request, payload);
	const std::uint64_t chunkStart = offset;

	connect(reply, &QNetworkReply::uploadProgress, this, [this, chunkStart](qint64 sent, qint64) {
		if (fileSize == 0)
			return;

		const auto uploaded = chunkStart + static_cast<std::uint64_t>(std::max<qint64>(0, sent));
		const int progress = static_cast<int>((uploaded * 100) / fileSize);
		emit progressChanged(progress);
	});

	connect(reply, &QNetworkReply::finished, this, [this, reply, end]() {
		reply->deleteLater();

		const long httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toLongLong();
		const QByteArray body = reply->readAll();

		if (reply->error() != QNetworkReply::NoError && httpStatus != 308) {
			if (httpStatus == 0 || httpStatus >= 500) {
				retryOrQueryUploadStatusAsync();
				return;
			}

			fail(reply->errorString(), reply->error(), httpStatus, body);
			return;
		}

		if (httpStatus == 200 || httpStatus == 201) {
			transientRetries = 0;
			offset = fileSize;
			emit progressChanged(100);
			createClipProjectAsync();
			return;
		}

		if (httpStatus == 308) {
			transientRetries = 0;
			offset = parseNextOffsetFromRange(reply->rawHeader("Range"), end + 1);
			if (fileSize > 0) {
				emit progressChanged(static_cast<int>((offset * 100) / fileSize));
			}

			uploadNextChunkAsync();
			return;
		}

		if (httpStatus == 0 || httpStatus >= 500) {
			retryOrQueryUploadStatusAsync();
			return;
		}

		fail(QString("Opus Clip upload chunk failed. HTTP status %1").arg(httpStatus), httpStatus, httpStatus,
		     body);
	});
}

void OpusClipClient::queryUploadStatusAsync()
{
	QNetworkRequest request{QUrl(resumableSessionUrl)};
	request.setRawHeader("Content-Range", QString("bytes */%1").arg(fileSize).toUtf8());
	request.setHeader(QNetworkRequest::ContentLengthHeader, 0);
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);

	QNetworkReply *reply = network.put(request, QByteArray());

	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		const long httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toLongLong();
		const QByteArray body = reply->readAll();

		if (reply->error() != QNetworkReply::NoError && httpStatus != 308) {
			fail(reply->errorString(), reply->error(), httpStatus, body);
			return;
		}

		if (httpStatus == 200 || httpStatus == 201) {
			transientRetries = 0;
			offset = fileSize;
			emit progressChanged(100);
			createClipProjectAsync();
			return;
		}

		if (httpStatus == 308) {
			transientRetries = 0;
			offset = parseNextOffsetFromRange(reply->rawHeader("Range"), 0);
			if (fileSize > 0) {
				emit progressChanged(static_cast<int>((offset * 100) / fileSize));
			}

			uploadNextChunkAsync();
			return;
		}

		fail(QString("Failed to query Opus Clip resumable upload status. HTTP status %1").arg(httpStatus),
		     httpStatus, httpStatus, body);
	});
}

void OpusClipClient::retryOrQueryUploadStatusAsync()
{
	if (transientRetries >= MaxTransientRetries) {
		queryUploadStatusAsync();
		return;
	}

	const int delayMs = 1000 * (1 << transientRetries);
	transientRetries++;
	QTimer::singleShot(delayMs, this, [this]() { queryUploadStatusAsync(); });
}

void OpusClipClient::createClipProjectAsync()
{
	QNetworkRequest request(QUrl("https://api.opus.pro/api/clip-projects"));
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	request.setRawHeader("Accept", "application/json");
	request.setRawHeader("Authorization", "Bearer " + apiKey.toUtf8());
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);

	QJsonObject body;
	body.insert("videoUrl", uploadId);

	QNetworkReply *reply = network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));

	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		const long httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toLongLong();
		const QByteArray body = reply->readAll();

		if (reply->error() != QNetworkReply::NoError) {
			fail(reply->errorString(), reply->error(), httpStatus, body);
			return;
		}

		if (httpStatus < 200 || httpStatus >= 300) {
			fail(QString("Failed to create Opus Clip project. HTTP status %1").arg(httpStatus), httpStatus,
			     httpStatus, body);
			return;
		}

		UploadResult result;
		result.ok = true;
		result.completed = true;
		result.httpStatus = httpStatus;
		result.response = body.toStdString();
		result.sessionUrl = resumableSessionUrl.toStdString();
		result.uploadId = uploadId.toStdString();
		result.uploadedBytes = fileSize;
		result.nextOffset = fileSize;

		const QJsonDocument document = QJsonDocument::fromJson(body);
		if (document.isObject()) {
			const QJsonObject object = document.object();
			const QString projectId = object.value("id").toString(object.value("projectId").toString());
			result.projectId = projectId.toStdString();
		}

		finish(result);
	});
}

void OpusClipClient::fail(const QString &message, int code, long httpStatus, const QByteArray &body)
{
	if (uploadFile.isOpen()) {
		uploadFile.close();
	}

	UploadResult result;
	result.ok = false;
	result.completed = false;
	result.httpStatus = httpStatus;
	result.response = body.toStdString();
	result.sessionUrl = resumableSessionUrl.toStdString();
	result.uploadId = uploadId.toStdString();
	result.nextOffset = offset;
	result.uploadedBytes = offset;
	result.error = {code, message.toStdString()};

	emit uploadFailed(result);
}

void OpusClipClient::finish(const UploadResult &result)
{
	if (uploadFile.isOpen()) {
		uploadFile.close();
	}

	emit uploadFinished(result);
}

std::uint64_t OpusClipClient::fileSizeOrZero(const QString &path)
{
	const QFileInfo info(path);
	if (!info.exists() || !info.isFile()) {
		return 0;
	}

	const qint64 size = info.size();
	if (size <= 0) {
		return 0;
	}

	return static_cast<std::uint64_t>(size);
}

std::uint64_t OpusClipClient::parseNextOffsetFromRange(const QByteArray &rangeHeader, std::uint64_t fallback)
{
	const QString range = QString::fromUtf8(rangeHeader).trimmed();
	const int dash = range.indexOf('-');
	if (dash < 0) {
		return fallback;
	}

	bool ok = false;
	const std::uint64_t lastReceived = range.mid(dash + 1).trimmed().toULongLong(&ok);
	if (!ok) {
		return fallback;
	}

	return lastReceived + 1;
}
