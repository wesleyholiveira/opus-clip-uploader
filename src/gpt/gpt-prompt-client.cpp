#include "gpt/gpt-prompt-client.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QStringList>
#include <QMap>

#include <obs-module.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "gpt/gpt-prompt-store.hpp"
#include "utils/config.hpp"

static constexpr const char *OPENAI_CHAT_COMPLETIONS_URL = "https://api.openai.com/v1/chat/completions";
static constexpr const char *SEMANTIC_GATE_FAILURE_PREFIX = "NO_STRONG_CLIP_FOUND:";
static constexpr const char *OPUS_PROMPT_PREFIX = "OPUS_PROMPT:";
static constexpr const char *REPAIR_TEMPLATE_SECTION = "repair_opus_prompt";

static constexpr const char *CONFIG_GPT_INPUT_TEMPLATE_PREFIX = "gpt.input_text_template.v33.";
static constexpr const char *CONFIG_OPUS_SOURCE_LANGUAGE = "opus_source_lang";
static constexpr const char *CONFIG_GPT_PROMPT_REPAIR_MODE = "gpt.prompt_repair_mode";
static constexpr const char *CONFIG_GPT_TRANSCRIPT_CONTEXT_PADDING_SEC = "gpt_transcript_context_padding_sec";
static constexpr double DEFAULT_GPT_TRANSCRIPT_CONTEXT_PADDING_SEC = 60.0;
static constexpr const char *GPT_PROMPT_REPAIR_MODE_GPT = "gpt";
static constexpr int GPT_PROMPT_SEED = 12345;

static QString loadTemplateSectionFromRuntimeFile(const QString &sectionName, const QString &logLabel);

static double configuredGptTranscriptContextPaddingSeconds()
{
	bool ok = false;
	const double value = PluginConfig::getValue(QString::fromLatin1(CONFIG_GPT_TRANSCRIPT_CONTEXT_PADDING_SEC),
						    QString::number(DEFAULT_GPT_TRANSCRIPT_CONTEXT_PADDING_SEC, 'f', 0))
				     .toDouble(&ok);

	if (!ok || !std::isfinite(value))
		return DEFAULT_GPT_TRANSCRIPT_CONTEXT_PADDING_SEC;

	return std::clamp(value, 0.0, 600.0);
}

static QString lineValueForPrefix(const QString &text, const char *prefix)
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

static double selectedDurationSec(const RecordingTranscript &selectedRangeTranscript,
				  const CurationSettings &curationSettings)
{
	double duration = 0.0;

	for (const ClipDuration &range : curationSettings.clipDurations) {
		if (range.endSec > range.startSec)
			duration += range.endSec - range.startSec;
	}

	if (duration <= 0.0 && curationSettings.rangeEndSec > curationSettings.rangeStartSec)
		duration = curationSettings.rangeEndSec - curationSettings.rangeStartSec;

	if (duration <= 0.0 && !selectedRangeTranscript.segments.isEmpty()) {
		const double startSec = selectedRangeTranscript.segments.first().startSec;
		const double endSec = selectedRangeTranscript.segments.last().endSec;
		if (endSec > startSec)
			duration = endSec - startSec;
	}

	return duration;
}

static QString curationScopeForDuration(double durationSec)
{
	if (durationSec >= 2400.0)
		return QStringLiteral("large_range_multiple_clips");

	if (durationSec >= 600.0)
		return QStringLiteral("medium_range_one_or_more_clips");

	return QStringLiteral("short_range_best_moment");
}

static QString opusPromptPayload(const QString &prompt)
{
	const QString value = lineValueForPrefix(prompt, OPUS_PROMPT_PREFIX);
	if (!value.trimmed().isEmpty())
		return value.trimmed();

	return prompt.trimmed();
}

static int wordCount(const QString &text)
{
	return text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size();
}

static int substringCount(const QString &text, const QString &needle)
{
	if (needle.isEmpty())
		return 0;

	int count = 0;
	int index = 0;
	while ((index = text.indexOf(needle, index, Qt::CaseInsensitive)) >= 0) {
		++count;
		index += needle.size();
	}
	return count;
}

static bool containsAnyPhrase(const QString &text, const QStringList &phrases)
{
	for (const QString &phrase : phrases) {
		if (text.contains(phrase, Qt::CaseInsensitive))
			return true;
	}
	return false;
}

static void applyDeterministicSampling(QJsonObject &payload);
static QJsonObject buildChatCompletionsPayload(const QString &model, const QString &inputText, int maxTokens,
					       bool jsonMode = true);
static bool gptPromptRepairEnabled();
static QStringList importantNamedReferences(const RecordingTranscript &transcript);
static bool transcriptHasPronounDependentNamedReference(const RecordingTranscript &transcript);
static bool transcriptHasReferenceBackedUnlockMethod(const RecordingTranscript &transcript);
static bool promptContainsAnyNamedReference(const QString &prompt, const RecordingTranscript &transcript);
static QString deterministicPromptTarget(const QString &generatedPrompt,
					 const RecordingTranscript &selectedRangeTranscript,
					 const CurationSettings &curationSettings);
static QString deterministicOpusPromptFallback(const QString &generatedPrompt,
					       const RecordingTranscript &selectedRangeTranscript,
					       const CurationSettings &curationSettings);
static QString renderStructuredPlanToOpusPrompt(const QJsonObject &plan,
						const RecordingTranscript &selectedRangeTranscript,
						const CurationSettings &curationSettings);
static QString normalizeStructuredPlanOutput(const QString &rawOutput,
					     const RecordingTranscript &selectedRangeTranscript,
					     const CurationSettings &curationSettings);

