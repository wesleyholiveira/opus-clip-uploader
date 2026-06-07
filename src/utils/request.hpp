#pragma once

#include "models/upload-result.hpp"

#include <QObject>
#include <QNetworkAccessManager>
#include <QFile>
#include <QString>

#include <cstdint>

class DriveRequest : public QObject {
	Q_OBJECT

public:
	explicit DriveRequest(QString accessToken, QObject *parent = nullptr);

	void uploadFileResumableAsync(const QString &path, const QString &fileName, const QString &mimeType,
				      const QString &folderName = QString());

signals:
	void progressChanged(int progress);
	void uploadFinished(const UploadChunkResult &result);
	void uploadFailed(const UploadChunkResult &result);

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
	void finish(const UploadChunkResult &result);

	static QString escapeDriveQueryString(QString value);
	static std::uint64_t fileSizeOrZero(const QString &path);
	static std::uint64_t parseNextOffsetFromRange(const QByteArray &rangeHeader, std::uint64_t fallback);
};
