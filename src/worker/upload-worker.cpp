#include "worker/upload-worker.hpp"

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

#include "opus/opus-clip-client.hpp"

#include <utility>

static QString obsText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

UploadWorker::UploadWorker(QString apiKey, QString filePath, QString fileName, QString mimeType,
			   QString brandTemplateId, QString sourceLang, CurationSettings curationSettings,
			   QString openAiApiKey, QString openAiModel, QObject *parent)
	: QObject(parent),
	  apiKey(std::move(apiKey)),
	  filePath(std::move(filePath)),
	  fileName(std::move(fileName)),
	  mimeType(std::move(mimeType)),
	  brandTemplateId(std::move(brandTemplateId)),
	  sourceLang(std::move(sourceLang)),
	  curationSettings(std::move(curationSettings)),
	  openAiApiKey(std::move(openAiApiKey)),
	  openAiModel(std::move(openAiModel))
{
	if (this->sourceLang.trimmed().isEmpty())
		this->sourceLang = "auto";
}

void UploadWorker::run()
{
	startOpusUpload(curationSettings);
}

void UploadWorker::startOpusUpload(const CurationSettings &settings)
{
	auto *request = new OpusClipClient(apiKey, brandTemplateId, sourceLang, settings, this);

	connect(request, &OpusClipClient::progressChanged, this,
		[this](int progress, const QString &message) { emit progressChanged(progress, message); });

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
