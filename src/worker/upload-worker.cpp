#include <obs-frontend-api.h>
#include "plugin-support.h"
#include "worker/upload-worker.hpp"
#include "utils/request.hpp"

#include <utility>

UploadWorker::UploadWorker(QString accessToken, QString filePath, QString fileName, QString mimeType, QString folderId,
			   QObject *parent)
	: QObject(parent),
	  accessToken(std::move(accessToken)),
	  filePath(std::move(filePath)),
	  fileName(std::move(fileName)),
	  mimeType(std::move(mimeType)),
	  folderId(std::move(folderId))
{
}

void UploadWorker::run()
{
	auto *request = new DriveRequest(accessToken, this);

	connect(request, &DriveRequest::progressChanged, this,
		[this](int progress) { emit progressChanged(progress); });

	connect(request, &DriveRequest::uploadFinished, this, [this, request](const UploadResult &) {
		emit finished();
		request->deleteLater();
	});

	connect(request, &DriveRequest::uploadFailed, this, [this, request](const UploadResult &result) {
		const QString message = QString::fromStdString(result.error.message);
		emit failed(message);
		request->deleteLater();
	});

	request->uploadFileResumableAsync(filePath, fileName, mimeType, folderId);
}