static QStringList generatedPromptQualityIssues(const QString &prompt,
						const RecordingTranscript &selectedRangeTranscript,
						const CurationSettings &curationSettings)
{
	QStringList issues;
	const QString opusPrompt = opusPromptPayload(prompt);
	const QString normalized = opusPrompt.simplified();
	const QString lower = normalized.toLower();

	if (normalized.isEmpty()) {
		issues << QStringLiteral("empty prompt");
		return issues;
	}

	if (!normalized.startsWith(QStringLiteral("Find "), Qt::CaseInsensitive))
		issues << QStringLiteral("prompt does not start with Find");

	const int words = wordCount(normalized);
	if (words > 95)
		issues << QStringLiteral("too verbose for a predictable ClipAnything search prompt");

	if (substringCount(normalized, QStringLiteral(". ")) > 2)
		issues << QStringLiteral("does not follow the expected 3-sentence maximum");

	if (lower.contains(QStringLiteral("especially")))
		issues << QStringLiteral("uses especially to append adjacent topics");

	if (normalized.contains(QLatin1Char(':')))
		issues << QStringLiteral("uses a colon that may introduce a topic catalog");

	if (lower.contains(QStringLiteral("also look for")) || lower.contains(QStringLiteral("also find")))
		issues << QStringLiteral("adds a second search target");

	const QRegularExpression genderedSpeakerPattern(QStringLiteral("\\bspeaker\\b[^.]{0,80}\\b(his|her)\\b"),
							QRegularExpression::CaseInsensitiveOption);
	if (genderedSpeakerPattern.match(normalized).hasMatch())
		issues << QStringLiteral("assumes the speaker's gender");

	if (lower.contains(QStringLiteral("why it matters")) &&
	    (lower.contains(QStringLiteral("how it works")) || lower.contains(QStringLiteral("what the plan"))))
		issues << QStringLiteral("uses broad alternative objectives");

	if (substringCount(lower, QStringLiteral(" or ")) >= 2)
		issues << QStringLiteral("uses too many OR alternatives");

	if (substringCount(normalized, QStringLiteral(",")) >= 7)
		issues << QStringLiteral("looks like a topic catalog");

	const bool mentionsMethodOrRoutine = containsAnyPhrase(
		lower, {QStringLiteral("method"), QStringLiteral("routine"), QStringLiteral("daily loop"),
			QStringLiteral("framework"), QStringLiteral("process")});
	const bool mentionsRoadmap = containsAnyPhrase(
		lower, {QStringLiteral("roadmap"), QStringLiteral("8-week"), QStringLiteral("eight-week"),
			QStringLiteral("week-by-week"), QStringLiteral("progression")});
	const bool mentionsTools =
		containsAnyPhrase(lower, {QStringLiteral("tandem"), QStringLiteral("tool"), QStringLiteral("tools"),
					  QStringLiteral("artificial intelligence"), QStringLiteral("gpt")});
	const bool mentionsMotivation =
		containsAnyPhrase(lower, {QStringLiteral("motivation"), QStringLiteral("why they started"),
					  QStringLiteral("why the speaker started"),
					  QStringLiteral("why she is starting"), QStringLiteral("why he is starting"),
					  QStringLiteral("reason for learning"), QStringLiteral("reason for starting"),
					  QStringLiteral("starting this challenge"), QStringLiteral("challenge")});

	if (mentionsMethodOrRoutine && mentionsRoadmap)
		issues << QStringLiteral("mixes method/routine with roadmap/progression");

	if (mentionsMethodOrRoutine && mentionsTools && mentionsRoadmap)
		issues << QStringLiteral("mixes method, tools, and roadmap in one target");

	if (mentionsMethodOrRoutine && mentionsMotivation)
		issues << QStringLiteral("mixes motivation/challenge with method/routine");

	if (mentionsMethodOrRoutine && mentionsMotivation && mentionsRoadmap)
		issues << QStringLiteral("mixes motivation, method, and roadmap");

	const bool referenceBackedUnlockMethod = transcriptHasReferenceBackedUnlockMethod(selectedRangeTranscript);
	if (referenceBackedUnlockMethod &&
	    containsAnyPhrase(lower, {QStringLiteral("daily loop"), QStringLiteral("daily study loop"),
				      QStringLiteral("study routine"), QStringLiteral("input"),
				      QStringLiteral("spaced repetition"), QStringLiteral("vocabulary"),
				      QStringLiteral("practice"), QStringLiteral("natives"),
				      QStringLiteral("reason for learning"), QStringLiteral("starting this challenge"),
				      QStringLiteral("challenge"), QStringLiteral("roadmap"), QStringLiteral("8-week"),
				      QStringLiteral("plan")}))
		issues << QStringLiteral(
			"mixes reference-backed unlock method with routine, motivation, roadmap, tools, or practice");

	if (referenceBackedUnlockMethod &&
	    (lower.contains(QStringLiteral("making sure")) || lower.contains(QStringLiteral("make sure"))))
		issues << QStringLiteral("uses a meta-instruction instead of a ClipAnything search target");

	const QString scope = curationScopeForDuration(selectedDurationSec(selectedRangeTranscript, curationSettings));
	if (scope == QStringLiteral("short_range_best_moment") &&
	    (mentionsRoadmap || substringCount(lower, QStringLiteral(" and ")) >= 5))
		issues << QStringLiteral("too broad for the short clip preset");

	if (transcriptHasPronounDependentNamedReference(selectedRangeTranscript) &&
	    !promptContainsAnyNamedReference(normalized, selectedRangeTranscript))
		issues << QStringLiteral("drops a named reference needed to resolve pronouns or indirect references");

	if (lower.contains(QStringLiteral("duration")) || lower.contains(QStringLiteral("seconds")) ||
	    lower.contains(QStringLiteral("minutes")) || lower.contains(QStringLiteral("timestamps")))
		issues << QStringLiteral("mentions duration or timestamps");

	issues.removeDuplicates();
	return issues;
}

static bool shouldRepairGeneratedPrompt(const QString &prompt, const RecordingTranscript &selectedRangeTranscript,
					const CurationSettings &curationSettings, QStringList *issues = nullptr)
{
	const QStringList detectedIssues =
		generatedPromptQualityIssues(prompt, selectedRangeTranscript, curationSettings);
	if (issues)
		*issues = detectedIssues;
	return !detectedIssues.isEmpty();
}

static int transcriptTextCharCount(const RecordingTranscript &transcript)
{
	int chars = 0;
	for (const TranscriptSegment &segment : transcript.segments)
		chars += segment.text.trimmed().size();
	return chars;
}

static bool allowNoStrongFailure(const RecordingTranscript &selectedRangeTranscript,
				 const CurationSettings &curationSettings)
{
	const double durationSec = selectedDurationSec(selectedRangeTranscript, curationSettings);
	const int segmentCount = selectedRangeTranscript.segments.size();
	const int textChars = transcriptTextCharCount(selectedRangeTranscript);

	return durationSec < 90.0 || segmentCount < 8 || textChars < 400;
}

static QString renderFallbackRubricTemplate(QString templateText, const CurationSettings &curationSettings)
{
	QString topic = curationSettings.topicKeywords.join(QStringLiteral(", ")).trimmed();
	if (topic.isEmpty())
		topic = QStringLiteral("the strongest topics in the selected range");

	templateText.replace(QStringLiteral("{{topic_keywords}}"), topic);
	return templateText.trimmed();
}

static QString fallbackRubricOpusPrompt(const CurationSettings &curationSettings)
{
	const QString runtimeTemplate = loadTemplateSectionFromRuntimeFile(QStringLiteral("fallback_opus_prompt"),
									   QStringLiteral("GPT fallback rubric"));
	if (!runtimeTemplate.trimmed().isEmpty())
		return renderFallbackRubricTemplate(runtimeTemplate, curationSettings);

	QString topic = curationSettings.topicKeywords.join(QStringLiteral(", ")).trimmed();
	if (topic.isEmpty())
		topic = QStringLiteral("the strongest topics in the selected range");

	return QStringLiteral(
		       "Find self-contained clips within the selected range from one dominant sub-arc about %1. "
		       "Prefer complete arcs with a natural start, focused development, clear payoff, and natural ending. "
		       "Avoid isolated fragments, housekeeping, timestamps, visual effects, and duration requests.")
		.arg(topic);
}

static QString jsonStringValue(const QJsonObject &object, const QString &key)
{
	return object.value(key).toString().simplified();
}

static QString cleanPlanPhrase(QString phrase)
{
	phrase = phrase.simplified();
	phrase.remove(
		QRegularExpression(QStringLiteral("^OPUS_PROMPT\\s*:\\s*"), QRegularExpression::CaseInsensitiveOption));
	phrase.remove(QRegularExpression(QStringLiteral("^[\\-–—•]+\\s*")));
	while (phrase.endsWith(QLatin1Char('.')) || phrase.endsWith(QLatin1Char(';')) ||
	       phrase.endsWith(QLatin1Char(':')))
		phrase.chop(1);
	return phrase.trimmed();
}

static QString stripLeadingFindTarget(QString target)
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

static QString arcVerbForType(QString arcType)
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

static QString scopeFindPhrase(const RecordingTranscript &selectedRangeTranscript,
			       const CurationSettings &curationSettings)
{
	const QString scope = curationScopeForDuration(selectedDurationSec(selectedRangeTranscript, curationSettings));
	if (scope == QStringLiteral("large_range_multiple_clips"))
		return QStringLiteral("Find multiple strong self-contained clips where");
	if (scope == QStringLiteral("short_range_best_moment"))
		return QStringLiteral("Find the strongest self-contained moment where");
	return QStringLiteral("Find one or more strong self-contained clips where");
}

static QJsonObject extractJsonObjectFromText(const QString &rawOutput)
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

static QString renderStructuredPlanToOpusPrompt(const QJsonObject &plan,
						const RecordingTranscript &selectedRangeTranscript,
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
	QString ending = cleanPlanPhrase(jsonStringValue(plan, QStringLiteral("ending_criteria")));
	QString boundary = cleanPlanPhrase(jsonStringValue(plan, QStringLiteral("boundary_criteria")));

	if (target.isEmpty())
		target = deterministicPromptTarget(QString(), selectedRangeTranscript, curationSettings);
	if (opening.isEmpty())
		opening = QStringLiteral(
			"introduce the idea with enough local context for the first sentence to stand alone");
	if (development.isEmpty())
		development = QStringLiteral("develop one continuous local explanation or answer");
	if (ending.isEmpty())
		ending = QStringLiteral("end after the local point is resolved");
	if (boundary.isEmpty())
		boundary = QStringLiteral("unfinished setup, list items, or mid-thought transitions");

	if (!contextPhrase.isEmpty() && !contextPhrase.startsWith(QStringLiteral("with"), Qt::CaseInsensitive) &&
	    !contextPhrase.startsWith(QStringLiteral("through"), Qt::CaseInsensitive) &&
	    !contextPhrase.startsWith(QStringLiteral("using"), Qt::CaseInsensitive) &&
	    !contextPhrase.startsWith(QStringLiteral("from"), Qt::CaseInsensitive) &&
	    !contextPhrase.startsWith(QStringLiteral("about"), Qt::CaseInsensitive))
		contextPhrase = QStringLiteral("with context from %1").arg(contextPhrase);

	const QString sentence1 =
		QStringLiteral("%1 the speaker %2 %3%4.")
			.arg(scopeFindPhrase(selectedRangeTranscript, curationSettings), arcVerbForType(arcType),
			     target, contextPhrase.isEmpty() ? QString() : QStringLiteral(" ") + contextPhrase);
	const QString sentence2 = QStringLiteral("Prioritize moments that %1, then %2.").arg(opening, development);
	const QString sentence3 = QStringLiteral("Prefer clips that %1, with clean natural boundaries and without %2.")
					  .arg(ending, boundary);

	return QStringLiteral("%1 %2 %3").arg(sentence1, sentence2, sentence3).simplified();
}

