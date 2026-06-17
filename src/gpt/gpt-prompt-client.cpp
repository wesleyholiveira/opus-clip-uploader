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

static constexpr const char *CONFIG_GPT_INPUT_TEMPLATE_PREFIX = "gpt.input_text_template.v2.";
static constexpr const char *CONFIG_OPUS_SOURCE_LANGUAGE = "opus_source_lang";

static QString normalizePromptLanguage(QString sourceLanguage)
{
	sourceLanguage = sourceLanguage.trimmed().toLower();

	if (sourceLanguage == QStringLiteral("pt") || sourceLanguage == QStringLiteral("pt-br") ||
	    sourceLanguage == QStringLiteral("portuguese"))
		return QStringLiteral("pt");

	if (sourceLanguage == QStringLiteral("en") || sourceLanguage == QStringLiteral("en-us") ||
	    sourceLanguage == QStringLiteral("english"))
		return QStringLiteral("en");

	return QStringLiteral("auto");
}

static bool usePortuguesePromptText(const QString &sourceLanguage)
{
	return normalizePromptLanguage(sourceLanguage) == QStringLiteral("pt");
}

static QString formatSelectedRanges(const CurationSettings &curationSettings, const QString &sourceLanguage)
{
	QString text;
	const bool portuguese = usePortuguesePromptText(sourceLanguage);

	if (!curationSettings.clipDurations.isEmpty()) {
		text += portuguese
				? QStringLiteral("Ranges/marcações escolhidos pelo usuário, em ordem de prioridade:\n")
				: QStringLiteral("User-selected ranges/markers, in priority order:\n");
		int index = 1;
		for (const ClipDuration &range : curationSettings.clipDurations) {
			const QString rangeLine = portuguese ? QStringLiteral("%1. %2s-%3s\n")
							     : QStringLiteral("%1. %2s-%3s\n");
			text += rangeLine.arg(index++).arg(range.startSec, 0, 'f', 3).arg(range.endSec, 0, 'f', 3);
		}
	} else {
		text += portuguese
				? QStringLiteral(
					  "Não há ranges/marcações específicos escolhidos pelo usuário. Escolha os melhores trechos usando a transcrição.\n")
				: QStringLiteral(
					  "There are no specific user-selected ranges/markers. Choose the best moments using the transcript.\n");
	}

	return text.trimmed();
}

static QString formatTopicKeywords(const CurationSettings &curationSettings, const QString &sourceLanguage)
{
	if (curationSettings.topicKeywords.isEmpty()) {
		return usePortuguesePromptText(sourceLanguage)
			       ? QStringLiteral("Nenhuma keyword/tema específico foi informado pelo usuário.")
			       : QStringLiteral("No specific keyword/topic was provided by the user.");
	}

	return curationSettings.topicKeywords.join(QStringLiteral(", ")).trimmed();
}

static QString formatTranscript(const RecordingTranscript &transcript)
{
	QString text;

	for (const TranscriptSegment &segment : transcript.segments) {
		if (segment.text.trimmed().isEmpty())
			continue;

		text += QStringLiteral("[%1s-%2s] %3\n")
				.arg(segment.startSec, 0, 'f', 2)
				.arg(segment.endSec, 0, 'f', 2)
				.arg(segment.text.trimmed());
	}

	return text.trimmed();
}

static QString renderInputTemplate(QString templateText, const QString &videoPath,
				   const RecordingTranscript &transcript, const CurationSettings &curationSettings,
				   const QString &sourceLanguage)
{
	templateText.replace(QStringLiteral("{{video_file}}"), QFileInfo(videoPath).fileName());
	templateText.replace(QStringLiteral("{{range_start_sec}}"),
			     QString::number(curationSettings.rangeStartSec, 'f', 3));
	templateText.replace(QStringLiteral("{{range_end_sec}}"),
			     QString::number(curationSettings.rangeEndSec, 'f', 3));
	templateText.replace(QStringLiteral("{{selected_ranges}}"),
			     formatSelectedRanges(curationSettings, sourceLanguage));
	templateText.replace(QStringLiteral("{{topic_keywords}}"),
			     formatTopicKeywords(curationSettings, sourceLanguage));
	templateText.replace(QStringLiteral("{{source_language}}"), normalizePromptLanguage(sourceLanguage));
	templateText.replace(QStringLiteral("{{transcript}}"), formatTranscript(transcript));

	return templateText.trimmed();
}

