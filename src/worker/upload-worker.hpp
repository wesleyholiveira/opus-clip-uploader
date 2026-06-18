#pragma once

#include <QObject>
#include <QString>

#include <atomic>

#include "models/curation-settings.hpp"

class OpusClipClient;
class QProcess;

class UploadWorker : public QObject {
	Q_OBJECT

public:
	explicit UploadWorker(QString apiKey, QString filePath, QString fileName, QString mimeType,
			      QString brandTemplateId = {}, QString sourceLang = "auto",
			      CurationSettings curationSettings = {}, QString openAiApiKey = {},
			      QString openAiModel = {}, QObject *parent = nullptr);

public slots:
	void run();
	void cancel();

signals:
	void progressChanged(int value, QString message);
	void finished(QString projectId);
	void failed(QString message);

private:
	struct ResampleResult {
		bool usedResampledVideo = false;
		QString filePath;
		QString fileName;
		QString mimeType;
		CurationSettings curationSettings;
	};

	QString apiKey;
	QString filePath;
	QString fileName;
	QString mimeType;
	QString brandTemplateId;
	QString sourceLang;
	CurationSettings curationSettings;
	QString openAiApiKey;
	QString openAiModel;
	OpusClipClient *client = nullptr;
	QProcess *currentProcess = nullptr;
	std::atomic_bool cancelRequested{false};

	ResampleResult prepareUploadVideo();
	void startOpusUpload(const QString &uploadFilePath, const QString &uploadFileName,
			     const QString &uploadMimeType, const CurationSettings &settings, bool hasResamplePhase);
	bool runProcess(const QString &program, const QStringList &arguments, int progressStart, int progressEnd,
			const QString &message, QString *lastOutput = nullptr);
};
