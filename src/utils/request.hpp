#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QFile>
#include <QString>

#include <cstdint>
#include <functional>
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
	std::uint64_t nextOffset = 0;
	std::uint64_t uploadedBytes = 0;
	UploadError error;
};

class DriveRequest : public QObject {
	Q_OBJECT

public:
	explicit DriveRequest(QString accessToken, QObject *parent = nullptr);

	void uploadFileResumableAsync(const QString &path, const QString &fileName, const QString &mimeType,
				      const QString &folderName = QString());

signals:
	void progressChanged(int progress);
	void uploadFinished(const UploadResult &result);
	void uploadFailed(const UploadResult &result);

private:
	static constexpr std::uint64_t DefaultChunkSize = 8 * 1024 * 1024;

	QNetworkAccessManager network;
	QString accessToken;

	QString path;
	QString fileName;
	QString mimeType;
	QString folderName;
	QString folderId;
	QString sessionUrl;

	std::uint64_t fileSize = 0;
	std::uint64_t offset = 0;

	void findFolderIdByNameAsync();
	void createResumableSessionAsync();
	void uploadNextChunkAsync();
	void queryUploadStatusAsync();

	void fail(const QString &message, int code = -1, long httpStatus = 0, const QByteArray &body = {});
	void finish(const UploadResult &result);

	static QString escapeDriveQueryString(QString value);
	static std::uint64_t fileSizeOrZero(const QString &path);
	static std::uint64_t parseNextOffsetFromRange(const QByteArray &rangeHeader, std::uint64_t fallback);
};
