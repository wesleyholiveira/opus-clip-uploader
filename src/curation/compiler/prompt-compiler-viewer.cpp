#include "curation/compiler/prompt-compiler-internal.hpp"

#include "curation/curation-preset.hpp"
#include "curation/curation-rules.hpp"
#include "curation/opus-prompt-renderer.hpp"

#include <QRegularExpression>

using namespace Curation;

bool Curation::isGenericViewerContext(const QString &contextPhrase)
{
	const QString lower = contextPhrase.toLower();
	return contextPhrase.isEmpty() || lower.contains(QStringLiteral("viewer question")) ||
	       lower.contains(QStringLiteral("viewer message")) || lower.contains(QStringLiteral("viewer comment")) ||
	       lower.contains(QStringLiteral("same message")) || lower.contains(QStringLiteral("that one message")) ||
	       lower.startsWith(QStringLiteral("from a viewer")) || lower.startsWith(QStringLiteral("from the viewer"));
}

bool Curation::looksLikePortugueseViewerTarget(const QString &target)
{
	return containsPortuguesePromptMarkers(target);
}

bool Curation::isGenericViewerTarget(const QString &target)
{
	QString normalized = target.toLower().simplified();
	normalized.replace(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}]+")), QStringLiteral(" "));
	normalized.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
	normalized = normalized.trimmed();

	if (normalized.isEmpty())
		return true;

	if (normalized.startsWith(QStringLiteral("one clear idea")) ||
	    normalized.startsWith(QStringLiteral("the selected idea")) ||
	    normalized.contains(QStringLiteral("selected range")) ||
	    normalized.contains(QStringLiteral("that same message")) ||
	    normalized.contains(QStringLiteral("one exchange")))
		return true;

	static const QStringList genericPhrases{
		QStringLiteral("em uma conversa de live"),
		QStringLiteral("em uma live"),
		QStringLiteral("conversa de live"),
		QStringLiteral("live conversation"),
		QStringLiteral("conversation in a live"),
		QStringLiteral("live stream conversation"),
		QStringLiteral("stream conversation"),
		QStringLiteral("viewer interaction"),
		QStringLiteral("viewer message"),
		QStringLiteral("viewer question"),
		QStringLiteral("viewer comment"),
		QStringLiteral("chat message"),
		QStringLiteral("live chat"),
		QStringLiteral("general chat"),
		QStringLiteral("stream chat"),
	};

	for (const QString &phrase : genericPhrases) {
		if (normalized == phrase || normalized.contains(phrase))
			return true;
	}

	return false;
}

QString Curation::cleanViewerTarget(QString target)
{
	target = compactPlanPhrase(stripLeadingFindTarget(target), 90);
	target.remove(QRegularExpression(
		QStringLiteral(
			"\\s+(?:while\\s+)?(?:from|inside|within)\\s+(?:a|the)\\s+viewer\\s+(?:question|message|comment).*$"),
		QRegularExpression::CaseInsensitiveOption));
	target.remove(QRegularExpression(
		QStringLiteral(
			"^(?:a|the)\\s+viewer\\s+(?:question|message|comment)\\s+(?:about|on|asking\\s+about)\\s+"),
		QRegularExpression::CaseInsensitiveOption));
	target.remove(QRegularExpression(
		QStringLiteral("^viewer\\s+(?:question|message|comment)\\s+(?:about|on|asking\\s+about)\\s+"),
		QRegularExpression::CaseInsensitiveOption));
	target.remove(QRegularExpression(QStringLiteral("^(?:about|on|regarding|concerning)\\s+"),
					 QRegularExpression::CaseInsensitiveOption));
	target.remove(QRegularExpression(
		QStringLiteral(
			"^(?:an?\\s+)?(?:[a-z][a-z-]*\\s+){0,3}(?:answer|response|reply|advice)\\s+(?:about|on|regarding|concerning)\\s+"),
		QRegularExpression::CaseInsensitiveOption));
	target.remove(QRegularExpression(QStringLiteral("^resposta\\s+(?:sobre|a|para)\\s+"),
					 QRegularExpression::CaseInsensitiveOption));
	target.remove(QRegularExpression(QStringLiteral("^pergunta\\s+(?:sobre|a|para)\\s+"),
					 QRegularExpression::CaseInsensitiveOption));

	if (looksLikePortugueseViewerTarget(target) || isGenericViewerTarget(target))
		return {};

	target = compactPlanPhrase(target, 72);
	if (!hasEnglishOnlyPromptText(target) || isGenericViewerTarget(target))
		return {};

	return target;
}

QString Curation::explicitViewerTargetFromSettings(const CurationSettings &curationSettings)
{
	QStringList cleanedKeywords;
	for (const QString &keyword : curationSettings.topicKeywords) {
		const QString cleaned = cleanViewerTarget(keyword);
		if (!cleaned.isEmpty())
			cleanedKeywords << cleaned;
	}

	if (cleanedKeywords.isEmpty())
		return {};

	return compactPlanPhrase(cleanedKeywords.join(QStringLiteral(" ")), 72);
}

