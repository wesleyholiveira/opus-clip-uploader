#include "curation/compiler/opus-prompt-compiler.hpp"

#include "curation/compiler/prompt-compiler-internal.hpp"

#include <QRegularExpression>

using namespace Curation;

QString Curation::semanticGateFailurePrefix()
{
	return QString::fromLatin1(semanticGateFailurePrefixLiteral());
}

QString Curation::promptGenerationBlockedPrefix()
{
	return QString::fromLatin1(promptGenerationBlockedPrefixLiteral());
}

bool Curation::isSemanticGateFailurePrompt(const QString &prompt)
{
	return prompt.trimmed().startsWith(QString::fromLatin1(semanticGateFailurePrefixLiteral()), Qt::CaseInsensitive);
}

QString Curation::semanticGateFailureReason(const QString &prompt)
{
	QString text = prompt.trimmed();
	if (!isSemanticGateFailurePrompt(text))
		return {};

	text.remove(QRegularExpression(QStringLiteral("^NO_STRONG_CLIP_FOUND\\s*:\\s*"),
				       QRegularExpression::CaseInsensitiveOption));
	return text.trimmed();
}

QString Curation::promptGenerationBlockedPrompt(const QString &reason)
{
	const QString cleanReason = reason.trimmed().isEmpty()
					    ? QStringLiteral("Prompt generation was blocked before calling GPT.")
					    : reason.trimmed();
	return QString::fromLatin1(promptGenerationBlockedPrefixLiteral()) + QLatin1Char(' ') + cleanReason;
}

bool Curation::isPromptGenerationBlockedPrompt(const QString &prompt)
{
	return prompt.trimmed().startsWith(QString::fromLatin1(promptGenerationBlockedPrefixLiteral()),
					     Qt::CaseInsensitive);
}

QString Curation::promptGenerationBlockedReason(const QString &prompt)
{
	QString text = prompt.trimmed();
	if (!isPromptGenerationBlockedPrompt(text))
		return {};

	text.remove(QRegularExpression(QStringLiteral("^GPT_PROMPT_BLOCKED\\s*:\\s*"),
				       QRegularExpression::CaseInsensitiveOption));
	return text.trimmed();
}
