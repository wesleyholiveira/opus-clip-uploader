#pragma once

#include <QObject>
#include <QFile>
#include <QNetworkAccessManager>
#include <QString>

#include <cstdint>
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
	std::string sessionUrl;
	std::string projectId;
	std::string uploadId;
	std::uint64_t nextOffset = 0;
	std::uint64_t uploadedBytes = 0;
	UploadError error;
};

class OpusClipClient : public QObject {
	Q_OBJECT

public:
	explicit OpusClipClient(QString apiKey, QObject *parent = nullptr);

	void uploadFileResumableAndCreateProjectAsync(const QString &path, const QString &fileName,
							 const QString &mimeType);

signals:
	void progressChanged(int progress);
	void uploadFinished(const UploadResult &result);
	void uploadFailed(const UploadResult &result);

private:
	static constexpr std::uint64_t DefaultChunkSize = 8 * 1024 * 1024;
	static constexpr int MaxTransientRetries = 5;

	QNetworkAccessManager network;
	QString apiKey;
	QString path;
	QString fileName;
	QString mimeType;
	QString uploadLinkUrl;
	QString resumableSessionUrl;
	QString uploadId;
	QFile uploadFile;
	std::uint64_t fileSize = 0;
	std::uint64_t offset = 0;
	int transientRetries = 0;

	void generateUploadLinkAsync();
	void initiateResumableUploadSessionAsync();
	void uploadNextChunkAsync();
	void queryUploadStatusAsync();
	void retryOrQueryUploadStatusAsync();
	void createClipProjectAsync();

	void fail(const QString &message, int code = -1, long httpStatus = 0, const QByteArray &body = {});
	void finish(const UploadResult &result);

	static std::uint64_t fileSizeOrZero(const QString &path);
	static std::uint64_t parseNextOffsetFromRange(const QByteArray &rangeHeader, std::uint64_t fallback);
};
