#ifdef __cplusplus
extern "C" {
#endif

#include <obs-module.h>
#include "plugin-support.h"

#ifdef __cplusplus
}
#endif

#include "utils/request.hpp"

#include <QByteArray>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

DriveRequest::DriveRequest(QString accessToken, QObject *parent) : QObject(parent), accessToken(std::move(accessToken))
{
}

void DriveRequest::uploadFileResumableAsync(const QString &path, const QString &fileName, const QString &mimeType,
					    const QString &folderName)
{
	this->path = path;
	this->fileName = fileName;
	this->mimeType = mimeType;
	this->folderName = folderName;
	this->folderId.clear();
	this->sessionUrl.clear();
	this->offset = 0;

	this->fileSize = fileSizeOrZero(path);

	if (this->fileSize == 0) {
		fail(QString("Could not determine file size: %1").arg(path));
		return;
	}

	emit progressChanged(0);

	if (!folderName.isEmpty()) {
		findFolderIdByNameAsync();
		return;
	}

	createResumableSessionAsync();
}

void DriveRequest::findFolderIdByNameAsync()
{
	const QString query = QString("name='%1' and mimeType='application/vnd.google-apps.folder' and trashed=false")
				      .arg(escapeDriveQueryString(folderName));

	QUrl url("https://www.googleapis.com/drive/v3/files");

	QUrlQuery urlQuery;
	urlQuery.addQueryItem("q", query);
	urlQuery.addQueryItem("fields", "files(id,name)");
	urlQuery.addQueryItem("pageSize", "10");
	urlQuery.addQueryItem("spaces", "drive");

	url.setQuery(urlQuery);

	QNetworkRequest request(url);
	request.setRawHeader("Authorization", "Bearer " + accessToken.toUtf8());
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);

	QNetworkReply *reply = network.get(request);

	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		const long httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toLongLong();

		const QByteArray body = reply->readAll();

		if (reply->error() != QNetworkReply::NoError) {
			fail(reply->errorString(), reply->error(), httpStatus, body);
			return;
		}

		if (httpStatus < 200 || httpStatus >= 300) {
			fail(QString("Failed to find Google Drive folder. HTTP status %1").arg(httpStatus), httpStatus,
			     httpStatus, body);
			return;
		}

		const QJsonDocument document = QJsonDocument::fromJson(body);

		if (!document.isObject()) {
			fail("Invalid Google Drive folder search response", -1, httpStatus, body);
			return;
		}

		const QJsonArray files = document.object().value("files").toArray();

		if (files.isEmpty()) {
			fail(QString("Could not find Google Drive folder by name: %1").arg(folderName), -1, httpStatus,
			     body);
			return;
		}

		folderId = files.first().toObject().value("id").toString();

		if (folderId.isEmpty()) {
			fail("Google Drive folder response did not contain folder id", -1, httpStatus, body);
			return;
		}

		createResumableSessionAsync();
	});
}

void DriveRequest::createResumableSessionAsync()
{
	QUrl url("https://www.googleapis.com/upload/drive/v3/files");

	QUrlQuery query;
	query.addQueryItem("uploadType", "resumable");
	url.setQuery(query);

	QJsonObject metadata;
	metadata.insert("name", fileName);

	if (!folderId.isEmpty()) {
		QJsonArray parents;
		parents.append(folderId);
		metadata.insert("parents", parents);
	}

	const QByteArray payload = QJsonDocument(metadata).toJson(QJsonDocument::Compact);

	QNetworkRequest request(url);
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json; charset=UTF-8");
	request.setRawHeader("Authorization", "Bearer " + accessToken.toUtf8());
	request.setRawHeader("X-Upload-Content-Type", mimeType.toUtf8());
	request.setRawHeader("X-Upload-Content-Length", QByteArray::number(fileSize));
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);

	QNetworkReply *reply = network.post(request, payload);

	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		const long httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toLongLong();

		const QByteArray body = reply->readAll();

		if (reply->error() != QNetworkReply::NoError) {
			fail(reply->errorString(), reply->error(), httpStatus, body);
			return;
		}

		if (httpStatus < 200 || httpStatus >= 300) {
			fail(QString("Failed to create resumable upload session. HTTP status %1").arg(httpStatus),
			     httpStatus, httpStatus, body);
			return;
		}

		const QByteArray location = reply->rawHeader("Location");

		if (location.isEmpty()) {
			fail("Google Drive did not return a resumable session Location header", -1, httpStatus, body);
			return;
		}

		sessionUrl = QString::fromUtf8(location);
		offset = 0;

		uploadNextChunkAsync();
	});
}

