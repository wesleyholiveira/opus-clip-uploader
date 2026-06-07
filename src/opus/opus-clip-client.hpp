#pragma once

#include "models/curation-settings.hpp"
#include "models/upload-result.hpp"

#include <QObject>
#include <QByteArray>
#include <QNetworkAccessManager>
#include <QString>

class OpusClipClient : public QObject {
	Q_OBJECT

public:
	explicit OpusClipClient(QString apiKey, QString brandTemplateId = {}, QString sourceLang = "auto",
				CurationSettings curationSettings = {}, QObject *parent = nullptr);
	void uploadFileResumableAndCreateProjectAsync(const QString &filePath, const QString &fileName,
						      const QString &mimeType);

signals:
	void progressChanged(int progress);
	void uploadFinished(const OpusUploadResult &result);
	void uploadFailed(const OpusUploadResult &result);

private:
	QNetworkAccessManager network;
	QString apiKey;
	QString brandTemplateId;
	QString sourceLang;
	CurationSettings curationSettings;

	void createUploadLink(const QString &filePath);
	void startResumableSession(const QString &filePath, const QString &uploadUrl, const QString &uploadId);
	void uploadFileToResumableLocation(const QString &filePath, const QString &location, const QString &uploadId);
	void createClipProject(const QString &uploadId);

	void fail(const QString &message, int code = -1, long httpStatus = 0, const QByteArray &body = {});

	QString extractProjectId(const QByteArray &response, const QString &fallbackUploadId) const;
};