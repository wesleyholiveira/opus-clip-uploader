#include "curation/compiler/prompt-compiler-internal.hpp"

#include "curation/curation-preset.hpp"
#include "curation/curation-rules.hpp"
#include "curation/curation-signals.hpp"
#include "curation/opus-prompt-renderer.hpp"

#include <QRegularExpression>

using namespace Curation;

namespace {

static constexpr const char *kSemanticGateFailurePrefix = "NO_STRONG_CLIP_FOUND:";
static constexpr const char *kPromptGenerationBlockedPrefix = "GPT_PROMPT_BLOCKED:";
static constexpr const char *kOpusPromptPrefix = "OPUS_PROMPT:";

}

const char *Curation::semanticGateFailurePrefixLiteral()
{
	return kSemanticGateFailurePrefix;
}

const char *Curation::promptGenerationBlockedPrefixLiteral()
{
	return kPromptGenerationBlockedPrefix;
}

const char *Curation::opusPromptPrefixLiteral()
{
	return kOpusPromptPrefix;
}

bool Curation::containsAnyPhrase(const QString &text, const QStringList &phrases)
{
	for (const QString &phrase : phrases) {
		if (text.contains(phrase, Qt::CaseInsensitive))
			return true;
	}
	return false;
}

QString Curation::lineValueForPrefix(const QString &text, const char *prefix)
{
	const QString prefixText = QString::fromLatin1(prefix);
	const QStringList lines = text.split(QLatin1Char('\n'));

	for (QString line : lines) {
		line = line.trimmed();
		if (line.startsWith(prefixText, Qt::CaseInsensitive))
			return line.mid(prefixText.size()).trimmed();
	}

	return {};
}

int Curation::transcriptTextCharCount(const RecordingTranscript &transcript)
{
	int chars = 0;
	for (const TranscriptSegment &segment : transcript.segments)
		chars += segment.text.trimmed().size();
	return chars;
}

bool Curation::allowNoStrongFailure(const RecordingTranscript &selectedRangeTranscript,
				    const CurationSettings &curationSettings)
{
	const double durationSec = selectedDurationSeconds(selectedRangeTranscript, curationSettings);
	const int segmentCount = selectedRangeTranscript.segments.size();
	const int textChars = transcriptTextCharCount(selectedRangeTranscript);

	return durationSec < 90.0 || segmentCount < 8 || textChars < 400;
}

QString Curation::renderFallbackRubricTemplate(QString templateText, const CurationSettings &curationSettings)
{
	QString topic = curationSettings.topicKeywords.join(QStringLiteral(", ")).trimmed();
	if (topic.isEmpty())
		topic = QStringLiteral("the strongest topics in the selected range");

	templateText.replace(QStringLiteral("{{topic_keywords}}"), topic);
	templateText.replace(
		QStringLiteral("{{curation_preset}}"),
		CurationPreset::labelForId(CurationPreset::resolveId(curationSettings, curationSettings.aiPrompt)));
	templateText.replace(QStringLiteral("{{preset_opus_prompt}}"),
			     CurationPreset::fallbackOpusPrompt(curationSettings, true));
	return templateText.trimmed();
}

QString Curation::jsonStringValue(const QJsonObject &object, const QString &key)
{
	return object.value(key).toString().simplified();
}

QString Curation::cleanPlanPhrase(QString phrase)
{
	phrase = phrase.simplified();
	phrase.remove(
		QRegularExpression(QStringLiteral("^OPUS_PROMPT\\s*:\\s*"), QRegularExpression::CaseInsensitiveOption));
	phrase.remove(QRegularExpression(QStringLiteral("^[\\-–—•]+\\s*")));
	phrase.replace(QLatin1Char(':'), QLatin1Char(','));
	while (phrase.endsWith(QLatin1Char('.')) || phrase.endsWith(QLatin1Char(';')) ||
	       phrase.endsWith(QLatin1Char(':')) || phrase.endsWith(QLatin1Char(',')))
		phrase.chop(1);
	return phrase.trimmed();
}

QString Curation::compactPlanPhrase(QString phrase, int maxChars)
{
	phrase = cleanPlanPhrase(phrase);
	if (maxChars > 0 && phrase.size() > maxChars) {
		phrase = phrase.left(maxChars);
		const int lastSpace = phrase.lastIndexOf(QLatin1Char(' '));
		if (lastSpace > maxChars / 2)
			phrase = phrase.left(lastSpace);
	}
	return cleanPlanPhrase(phrase);
}