void DriveRequest::uploadNextChunkAsync()
{
	if (offset >= fileSize) {
		UploadResult result;
		result.ok = true;
		result.completed = true;
		result.httpStatus = 200;
		result.sessionUrl = sessionUrl.toStdString();
		result.nextOffset = fileSize;
		result.uploadedBytes = fileSize;

		finish(result);
		return;
	}

	QFile file(path);

	if (!file.open(QIODevice::ReadOnly)) {
		fail(QString("Failed to open file: %1").arg(path));
		return;
	}

	if (!file.seek(static_cast<qint64>(offset))) {
		fail(QString("Failed to seek file to offset %1").arg(offset));
		return;
	}

	const std::uint64_t remaining = fileSize - offset;
	const std::uint64_t currentChunkSize = std::min(DefaultChunkSize, remaining);
	const std::uint64_t end = offset + currentChunkSize - 1;

	const QByteArray payload = file.read(static_cast<qint64>(currentChunkSize));

	if (static_cast<std::uint64_t>(payload.size()) != currentChunkSize) {
		fail(QString("Failed to read upload chunk. Expected %1 bytes, got %2")
			     .arg(currentChunkSize)
			     .arg(payload.size()));
		return;
	}

	QNetworkRequest request{QUrl(sessionUrl)};
	request.setHeader(QNetworkRequest::ContentTypeHeader, mimeType);
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

		// Google usa 308 como "Resume Incomplete".
		// QNetworkReply pode considerar isso como erro HTTP, então não trate 308 como falha aqui.
		if (reply->error() != QNetworkReply::NoError && httpStatus != 308) {
			if (httpStatus == 0 || httpStatus >= 500) {
				queryUploadStatusAsync();
				return;
			}

			fail(reply->errorString(), reply->error(), httpStatus, body);
			return;
		}

		if (httpStatus == 200 || httpStatus == 201) {
			emit progressChanged(100);

			UploadResult result;
			result.ok = true;
			result.completed = true;
			result.httpStatus = httpStatus;
			result.response = body.toStdString();
			result.sessionUrl = sessionUrl.toStdString();
			result.nextOffset = fileSize;
			result.uploadedBytes = fileSize;

			finish(result);
			return;
		}

		if (httpStatus == 308) {
			offset = parseNextOffsetFromRange(reply->rawHeader("Range"), end + 1);

			if (fileSize > 0) {
				emit progressChanged(static_cast<int>((offset * 100) / fileSize));
			}

			uploadNextChunkAsync();
			return;
		}

		if (httpStatus == 0 || httpStatus >= 500) {
			queryUploadStatusAsync();
			return;
		}

		fail(QString("Upload chunk failed. HTTP status %1").arg(httpStatus), httpStatus, httpStatus, body);
	});
}

void DriveRequest::queryUploadStatusAsync()
{
	QNetworkRequest request{QUrl(sessionUrl)};
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
			emit progressChanged(100);

			UploadResult result;
			result.ok = true;
			result.completed = true;
			result.httpStatus = httpStatus;
			result.response = body.toStdString();
			result.sessionUrl = sessionUrl.toStdString();
			result.nextOffset = fileSize;
			result.uploadedBytes = fileSize;

			finish(result);
			return;
		}

		if (httpStatus == 308) {
			offset = parseNextOffsetFromRange(reply->rawHeader("Range"), 0);

			if (fileSize > 0) {
				emit progressChanged(static_cast<int>((offset * 100) / fileSize));
			}

			uploadNextChunkAsync();
			return;
		}

		fail(QString("Failed to query resumable upload status. HTTP status %1").arg(httpStatus), httpStatus,
		     httpStatus, body);
	});
}

void DriveRequest::fail(const QString &message, int code, long httpStatus, const QByteArray &body)
{
	UploadResult result;
	result.ok = false;
	result.completed = false;
	result.httpStatus = httpStatus;
	result.response = body.toStdString();
	result.sessionUrl = sessionUrl.toStdString();
	result.nextOffset = offset;
	result.uploadedBytes = offset;
	result.error = {code, message.toStdString()};

	emit uploadFailed(result);
}

void DriveRequest::finish(const UploadResult &result)
{
	emit uploadFinished(result);
}

QString DriveRequest::escapeDriveQueryString(QString value)
{
	value.replace("\\", "\\\\");
	value.replace("'", "\\'");
	return value;
}

std::uint64_t DriveRequest::fileSizeOrZero(const QString &path)
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

std::uint64_t DriveRequest::parseNextOffsetFromRange(const QByteArray &rangeHeader, std::uint64_t fallback)
{
	// Exemplo: Range: bytes=0-1048575 => próximo offset: 1048576.
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