static QString normalizeStructuredPlanOutput(const QString &rawOutput,
					     const RecordingTranscript &selectedRangeTranscript,
					     const CurationSettings &curationSettings)
{
	const QString trimmedOutput = rawOutput.trimmed();
	if (trimmedOutput.isEmpty())
		return {};

	if (trimmedOutput.startsWith(QString::fromLatin1(SEMANTIC_GATE_FAILURE_PREFIX), Qt::CaseInsensitive)) {
		if (allowNoStrongFailure(selectedRangeTranscript, curationSettings))
			return trimmedOutput;

		blog(LOG_INFO,
		     "Ignoring GPT semantic gate failure for non-trivial selected range. selectedSegments=%d selectedDurationSec=%.3f reason=%s",
		     static_cast<int>(selectedRangeTranscript.segments.size()),
		     selectedDurationSec(selectedRangeTranscript, curationSettings),
		     trimmedOutput.toUtf8().constData());
		return fallbackRubricOpusPrompt(curationSettings);
	}

	const QJsonObject plan = extractJsonObjectFromText(trimmedOutput);
	if (!plan.isEmpty()) {
		const QString renderedPrompt =
			renderStructuredPlanToOpusPrompt(plan, selectedRangeTranscript, curationSettings).trimmed();
		if (renderedPrompt.startsWith(QString::fromLatin1(SEMANTIC_GATE_FAILURE_PREFIX), Qt::CaseInsensitive) &&
		    !allowNoStrongFailure(selectedRangeTranscript, curationSettings)) {
			blog(LOG_INFO,
			     "Ignoring GPT semantic gate failure for non-trivial structured plan. selectedSegments=%d selectedDurationSec=%.3f reason=%s",
			     static_cast<int>(selectedRangeTranscript.segments.size()),
			     selectedDurationSec(selectedRangeTranscript, curationSettings),
			     renderedPrompt.toUtf8().constData());
			return fallbackRubricOpusPrompt(curationSettings);
		}
		return renderedPrompt;
	}

	const QString opusPrompt = lineValueForPrefix(trimmedOutput, OPUS_PROMPT_PREFIX);
	if (!opusPrompt.isEmpty())
		return opusPrompt;

	return trimmedOutput;
}

static QString normalizeGeneratedOutput(const QString &rawOutput, const RecordingTranscript &selectedRangeTranscript,
					const CurationSettings &curationSettings)
{
	return normalizeStructuredPlanOutput(rawOutput, selectedRangeTranscript, curationSettings);
}

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

