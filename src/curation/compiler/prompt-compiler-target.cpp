#include "curation/compiler/prompt-compiler-internal.hpp"

#include "curation/analysis/named-reference-detector.hpp"
#include "curation/compiler/prompt-quality-gate.hpp"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>

using namespace Curation;

QJsonObject Curation::extractJsonObjectFromText(const QString &rawOutput)
{
	QString text = rawOutput.trimmed();
	if (text.startsWith(QStringLiteral("```"))) {
		text.remove(QRegularExpression(QStringLiteral("^```(?:json)?\\s*"),
					       QRegularExpression::CaseInsensitiveOption));
		text.remove(QRegularExpression(QStringLiteral("\\s*```$")));
		text = text.trimmed();
	}

	QJsonParseError error;
	QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &error);
	if (!doc.isObject()) {
		const int firstBrace = text.indexOf(QLatin1Char('{'));
		const int lastBrace = text.lastIndexOf(QLatin1Char('}'));
		if (firstBrace >= 0 && lastBrace > firstBrace) {
			const QString slice = text.mid(firstBrace, lastBrace - firstBrace + 1);
			doc = QJsonDocument::fromJson(slice.toUtf8(), &error);
		}
	}

	if (!doc.isObject())
		return {};

	QJsonObject object = doc.object();
	if (object.contains(QStringLiteral("plan")) && object.value(QStringLiteral("plan")).isObject())
		object = object.value(QStringLiteral("plan")).toObject();

	return object;
}

QString Curation::firstCleanTopicKeyword(const CurationSettings &curationSettings)
{
	for (QString keyword : curationSettings.topicKeywords) {
		keyword = keyword.simplified();
		if (keyword.isEmpty())
			continue;

		keyword.replace(QRegularExpression(QStringLiteral("[\r\n\t]+")), QStringLiteral(" "));
		keyword = keyword.section(QLatin1Char(','), 0, 0).section(QStringLiteral(" and "), 0, 0).trimmed();
		if (!keyword.isEmpty() && hasEnglishOnlyPromptText(keyword))
			return keyword.left(90).trimmed();
	}

	return {};
}

QString Curation::deterministicPromptTarget(const QString &generatedPrompt,
					    const RecordingTranscript &selectedRangeTranscript,
					    const CurationSettings &curationSettings)
{
	QString transcriptText;
	for (const TranscriptSegment &segment : selectedRangeTranscript.segments) {
		if (!segment.text.trimmed().isEmpty()) {
			if (!transcriptText.isEmpty())
				transcriptText += QLatin1Char(' ');
			transcriptText += segment.text.trimmed();
		}
	}

	const QString combinedLower =
		(opusPromptPayload(generatedPrompt) + QLatin1Char(' ') + transcriptText).toLower();
	const QStringList namedReferences = importantNamedReferences(selectedRangeTranscript);
	const bool needsNamedReference = transcriptHasPronounDependentNamedReference(selectedRangeTranscript) &&
					 !namedReferences.isEmpty();
	const bool hasReferenceBackedUnlockMethod = transcriptHasReferenceBackedUnlockMethod(selectedRangeTranscript);
	const QString namedReference = namedReferences.isEmpty() ? QString() : namedReferences.first();
	const bool mentionsGerman = combinedLower.contains(QStringLiteral("german"));
	const bool mentionsUnlock =
		containsAnyPhrase(combinedLower, {QStringLiteral("unlock"), QStringLiteral("key clicks"),
						  QStringLiteral("one key"), QStringLiteral("one logic")});
	const bool mentionsRoutine = containsAnyPhrase(
		combinedLower, {QStringLiteral("daily loop"), QStringLiteral("routine"),
				QStringLiteral("spaced repetition"), QStringLiteral("practice with natives")});
	const bool mentionsRoadmap =
		containsAnyPhrase(combinedLower, {QStringLiteral("roadmap"), QStringLiteral("8-week"),
						  QStringLiteral("eight-week"), QStringLiteral("week-by-week")});

	if (hasReferenceBackedUnlockMethod && mentionsGerman)
		return QStringLiteral("the unlock method for learning German through %1's language-learning approach")
			.arg(namedReference);

	if (hasReferenceBackedUnlockMethod)
		return QStringLiteral("the unlock method through %1's language-learning approach").arg(namedReference);

	if (needsNamedReference)
		return QStringLiteral("the selected idea connected to %1").arg(namedReference);

	if (mentionsUnlock && mentionsGerman)
		return QStringLiteral("the unlock-based method for learning German");

	if (mentionsRoutine && mentionsGerman)
		return QStringLiteral("the daily German-learning loop");

	if (mentionsRoadmap && mentionsGerman)
		return QStringLiteral("one resolved part of the German-learning plan");

	const QString keyword = firstCleanTopicKeyword(curationSettings);
	if (!keyword.isEmpty())
		return QStringLiteral("one clear idea about %1").arg(keyword);

	return QStringLiteral("one clear idea from the selected range");
}
