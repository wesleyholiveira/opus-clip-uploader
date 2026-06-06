#pragma once

#include <QObject>
#include <QByteArray>
#include <QNetworkAccessManager>
#include <QString>

#include <string>

struct UploadError {
	int code = 0;
	std::string message;
};

struct UploadResult {
	bool ok = false;
	bool completed = false;
	long httpStatus = 0;
	std::string response;
	std::string projectId;
	std::string uploadId;
	UploadError error;
};

class OpusClipClient : public QObject {
	Q_OBJECT

public:
	explicit OpusClipClient(QString apiKey, QObject *parent = nullptr);

	void uploadFileResumableAndCreateProjectAsync(const QString &filePath, const QString &fileName,
						      const QString &mimeType);

signals:
	void progressChanged(int progress);
	void uploadFinished(const UploadResult &result);
	void uploadFailed(const UploadResult &result);

private:
	QNetworkAccessManager network;
	QString apiKey;

	void createUploadLink(const QString &filePath);
	void startResumableSession(const QString &filePath, const QString &uploadUrl, const QString &uploadId);
	void uploadFileToResumableLocation(const QString &filePath, const QString &location, const QString &uploadId);
	void createClipProject(const QString &uploadId);

	void fail(const QString &message, int code = -1, long httpStatus = 0, const QByteArray &body = {});

	QString extractProjectId(const QByteArray &response, const QString &fallbackUploadId) const;
};