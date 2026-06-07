#include "worker/upload-worker.hpp"
#include "opus/opus-clip-client.hpp"

#include <utility>

UploadWorker::UploadWorker(QString apiKey, QString filePath, QString fileName, QString mimeType,
			   QString brandTemplateId, QString sourceLang, CurationSettings curationSettings,
			   QObject *parent)
	: QObject(parent),
	  apiKey(std::move(apiKey)),
	  filePath(std::move(filePath)),
	  fileName(std::move(fileName)),
	  mimeType(std::move(mimeType)),
	  brandTemplateId(std::move(brandTemplateId)),
	  sourceLang(std::move(sourceLang)),
	  curationSettings(std::move(curationSettings))
{
	if (this->sourceLang.trimmed().isEmpty())
		this->sourceLang = "auto";
}

void UploadWorker::run()
{
	auto *request = new OpusClipClient(apiKey, brandTemplateId, sourceLang, curationSettings, this);

	connect(request, &OpusClipClient::progressChanged, this,
		[this](int progress) { emit progressChanged(progress); });

	connect(request, &OpusClipClient::uploadFinished, this, [this, request](const OpusUploadResult &result) {
		emit finished(QString::fromStdString(result.projectId));
		request->deleteLater();
	});

	connect(request, &OpusClipClient::uploadFailed, this, [this, request](const OpusUploadResult &result) {
		QString message = QString::fromUtf8(result.error.message.c_str());
		if (result.httpStatus > 0)
			message += QString(" (HTTP %1)").arg(result.httpStatus);

		emit failed(message);
		request->deleteLater();
	});

	request->uploadFileResumableAndCreateProjectAsync(filePath, fileName, mimeType);
}