QString Curation::stripLeadingFindTarget(QString target)
{
	target = cleanPlanPhrase(target);
	target.remove(QRegularExpression(
		QStringLiteral(
			"^find\\s+(one\\s+|multiple\\s+|the\\s+)?(clear\\s+|strong\\s+|self-contained\\s+|strongest\\s+)*(clip|clips|moment)s?\\s+(where|about)\\s+"),
		QRegularExpression::CaseInsensitiveOption));
	target.remove(QRegularExpression(
		QStringLiteral(
			"^the\\s+speaker\\s+(explains|answers|tells|argues|demonstrates|compares|walks through)\\s+"),
		QRegularExpression::CaseInsensitiveOption));
	return cleanPlanPhrase(target);
}

QString Curation::arcVerbForType(QString arcType)
{
	arcType = arcType.trimmed().toLower();
	if (arcType == QStringLiteral("advice_answer") || arcType == QStringLiteral("qa_answer"))
		return QStringLiteral("answers");
	if (arcType == QStringLiteral("story"))
		return QStringLiteral("tells");
	if (arcType == QStringLiteral("opinion"))
		return QStringLiteral("argues");
	if (arcType == QStringLiteral("comparison"))
		return QStringLiteral("compares");
	if (arcType == QStringLiteral("demo_or_walkthrough") || arcType == QStringLiteral("tutorial_step"))
		return QStringLiteral("walks through");
	return QStringLiteral("explains");
}

bool Curation::shouldRenderMultipleClips(const RecordingTranscript &selectedRangeTranscript,
					 const CurationSettings &curationSettings, const QString &hint)
{
	const Intent intent = resolveIntent(curationSettings, selectedRangeTranscript, hint);
	return shouldUseMultipleClips(intent);
}

QString Curation::scopeFindPhrase(const RecordingTranscript &selectedRangeTranscript,
				  const CurationSettings &curationSettings, const QString &hint)
{
	if (shouldRenderMultipleClips(selectedRangeTranscript, curationSettings, hint))
		return QStringLiteral("Find strong self-contained clips where");
	return QStringLiteral("Find the strongest self-contained clip where");
}

QString Curation::trimmedJoinNonEmpty(const QStringList &parts, const QString &separator)
{
	QStringList cleanParts;
	for (QString part : parts) {
		part = cleanPlanPhrase(part);
		if (!part.isEmpty())
			cleanParts << part;
	}
	return cleanParts.join(separator).simplified();
}

bool Curation::containsPortuguesePromptMarkers(const QString &text)
{
	QString normalized = text.toLower().simplified();
	normalized.replace(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}]+")), QStringLiteral(" "));
	const QString padded = QStringLiteral(" ") + normalized + QLatin1Char(' ');

	for (const QString &hardToken :
	     {QStringLiteral("resposta"), QStringLiteral("pedir"), QStringLiteral("aumento"), QStringLiteral("chefe"),
	      QStringLiteral("patrão"), QStringLiteral("patrao"), QStringLiteral("dúvida"), QStringLiteral("duvida"),
	      QStringLiteral("pergunta"), QStringLiteral("quando"), QStringLiteral("termina"),
	      QStringLiteral("terminar"), QStringLiteral("conclui"), QStringLiteral("pessoa"),
	      QStringLiteral("mensagem"), QStringLiteral("espectador")}) {
		if (padded.contains(QStringLiteral(" ") + hardToken + QLatin1Char(' ')))
			return true;
	}

	int softHits = 0;
	for (const QString &softToken :
	     {QStringLiteral("sobre"), QStringLiteral("como"), QStringLiteral("para"), QStringLiteral("com"),
	      QStringLiteral("porque"), QStringLiteral("porquê"), QStringLiteral("uma"), QStringLiteral("em"),
	      QStringLiteral("de"), QStringLiteral("da"), QStringLiteral("do"), QStringLiteral("dos"),
	      QStringLiteral("das")}) {
		if (padded.contains(QStringLiteral(" ") + softToken + QLatin1Char(' ')))
			++softHits;
	}

	return softHits >= 2;
}

bool Curation::hasEnglishOnlyPromptText(const QString &text)
{
	return !containsPortuguesePromptMarkers(text);
}

QString Curation::englishOnlyPresetFallback(const RecordingTranscript &selectedRangeTranscript,
					    const CurationSettings &curationSettings, const QString &hint)
{
	const Intent intent = resolveIntent(curationSettings, selectedRangeTranscript, hint);
	return OpusPromptRenderer::renderIntentPrompt(intent);
}
