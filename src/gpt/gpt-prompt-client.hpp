#pragma once

#include "models/curation-settings.hpp"
#include "models/transcript.hpp"

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QString>

class GptPromptClient : public QObject {
	Q_OBJECT

public:
	explicit GptPromptClient(QString apiKey, QString model = {}, QObject *parent = nullptr);
	void cancel();
	void createOpusPromptAsync(const QString &videoPath, const RecordingTranscript &transcript,
					 const CurationSettings &curationSettings);

signals:
	void promptReady(QString prompt);
	void promptFailed(QString message);

private:
	QNetworkAccessManager network;
	QPointer<QNetworkReply> currentReply;
	QString apiKey;
	QString model;
	bool cancelRequested = false;

	QString buildInputText(const QString &videoPath, const RecordingTranscript &transcript,
			     const CurationSettings &curationSettings) const;
	QString extractOutputText(const QByteArray &response) const;
};
