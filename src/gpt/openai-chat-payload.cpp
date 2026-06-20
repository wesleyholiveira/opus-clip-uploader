#include "gpt/openai-chat-payload.hpp"

#include <QJsonArray>

namespace {

constexpr int GPT_PROMPT_SEED = 12345;

void applyDeterministicSampling(QJsonObject &payload)
{
	payload.insert(QStringLiteral("temperature"), 0.0);
	payload.insert(QStringLiteral("top_p"), 1.0);
	payload.insert(QStringLiteral("seed"), GPT_PROMPT_SEED);
}

} // namespace

namespace OpenAiChatPayload {

QJsonObject build(const QString &model, const QString &inputText, int maxTokens, bool jsonMode)
{
	QJsonObject payload;
	payload.insert(QStringLiteral("model"), model.trimmed());
	payload.insert(QStringLiteral("max_completion_tokens"), maxTokens);
	applyDeterministicSampling(payload);

	if (jsonMode) {
		QJsonObject responseFormat;
		responseFormat.insert(QStringLiteral("type"), QStringLiteral("json_object"));
		payload.insert(QStringLiteral("response_format"), responseFormat);
	}

	QJsonArray messages;
	QJsonObject systemMessage;
	systemMessage.insert(QStringLiteral("role"), QStringLiteral("system"));
	systemMessage.insert(
		QStringLiteral("content"),
		jsonMode
			? QStringLiteral("You are a deterministic ClipAnything prompt planner for Opus Clip. "
					 "Return valid JSON only. Do not write the final Opus prompt directly.")
			: QStringLiteral(
				  "You generate concise English ClipAnything search prompts for Opus Clip. "
				  "Follow the user's formatting rules exactly and return only the requested output line."));
	messages.append(systemMessage);

	QJsonObject userMessage;
	userMessage.insert(QStringLiteral("role"), QStringLiteral("user"));
	userMessage.insert(QStringLiteral("content"), inputText);
	messages.append(userMessage);

	payload.insert(QStringLiteral("messages"), messages);
	return payload;
}

} // namespace OpenAiChatPayload
