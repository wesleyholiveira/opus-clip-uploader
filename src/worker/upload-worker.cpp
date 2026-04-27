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
	DriveRequest request(accessToken.toStdString());

	UploadResult result = request.uploadFileResumable(filePath.toStdString(), fileName.toStdString(),
							  mimeType.toStdString(), folderId.toStdString(),
							  [this](int progress) { emit progressChanged(progress); });

	if (!result.ok) {
		obs_log(LOG_ERROR, "Upload failed. HTTP: %ld, Code: %d, Message: %s, Response: %s", result.httpStatus,
			result.error.code, result.error.message.c_str(), result.response.c_str());
	} else {
		obs_log(LOG_INFO, "Upload succeeded: %s", result.response.c_str());
	}

	emit finished();
}
