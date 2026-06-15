#include "gpt/gpt-prompt-client.hpp"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <utility>

static constexpr const char *OPENAI_RESPONSES_URL = "https://api.openai.com/v1/responses";

GptPromptClient::GptPromptClient(QString apiKey, QString model, QObject *parent)
	: QObject(parent),
	  apiKey(std::move(apiKey)),
	  model(std::move(model))
{
	if (this->model.trimmed().isEmpty())
		this->model = QStringLiteral("gpt-5.4-mini");
}

void GptPromptClient::cancel()
{
	cancelRequested = true;
	if (currentReply)
		currentReply->abort();
}

QString GptPromptClient::buildInputText(const QString &videoPath, const RecordingTranscript &transcript,
					const CurationSettings &curationSettings) const
{
	QString text;

	text += QStringLiteral(
		"Você é um estrategista sênior de cortes virais para vídeos curtos.\n"
		"Sua tarefa NÃO é criar os cortes diretamente. Sua tarefa é gerar um prompt final, em português, para ser enviado ao Opus Clip como custom prompt.\n"
		"O Opus Clip usará esse prompt para decidir quais trechos cortar, então o prompt precisa ser objetivo, acionável e baseado em timestamps.\n\n"

		"Regras obrigatórias para sua resposta:\n"
		"- Retorne SOMENTE o prompt final para o Opus Clip.\n"
		"- Não use markdown.\n"
		"- Não explique seu raciocínio.\n"
		"- Não invente falas, temas, promessas ou eventos que não aparecem na transcrição.\n"
		"- Preserve os timestamps importantes no prompt final.\n"
		"- Se existirem marcadores/ranges escolhidos pelo usuário, eles são prioridade máxima.\n"
		"- O prompt final deve orientar o Opus a priorizar cortes dentro dos ranges marcados.\n"
		"- Permita usar poucos segundos antes/depois dos ranges apenas quando isso melhorar contexto, gancho ou fechamento.\n"
		"- Se não houver ranges marcados, escolha os melhores momentos com base na transcrição.\n\n"

		"O prompt final para o Opus deve conter instruções como:\n"
		"- priorizar trechos com gancho forte nos primeiros segundos;\n"
		"- priorizar explicações claras, opiniões fortes, tensão, surpresa, humor, contraste, quebra de expectativa ou punchline;\n"
		"- evitar trechos dependentes demais de contexto anterior;\n"
		"- remover pausas, repetições, hesitações e partes sem valor;\n"
		"- buscar cortes com começo claro, desenvolvimento curto e fechamento satisfatório;\n"
		"- preferir cortes entre 30 e 60 segundos, salvo quando um trecho menor for mais forte;\n"
		"- manter fidelidade total ao que foi dito na transcrição.\n\n");

	text += QStringLiteral("Vídeo: %1\n").arg(QFileInfo(videoPath).fileName());

	text += QStringLiteral("Faixa geral selecionada pelo usuário: %1 até %2 segundos\n")
			.arg(curationSettings.rangeStartSec, 0, 'f', 3)
			.arg(curationSettings.rangeEndSec, 0, 'f', 3);

	if (!curationSettings.clipDurations.isEmpty()) {
		text += QStringLiteral("\nRanges/marcações escolhidos pelo usuário, em ordem de prioridade:\n");
		int index = 1;
		for (const ClipDuration &range : curationSettings.clipDurations) {
			text += QStringLiteral("%1. %2 até %3 segundos\n")
					.arg(index++)
					.arg(range.startSec, 0, 'f', 3)
					.arg(range.endSec, 0, 'f', 3);
		}
	} else {
		text += QStringLiteral(
			"\nNão há ranges/marcações específicos escolhidos pelo usuário. Escolha os melhores trechos usando a transcrição.\n");
	}

	if (!curationSettings.topicKeywords.isEmpty()) {
		text += QStringLiteral("\nKeywords/temas desejados pelo usuário:\n");
		text += curationSettings.topicKeywords.join(QStringLiteral(", "));
		text += QLatin1Char('\n');
	}

	text += QStringLiteral(
		"\nAgora analise a transcrição abaixo e gere o prompt final para o Opus Clip.\n"
		"O prompt final deve citar explicitamente os melhores timestamps/ranges e explicar, em uma frase curta, o tipo de corte esperado em cada um.\n"
		"Não copie a transcrição inteira. Sintetize apenas as instruções úteis para o Opus.\n\n"
		"Transcrição com timestamps:\n");

	for (const TranscriptSegment &segment : transcript.segments) {
		if (segment.text.trimmed().isEmpty())
			continue;

		text += QStringLiteral("[%1 - %2] %3\n")
				.arg(segment.startSec, 0, 'f', 2)
				.arg(segment.endSec, 0, 'f', 2)
				.arg(segment.text.trimmed());
	}

	return text;
}

