#include "curation/compiler/opus-prompt-compiler.hpp"

#include "curation/analysis/named-reference-detector.hpp"
#include "curation/compiler/prompt-quality-gate.hpp"
#include "curation/curation-preset.hpp"
#include "curation/curation-rules.hpp"
#include "curation/curation-signals.hpp"
#include "curation/opus-prompt-renderer.hpp"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace {

static constexpr const char *SEMANTIC_GATE_FAILURE_PREFIX = "NO_STRONG_CLIP_FOUND:";
static constexpr const char *PROMPT_GENERATION_BLOCKED_PREFIX = "GPT_PROMPT_BLOCKED:";
static constexpr const char *OPUS_PROMPT_PREFIX = "OPUS_PROMPT:";

bool containsAnyPhrase(const QString &text, const QStringList &phrases)
{
	for (const QString &phrase : phrases) {
		if (text.contains(phrase, Qt::CaseInsensitive))
			return true;
	}
	return false;
}

QString lineValueForPrefix(const QString &text, const char *prefix)
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

int transcriptTextCharCount(const RecordingTranscript &transcript)
{
	int chars = 0;
	for (const TranscriptSegment &segment : transcript.segments)
		chars += segment.text.trimmed().size();
	return chars;
}

bool allowNoStrongFailure(const RecordingTranscript &selectedRangeTranscript, const CurationSettings &curationSettings)
{
	const double durationSec = Curation::selectedDurationSeconds(selectedRangeTranscript, curationSettings);
	const int segmentCount = selectedRangeTranscript.segments.size();
	const int textChars = transcriptTextCharCount(selectedRangeTranscript);

	return durationSec < 90.0 || segmentCount < 8 || textChars < 400;
}

QString renderFallbackRubricTemplate(QString templateText, const CurationSettings &curationSettings)
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

QString jsonStringValue(const QJsonObject &object, const QString &key)
{
	return object.value(key).toString().simplified();
}

QString cleanPlanPhrase(QString phrase)
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

QString compactPlanPhrase(QString phrase, int maxChars)
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

QString deterministicPromptTarget(const QString &generatedPrompt, const RecordingTranscript &selectedRangeTranscript,
				  const CurationSettings &curationSettings);

QString stripLeadingFindTarget(QString target)
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

QString arcVerbForType(QString arcType)
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

bool shouldRenderMultipleClips(const RecordingTranscript &selectedRangeTranscript,
			       const CurationSettings &curationSettings, const QString &hint = {})
{
	const Curation::Intent intent = Curation::resolveIntent(curationSettings, selectedRangeTranscript, hint);
	return Curation::shouldUseMultipleClips(intent);
}

QString scopeFindPhrase(const RecordingTranscript &selectedRangeTranscript, const CurationSettings &curationSettings,
			const QString &hint = {})
{
	if (shouldRenderMultipleClips(selectedRangeTranscript, curationSettings, hint))
		return QStringLiteral("Find strong self-contained clips where");
	return QStringLiteral("Find the strongest self-contained clip where");
}

QString trimmedJoinNonEmpty(const QStringList &parts, const QString &separator)
{
	QStringList cleanParts;
	for (QString part : parts) {
		part = cleanPlanPhrase(part);
		if (!part.isEmpty())
			cleanParts << part;
	}
	return cleanParts.join(separator).simplified();
}

bool isGenericViewerContext(const QString &contextPhrase)
{
	const QString lower = contextPhrase.toLower();
	return contextPhrase.isEmpty() || lower.contains(QStringLiteral("viewer question")) ||
	       lower.contains(QStringLiteral("viewer message")) || lower.contains(QStringLiteral("viewer comment")) ||
	       lower.contains(QStringLiteral("same message")) || lower.contains(QStringLiteral("that one message")) ||
	       lower.startsWith(QStringLiteral("from a viewer")) || lower.startsWith(QStringLiteral("from the viewer"));
}

