#pragma once

#include "models/curation-settings.hpp"
#include "models/upload-result.hpp"

#include <QObject>
#include <QByteArray>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QString>
#include <QStringList>

class OpusClipClient : public QObject {
	Q_OBJECT

public:
	explicit OpusClipClient(QString apiKey, QString brandTemplateId = {}, QString sourceLang = "auto",
				CurationSettings curationSettings = {}, QObject *parent = nullptr);
	void cancel();
	void uploadFileResumableAndCreateProjectAsync(const QString &filePath, const QString &fileName,
						      const QString &mimeType);

signals:
	void progressChanged(int progress, const QString &message);
	void uploadFinished(const OpusUploadResult &result);
	void uploadFailed(const OpusUploadResult &result);

private:
	QNetworkAccessManager network;
	QPointer<QNetworkReply> currentReply;
	QPointer<QFile> currentUploadFile;
	QString apiKey;
	QString brandTemplateId;
	QString sourceLang;
	CurationSettings curationSettings;
	QStringList createdProjectIds;
	bool cancelRequested = false;
	bool terminalSignalEmitted = false;

	void createUploadLink(const QString &filePath);
	void startResumableSession(const QString &filePath, const QString &uploadUrl, const QString &uploadId);
	void uploadFileToResumableLocation(const QString &filePath, const QString &location, const QString &uploadId);
	void createNextClipProject(const QString &uploadId, int projectIndex);
	void createClipProject(const QString &uploadId, const ClipDuration &range, int projectIndex, int totalProjects);
	QVector<ClipDuration> projectRanges() const;

	void fail(const QString &message, int code = -1, long httpStatus = 0, const QByteArray &body = {});
	void emitCanceledIfNeeded();

	QString extractProjectId(const QByteArray &response, const QString &fallbackUploadId) const;
};
