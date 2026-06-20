#include "curation/compiler/prompt-quality-gate.hpp"

#include "curation/analysis/named-reference-detector.hpp"
#include "curation/curation-signals.hpp"

#include <QRegularExpression>

using namespace Curation;

namespace {

static constexpr const char *OPUS_PROMPT_PREFIX = "OPUS_PROMPT:";

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

int wordCount(const QString &text)
{
	return text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size();
}

int substringCount(const QString &text, const QString &needle)
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

bool containsAnyPhrase(const QString &text, const QStringList &phrases)
{
	for (const QString &phrase : phrases) {
		if (text.contains(phrase, Qt::CaseInsensitive))
			return true;
	}
	return false;
}

bool promptConstrainsSingleExchange(const QString &lowerPrompt)
{
	const bool hasSingleMessageLanguage = containsAnyPhrase(
		lowerPrompt, {QStringLiteral("single viewer message"), QStringLiteral("one viewer message"),
			      QStringLiteral("that one message"), QStringLiteral("same viewer message"),
			      QStringLiteral("same message"), QStringLiteral("that same message")});
	const bool hasResponseLanguage = containsAnyPhrase(
		lowerPrompt, {QStringLiteral("direct response"), QStringLiteral("speaker's response"),
			      QStringLiteral("complete response"), QStringLiteral("complete reaction"),
			      QStringLiteral("coherent response"), QStringLiteral("first natural resolution")});

	if (hasSingleMessageLanguage && hasResponseLanguage)
		return true;

	return containsAnyPhrase(lowerPrompt,
				 {QStringLiteral("one viewer"), QStringLiteral("one listener"),
				  QStringLiteral("one question"), QStringLiteral("one comment"),
				  QStringLiteral("one exchange"), QStringLiteral("one chat"),
				  QStringLiteral("one specific"), QStringLiteral("one complete answer"),
				  QStringLiteral("one complete response"), QStringLiteral("single question"),
				  QStringLiteral("single comment"), QStringLiteral("single exchange"),
				  QStringLiteral("one problem"), QStringLiteral("one piece of advice")});
}

} // namespace

QString Curation::opusPromptPayload(const QString &prompt)
{
	const QString value = lineValueForPrefix(prompt, OPUS_PROMPT_PREFIX);
	if (!value.trimmed().isEmpty())
		return value.trimmed();

	return prompt.trimmed();
}

QStringList Curation::promptQualityIssues(const QString &prompt, const RecordingTranscript &selectedRangeTranscript,
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

	const bool chatExchangeContext = looksLikeViewerExchange(selectedRangeTranscript, curationSettings, normalized);
	const bool multiTopicChatTarget = containsAnyPhrase(
		lower, {QStringLiteral("various interactions"), QStringLiteral("diverse interactions"),
			QStringLiteral("multiple interactions"), QStringLiteral("several interactions"),
			QStringLiteral("multiple viewer comments"), QStringLiteral("several viewer comments"),
			QStringLiteral("different viewer comments"), QStringLiteral("different chat messages"),
			QStringLiteral("unrelated chat messages"), QStringLiteral("rapid topic switches"),
			QStringLiteral("casual discussions"), QStringLiteral("casual conversations"),
			QStringLiteral("comments and advice"), QStringLiteral("viewer comments and")});
	if (multiTopicChatTarget)
		issues << QStringLiteral("targets multiple unrelated chat/comment topics");

	if (chatExchangeContext && !promptConstrainsSingleExchange(lower) &&
	    containsAnyPhrase(lower,
			      {QStringLiteral("viewer"), QStringLiteral("chat"), QStringLiteral("comment"),
			       QStringLiteral("question"), QStringLiteral("advice"), QStringLiteral("relationship"),
			       QStringLiteral("interactions"), QStringLiteral("conversations")}))
		issues << QStringLiteral("does not constrain chat/Q&A content to one complete exchange");

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

	const QString scope = scopeForDuration(selectedDurationSeconds(selectedRangeTranscript, curationSettings));
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

bool Curation::shouldRepairPrompt(const QString &prompt, const RecordingTranscript &selectedRangeTranscript,
			const CurationSettings &curationSettings, QStringList *issues)
{
	const QStringList detectedIssues = promptQualityIssues(prompt, selectedRangeTranscript, curationSettings);
	if (issues)
		*issues = detectedIssues;
	return !detectedIssues.isEmpty();
}
