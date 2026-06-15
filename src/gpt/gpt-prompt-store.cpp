#include "gpt/gpt-prompt-store.hpp"

#include "utils/config.hpp"

#include <QCryptographicHash>
#include <QFileInfo>

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

static QString pendingKeyForVideoPath(const QString &videoPath)
{
	return QStringLiteral("gpt_prompt_pending.%1").arg(GptPromptStore::keyForVideoPath(videoPath).section(QLatin1Char('.'), -1));
}

QString GptPromptStore::loadForVideoPath(const QString &videoPath)
{
	return PluginConfig::getValue(keyForVideoPath(videoPath)).trimmed();
}

void GptPromptStore::saveForVideoPath(const QString &videoPath, const QString &prompt)
{
	const QString trimmedPrompt = prompt.trimmed();
	if (trimmedPrompt.isEmpty())
		return;

	PluginConfig::setValue(keyForVideoPath(videoPath), trimmedPrompt);
	clearPendingForVideoPath(videoPath);
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