void GptPromptClient::createOpusPromptAsync(const QString &videoPath, const RecordingTranscript &transcript,
					    const CurationSettings &curationSettings)
{
	if (apiKey.trimmed().isEmpty()) {
		emit promptFailed(QStringLiteral("OpenAI API key is empty"));
		return;
	}

	if (transcript.segments.isEmpty()) {
		emit promptFailed(QStringLiteral("No transcript available for this video"));
		return;
	}

	QNetworkRequest request{QUrl(QString::fromLatin1(OPENAI_RESPONSES_URL))};
	request.setRawHeader("Accept", "application/json");
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(apiKey.trimmed()).toUtf8());

	QJsonObject payload;
	payload.insert(QStringLiteral("model"), model.trimmed());
	payload.insert(QStringLiteral("input"), buildInputText(videoPath, transcript, curationSettings));
	payload.insert(QStringLiteral("max_output_tokens"), 1200);

	cancelRequested = false;
	QNetworkReply *reply = network.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
	currentReply = reply;

	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		const QByteArray response = reply->readAll();
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QNetworkReply::NetworkError error = reply->error();
		const QString errorString = reply->errorString();
		if (currentReply == reply)
			currentReply = nullptr;
		reply->deleteLater();

		if (cancelRequested || error == QNetworkReply::OperationCanceledError) {
			cancelRequested = false;
			emit promptFailed(QStringLiteral("Canceled"));
			return;
		}

		if (error != QNetworkReply::NoError || status < 200 || status >= 300) {
			emit promptFailed(QStringLiteral("GPT prompt generation failed: %1 - %2")
						  .arg(errorString, QString::fromUtf8(response)));
			return;
		}

		const QString outputText = extractOutputText(response).trimmed();
		if (outputText.isEmpty()) {
			emit promptFailed(QStringLiteral("GPT response did not contain output text"));
			return;
		}

		emit promptReady(outputText);
	});
}

QString GptPromptClient::extractOutputText(const QByteArray &response) const
{
	const QJsonDocument doc = QJsonDocument::fromJson(response);
	if (!doc.isObject())
		return {};

	const QJsonObject root = doc.object();
	const QString direct = root.value(QStringLiteral("output_text")).toString();
	if (!direct.trimmed().isEmpty())
		return direct;

	QString combined;
	const QJsonArray output = root.value(QStringLiteral("output")).toArray();
	for (const QJsonValue &itemValue : output) {
		const QJsonObject item = itemValue.toObject();
		const QJsonArray content = item.value(QStringLiteral("content")).toArray();
		for (const QJsonValue &contentValue : content) {
			const QJsonObject contentItem = contentValue.toObject();
			const QString text = contentItem.value(QStringLiteral("text")).toString();
			if (!text.trimmed().isEmpty()) {
				if (!combined.isEmpty())
					combined += QLatin1Char('\n');
				combined += text;
			}
		}
	}

	return combined;
}
