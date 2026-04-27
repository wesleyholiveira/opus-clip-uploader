#pragma once

#include <QObject>
#include <QString>

class UploadWorker : public QObject {
    Q_OBJECT

public:
    explicit UploadWorker(
        QString accessToken,
        QString filePath,
        QString fileName,
        QString mimeType,
        QString folderId = {},
        QObject* parent = nullptr
    );

public slots:
    void run();

signals:
    void progressChanged(int value);
    void finished();
    void failed(QString message);

private:
    QString accessToken;
    QString filePath;
    QString fileName;
    QString mimeType;
    QString folderId;
};