bool Curation::isStructuredViewerMessagePrompt(const QString &prompt)
{
	const QString lower = prompt.toLower();
	const bool mentionsViewerMessage = lower.contains(QStringLiteral("viewer message"));
	const bool constrainsOneMessage = lower.contains(QStringLiteral("single viewer message")) ||
					  lower.contains(QStringLiteral("one viewer message")) ||
					  lower.contains(QStringLiteral("that same message")) ||
					  lower.contains(QStringLiteral("same message"));
	const bool followsResponse = lower.contains(QStringLiteral("direct response")) ||
				     lower.contains(QStringLiteral("coherent response")) ||
				     lower.contains(QStringLiteral("complete reaction"));
	const bool hasViewerBoundary = lower.contains(QStringLiteral("another viewer message")) ||
				       lower.contains(QStringLiteral("next viewer message")) ||
				       lower.contains(QStringLiteral("different topic"));

	return mentionsViewerMessage && constrainsOneMessage && followsResponse && hasViewerBoundary;
}

QString Curation::viewerMessageTargetSuffix(QString target, QString contextPhrase)
{
	target = cleanViewerTarget(target);
	contextPhrase = cleanViewerTarget(contextPhrase);

	if (!target.isEmpty() && !isGenericViewerContext(target)) {
		target.remove(QRegularExpression(QStringLiteral("^(?:about|on|regarding|concerning)\\s+"),
					     QRegularExpression::CaseInsensitiveOption));
		target = compactPlanPhrase(target, 72);
		if (!target.isEmpty())
			return QStringLiteral("about %1").arg(target).simplified();
	}

	if (!isGenericViewerContext(contextPhrase)) {
		contextPhrase.remove(QRegularExpression(
			QStringLiteral("^(from|inside|within)\\s+(a|the)\\s+viewer\\s+(question|message|comment)\\s*"),
			QRegularExpression::CaseInsensitiveOption));
		contextPhrase = compactPlanPhrase(contextPhrase, 56);
		if (!contextPhrase.isEmpty())
			return QStringLiteral("about %1").arg(contextPhrase).simplified();
	}

	return {};
}

QString Curation::viewerTargetFromRenderedPrompt(const QString &prompt)
{
	QString text = lineValueForPrefix(prompt, opusPromptPrefixLiteral());
	if (text.trimmed().isEmpty())
		text = prompt;
	text = text.simplified();

	QRegularExpression specificallyAboutPattern(QStringLiteral("\\bspecifically\\s+about\\s+([^\\.]+)"),
						    QRegularExpression::CaseInsensitiveOption);
	QRegularExpressionMatch match = specificallyAboutPattern.match(text);
	if (match.hasMatch()) {
		const QString target = cleanViewerTarget(match.captured(1));
		if (!target.isEmpty())
			return target;
	}

	QRegularExpression messageAboutPattern(QStringLiteral("\\bsingle\\s+viewer\\s+message\\s+about\\s+([^\\.]+)"),
					       QRegularExpression::CaseInsensitiveOption);
	match = messageAboutPattern.match(text);
	if (match.hasMatch()) {
		const QString target = cleanViewerTarget(match.captured(1));
		if (!target.isEmpty())
			return target;
	}

	return {};
}

bool Curation::isSafeViewerStopPhrase(const QString &phrase)
{
	const QString lower = phrase.toLower();
	if (phrase.isEmpty() || phrase.size() > 52 || !hasEnglishOnlyPromptText(phrase))
		return false;

	return !containsAnyPhrase(lower,
				  {QStringLiteral("do not"), QStringLiteral("don't"), QStringLiteral("avoid"),
				   QStringLiteral("instead of"), QStringLiteral("cut off"), QStringLiteral("roll into"),
				   QStringLiteral("speaker finishes the explanation that"), QStringLiteral("ends when"),
				   QStringLiteral("end when"), QStringLiteral("stop when"), QStringLiteral("stops when"),
				   QStringLiteral("answer concludes that"), QStringLiteral("concludes that the person"),
				   QStringLiteral("the person")});
}

QString Curation::viewerStopPhrase(QString ending)
{
	ending = compactPlanPhrase(ending, 52);
	if (!isSafeViewerStopPhrase(ending))
		return QStringLiteral("at the first natural resolution");

	const QString lowerEnding = ending.toLower();
	if (lowerEnding.startsWith(QStringLiteral("at ")) || lowerEnding.startsWith(QStringLiteral("after ")) ||
	    lowerEnding.startsWith(QStringLiteral("once ")) || lowerEnding.startsWith(QStringLiteral("before ")))
		return ending;

	if (lowerEnding.startsWith(QStringLiteral("when "))) {
		const QString clause = ending.mid(5).trimmed();
		if (!clause.isEmpty() && !clause.startsWith(QStringLiteral("end "), Qt::CaseInsensitive) &&
		    !clause.startsWith(QStringLiteral("ends "), Qt::CaseInsensitive) &&
		    !clause.startsWith(QStringLiteral("stop "), Qt::CaseInsensitive) &&
		    !clause.startsWith(QStringLiteral("stops "), Qt::CaseInsensitive))
			return ending;
		return QStringLiteral("at the first natural resolution");
	}

	return QStringLiteral("at the first natural resolution");
}

QString Curation::renderViewerMessagePlanPrompt(const QJsonObject &plan,
						const RecordingTranscript &selectedRangeTranscript,
						const CurationSettings &curationSettings, const QString &planHint)
{
	Q_UNUSED(plan);
	const bool multipleClips = shouldRenderMultipleClips(selectedRangeTranscript, curationSettings, planHint);
	const QString target = explicitViewerTargetFromSettings(curationSettings);
	const QString targetSuffix = viewerMessageTargetSuffix(target, QString());
	const QString prompt = renderViewerMessagePrompt(multipleClips, targetSuffix, targetSuffix.isEmpty());
	if (!hasEnglishOnlyPromptText(prompt))
		return englishOnlyPresetFallback(selectedRangeTranscript, curationSettings, planHint);

	return prompt;
}