bool containsPortuguesePromptMarkers(const QString &text)
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
	for (const QString &softToken : {QStringLiteral("sobre"), QStringLiteral("como"), QStringLiteral("para"),
					 QStringLiteral("com"), QStringLiteral("porque"), QStringLiteral("porquê")}) {
		if (padded.contains(QStringLiteral(" ") + softToken + QLatin1Char(' ')))
			++softHits;
	}

	return softHits >= 2;
}

bool hasEnglishOnlyPromptText(const QString &text)
{
	return !containsPortuguesePromptMarkers(text);
}

QString englishOnlyPresetFallback(const RecordingTranscript &selectedRangeTranscript,
				  const CurationSettings &curationSettings, const QString &hint = {})
{
	const Curation::Intent intent = Curation::resolveIntent(curationSettings, selectedRangeTranscript, hint);
	return OpusPromptRenderer::renderIntentPrompt(intent);
}

bool looksLikePortugueseViewerTarget(const QString &target)
{
	return containsPortuguesePromptMarkers(target);
}

QString cleanViewerTarget(QString target)
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

	if (looksLikePortugueseViewerTarget(target))
		return {};

	target = compactPlanPhrase(target, 72);
	if (!hasEnglishOnlyPromptText(target))
		return {};

	return target;
}

bool isStructuredViewerMessagePrompt(const QString &prompt)
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

QString viewerMessageTargetSuffix(QString target, QString contextPhrase)
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

bool isSafeViewerStopPhrase(const QString &phrase)
{
	const QString lower = phrase.toLower();
	if (phrase.isEmpty() || phrase.size() > 52 || !hasEnglishOnlyPromptText(phrase))
		return false;

	return !containsAnyPhrase(lower,
				  {QStringLiteral("do not"), QStringLiteral("don't"), QStringLiteral("avoid"),
				   QStringLiteral("instead of"), QStringLiteral("cut off"), QStringLiteral("roll into"),
				   QStringLiteral("speaker finishes the explanation that"), QStringLiteral("ends when"),
				   QStringLiteral("end when"), QStringLiteral("stop when"),
				   QStringLiteral("stops when"), QStringLiteral("answer concludes that"),
				   QStringLiteral("concludes that the person"), QStringLiteral("the person")});
}

QString viewerStopPhrase(QString ending)
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

QString renderViewerMessagePlanPrompt(const QJsonObject &plan, const RecordingTranscript &selectedRangeTranscript,
				      const CurationSettings &curationSettings, const QString &planHint)
{
	const bool multipleClips = shouldRenderMultipleClips(selectedRangeTranscript, curationSettings, planHint);
	const QString findPrefix = multipleClips ? QStringLiteral("Find self-contained clips")
						 : QStringLiteral("Find the strongest self-contained clip");
	const QString choosePrefix = multipleClips ? QStringLiteral("Choose clips that stop")
						   : QStringLiteral("Choose a clip that stops");

	QString target = jsonStringValue(plan, QStringLiteral("main_target"));
	if (target.isEmpty())
		target = deterministicPromptTarget(QString(), selectedRangeTranscript, curationSettings);

	const QString targetSuffix =
		viewerMessageTargetSuffix(target, jsonStringValue(plan, QStringLiteral("context_phrase")));
	const bool hasTarget = !targetSuffix.isEmpty();

	const QString sentence1 =
		hasTarget ? QStringLiteral(
				    "%1 built from one complete response to a single viewer message specifically %2.")
				    .arg(findPrefix, targetSuffix)
			  : QStringLiteral("%1 built from one complete response to a single viewer message.")
				    .arg(findPrefix);
	const QString sentence2 =
		hasTarget
			? QStringLiteral(
				  "Include only enough of that viewer message for context, then follow the speaker's direct answer to that same message until its first complete resolution.")
			: QStringLiteral(
				  "Prioritize clearly useful viewer messages with emotional consequence while keeping only that one exchange; include only enough of that message for context, then follow the speaker's direct answer to that same message until its first complete resolution.");
	const QString sentence3 =
		hasTarget
			? QStringLiteral(
				  "%1 at the first complete resolution while staying on that viewer message, before the speaker reads another viewer message, answers another viewer message, switches to stream management, thanks a donor, or moves to a different topic.")
				  .arg(choosePrefix)
			: QStringLiteral(
				  "%1 before the speaker reads another viewer message, answers another viewer message, switches to stream management, thanks a donor, or moves to a different topic.")
				  .arg(choosePrefix);

	const QString prompt = QStringLiteral("%1 %2 %3").arg(sentence1, sentence2, sentence3).simplified();
	if (!hasEnglishOnlyPromptText(prompt))
		return englishOnlyPresetFallback(selectedRangeTranscript, curationSettings, planHint);

	return prompt;
}