static QString defaultPortugueseInputTextTemplate()
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
		"- Use timestamps somente em segundos, no formato 123.45s-156.78s. Não use HH:MM:SS nem MM:SS.\n"
		"- A transcrição abaixo já deve estar limitada aos ranges escolhidos; não recomende cortes fora desses ranges.\n"
		"- Se existirem marcadores/ranges escolhidos pelo usuário, eles são prioridade máxima.\n"
		"- O prompt final deve orientar o Opus a priorizar cortes dentro dos ranges marcados.\n"
		"- Permita usar poucos segundos antes/depois dos ranges apenas quando isso melhorar contexto, gancho ou fechamento.\n"
		"- Se não houver ranges marcados, escolha os melhores momentos com base na transcrição.\n"
		"- Cada corte deve tratar de exatamente um assunto principal. Não misture dois assuntos diferentes no mesmo corte.\n"
		"- Cada corte precisa ter começo, meio e fim: gancho/entrada clara, desenvolvimento compreensível e fechamento natural.\n"
		"- Não escolha um trecho só porque ele tem um hook isolado. O corte precisa conter o contexto que torna o hook compreensível.\n"
		"- Se o melhor hook começa no meio do assunto, mova o início alguns segundos para trás até a introdução ficar clara; se não houver fechamento natural, rejeite o trecho.\n"
		"- Rejeite cortes que começam no meio de uma frase, de uma resposta ou de uma piada, ou que terminam sem conclusão, payoff ou transição natural.\n"
		"- Rejeite cortes que só funcionam se o espectador já viu muitos minutos anteriores da live.\n\n"

		"O prompt final para o Opus deve conter instruções como:\n"
		"- priorizar trechos com gancho forte nos primeiros segundos, desde que o contexto esteja claro dentro do próprio corte;\n"
		"- priorizar janelas completas de assunto: setup/contexto, desenvolvimento e conclusão/payoff;\n"
		"- priorizar explicações claras, opiniões fortes, tensão, surpresa, humor, contraste, quebra de expectativa ou punchline;\n"
		"- manter cada corte focado em um único assunto, sem juntar tópicos paralelos ou conversas diferentes;\n"
		"- evitar trechos dependentes demais de contexto anterior;\n"
		"- remover pausas, repetições, hesitações e partes sem valor;\n"
		"- recortar fora do clipe qualquer parte que não pertença diretamente ao assunto principal do corte;\n"
		"- remover trechos meta/repetitivos da live, especialmente comentários sobre mensagens atrasadas, chat atrasado, leitura de mensagens, PIX, comentários com estrelinha, doações, super chat ou pedidos para pagar para a mensagem ser lida antes, exceto se isso for o assunto principal e tiver valor de entretenimento;\n"
		"- buscar cortes com começo claro, desenvolvimento curto e fechamento satisfatório;\n"
		"- começar alguns segundos antes do hook quando isso for necessário para contextualizar o assunto;\n"
		"- terminar apenas depois que houver uma conclusão, payoff, virada de assunto ou fechamento natural;\n"
		"- preferir cortes entre 30 e 60 segundos, salvo quando um trecho menor for mais forte;\n"
		"- manter fidelidade total ao que foi dito na transcrição.\n\n"

		"Vídeo: {{video_file}}\n"
		"Idioma de origem configurado: {{source_language}}\n"
		"Faixa geral selecionada pelo usuário: {{range_start_sec}} até {{range_end_sec}} segundos\n\n"

		"Ranges/marcações escolhidos pelo usuário:\n"
		"{{selected_ranges}}\n\n"

		"Keywords/temas desejados pelo usuário:\n"
		"{{topic_keywords}}\n\n"

		"Agora analise a transcrição abaixo e gere o prompt final para o Opus Clip.\n"
		"O prompt final deve citar explicitamente os melhores timestamps/ranges em segundos, no formato 123.45s-156.78s, e explicar, em uma frase curta, o tipo de corte esperado em cada um.\n"
		"Ao escolher ou descrever ranges, priorize janelas em que o assunto principal começa e termina naturalmente.\n"
		"Não recomende um corte se ele começa do nada, depende de contexto anterior que não está no clipe ou termina antes do payoff/conclusão.\n"
		"Se um trecho bom contém interrupções sobre chat atrasado, PIX, estrelinha, leitura de mensagens ou doações, instrua o Opus a cortar essas partes fora.\n"
		"Não copie a transcrição inteira. Sintetize apenas as instruções úteis para o Opus.\n\n"
		"Transcrição filtrada pelos ranges selecionados, com timestamps em segundos:\n"
		"{{transcript}}\n");
}

