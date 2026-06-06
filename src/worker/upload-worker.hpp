#pragma once

#include <QObject>
#include <QString>

class UploadWorker : public QObject {
	Q_OBJECT

public:
	explicit UploadWorker(QString apiKey, QString filePath, QString fileName, QString mimeType,
			      QObject *parent = nullptr);

public slots:
	void run();

signals:
	void progressChanged(int value);
	void finished(QString projectId);
	void failed(QString message);

private:
	QString apiKey;
	QString filePath;
	QString fileName;
	QString mimeType;
};