QJsonObject extractJsonObjectFromText(const QString &rawOutput)
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

QString firstCleanTopicKeyword(const CurationSettings &curationSettings)
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

QString deterministicPromptTarget(const QString &generatedPrompt, const RecordingTranscript &selectedRangeTranscript,
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
		(Curation::opusPromptPayload(generatedPrompt) + QLatin1Char(' ') + transcriptText).toLower();
	const QStringList namedReferences = Curation::importantNamedReferences(selectedRangeTranscript);
	const bool needsNamedReference =
		Curation::transcriptHasPronounDependentNamedReference(selectedRangeTranscript) &&
		!namedReferences.isEmpty();
	const bool hasReferenceBackedUnlockMethod =
		Curation::transcriptHasReferenceBackedUnlockMethod(selectedRangeTranscript);
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

} // namespace

namespace Curation {

QString semanticGateFailurePrefix()
{
	return QString::fromLatin1(SEMANTIC_GATE_FAILURE_PREFIX);
}

QString promptGenerationBlockedPrefix()
{
	return QString::fromLatin1(PROMPT_GENERATION_BLOCKED_PREFIX);
}

bool isSemanticGateFailurePrompt(const QString &prompt)
{
	return prompt.trimmed().startsWith(QString::fromLatin1(SEMANTIC_GATE_FAILURE_PREFIX), Qt::CaseInsensitive);
}

QString semanticGateFailureReason(const QString &prompt)
{
	QString text = prompt.trimmed();
	if (!isSemanticGateFailurePrompt(text))
		return {};

	text.remove(QRegularExpression(QStringLiteral("^NO_STRONG_CLIP_FOUND\\s*:\\s*"),
				       QRegularExpression::CaseInsensitiveOption));
	return text.trimmed();
}

QString promptGenerationBlockedPrompt(const QString &reason)
{
	const QString cleanReason = reason.trimmed().isEmpty()
					    ? QStringLiteral("Prompt generation was blocked before calling GPT.")
					    : reason.trimmed();
	return QString::fromLatin1(PROMPT_GENERATION_BLOCKED_PREFIX) + QLatin1Char(' ') + cleanReason;
}

bool isPromptGenerationBlockedPrompt(const QString &prompt)
{
	return prompt.trimmed().startsWith(QString::fromLatin1(PROMPT_GENERATION_BLOCKED_PREFIX), Qt::CaseInsensitive);
}

QString promptGenerationBlockedReason(const QString &prompt)
{
	QString text = prompt.trimmed();
	if (!isPromptGenerationBlockedPrompt(text))
		return {};

	text.remove(QRegularExpression(QStringLiteral("^GPT_PROMPT_BLOCKED\\s*:\\s*"),
				       QRegularExpression::CaseInsensitiveOption));
	return text.trimmed();
}

QString fallbackRubricOpusPrompt(const CurationSettings &curationSettings, const TemplateSectionLoader &templateLoader)
{
	if (templateLoader) {
		const QString runtimeTemplate =
			templateLoader(QStringLiteral("fallback_opus_prompt"), QStringLiteral("GPT fallback rubric"));
		if (!runtimeTemplate.trimmed().isEmpty())
			return renderFallbackRubricTemplate(runtimeTemplate, curationSettings);
	}

	return CurationPreset::fallbackOpusPrompt(curationSettings, true);
}

QString renderPlanToOpusPrompt(const QJsonObject &plan, const RecordingTranscript &selectedRangeTranscript,
			       const CurationSettings &curationSettings)
{
	const bool noStrongClip = plan.value(QStringLiteral("no_strong_clip_found")).toBool(false);
	if (noStrongClip) {
		QString reason = jsonStringValue(plan, QStringLiteral("reason"));
		if (reason.isEmpty())
			reason = QStringLiteral(
				"No complete self-contained moment with a clear payoff was found in the selected range.");
		return QString::fromLatin1(SEMANTIC_GATE_FAILURE_PREFIX) + QLatin1Char(' ') + reason;
	}

	const QString arcType = jsonStringValue(plan, QStringLiteral("arc_type"));
	QString target = stripLeadingFindTarget(jsonStringValue(plan, QStringLiteral("main_target")));
	QString contextPhrase = cleanPlanPhrase(jsonStringValue(plan, QStringLiteral("context_phrase")));
	QString opening = cleanPlanPhrase(jsonStringValue(plan, QStringLiteral("opening_criteria")));
	QString development = cleanPlanPhrase(jsonStringValue(plan, QStringLiteral("development_criteria")));
	QString continuity = cleanPlanPhrase(jsonStringValue(plan, QStringLiteral("continuity_criteria")));
	QString ending = cleanPlanPhrase(jsonStringValue(plan, QStringLiteral("ending_criteria")));
	QString boundary = cleanPlanPhrase(jsonStringValue(plan, QStringLiteral("boundary_criteria")));

	if (target.isEmpty())
		target = deterministicPromptTarget(QString(), selectedRangeTranscript, curationSettings);
	if (opening.isEmpty())
		opening = QStringLiteral("start with enough local setup");
	if (development.isEmpty())
		development = QStringLiteral("follow the same continuous idea");
	if (continuity.isEmpty())
		continuity = QStringLiteral("minimal dead air and no unrelated topic switches");
	if (ending.isEmpty())
		ending = QStringLiteral("resolve the local point clearly");
	if (boundary.isEmpty())
		boundary = QStringLiteral("unfinished setup, list fragments, or transitions into a different topic");

	const QString planHint = trimmedJoinNonEmpty({arcType, target, contextPhrase, opening, development, continuity,
						      ending, boundary},
						     QStringLiteral(" "));
	const Curation::Intent intent = Curation::resolveIntent(curationSettings, selectedRangeTranscript, planHint);
	if (intent.resolvedPresetId == CurationPreset::viewerMessageResponsePresetId() &&
	    !Curation::transcriptHasReferenceBackedUnlockMethod(selectedRangeTranscript))
		return renderViewerMessagePlanPrompt(plan, selectedRangeTranscript, curationSettings, planHint);

	QString sentence1;
	QString sentence2;
	QString sentence3;

	if (Curation::transcriptHasReferenceBackedUnlockMethod(selectedRangeTranscript)) {
		const QStringList references = Curation::importantNamedReferences(selectedRangeTranscript);
		const QString reference = references.isEmpty() ? QStringLiteral("the named method")
							       : references.first();
		const QString referenceBackedTarget =
			deterministicPromptTarget(QString(), selectedRangeTranscript, curationSettings);
		if (!referenceBackedTarget.isEmpty())
			target = referenceBackedTarget;

		sentence1 = QStringLiteral("%1 the speaker %2 %3.")
				    .arg(scopeFindPhrase(selectedRangeTranscript, curationSettings, planHint),
					 arcVerbForType(arcType), target);
		sentence2 =
			QStringLiteral(
				"Prioritize moments that introduce %1 with the key/unlock metaphor before later pronouns, then show how one piece of language logic becomes easier to understand with continuous spoken development and minimal dead air.")
				.arg(reference);
		sentence3 = QStringLiteral(
			"Prefer clips that end after the local point is resolved, with clean natural boundaries and without unfinished setup, list items, long pauses, or mid-thought transitions.");
	} else {
		sentence1 = QStringLiteral("%1 the speaker %2 %3%4.")
				    .arg(scopeFindPhrase(selectedRangeTranscript, curationSettings, planHint),
					 arcVerbForType(arcType), target,
					 contextPhrase.isEmpty() ? QString() : QStringLiteral(" ") + contextPhrase);
		sentence2 = QStringLiteral("Prioritize moments that %1, then %2, keeping %3.")
				    .arg(opening, development, continuity);
		sentence3 = QStringLiteral("Prefer clips that %1, with clean natural boundaries and without %2.")
				    .arg(ending, boundary);
	}

	return QStringLiteral("%1 %2 %3").arg(sentence1, sentence2, sentence3).simplified();
}

QString normalizePlanOutput(const QString &rawOutput, const RecordingTranscript &selectedRangeTranscript,
			    const CurationSettings &curationSettings, const TemplateSectionLoader &templateLoader)
{
	const QString trimmedOutput = rawOutput.trimmed();
	if (trimmedOutput.isEmpty())
		return {};

	if (trimmedOutput.startsWith(QString::fromLatin1(SEMANTIC_GATE_FAILURE_PREFIX), Qt::CaseInsensitive)) {
		if (allowNoStrongFailure(selectedRangeTranscript, curationSettings))
			return trimmedOutput;

		return fallbackRubricOpusPrompt(curationSettings, templateLoader);
	}

	const QJsonObject plan = extractJsonObjectFromText(trimmedOutput);
	if (!plan.isEmpty()) {
		const QString renderedPrompt =
			renderPlanToOpusPrompt(plan, selectedRangeTranscript, curationSettings).trimmed();
		if (renderedPrompt.startsWith(QString::fromLatin1(SEMANTIC_GATE_FAILURE_PREFIX), Qt::CaseInsensitive) &&
		    !allowNoStrongFailure(selectedRangeTranscript, curationSettings))
			return fallbackRubricOpusPrompt(curationSettings, templateLoader);
		if (!hasEnglishOnlyPromptText(renderedPrompt))
			return englishOnlyPresetFallback(selectedRangeTranscript, curationSettings, renderedPrompt);
		return renderedPrompt;
	}

	const QString opusPrompt = lineValueForPrefix(trimmedOutput, OPUS_PROMPT_PREFIX);
	if (!opusPrompt.isEmpty())
		return hasEnglishOnlyPromptText(opusPrompt)
			       ? opusPrompt
			       : englishOnlyPresetFallback(selectedRangeTranscript, curationSettings, opusPrompt);

	return hasEnglishOnlyPromptText(trimmedOutput)
		       ? trimmedOutput
		       : englishOnlyPresetFallback(selectedRangeTranscript, curationSettings, trimmedOutput);
}

QString deterministicOpusPromptFallback(const QString &generatedPrompt,
					const RecordingTranscript &selectedRangeTranscript,
					const CurationSettings &curationSettings)
{
	const QString scope = Curation::scopeForDuration(
		Curation::selectedDurationSeconds(selectedRangeTranscript, curationSettings));
	const QString target = deterministicPromptTarget(generatedPrompt, selectedRangeTranscript, curationSettings);
	const QStringList namedReferences = Curation::importantNamedReferences(selectedRangeTranscript);
	const bool needsNamedReference =
		Curation::transcriptHasPronounDependentNamedReference(selectedRangeTranscript) &&
		!namedReferences.isEmpty();
	const bool hasReferenceBackedUnlockMethod =
		Curation::transcriptHasReferenceBackedUnlockMethod(selectedRangeTranscript);
	const Curation::Intent intent =
		Curation::resolveIntent(curationSettings, selectedRangeTranscript, generatedPrompt);
	const bool viewerExchange = intent.resolvedPresetId == CurationPreset::viewerMessageResponsePresetId() &&
				    !hasReferenceBackedUnlockMethod;

	QString sentence1;
	if (viewerExchange) {
		return OpusPromptRenderer::renderIntentPrompt(intent);
	} else if (Curation::shouldUseMultipleClips(intent) && scope == QStringLiteral("large_range_multiple_clips"))
		sentence1 = QStringLiteral("Find multiple strong self-contained clips where the speaker explains %1.")
				    .arg(target);
	else if (scope == QStringLiteral("short_range_best_moment"))
		sentence1 = QStringLiteral("Find the strongest self-contained moment where the speaker explains %1.")
				    .arg(target);
	else
		sentence1 =
			QStringLiteral("Find one clear self-contained clip where the speaker explains %1.").arg(target);

	QString sentence2;
	if (viewerExchange) {
		sentence2 = QStringLiteral(
			"Prioritize emotionally consequential and clearly useful exchanges over casual banter, starting with only enough setup from that message and following one continuous response.");
	} else if (hasReferenceBackedUnlockMethod) {
		sentence2 =
			QStringLiteral(
				"Prioritize moments that introduce %1 with the key/unlock metaphor before later pronouns, then show how one piece of language logic becomes easier to understand with continuous spoken development and minimal dead air.")
				.arg(namedReferences.first());
	} else if (needsNamedReference) {
		sentence2 =
			QStringLiteral(
				"Prioritize moments that introduce %1 or the target idea before later pronouns and indirect references, then develop one continuous local explanation with minimal dead air.")
				.arg(namedReferences.first());
	} else if (scope == QStringLiteral("short_range_best_moment")) {
		sentence2 = QStringLiteral(
			"Prioritize moments that introduce the idea with enough local context and complete one resolved part of the explanation with minimal dead air.");
	} else {
		sentence2 = QStringLiteral(
			"Prioritize moments that introduce the idea with enough local context and develop the same continuous explanation with minimal dead air.");
	}

	const QString sentence3 =
		viewerExchange
			? QStringLiteral(
				  "Choose clips that stop at the first resolved response; do not continue just to make the clip longer, and avoid clips that continue into the next viewer message, stream housekeeping, or another topic.")
			: QStringLiteral(
				  "Prefer clips that end after the local point is resolved, with clean natural boundaries and without unfinished setup, list items, long pauses, or mid-thought transitions.");

	return QStringLiteral("%1 %2 %3").arg(sentence1, sentence2, sentence3).trimmed();
}

QString applyResolvedPresetPromptGuard(const QString &prompt, const RecordingTranscript &selectedRangeTranscript,
				       const CurationSettings &curationSettings)
{
	if (isSemanticGateFailurePrompt(prompt))
		return prompt;

	if (!hasEnglishOnlyPromptText(prompt))
		return englishOnlyPresetFallback(selectedRangeTranscript, curationSettings, prompt);

	const Curation::Intent intent = Curation::resolveIntent(curationSettings, selectedRangeTranscript, prompt);
	if (intent.resolvedPresetId == CurationPreset::autoPresetId() ||
	    Curation::transcriptHasReferenceBackedUnlockMethod(selectedRangeTranscript))
		return prompt;

	const QString lowerPrompt = prompt.toLower();
	const bool genericLocalIdeaPrompt =
		lowerPrompt.contains(QStringLiteral("one clear local idea")) ||
		lowerPrompt.contains(QStringLiteral("local idea from setup to conclusion")) ||
		lowerPrompt.contains(QStringLiteral("local point resolves"));

	if (intent.resolvedPresetId == CurationPreset::viewerMessageResponsePresetId() &&
	    !CurationPreset::isViewerMessageResponsePrompt(prompt) && !isStructuredViewerMessagePrompt(prompt))
		return OpusPromptRenderer::renderIntentPrompt(intent);

	if (genericLocalIdeaPrompt && intent.resolvedPresetId != CurationPreset::autoPresetId())
		return OpusPromptRenderer::renderIntentPrompt(intent);

	return prompt;
}

} // namespace Curation