static QString defaultEnglishInputTextTemplate(bool autoDetectLanguage)
{
	const QString languageInstruction =
		autoDetectLanguage
			? QStringLiteral(
				  "Your task is NOT to create the clips directly. Your task is to generate a final prompt for Opus Clip as a custom prompt. Write the final Opus prompt in the same language detected in the transcript; if the language is ambiguous, use English.\n")
			: QStringLiteral(
				  "Your task is NOT to create the clips directly. Your task is to generate a final prompt, in English, for Opus Clip as a custom prompt.\n");

	return QStringLiteral("You are a senior strategist for viral short-form clips extracted from livestreams.\n") +
	       languageInstruction +
	       QStringLiteral(
		       "Opus Clip will use this prompt to decide which sections to cut, so the prompt must be objective, actionable, and based on timestamps.\n\n"

		       "Mandatory rules for your response:\n"
		       "- Return ONLY the final prompt for Opus Clip.\n"
		       "- Do not use markdown.\n"
		       "- Do not explain your reasoning.\n"
		       "- Do not invent quotes, topics, promises, or events that do not appear in the transcript.\n"
		       "- Preserve the important timestamps in the final prompt.\n"
		       "- Use timestamps only in seconds, using the format 123.45s-156.78s. Do not use HH:MM:SS or MM:SS.\n"
		       "- The transcript below should already be limited to the selected ranges; do not recommend clips outside those ranges.\n"
		       "- If user-selected markers/ranges exist, they are the highest priority.\n"
		       "- The final prompt must tell Opus to prioritize clips inside the marked ranges.\n"
		       "- Allow using a few seconds before/after the ranges only when it improves context, hook, or ending.\n"
		       "- If there are no marked ranges, choose the best moments based on the transcript.\n"
		       "- Each clip must focus on exactly one main subject. Do not mix two different subjects in the same clip.\n"
		       "- Each clip must have a beginning, middle, and end: clear hook/opening, understandable development, and natural closing.\n"
		       "- Do not choose a section only because it has an isolated hook. The clip must include the context that makes the hook understandable.\n"
		       "- If the best hook starts in the middle of the subject, move the start a few seconds earlier until the setup is clear; if there is no natural ending, reject the section.\n"
		       "- Reject clips that start mid-sentence, mid-answer, or mid-joke, or that end without a conclusion, payoff, or natural transition.\n"
		       "- Reject clips that only work if the viewer has already watched many previous minutes of the livestream.\n\n"

		       "The final Opus prompt should include instructions such as:\n"
		       "- prioritize sections with a strong hook in the first seconds, as long as the context is clear inside the clip itself;\n"
		       "- prioritize complete subject windows: setup/context, development, and conclusion/payoff;\n"
		       "- prioritize clear explanations, strong opinions, tension, surprise, humor, contrast, expectation breaks, or punchlines;\n"
		       "- keep each clip focused on a single subject, without merging parallel topics or unrelated conversations;\n"
		       "- avoid sections that depend too much on previous context;\n"
		       "- remove pauses, repetitions, hesitations, and low-value parts;\n"
		       "- cut out any part that does not directly belong to the main subject of the clip;\n"
		       "- remove repetitive livestream meta sections, especially comments about delayed messages, delayed chat, reading messages, PIX, star comments, donations, super chat, or requests to pay so a message is read earlier, unless that is the main subject and has entertainment value;\n"
		       "- look for clips with a clear beginning, short development, and satisfying ending;\n"
		       "- start a few seconds before the hook when that is necessary to contextualize the subject;\n"
		       "- end only after there is a conclusion, payoff, topic turn, or natural closing;\n"
		       "- prefer clips between 30 and 60 seconds, unless a shorter section is stronger;\n"
		       "- stay fully faithful to what was said in the transcript.\n\n"

		       "Video: {{video_file}}\n"
		       "Configured source language: {{source_language}}\n"
		       "General range selected by the user: {{range_start_sec}} to {{range_end_sec}} seconds\n\n"

		       "User-selected ranges/markers:\n"
		       "{{selected_ranges}}\n\n"

		       "Desired keywords/topics from the user:\n"
		       "{{topic_keywords}}\n\n"

		       "Now analyze the transcript below and generate the final prompt for Opus Clip.\n"
		       "The final prompt must explicitly cite the best timestamps/ranges in seconds, using the format 123.45s-156.78s, and explain, in one short sentence, the expected type of clip for each one.\n"
		       "When choosing or describing ranges, prioritize windows where the main subject starts and ends naturally.\n"
		       "Do not recommend a clip if it starts out of nowhere, depends on previous context that is not inside the clip, or ends before the payoff/conclusion.\n"
		       "If a good section contains interruptions about delayed chat, PIX, stars, reading messages, or donations, instruct Opus to cut those parts out.\n"
		       "Do not copy the full transcript. Summarize only the useful instructions for Opus.\n\n"
		       "Transcript filtered by the selected ranges, with timestamps in seconds:\n"
		       "{{transcript}}\n");
}

