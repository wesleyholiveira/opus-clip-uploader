#include "gpt/gpt-prompt-store.hpp"

#include "utils/config.hpp"

#include <QCryptographicHash>
#include <QFileInfo>

static bool isSemanticGateFailurePrompt(const QString &prompt)
{
	return prompt.trimmed().startsWith(QStringLiteral("NO_STRONG_CLIP_FOUND:"), Qt::CaseInsensitive);
}

QString GptPromptStore::safeFileKey(const QString &videoPath)
{
	const QFileInfo info(videoPath);
	const QString stableName = info.fileName().trimmed().isEmpty() ? videoPath : info.fileName();
	const QByteArray hash = QCryptographicHash::hash(stableName.toUtf8(), QCryptographicHash::Sha256).toHex();
	return QString::fromLatin1(hash.left(24));
}

QString GptPromptStore::keyForVideoPath(const QString &videoPath)
{
	return QStringLiteral("gpt_prompt.%1").arg(safeFileKey(videoPath));
}

QString GptPromptStore::keyForInput(const QString &model, const QString &inputText)
{
	const QString normalizedModel = model.trimmed().toLower();
	const QString normalizedInput = inputText.trimmed();
	const QByteArray material =
		normalizedModel.toUtf8() + "\n---clip-cropper-gpt-input---\n" + normalizedInput.toUtf8();
	const QByteArray hash = QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex();
	return QStringLiteral("gpt_prompt_input.v52.%1").arg(QString::fromLatin1(hash.left(32)));
}

static QString pendingKeyForVideoPath(const QString &videoPath)
{
	return QStringLiteral("gpt_prompt_pending.%1")
		.arg(GptPromptStore::keyForVideoPath(videoPath).section(QLatin1Char('.'), -1));
}

QString GptPromptStore::loadForVideoPath(const QString &videoPath)
{
	return PluginConfig::getValue(keyForVideoPath(videoPath)).trimmed();
}

QString GptPromptStore::loadForInput(const QString &model, const QString &inputText)
{
	if (inputText.trimmed().isEmpty())
		return {};

	const QString key = keyForInput(model, inputText);
	const QString cached = PluginConfig::getValue(key).trimmed();
	if (isSemanticGateFailurePrompt(cached)) {
		PluginConfig::removeValue(key);
		return {};
	}

	return cached;
}

void GptPromptStore::saveForVideoPath(const QString &videoPath, const QString &prompt)
{
	const QString trimmedPrompt = prompt.trimmed();
	if (trimmedPrompt.isEmpty())
		return;

	PluginConfig::setValue(keyForVideoPath(videoPath), trimmedPrompt);
	clearPendingForVideoPath(videoPath);
}

void GptPromptStore::saveForInput(const QString &model, const QString &inputText, const QString &prompt)
{
	const QString trimmedPrompt = prompt.trimmed();
	if (inputText.trimmed().isEmpty() || trimmedPrompt.isEmpty())
		return;

	if (isSemanticGateFailurePrompt(trimmedPrompt))
		return;

	PluginConfig::setValue(keyForInput(model, inputText), trimmedPrompt);
}

bool GptPromptStore::isPendingForVideoPath(const QString &videoPath)
{
	const QString value = PluginConfig::getValue(pendingKeyForVideoPath(videoPath)).trimmed().toLower();
	return value == QStringLiteral("true") || value == QStringLiteral("1") || value == QStringLiteral("yes");
}

void GptPromptStore::markPendingForVideoPath(const QString &videoPath)
{
	PluginConfig::setValue(pendingKeyForVideoPath(videoPath), QStringLiteral("true"));
}

void GptPromptStore::clearPendingForVideoPath(const QString &videoPath)
{
	PluginConfig::removeValue(pendingKeyForVideoPath(videoPath));
}

void GptPromptStore::removeForVideoPath(const QString &videoPath)
{
	PluginConfig::removeValue(keyForVideoPath(videoPath));
	clearPendingForVideoPath(videoPath);
}
