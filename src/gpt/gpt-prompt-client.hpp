#pragma once

#include "models/curation-settings.hpp"
#include "models/transcript.hpp"

#include <QObject>
#include <QNetworkAccessManager>
#include <QString>

class GptPromptClient : public QObject {
	Q_OBJECT

public:
	explicit GptPromptClient(QString apiKey, QString model = {}, QObject *parent = nullptr);
	void createOpusPromptAsync(const QString &videoPath, const RecordingTranscript &transcript,
					 const CurationSettings &curationSettings);

signals:
	void promptReady(QString prompt);
	void promptFailed(QString message);

private:
	QNetworkAccessManager network;
	QString apiKey;
	QString model;

	QString buildInputText(const QString &videoPath, const RecordingTranscript &transcript,
			     const CurationSettings &curationSettings) const;
	QString extractOutputText(const QByteArray &response) const;
};