static QString promptSourceLanguageForCuration(const CurationSettings &curationSettings)
{
	QString sourceLanguage = curationSettings.sourceLanguage.trimmed();
	if (sourceLanguage.isEmpty())
		sourceLanguage = PluginConfig::getValue(QString::fromLatin1(CONFIG_OPUS_SOURCE_LANGUAGE),
							QStringLiteral("auto"));

	return sourceLanguage.trimmed().isEmpty() ? QStringLiteral("auto") : sourceLanguage;
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

static void applyDeterministicSampling(QJsonObject &payload)
{
	payload.insert(QStringLiteral("temperature"), 0.0);
	payload.insert(QStringLiteral("top_p"), 1.0);
	payload.insert(QStringLiteral("seed"), GPT_PROMPT_SEED);
}

static QJsonObject buildChatCompletionsPayload(const QString &model, const QString &inputText, int maxTokens,
					       bool jsonMode)
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

static bool gptPromptRepairEnabled()
{
	const QString mode =
		PluginConfig::getValue(QString::fromLatin1(CONFIG_GPT_PROMPT_REPAIR_MODE), QStringLiteral("off"))
			.trimmed()
			.toLower();
	return mode == QString::fromLatin1(GPT_PROMPT_REPAIR_MODE_GPT);
}

static QStringList namedReferenceWords(const QString &reference)
{
	return reference.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

static bool sameReferenceWord(const QString &a, const QString &b)
{
	return a.compare(b, Qt::CaseInsensitive) == 0;
}

static QString mergedOverlappingReference(const QString &a, const QString &b)
{
	const QStringList aWords = namedReferenceWords(a);
	const QStringList bWords = namedReferenceWords(b);
	if (aWords.isEmpty() || bWords.isEmpty())
		return {};

	const int maxOverlap = std::min(aWords.size(), bWords.size());
	for (int overlap = maxOverlap; overlap >= 1; --overlap) {
		bool aSuffixMatchesBPrefix = true;
		for (int index = 0; index < overlap; ++index) {
			if (!sameReferenceWord(aWords.at(aWords.size() - overlap + index), bWords.at(index))) {
				aSuffixMatchesBPrefix = false;
				break;
			}
		}

		if (aSuffixMatchesBPrefix) {
			QStringList merged = aWords;
			for (int index = overlap; index < bWords.size(); ++index)
				merged.append(bWords.at(index));
			return merged.join(QLatin1Char(' ')).simplified();
		}

		bool bSuffixMatchesAPrefix = true;
		for (int index = 0; index < overlap; ++index) {
			if (!sameReferenceWord(bWords.at(bWords.size() - overlap + index), aWords.at(index))) {
				bSuffixMatchesAPrefix = false;
				break;
			}
		}

		if (bSuffixMatchesAPrefix) {
			QStringList merged = bWords;
			for (int index = overlap; index < aWords.size(); ++index)
				merged.append(aWords.at(index));
			return merged.join(QLatin1Char(' ')).simplified();
		}
	}

	return {};
}

static void addOverlappingNamedReferences(QMap<QString, int> &counts)
{
	bool changed = true;
	while (changed) {
		changed = false;
		const QStringList references = counts.keys();

		for (int i = 0; i < references.size(); ++i) {
			for (int j = i + 1; j < references.size(); ++j) {
				const QString merged = mergedOverlappingReference(references.at(i), references.at(j));
				if (merged.isEmpty())
					continue;

				const int mergedWordCount = namedReferenceWords(merged).size();
				const int currentMaxWordCount = std::max(namedReferenceWords(references.at(i)).size(),
									 namedReferenceWords(references.at(j)).size());
				if (mergedWordCount <= currentMaxWordCount || mergedWordCount > 6)
					continue;

				const int mergedCount = counts.value(references.at(i)) + counts.value(references.at(j));
				if (counts.value(merged) < mergedCount) {
					counts[merged] = mergedCount;
					changed = true;
				}
			}
		}
	}
}

static QStringList importantNamedReferences(const RecordingTranscript &transcript)
{
	QMap<QString, int> counts;
	static const QSet<QString> ignored = {QStringLiteral("The"),      QStringLiteral("This"),
					      QStringLiteral("That"),     QStringLiteral("Then"),
					      QStringLiteral("And"),      QStringLiteral("But"),
					      QStringLiteral("So"),       QStringLiteral("If"),
					      QStringLiteral("When"),     QStringLiteral("For"),
					      QStringLiteral("Week"),     QStringLiteral("German"),
					      QStringLiteral("Japanese"), QStringLiteral("Brazilian"),
					      QStringLiteral("Brazil"),   QStringLiteral("UTC")};
	static const QSet<QString> ignoredLowerPrefixes = {
		QStringLiteral("and"),     QStringLiteral("but"),    QStringLiteral("the"),
		QStringLiteral("this"),    QStringLiteral("that"),   QStringLiteral("then"),
		QStringLiteral("when"),    QStringLiteral("with"),   QStringLiteral("from"),
		QStringLiteral("through"), QStringLiteral("using"),  QStringLiteral("like"),
		QStringLiteral("about"),   QStringLiteral("after"),  QStringLiteral("before"),
		QStringLiteral("into"),    QStringLiteral("only"),   QStringLiteral("also"),
		QStringLiteral("real"),    QStringLiteral("first"),  QStringLiteral("second"),
		QStringLiteral("third"),   QStringLiteral("week"),   QStringLiteral("what"),
		QStringLiteral("where"),   QStringLiteral("which"),  QStringLiteral("while"),
		QStringLiteral("because"), QStringLiteral("there"),  QStringLiteral("their"),
		QStringLiteral("your"),    QStringLiteral("some"),   QStringLiteral("many"),
		QStringLiteral("very"),    QStringLiteral("just"),   QStringLiteral("kind"),
		QStringLiteral("sort"),    QStringLiteral("will"),   QStringLiteral("would"),
		QStringLiteral("could"),   QStringLiteral("should"), QStringLiteral("have"),
		QStringLiteral("been"),    QStringLiteral("were"),   QStringLiteral("they"),
		QStringLiteral("them"),    QStringLiteral("than"),   QStringLiteral("how"),
		QStringLiteral("why"),     QStringLiteral("who"),    QStringLiteral("whom"),
		QStringLiteral("whose")};

	const QRegularExpression namedPhrasePattern(
		QStringLiteral("\\b([A-Z][\\p{L}0-9]*(?:\\s+[A-Z][\\p{L}0-9]*){1,3})\\b"),
		QRegularExpression::UseUnicodePropertiesOption);
	const QRegularExpression prefixedNamedPhrasePattern(
		QStringLiteral("\\b([a-z][\\p{L}0-9]{2,})\\s+([A-Z][\\p{L}0-9]*(?:\\s+[A-Z][\\p{L}0-9]*){1,3})\\b"),
		QRegularExpression::UseUnicodePropertiesOption);

	for (const TranscriptSegment &segment : transcript.segments) {
		const QString text = segment.text.trimmed();
		if (text.isEmpty())
			continue;

		QRegularExpressionMatchIterator prefixedIterator = prefixedNamedPhrasePattern.globalMatch(text);
		while (prefixedIterator.hasNext()) {
			const QRegularExpressionMatch match = prefixedIterator.next();
			const QString prefix = match.captured(1).simplified();
			QString reference = match.captured(2).simplified();
			if (prefix.isEmpty() || reference.isEmpty())
				continue;

			if (ignoredLowerPrefixes.contains(prefix.toLower()))
				continue;

			const QString firstReferenceWord = reference.section(QLatin1Char(' '), 0, 0).trimmed();
			if (ignored.contains(reference) || ignored.contains(firstReferenceWord))
				continue;

			QString normalizedPrefix = prefix.toLower();
			normalizedPrefix.replace(0, 1, normalizedPrefix.left(1).toUpper());
			reference = QStringList{normalizedPrefix, reference}.join(QLatin1Char(' ')).simplified();
			counts[reference] += 2;
		}

		QRegularExpressionMatchIterator iterator = namedPhrasePattern.globalMatch(text);
		while (iterator.hasNext()) {
			QString reference = iterator.next().captured(1).simplified();
			if (reference.isEmpty())
				continue;

			const QString firstWord = reference.section(QLatin1Char(' '), 0, 0).trimmed();
			if (ignored.contains(reference) || ignored.contains(firstWord))
				continue;

			counts[reference] += 1;
		}
	}

	addOverlappingNamedReferences(counts);

	QStringList references = counts.keys();
	std::sort(references.begin(), references.end(), [&counts](const QString &a, const QString &b) {
		const int wordCountA = namedReferenceWords(a).size();
		const int wordCountB = namedReferenceWords(b).size();
		if (wordCountA != wordCountB)
			return wordCountA > wordCountB;

		if (a.size() != b.size())
			return a.size() > b.size();

		const int countA = counts.value(a);
		const int countB = counts.value(b);
		if (countA != countB)
			return countA > countB;

		return a.localeAwareCompare(b) < 0;
	});

	QStringList output;
	for (const QString &reference : references) {
		bool coveredByLongerReference = false;
		for (const QString &existing : output) {
			if (existing.contains(reference, Qt::CaseInsensitive) ||
			    reference.contains(existing, Qt::CaseInsensitive)) {
				coveredByLongerReference = true;
				break;
			}
		}

		if (!coveredByLongerReference)
			output.append(reference);
	}

	std::sort(output.begin(), output.end(), [&counts](const QString &a, const QString &b) {
		const int wordCountA = namedReferenceWords(a).size();
		const int wordCountB = namedReferenceWords(b).size();
		if (wordCountA != wordCountB)
			return wordCountA > wordCountB;

		if (a.size() != b.size())
			return a.size() > b.size();

		const int countA = counts.value(a);
		const int countB = counts.value(b);
		if (countA != countB)
			return countA > countB;

		return a.localeAwareCompare(b) < 0;
	});

	if (output.size() > 6)
		output = output.mid(0, 6);

	return output;
}

static QString formatImportantNamedReferences(const RecordingTranscript &transcript)
{
	const QStringList references = importantNamedReferences(transcript);
	if (references.isEmpty())
		return QStringLiteral("No strong named reference detected in the selected range.");

	return references.join(QStringLiteral(", "));
}

static bool transcriptHasPronounDependentNamedReference(const RecordingTranscript &transcript)
{
	if (importantNamedReferences(transcript).isEmpty())
		return false;

	QString text;
	for (const TranscriptSegment &segment : transcript.segments) {
		if (!segment.text.trimmed().isEmpty()) {
			if (!text.isEmpty())
				text += QLatin1Char(' ');
			text += segment.text.trimmed();
		}
	}
	text = text.toLower();
	return containsAnyPhrase(text, {QStringLiteral("the way she"), QStringLiteral("the way he"),
					QStringLiteral("the way they"), QStringLiteral("she teaches"),
					QStringLiteral("he teaches"), QStringLiteral("they teach"),
					QStringLiteral("this method"), QStringLiteral("that approach"),
					QStringLiteral("the way it works")});
}

static bool transcriptHasReferenceBackedUnlockMethod(const RecordingTranscript &transcript)
{
	if (importantNamedReferences(transcript).isEmpty())
		return false;

	QString text;
	for (const TranscriptSegment &segment : transcript.segments) {
		if (!segment.text.trimmed().isEmpty()) {
			if (!text.isEmpty())
				text += QLatin1Char(' ');
			text += segment.text.trimmed();
		}
	}

	const QString lower = text.toLower();
	const bool unlockMethod = containsAnyPhrase(lower, {QStringLiteral("unlock"), QStringLiteral("key clicks"),
							    QStringLiteral("one key"), QStringLiteral("one logic"),
							    QStringLiteral("core logic")});
	if (!unlockMethod)
		return false;

	const bool methodReferenceSignal = containsAnyPhrase(
		lower, {QStringLiteral("method"), QStringLiteral("approach"), QStringLiteral("framework"),
			QStringLiteral("style"), QStringLiteral("language-learning"), QStringLiteral("the way"),
			QStringLiteral("teaches"), QStringLiteral("teach")});

	return methodReferenceSignal && (transcriptHasPronounDependentNamedReference(transcript) ||
					 !importantNamedReferences(transcript).isEmpty());
}

static bool promptContainsAnyNamedReference(const QString &prompt, const RecordingTranscript &transcript)
{
	const QString lowerPrompt = prompt.toLower();
	for (const QString &reference : importantNamedReferences(transcript)) {
		const QString lowerReference = reference.toLower();
		if (lowerPrompt.contains(lowerReference))
			return true;

		const QStringList parts = reference.split(QLatin1Char(' '), Qt::SkipEmptyParts);
		if (parts.size() >= 2) {
			const QString shortReference =
				QStringList{parts.at(0), parts.at(1)}.join(QStringLiteral(" ")).toLower();
			if (lowerPrompt.contains(shortReference))
				return true;
		}
	}

	return false;
}

static QString firstCleanTopicKeyword(const CurationSettings &curationSettings)
{
	for (QString keyword : curationSettings.topicKeywords) {
		keyword = keyword.simplified();
		if (keyword.isEmpty())
			continue;

		keyword.replace(QRegularExpression(QStringLiteral("[\r\n\t]+")), QStringLiteral(" "));
		keyword = keyword.section(QLatin1Char(','), 0, 0).section(QStringLiteral(" and "), 0, 0).trimmed();
		if (!keyword.isEmpty())
			return keyword.left(90).trimmed();
	}

	return {};
}

static QString deterministicPromptTarget(const QString &generatedPrompt,
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

static QString deterministicOpusPromptFallback(const QString &generatedPrompt,
					       const RecordingTranscript &selectedRangeTranscript,
					       const CurationSettings &curationSettings)
{
	const QString scope = curationScopeForDuration(selectedDurationSec(selectedRangeTranscript, curationSettings));
	const QString target = deterministicPromptTarget(generatedPrompt, selectedRangeTranscript, curationSettings);
	const QStringList namedReferences = importantNamedReferences(selectedRangeTranscript);
	const bool needsNamedReference = transcriptHasPronounDependentNamedReference(selectedRangeTranscript) &&
					 !namedReferences.isEmpty();
	const bool hasReferenceBackedUnlockMethod = transcriptHasReferenceBackedUnlockMethod(selectedRangeTranscript);

	QString sentence1;
	if (scope == QStringLiteral("large_range_multiple_clips"))
		sentence1 = QStringLiteral("Find multiple strong self-contained clips where the speaker explains %1.")
				    .arg(target);
	else if (scope == QStringLiteral("short_range_best_moment"))
		sentence1 = QStringLiteral("Find the strongest self-contained moment where the speaker explains %1.")
				    .arg(target);
	else
		sentence1 =
			QStringLiteral("Find one clear self-contained clip where the speaker explains %1.").arg(target);

	QString sentence2;
	if (hasReferenceBackedUnlockMethod) {
		sentence2 =
			QStringLiteral(
				"Prioritize moments that introduce %1 or the key/unlock metaphor before later pronouns, then show how one piece of language logic becomes easier to understand.")
				.arg(namedReferences.first());
	} else if (needsNamedReference) {
		sentence2 =
			QStringLiteral(
				"Prioritize moments that introduce %1 or the target idea before later pronouns and indirect references, then develop one continuous local explanation.")
				.arg(namedReferences.first());
	} else if (scope == QStringLiteral("short_range_best_moment")) {
		sentence2 = QStringLiteral(
			"Prioritize moments that introduce the idea with enough local context and complete one resolved part of the explanation.");
	} else {
		sentence2 = QStringLiteral(
			"Prioritize moments that introduce the idea with enough local context and develop the same continuous explanation.");
	}

	const QString sentence3 = QStringLiteral(
		"Prefer clips that end after the local point is resolved, with clean natural boundaries and without unfinished setup, list items, and mid-thought transitions.");

	return QStringLiteral("%1 %2 %3").arg(sentence1, sentence2, sentence3).trimmed();
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

static bool transcriptSegmentIntersectsRange(const TranscriptSegment &segment, double startSec, double endSec)
{
	return segment.endSec > startSec && segment.startSec < endSec;
}

static QString formatExpandedContextTranscript(const RecordingTranscript &transcript,
					       const CurationSettings &curationSettings)
{
	const double contextPaddingSec = configuredGptTranscriptContextPaddingSeconds();
	constexpr int maxContextChars = 60000;
	QString text;
	const QVector<ClipDuration> ranges = [&curationSettings]() {
		QVector<ClipDuration> result;
		for (const ClipDuration &range : curationSettings.clipDurations) {
			if (range.endSec > range.startSec)
				result.append(range);
		}
		if (result.isEmpty() && curationSettings.rangeEndSec > curationSettings.rangeStartSec)
			result.append({curationSettings.rangeStartSec, curationSettings.rangeEndSec});
		return result;
	}();

	for (const TranscriptSegment &segment : transcript.segments) {
		if (segment.text.trimmed().isEmpty())
			continue;

		bool includeSegment = ranges.isEmpty();
		for (const ClipDuration &range : ranges) {
			const double expandedStartSec = std::max(0.0, range.startSec - contextPaddingSec);
			const double expandedEndSec = range.endSec + contextPaddingSec;
			if (transcriptSegmentIntersectsRange(segment, expandedStartSec, expandedEndSec)) {
				includeSegment = true;
				break;
			}
		}

		if (!includeSegment)
			continue;

		text += QStringLiteral("[%1s-%2s] %3\n")
				.arg(segment.startSec, 0, 'f', 2)
				.arg(segment.endSec, 0, 'f', 2)
				.arg(segment.text.trimmed());

		if (text.size() >= maxContextChars) {
			text += QStringLiteral("\n[context truncated to keep the GPT request focused]\n");
			break;
		}
	}

	return text.trimmed();
}

static QString appendPreviewText(QString currentText, const QString &segmentText, int maxChars)
{
	const QString trimmed = segmentText.trimmed();
	if (trimmed.isEmpty() || currentText.size() >= maxChars)
		return currentText;

	if (!currentText.isEmpty())
		currentText += QLatin1Char(' ');

	const qsizetype remaining = static_cast<qsizetype>(maxChars) - currentText.size();
	if (trimmed.size() <= remaining)
		currentText += trimmed;
	else
		currentText += trimmed.left(std::max<qsizetype>(0, remaining)).trimmed() + QStringLiteral("...");

	return currentText;
}

static QString formatSemanticTimelineContext(const RecordingTranscript &transcript)
{
	if (transcript.segments.isEmpty())
		return QStringLiteral("No full-transcript timeline context available.");

	constexpr double windowSizeSec = 300.0;
	constexpr int maxWindowPreviewChars = 520;
	QString timeline;
	double windowStartSec =
		std::max(0.0, std::floor(transcript.segments.first().startSec / windowSizeSec) * windowSizeSec);
	double windowEndSec = windowStartSec + windowSizeSec;
	QString windowText;

	auto flushWindow = [&timeline, &windowText, &windowStartSec, &windowEndSec]() {
		const QString preview = windowText.trimmed();
		if (!preview.isEmpty()) {
			timeline += QStringLiteral("[%1s-%2s] %3\n")
					    .arg(windowStartSec, 0, 'f', 0)
					    .arg(windowEndSec, 0, 'f', 0)
					    .arg(preview);
		}
		windowText.clear();
	};

	for (const TranscriptSegment &segment : transcript.segments) {
		if (segment.text.trimmed().isEmpty())
			continue;

		while (segment.startSec >= windowEndSec) {
			flushWindow();
			windowStartSec = windowEndSec;
			windowEndSec += windowSizeSec;
		}

		windowText = appendPreviewText(windowText, segment.text, maxWindowPreviewChars);
	}

	flushWindow();
	return timeline.trimmed();
}

static QString renderInputTemplate(QString templateText, const QString &videoPath,
				   const RecordingTranscript &fullTranscript,
				   const RecordingTranscript &selectedRangeTranscript,
				   const CurationSettings &curationSettings, const QString &sourceLanguage)
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
	templateText.replace(QStringLiteral("{{genre}}"), curationSettings.genre.trimmed().isEmpty()
								  ? QStringLiteral("Auto")
								  : curationSettings.genre.trimmed());
	templateText.replace(QStringLiteral("{{named_references}}"),
			     formatImportantNamedReferences(selectedRangeTranscript));
	templateText.replace(QStringLiteral("{{source_language}}"), normalizePromptLanguage(sourceLanguage));
	templateText.replace(QStringLiteral("{{clip_length_preset}}"), curationSettings.clipLengthPreset.trimmed());
	const double durationSec = selectedDurationSec(selectedRangeTranscript, curationSettings);
	templateText.replace(QStringLiteral("{{selected_duration_sec}}"), QString::number(durationSec, 'f', 3));
	templateText.replace(QStringLiteral("{{selected_duration_min}}"), QString::number(durationSec / 60.0, 'f', 2));
	templateText.replace(QStringLiteral("{{curation_scope}}"), curationScopeForDuration(durationSec));
	templateText.replace(QStringLiteral("{{semantic_timeline}}"), formatSemanticTimelineContext(fullTranscript));
	templateText.replace(QStringLiteral("{{context_transcript}}"),
			     formatExpandedContextTranscript(fullTranscript, curationSettings));
	templateText.replace(QStringLiteral("{{selected_range_transcript}}"),
			     formatTranscript(selectedRangeTranscript));
	templateText.replace(QStringLiteral("{{transcript}}"), formatTranscript(selectedRangeTranscript));

	return templateText.trimmed();
}

static QString fallbackRepairInputTextTemplate()
{
	return QStringLiteral(
		"You are repairing an Opus Clip ClipAnything prompt that violated predictability rules.\n"
		"Return only one line: OPUS_PROMPT: <repaired English prompt>\n"
		"Rewrite the prompt into exactly 3 concise sentences.\n"
		"Sentence 1 must start with Find and describe exactly one search target.\n"
		"Sentence 2 must start with Prioritize moments that and describe the opening plus the same continuous sequence.\n"
		"Sentence 3 must start with Prefer clips that and describe the ending/resolution plus one boundary criterion.\n"
		"Do not use especially, a colon topic catalog, broad OR chains, duration, timestamps, editing instructions, or more than one adjacent sub-arc.\n"
		"Do not assume the speaker\'s gender; use the speaker instead of his, her, he, or she unless the transcript itself makes it necessary.\n"
		"For short clip presets, prefer one smaller resolved local idea over a method + roadmap + tools + motivation bundle.\n"
		"If the selected range contains an important named person, creator, source, app, book, or method, preserve that name when it is needed to make pronouns or indirect references understandable.\n"
		"If a named reference is central to an unlock/method explanation, make the repaired target only that reference-backed method explanation and do not include daily routine, motivation, roadmap, tools, input, vocabulary, or practice unless it is directly inside the same reference-backed explanation.\n"
		"Do not write meta-instructions such as making sure a name is included; write the search target naturally.\n\n"
		"Detected issues: {{issues}}\n"
		"Curation scope: {{curation_scope}}\n"
		"Configured clip length preset: {{clip_length_preset}}\n"
		"User-selected ranges/markers:\n{{selected_ranges}}\n\n"
		"Desired keywords/topics from the user:\n{{topic_keywords}}\n\n"
		"Important named references detected in the selected range:\n{{named_references}}\n\n"
		"Previous OPUS_PROMPT:\n{{generated_opus_prompt}}\n\n"
		"Selected-range transcript:\n{{selected_range_transcript}}\n");
}

static QString renderRepairTemplate(QString templateText, const QString &generatedPrompt, const QStringList &issues,
				    const RecordingTranscript &selectedRangeTranscript,
				    const CurationSettings &curationSettings, const QString &sourceLanguage)
{
	const double durationSec = selectedDurationSec(selectedRangeTranscript, curationSettings);
	templateText.replace(QStringLiteral("{{generated_opus_prompt}}"), opusPromptPayload(generatedPrompt));
	templateText.replace(QStringLiteral("{{issues}}"), issues.join(QStringLiteral("; ")));
	templateText.replace(QStringLiteral("{{curation_scope}}"), curationScopeForDuration(durationSec));
	templateText.replace(QStringLiteral("{{clip_length_preset}}"), curationSettings.clipLengthPreset.trimmed());
	templateText.replace(QStringLiteral("{{selected_ranges}}"),
			     formatSelectedRanges(curationSettings, sourceLanguage));
	templateText.replace(QStringLiteral("{{topic_keywords}}"),
			     formatTopicKeywords(curationSettings, sourceLanguage));
	templateText.replace(QStringLiteral("{{named_references}}"),
			     formatImportantNamedReferences(selectedRangeTranscript));
	templateText.replace(QStringLiteral("{{selected_range_transcript}}"),
			     formatTranscript(selectedRangeTranscript));
	return templateText.trimmed();
}

static QString buildRepairInputText(const QString &generatedPrompt, const QStringList &issues,
				    const RecordingTranscript &selectedRangeTranscript,
				    const CurationSettings &curationSettings, const QString &sourceLanguage)
{
	QString repairTemplate = loadTemplateSectionFromRuntimeFile(QString::fromLatin1(REPAIR_TEMPLATE_SECTION),
								    QStringLiteral("GPT repair template"));
	if (repairTemplate.trimmed().isEmpty())
		repairTemplate = fallbackRepairInputTextTemplate();

	return renderRepairTemplate(repairTemplate, generatedPrompt, issues, selectedRangeTranscript, curationSettings,
				    sourceLanguage);
}

static QString normalizePromptTemplateLineEndings(QString text)
{
	text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
	text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
	return text.trimmed();
}

static QString promptTemplateSection(const QString &fileText, const QString &sectionName)
{
	QStringList lines = normalizePromptTemplateLineEndings(fileText).split(QLatin1Char('\n'));
	QString currentSection;
	QStringList sectionLines;

	for (const QString &line : lines) {
		const QString trimmed = line.trimmed();
		if (trimmed.startsWith(QLatin1Char('[')) && trimmed.endsWith(QLatin1Char(']'))) {
			const QString nextSection = trimmed.mid(1, trimmed.size() - 2).trimmed().toLower();
			if (currentSection == sectionName)
				break;
			currentSection = nextSection;
			continue;
		}

		if (currentSection == sectionName)
			sectionLines << line;
	}

	return sectionLines.join(QLatin1Char('\n')).trimmed();
}

static QString obsModuleFilePath(const QString &relativePath)
{
	const QByteArray relativePathBytes = relativePath.toUtf8();
	using ObsCharPtr = std::unique_ptr<char, decltype(&bfree)>;
	ObsCharPtr modulePath(obs_module_file(relativePathBytes.constData()), bfree);

	if (!modulePath || !modulePath.get() || modulePath.get()[0] == '\0')
		return {};

	return QDir::fromNativeSeparators(QString::fromUtf8(modulePath.get()));
}

static QStringList promptTemplateFileCandidates()
{
	QStringList candidates;

	const QString configuredPath =
		QString::fromLocal8Bit(qgetenv("CLIP_CROPPER_GPT_PROMPT_TEMPLATE_PATH")).trimmed();
	if (!configuredPath.isEmpty())
		candidates.append(QDir::fromNativeSeparators(configuredPath));

	candidates.append(obsModuleFilePath(QStringLiteral("gpt-prompts/opus-curation.ini")));
	candidates.append(QDir(QDir::currentPath()).filePath(QStringLiteral("data/gpt-prompts/opus-curation.ini")));

	candidates.removeAll(QString());
	candidates.removeDuplicates();
	return candidates;
}

static QString loadTemplateSectionFromRuntimeFile(const QString &sectionName, const QString &logLabel)
{
	const QString normalizedSectionName = sectionName.trimmed().toLower();
	if (normalizedSectionName.isEmpty())
		return {};

	for (const QString &candidate : promptTemplateFileCandidates()) {
		QFile file(candidate);
		if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text))
			continue;

		const QString fileText = QString::fromUtf8(file.readAll());
		const QString templateText = promptTemplateSection(fileText, normalizedSectionName);
		if (!templateText.trimmed().isEmpty()) {
			blog(LOG_INFO, "Loaded %s from runtime file. path=%s section=%s chars=%d",
			     logLabel.toUtf8().constData(), candidate.toUtf8().constData(),
			     normalizedSectionName.toUtf8().constData(), templateText.size());
			return templateText.trimmed();
		}
	}

	return {};
}

static QString loadTemplateFromRuntimeFile(const QString &sourceLanguage)
{
	const QString normalizedLanguage = normalizePromptLanguage(sourceLanguage);
	const QString sectionName = normalizedLanguage == QStringLiteral("pt") ? QStringLiteral("pt")
									       : QStringLiteral("en");
	return loadTemplateSectionFromRuntimeFile(sectionName, QStringLiteral("GPT prompt template"));
}

static QString fallbackPortugueseInputTextTemplate()
{
	return QStringLiteral(
		"Você é um planejador para prompts ClipAnything do Opus Clip. Retorne somente JSON válido. "
		"Não escreva o prompt final. O plugin vai renderizar o prompt final em inglês. "
		"Use os campos: no_strong_clip_found, reason, arc_type, main_target, context_phrase, opening_criteria, "
		"development_criteria, ending_criteria, boundary_criteria. "
		"Escolha um único arco discursivo dominante usando gênero e tópicos apenas como sinais fracos, sem catálogo. "
		"Campos textuais devem ser concisos e em inglês.\n\n"
		"Scope: {{curation_scope}}\nGenre: {{genre}}\nRanges: {{selected_ranges}}\nTopics: {{topic_keywords}}\n"
		"Named references: {{named_references}}\nSelected transcript:\n{{selected_range_transcript}}\n");
}

static QString fallbackEnglishInputTextTemplate()
{
	return QStringLiteral(
		"You are a ClipAnything prompt planner for Opus Clip. Return valid JSON only. "
		"Do not write the final prompt. The plugin will render the final English prompt. "
		"Use these fields: no_strong_clip_found, reason, arc_type, main_target, context_phrase, opening_criteria, "
		"development_criteria, ending_criteria, boundary_criteria. "
		"Choose one dominant discourse arc, using genre and topics only as weak priors and never as a catalog. "
		"All text fields must be concise and in English.\n\n"
		"Scope: {{curation_scope}}\nGenre: {{genre}}\nRanges: {{selected_ranges}}\nTopics: {{topic_keywords}}\n"
		"Named references: {{named_references}}\nSelected transcript:\n{{selected_range_transcript}}\n");
}

bool GptPromptClient::isSemanticGateFailurePrompt(const QString &prompt)
{
	return prompt.trimmed().startsWith(QString::fromLatin1(SEMANTIC_GATE_FAILURE_PREFIX), Qt::CaseInsensitive);
}

QString GptPromptClient::semanticGateFailureReason(const QString &prompt)
{
	QString trimmedPrompt = prompt.trimmed();
	if (!isSemanticGateFailurePrompt(trimmedPrompt))
		return {};

	trimmedPrompt = trimmedPrompt.mid(QString::fromLatin1(SEMANTIC_GATE_FAILURE_PREFIX).size()).trimmed();
	if (trimmedPrompt.isEmpty())
		return QStringLiteral(
			"No complete self-contained moment with a clear payoff was found in the selected range.");

	return trimmedPrompt;
}

QString GptPromptClient::inputTemplateConfigKey()
{
	return inputTemplateConfigKey(QStringLiteral("auto"));
}

QString GptPromptClient::inputTemplateConfigKey(const QString &sourceLanguage)
{
	return QString::fromLatin1(CONFIG_GPT_INPUT_TEMPLATE_PREFIX) + normalizePromptLanguage(sourceLanguage);
}

QString GptPromptClient::defaultInputTextTemplate()
{
	return defaultInputTextTemplate(QStringLiteral("auto"));
}

QString GptPromptClient::defaultInputTextTemplate(const QString &sourceLanguage)
{
	const QString normalizedLanguage = normalizePromptLanguage(sourceLanguage);
	const QString runtimeTemplate = loadTemplateFromRuntimeFile(sourceLanguage);
	if (!runtimeTemplate.trimmed().isEmpty())
		return runtimeTemplate.trimmed();

	if (normalizedLanguage == QStringLiteral("pt"))
		return fallbackPortugueseInputTextTemplate();

	return fallbackEnglishInputTextTemplate();
}

QString GptPromptClient::configuredInputTextTemplate()
{
	return configuredInputTextTemplate(QStringLiteral("auto"));
}

QString GptPromptClient::configuredInputTextTemplate(const QString &sourceLanguage)
{
	const QString configuredTemplate = PluginConfig::getValue(inputTemplateConfigKey(sourceLanguage)).trimmed();
	if (!configuredTemplate.isEmpty())
		return configuredTemplate;

	if (normalizePromptLanguage(sourceLanguage) != QStringLiteral("auto")) {
		const QString autoTemplate =
			PluginConfig::getValue(inputTemplateConfigKey(QStringLiteral("auto"))).trimmed();
		if (!autoTemplate.isEmpty())
			return autoTemplate;
	}

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

QString GptPromptClient::buildInputText(const QString &videoPath, const RecordingTranscript &fullTranscript,
					const RecordingTranscript &selectedRangeTranscript,
					const CurationSettings &curationSettings) const
{
	const QString sourceLanguage = promptSourceLanguageForCuration(curationSettings);
	return renderInputTemplate(configuredInputTextTemplate(sourceLanguage), videoPath, fullTranscript,
				   selectedRangeTranscript, curationSettings, sourceLanguage);
}

void GptPromptClient::createOpusPromptAsync(const QString &videoPath, const RecordingTranscript &transcript,
					    const CurationSettings &curationSettings)
{
	createOpusPromptAsync(videoPath, transcript, transcript, curationSettings);
}

void GptPromptClient::createOpusPromptAsync(const QString &videoPath, const RecordingTranscript &fullTranscript,
					    const RecordingTranscript &selectedRangeTranscript,
					    const CurationSettings &curationSettings)
{
	if (apiKey.trimmed().isEmpty()) {
		emit promptFailed(QStringLiteral("OpenAI API key is empty"));
		return;
	}

	if (selectedRangeTranscript.segments.isEmpty()) {
		emit promptFailed(QStringLiteral("No transcript available for this video"));
		return;
	}

	QNetworkRequest request{QUrl(QString::fromLatin1(OPENAI_CHAT_COMPLETIONS_URL))};
	request.setRawHeader("Accept", "application/json");
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(apiKey.trimmed()).toUtf8());

	const QString inputText = buildInputText(videoPath, fullTranscript, selectedRangeTranscript, curationSettings);
	const QString inputCacheKey = GptPromptStore::keyForInput(model, inputText);
	const QString cachedPrompt = GptPromptStore::loadForInput(model, inputText);
	cancelRequested = false;

	const double selectedSec = selectedDurationSec(selectedRangeTranscript, curationSettings);
	const QString curationScope = curationScopeForDuration(selectedSec);
	blog(LOG_INFO,
	     "GPT curation input prepared. video=%s fullSegments=%d selectedSegments=%d selectedDurationSec=%.3f scope=%s inputChars=%d cacheKey=%s",
	     videoPath.toUtf8().constData(), static_cast<int>(fullTranscript.segments.size()),
	     static_cast<int>(selectedRangeTranscript.segments.size()), selectedSec, curationScope.toUtf8().constData(),
	     inputText.size(), inputCacheKey.toUtf8().constData());

	if (!cachedPrompt.trimmed().isEmpty()) {
		QStringList cachedIssues;
		if (!shouldRepairGeneratedPrompt(cachedPrompt, selectedRangeTranscript, curationSettings,
						 &cachedIssues)) {
			blog(LOG_INFO,
			     "GPT prompt cache hit. Skipping OpenAI request. video=%s cacheKey=%s promptChars=%d",
			     videoPath.toUtf8().constData(), inputCacheKey.toUtf8().constData(),
			     cachedPrompt.trimmed().size());
			QTimer::singleShot(0, this, [this, cachedPrompt]() {
				if (!cancelRequested)
					emit promptReady(cachedPrompt.trimmed());
			});
			return;
		}

		blog(LOG_WARNING,
		     "GPT prompt cache hit failed predictability checks. Ignoring cached prompt. video=%s cacheKey=%s issues=%s",
		     videoPath.toUtf8().constData(), inputCacheKey.toUtf8().constData(),
		     cachedIssues.join(QStringLiteral("; ")).toUtf8().constData());
	}

	blog(LOG_INFO, "GPT prompt cache miss. Sending OpenAI request. video=%s cacheKey=%s model=%s",
	     videoPath.toUtf8().constData(), inputCacheKey.toUtf8().constData(), model.trimmed().toUtf8().constData());

	const QJsonObject payload = buildChatCompletionsPayload(model, inputText, 500);

	QNetworkReply *reply = network.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
	reply->setProperty("clipCropperGptInputText", inputText);
	currentReply = reply;

	connect(reply, &QNetworkReply::finished, this, [this, reply, selectedRangeTranscript, curationSettings]() {
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

		const QString rawOutputText = extractOutputText(response).trimmed();
		if (rawOutputText.isEmpty()) {
			emit promptFailed(QStringLiteral("GPT response did not contain output text"));
			return;
		}

		blog(LOG_INFO, "GPT raw curation response received. status=%d rawChars=%d", status,
		     rawOutputText.size());

		const QString outputText =
			normalizeGeneratedOutput(rawOutputText, selectedRangeTranscript, curationSettings).trimmed();
		if (outputText.isEmpty()) {
			emit promptFailed(QStringLiteral("GPT response did not contain a valid Opus prompt"));
			return;
		}

		const QString inputText = reply->property("clipCropperGptInputText").toString();
		const bool semanticGateFailure =
			outputText.startsWith(QString::fromLatin1(SEMANTIC_GATE_FAILURE_PREFIX), Qt::CaseInsensitive);
		if (semanticGateFailure) {
			blog(LOG_INFO, "GPT semantic gate result was not cached. reason=%s",
			     outputText.toUtf8().constData());
			emit promptReady(outputText);
			return;
		}

		QStringList qualityIssues;
		if (shouldRepairGeneratedPrompt(outputText, selectedRangeTranscript, curationSettings,
						&qualityIssues)) {
			blog(LOG_WARNING, "GPT Opus prompt failed predictability checks. issues=%s prompt=%s",
			     qualityIssues.join(QStringLiteral("; ")).toUtf8().constData(),
			     outputText.toUtf8().constData());

			if (!gptPromptRepairEnabled()) {
				const QString fallbackPrompt = deterministicOpusPromptFallback(
					outputText, selectedRangeTranscript, curationSettings);
				QStringList fallbackIssues;
				const bool fallbackStillInvalid = shouldRepairGeneratedPrompt(
					fallbackPrompt, selectedRangeTranscript, curationSettings, &fallbackIssues);
				blog(LOG_WARNING,
				     "GPT prompt repair is disabled. Using deterministic local fallback after predictability failure. issues=%s fallbackIssues=%s fallback=%s",
				     qualityIssues.join(QStringLiteral("; ")).toUtf8().constData(),
				     fallbackIssues.join(QStringLiteral("; ")).toUtf8().constData(),
				     fallbackPrompt.toUtf8().constData());
				if (!fallbackStillInvalid && !inputText.trimmed().isEmpty())
					GptPromptStore::saveForInput(model, inputText, fallbackPrompt);
				emit promptReady(fallbackPrompt);
				return;
			}

			const QString sourceLanguage = promptSourceLanguageForCuration(curationSettings);
			const QString repairInputText = buildRepairInputText(
				outputText, qualityIssues, selectedRangeTranscript, curationSettings, sourceLanguage);

			QNetworkRequest repairRequest{QUrl(QString::fromLatin1(OPENAI_CHAT_COMPLETIONS_URL))};
			repairRequest.setRawHeader("Accept", "application/json");
			repairRequest.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
			repairRequest.setRawHeader("Authorization",
						   QStringLiteral("Bearer %1").arg(apiKey.trimmed()).toUtf8());

			const QJsonObject repairPayload =
				buildChatCompletionsPayload(model, repairInputText, 350, false);

			QNetworkReply *repairReply = network.post(
				repairRequest, QJsonDocument(repairPayload).toJson(QJsonDocument::Compact));
			repairReply->setProperty("clipCropperGptInputText", inputText);
			currentReply = repairReply;

			connect(repairReply, &QNetworkReply::finished, this,
				[this, repairReply, outputText, inputText, selectedRangeTranscript,
				 curationSettings]() {
					const QByteArray repairResponse = repairReply->readAll();
					const int repairStatus =
						repairReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
					const QNetworkReply::NetworkError repairError = repairReply->error();
					const QString repairErrorString = repairReply->errorString();
					if (currentReply == repairReply)
						currentReply = nullptr;
					repairReply->deleteLater();

					if (cancelRequested || repairError == QNetworkReply::OperationCanceledError) {
						cancelRequested = false;
						emit promptFailed(QStringLiteral("Canceled"));
						return;
					}

					if (repairError != QNetworkReply::NoError || repairStatus < 200 ||
					    repairStatus >= 300) {
						const QString fallbackPrompt = deterministicOpusPromptFallback(
							outputText, selectedRangeTranscript, curationSettings);
						QStringList fallbackIssues;
						const bool fallbackStillInvalid = shouldRepairGeneratedPrompt(
							fallbackPrompt, selectedRangeTranscript, curationSettings,
							&fallbackIssues);
						blog(LOG_WARNING,
						     "GPT prompt repair failed. Using deterministic local fallback. status=%d error=%s response=%s fallbackIssues=%s fallback=%s",
						     repairStatus, repairErrorString.toUtf8().constData(),
						     repairResponse.constData(),
						     fallbackIssues.join(QStringLiteral("; ")).toUtf8().constData(),
						     fallbackPrompt.toUtf8().constData());
						if (!fallbackStillInvalid && !inputText.trimmed().isEmpty())
							GptPromptStore::saveForInput(model, inputText, fallbackPrompt);
						emit promptReady(fallbackPrompt);
						return;
					}

					const QString rawRepairOutputText = extractOutputText(repairResponse).trimmed();
					const QString repairedOutputText =
						normalizeGeneratedOutput(rawRepairOutputText, selectedRangeTranscript,
									 curationSettings)
							.trimmed();
					if (repairedOutputText.isEmpty()) {
						const QString fallbackPrompt = deterministicOpusPromptFallback(
							outputText, selectedRangeTranscript, curationSettings);
						QStringList fallbackIssues;
						const bool fallbackStillInvalid = shouldRepairGeneratedPrompt(
							fallbackPrompt, selectedRangeTranscript, curationSettings,
							&fallbackIssues);
						blog(LOG_WARNING,
						     "GPT prompt repair returned empty output. Using deterministic local fallback. fallbackIssues=%s fallback=%s",
						     fallbackIssues.join(QStringLiteral("; ")).toUtf8().constData(),
						     fallbackPrompt.toUtf8().constData());
						if (!fallbackStillInvalid && !inputText.trimmed().isEmpty())
							GptPromptStore::saveForInput(model, inputText, fallbackPrompt);
						emit promptReady(fallbackPrompt);
						return;
					}

					QStringList repairedIssues;
					const bool repairedStillNeedsRepair =
						shouldRepairGeneratedPrompt(repairedOutputText, selectedRangeTranscript,
									    curationSettings, &repairedIssues);
					if (repairedStillNeedsRepair) {
						const QString fallbackPrompt = deterministicOpusPromptFallback(
							repairedOutputText, selectedRangeTranscript, curationSettings);
						QStringList fallbackIssues;
						const bool fallbackStillInvalid = shouldRepairGeneratedPrompt(
							fallbackPrompt, selectedRangeTranscript, curationSettings,
							&fallbackIssues);
						blog(LOG_WARNING,
						     "GPT prompt repair still has predictability issues. Using deterministic local fallback. repairedIssues=%s fallbackIssues=%s fallback=%s",
						     repairedIssues.join(QStringLiteral("; ")).toUtf8().constData(),
						     fallbackIssues.join(QStringLiteral("; ")).toUtf8().constData(),
						     fallbackPrompt.toUtf8().constData());
						if (!fallbackStillInvalid && !inputText.trimmed().isEmpty())
							GptPromptStore::saveForInput(model, inputText, fallbackPrompt);
						emit promptReady(fallbackPrompt);
						return;
					}

					blog(LOG_INFO, "GPT Opus prompt repaired successfully. promptChars=%d",
					     repairedOutputText.size());
					if (!inputText.trimmed().isEmpty())
						GptPromptStore::saveForInput(model, inputText, repairedOutputText);

					emit promptReady(repairedOutputText);
				});
			return;
		}

		blog(LOG_INFO, "GPT Opus prompt generated successfully. promptChars=%d", outputText.size());
		if (!inputText.trimmed().isEmpty())
			GptPromptStore::saveForInput(model, inputText, outputText);

		emit promptReady(outputText);
	});
}

QString GptPromptClient::extractOutputText(const QByteArray &response) const
{
	const QJsonDocument doc = QJsonDocument::fromJson(response);
	if (!doc.isObject())
		return {};

	const QJsonObject root = doc.object();
	const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
	for (const QJsonValue &choiceValue : choices) {
		const QJsonObject choice = choiceValue.toObject();
		const QJsonObject message = choice.value(QStringLiteral("message")).toObject();
		const QString content = message.value(QStringLiteral("content")).toString();
		if (!content.trimmed().isEmpty())
			return content;
	}

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
