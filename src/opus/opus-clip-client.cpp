#include "opus/opus-clip-client.hpp"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <utility>

static constexpr const char *OPUS_API_BASE_URL = "https://api.opus.pro";

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

void OpusClipClient::uploadFileResumableAndCreateProjectAsync(const QString &filePath, const QString &fileName,
							      const QString &mimeType)
{
	Q_UNUSED(fileName);
	Q_UNUSED(mimeType);

	if (apiKey.trimmed().isEmpty()) {
		OpusUploadResult result;
		result.error.message = "Opus Clip API key is empty";
		emit uploadFailed(result);
		return;
	}

	QFileInfo info(filePath);

	if (!info.exists() || !info.isFile()) {
		OpusUploadResult result;
		result.error.message = "Invalid video file path";
		emit uploadFailed(result);
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

	connect(reply, &QNetworkReply::finished, this, [this, reply, filePath]() {
		const QByteArray response = reply->readAll();
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QNetworkReply::NetworkError error = reply->error();

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

	connect(reply, &QNetworkReply::finished, this, [this, reply, filePath, uploadId]() {
		const QByteArray response = reply->readAll();
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QNetworkReply::NetworkError error = reply->error();

		const QString location = QString::fromUtf8(reply->rawHeader("location"));

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

	connect(reply, &QNetworkReply::uploadProgress, this, [this](qint64 bytesSent, qint64 bytesTotal) {
		if (bytesTotal <= 0)
			return;

		const int progress = static_cast<int>((bytesSent * 100) / bytesTotal);
		emit progressChanged(qBound(0, progress, 100));
	});

	connect(reply, &QNetworkReply::finished, this, [this, reply, file, uploadId]() {
		const QByteArray response = reply->readAll();
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QNetworkReply::NetworkError error = reply->error();

		file->close();
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

		emit progressChanged(100);
		createClipProject(uploadId);
	});
}

void OpusClipClient::createClipProject(const QString &uploadId)
{
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
	range.insert("startSec", curationSettings.rangeStartSec);
	range.insert("endSec", curationSettings.rangeEndSec);
	curationPref.insert("range", range);

	QJsonArray clipDurations;
	for (const auto &duration : curationSettings.clipDurations) {
		QJsonArray item;
		item.append(duration.startSec);
		item.append(duration.endSec);
		clipDurations.append(item);
	}
	curationPref.insert("clipDurations", clipDurations);

	QJsonArray topicKeywords;
	for (const QString &keyword : curationSettings.topicKeywords)
		topicKeywords.append(keyword);

	curationPref.insert("model",
			    curationSettings.model.trimmed().isEmpty() ? "ClipAnything" : curationSettings.model);
	curationPref.insert("topicKeywords", topicKeywords);

	curationPref.insert("genre", curationSettings.genre.trimmed().isEmpty() ? "Auto" : curationSettings.genre);
	curationPref.insert("skipCurate", curationSettings.skipCurate);

	payload.insert("curationPref", curationPref);

	const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);

	QNetworkReply *reply = network.post(request, body);

	connect(reply, &QNetworkReply::finished, this, [this, reply, uploadId]() {
		const QByteArray response = reply->readAll();
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QNetworkReply::NetworkError error = reply->error();

		reply->deleteLater();

		if (error != QNetworkReply::NoError || status < 200 || status >= 300) {
			OpusUploadResult result;
			result.httpStatus = status;
			result.error.message = QString("Failed to create Opus Clip project: %1 - %2")
						       .arg(reply->errorString(), QString::fromUtf8(response))
						       .toStdString();

			emit uploadFailed(result);
			return;
		}

		OpusUploadResult result;
		result.httpStatus = status;
		result.projectId = extractProjectId(response, uploadId).toStdString();

		emit uploadFinished(result);
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