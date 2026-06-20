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
	static QString inputTemplateConfigKey();
	static QString inputTemplateConfigKey(const QString &sourceLanguage);
	static QString defaultInputTextTemplate();
	static QString defaultInputTextTemplate(const QString &sourceLanguage);
	static QString configuredInputTextTemplate();
	static QString configuredInputTextTemplate(const QString &sourceLanguage);
	static bool isSemanticGateFailurePrompt(const QString &prompt);
	static QString semanticGateFailureReason(const QString &prompt);
	static QString promptGenerationBlockedPrompt(const QString &reason);
	static bool isPromptGenerationBlockedPrompt(const QString &prompt);
	static QString promptGenerationBlockedReason(const QString &prompt);
	void cancel();
	void createOpusPromptAsync(const QString &videoPath, const RecordingTranscript &transcript,
				   const CurationSettings &curationSettings);
	void createOpusPromptAsync(const QString &videoPath, const RecordingTranscript &fullTranscript,
				   const RecordingTranscript &selectedRangeTranscript,
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

	QString buildInputText(const QString &videoPath, const RecordingTranscript &fullTranscript,
			       const RecordingTranscript &selectedRangeTranscript,
			       const CurationSettings &curationSettings) const;
	QString extractOutputText(const QByteArray &response) const;
};
