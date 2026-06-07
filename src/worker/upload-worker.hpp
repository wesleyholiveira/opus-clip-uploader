#pragma once

#include <QObject>
#include <QString>

#include "models/curation-settings.hpp"

class UploadWorker : public QObject {
	Q_OBJECT

public:
	explicit UploadWorker(QString apiKey, QString filePath, QString fileName, QString mimeType,
			      QString brandTemplateId = {}, QString sourceLang = "auto",
			      CurationSettings curationSettings = {}, QObject *parent = nullptr);

public slots:
	void run();

signals:
	void progressChanged(int value, QString message);
	void finished(QString projectId);
	void failed(QString message);

private:
	QString apiKey;
	QString filePath;
	QString fileName;
	QString mimeType;
	QString brandTemplateId;
	QString sourceLang;
	CurationSettings curationSettings;
};