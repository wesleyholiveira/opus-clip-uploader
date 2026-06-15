#include "gpt/gpt-prompt-client.hpp"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <utility>

#include "utils/config.hpp"

static constexpr const char *OPENAI_RESPONSES_URL = "https://api.openai.com/v1/responses";

static constexpr const char *CONFIG_GPT_INPUT_TEMPLATE = "gpt.input_text_template";

static QString formatSelectedRanges(const CurationSettings &curationSettings)
{
	QString text;

	if (!curationSettings.clipDurations.isEmpty()) {
		text += QStringLiteral("Ranges/marcações escolhidos pelo usuário, em ordem de prioridade:\n");
		int index = 1;
		for (const ClipDuration &range : curationSettings.clipDurations) {
			text += QStringLiteral("%1. %2 até %3 segundos\n")
					.arg(index++)
					.arg(range.startSec, 0, 'f', 3)
					.arg(range.endSec, 0, 'f', 3);
		}
	} else {
		text += QStringLiteral(
			"Não há ranges/marcações específicos escolhidos pelo usuário. Escolha os melhores trechos usando a transcrição.\n");
	}

	return text.trimmed();
}

static QString formatTopicKeywords(const CurationSettings &curationSettings)
{
	if (curationSettings.topicKeywords.isEmpty())
		return QStringLiteral("Nenhuma keyword/tema específico foi informado pelo usuário.");

	return curationSettings.topicKeywords.join(QStringLiteral(", ")).trimmed();
}

static QString formatTranscript(const RecordingTranscript &transcript)
{
	QString text;

	for (const TranscriptSegment &segment : transcript.segments) {
		if (segment.text.trimmed().isEmpty())
			continue;

		text += QStringLiteral("[%1 - %2] %3\n")
				.arg(segment.startSec, 0, 'f', 2)
				.arg(segment.endSec, 0, 'f', 2)
				.arg(segment.text.trimmed());
	}

	return text.trimmed();
}

static QString renderInputTemplate(QString templateText, const QString &videoPath,
					   const RecordingTranscript &transcript,
					   const CurationSettings &curationSettings)
{
	templateText.replace(QStringLiteral("{{video_file}}"), QFileInfo(videoPath).fileName());
	templateText.replace(QStringLiteral("{{range_start_sec}}"),
			     QString::number(curationSettings.rangeStartSec, 'f', 3));
	templateText.replace(QStringLiteral("{{range_end_sec}}"),
			     QString::number(curationSettings.rangeEndSec, 'f', 3));
	templateText.replace(QStringLiteral("{{selected_ranges}}"), formatSelectedRanges(curationSettings));
	templateText.replace(QStringLiteral("{{topic_keywords}}"), formatTopicKeywords(curationSettings));
	templateText.replace(QStringLiteral("{{transcript}}"), formatTranscript(transcript));

	return templateText.trimmed();
}

QString GptPromptClient::inputTemplateConfigKey()
{
	return QString::fromLatin1(CONFIG_GPT_INPUT_TEMPLATE);
}

QString GptPromptClient::defaultInputTextTemplate()
{
	return QStringLiteral(
		"Você é um estrategista sênior de cortes virais para vídeos curtos extraídos de lives.\n"
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
		"- Se não houver ranges marcados, escolha os melhores momentos com base na transcrição.\n"
		"- Cada corte deve tratar de exatamente um assunto principal. Não misture dois assuntos diferentes no mesmo corte.\n"
		"- Cada corte precisa ter começo, meio e fim: gancho/entrada clara, desenvolvimento compreensível e fechamento natural.\n"
		"- Rejeite cortes que só funcionam se o espectador já viu muitos minutos anteriores da live.\n\n"

		"O prompt final para o Opus deve conter instruções como:\n"
		"- priorizar trechos com gancho forte nos primeiros segundos;\n"
		"- priorizar explicações claras, opiniões fortes, tensão, surpresa, humor, contraste, quebra de expectativa ou punchline;\n"
		"- manter cada corte focado em um único assunto, sem juntar tópicos paralelos ou conversas diferentes;\n"
		"- evitar trechos dependentes demais de contexto anterior;\n"
		"- remover pausas, repetições, hesitações e partes sem valor;\n"
		"- recortar fora do clipe qualquer parte que não pertença diretamente ao assunto principal do corte;\n"
		"- remover trechos meta/repetitivos da live, especialmente comentários sobre mensagens atrasadas, chat atrasado, leitura de mensagens, PIX, comentários com estrelinha, doações, super chat ou pedidos para pagar para a mensagem ser lida antes, exceto se isso for o assunto principal e tiver valor de entretenimento;\n"
		"- buscar cortes com começo claro, desenvolvimento curto e fechamento satisfatório;\n"
		"- preferir cortes entre 30 e 60 segundos, salvo quando um trecho menor for mais forte;\n"
		"- manter fidelidade total ao que foi dito na transcrição.\n\n"

		"Vídeo: {{video_file}}\n"
		"Faixa geral selecionada pelo usuário: {{range_start_sec}} até {{range_end_sec}} segundos\n\n"

		"Ranges/marcações escolhidos pelo usuário:\n"
		"{{selected_ranges}}\n\n"

		"Keywords/temas desejados pelo usuário:\n"
		"{{topic_keywords}}\n\n"

		"Agora analise a transcrição abaixo e gere o prompt final para o Opus Clip.\n"
		"O prompt final deve citar explicitamente os melhores timestamps/ranges e explicar, em uma frase curta, o tipo de corte esperado em cada um.\n"
		"Ao escolher ou descrever ranges, priorize janelas em que o assunto principal começa e termina naturalmente.\n"
		"Se um trecho bom contém interrupções sobre chat atrasado, PIX, estrelinha, leitura de mensagens ou doações, instrua o Opus a cortar essas partes fora.\n"
		"Não copie a transcrição inteira. Sintetize apenas as instruções úteis para o Opus.\n\n"
		"Transcrição com timestamps:\n"
		"{{transcript}}\n");
}

QString GptPromptClient::configuredInputTextTemplate()
{
	return PluginConfig::getValue(inputTemplateConfigKey(), defaultInputTextTemplate()).trimmed();
}

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
	return renderInputTemplate(configuredInputTextTemplate(), videoPath, transcript, curationSettings);
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
