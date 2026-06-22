#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>

#include "models/curation-settings.hpp"

class OpusClipClient;
class QProcess;

class UploadWorker : public QObject {
	Q_OBJECT

public:
	explicit UploadWorker(QString apiKey, QString filePath, QString fileName, QString mimeType,
			      QString brandTemplateId = {}, QString sourceLang = "auto",
			      CurationSettings curationSettings = {}, QObject *parent = nullptr);

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

	struct PreparedUploadItem {
		QByteArray data;
		QString fileName;
		QString mimeType;
		CurationSettings curationSettings;
		double sourceStartSec = 0.0;
		double sourceEndSec = 0.0;
	};

	QString apiKey;
	QString filePath;
	QString fileName;
	QString mimeType;
	QString brandTemplateId;
	QString sourceLang;
	CurationSettings curationSettings;
	OpusClipClient *client = nullptr;
	QProcess *currentProcess = nullptr;
	std::atomic_bool cancelRequested{false};
	QVector<ClipDuration> independentUploadRanges;
	QStringList independentProjectIds;
	QVector<QPointer<OpusClipClient>> independentActiveClients;
	int independentUploadIndex = 0;

	ResampleResult prepareUploadVideo();
	PreparedUploadItem prepareIndependentRangeUploadVideo(const ClipDuration &range, int index, int total);
	void startIndependentRangeUploads(QVector<ClipDuration> ranges);
	void startNextIndependentRangeUpload();
	void removeIndependentActiveClient(OpusClipClient *opusClient);
	int activeIndependentClientCount() const;
	void startOpusUpload(const QString &uploadFilePath, const QString &uploadFileName,
			     const QString &uploadMimeType, const CurationSettings &settings, bool hasResamplePhase);
	bool runProcess(const QString &program, const QStringList &arguments, int progressStart, int progressEnd,
			const QString &message, QString *lastOutput = nullptr);
	bool runProcessCaptureStdout(const QString &program, const QStringList &arguments, int progressStart, int progressEnd,
				     const QString &message, QByteArray *stdoutData, QString *lastErrorOutput = nullptr);
};
