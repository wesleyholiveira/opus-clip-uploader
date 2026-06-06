#include <obs-frontend-api.h>
#include "plugin-support.h"
#include "worker/upload-worker.hpp"
#include "opus/opus-clip-client.hpp"

#include <utility>

UploadWorker::UploadWorker(QString apiKey, QString filePath, QString fileName, QString mimeType, QObject *parent)
	: QObject(parent),
	  apiKey(std::move(apiKey)),
	  filePath(std::move(filePath)),
	  fileName(std::move(fileName)),
	  mimeType(std::move(mimeType))
{
}

void UploadWorker::run()
{
	auto *request = new OpusClipClient(apiKey, this);

	connect(request, &OpusClipClient::progressChanged, this,
		[this](int progress) { emit progressChanged(progress); });

	connect(request, &OpusClipClient::uploadFinished, this, [this, request](const UploadResult &result) {
		emit finished(QString::fromStdString(result.projectId));
		request->deleteLater();
	});

	connect(request, &OpusClipClient::uploadFailed, this, [this, request](const UploadResult &result) {
		QString message = QString::fromStdString(result.error.message);
		if (result.httpStatus > 0) {
			message += QString(" (HTTP %1)").arg(result.httpStatus);
		}
		emit failed(message);
		request->deleteLater();
	});

	request->uploadFileResumableAndCreateProjectAsync(filePath, fileName, mimeType);
}