QString GptPromptClient::inputTemplateConfigKey()
{
	return inputTemplateConfigKey(
		PluginConfig::getValue(QString::fromLatin1(CONFIG_OPUS_SOURCE_LANGUAGE), QStringLiteral("auto")));
}

QString GptPromptClient::inputTemplateConfigKey(const QString &sourceLanguage)
{
	return QString::fromLatin1(CONFIG_GPT_INPUT_TEMPLATE_PREFIX) + normalizePromptLanguage(sourceLanguage);
}

QString GptPromptClient::defaultInputTextTemplate()
{
	return defaultInputTextTemplate(
		PluginConfig::getValue(QString::fromLatin1(CONFIG_OPUS_SOURCE_LANGUAGE), QStringLiteral("auto")));
}

QString GptPromptClient::defaultInputTextTemplate(const QString &sourceLanguage)
{
	const QString normalizedLanguage = normalizePromptLanguage(sourceLanguage);

	if (normalizedLanguage == QStringLiteral("pt"))
		return defaultPortugueseInputTextTemplate();

	return defaultEnglishInputTextTemplate(normalizedLanguage == QStringLiteral("auto"));
}

QString GptPromptClient::configuredInputTextTemplate()
{
	return configuredInputTextTemplate(
		PluginConfig::getValue(QString::fromLatin1(CONFIG_OPUS_SOURCE_LANGUAGE), QStringLiteral("auto")));
}

QString GptPromptClient::configuredInputTextTemplate(const QString &sourceLanguage)
{
	const QString configuredTemplate = PluginConfig::getValue(inputTemplateConfigKey(sourceLanguage)).trimmed();
	if (!configuredTemplate.isEmpty())
		return configuredTemplate;

	return defaultInputTextTemplate(sourceLanguage).trimmed();
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
	const QString sourceLanguage =
		PluginConfig::getValue(QString::fromLatin1(CONFIG_OPUS_SOURCE_LANGUAGE), QStringLiteral("auto"));
	return renderInputTemplate(configuredInputTextTemplate(sourceLanguage), videoPath, transcript, curationSettings,
				   sourceLanguage);
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
