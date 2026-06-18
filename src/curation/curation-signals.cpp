#include "curation/curation-signals.hpp"

#include <QRegularExpression>
#include <QStringList>

namespace {

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

static double transcriptDurationSeconds(const RecordingTranscript &transcript)
{
	if (transcript.segments.isEmpty())
		return 0.0;

	const double startSec = transcript.segments.first().startSec;
	const double endSec = transcript.segments.last().endSec;
	return endSec > startSec ? endSec - startSec : 0.0;
}

} // namespace

namespace Curation {

QString joinedTranscriptText(const RecordingTranscript &transcript)
{
	QString text;
	for (const TranscriptSegment &segment : transcript.segments) {
		const QString segmentText = segment.text.trimmed();
		if (segmentText.isEmpty())
			continue;

		if (!text.isEmpty())
			text += QLatin1Char(' ');
		text += segmentText;
	}
	return text.simplified();
}

double selectedDurationSeconds(const RecordingTranscript &selectedRangeTranscript,
			       const CurationSettings &curationSettings)
{
	double duration = 0.0;

	for (const ClipDuration &range : curationSettings.clipDurations) {
		if (range.endSec > range.startSec)
			duration += range.endSec - range.startSec;
	}

	if (duration <= 0.0 && curationSettings.rangeEndSec > curationSettings.rangeStartSec)
		duration = curationSettings.rangeEndSec - curationSettings.rangeStartSec;

	if (duration <= 0.0)
		duration = transcriptDurationSeconds(selectedRangeTranscript);

	return duration;
}

QString scopeForDuration(double durationSec)
{
	if (durationSec >= 2400.0)
		return QStringLiteral("large_range_multiple_clips");

	if (durationSec >= 180.0)
		return QStringLiteral("medium_range_multiple_independent_clips");

	return QStringLiteral("short_range_best_moment");
}

bool textHasViewerExchangeSignals(const QString &text)
{
	const QString lower = text.toLower();
	const bool phraseSignal = containsAnyPhrase(lower, {QStringLiteral("viewer"),
							    QStringLiteral("listener"),
							    QStringLiteral("chat"),
							    QStringLiteral("comment"),
							    QStringLiteral("question"),
							    QStringLiteral("answer"),
							    QStringLiteral("message"),
							    QStringLiteral("live"),
							    QStringLiteral("stream"),
							    QStringLiteral("q&a"),
							    QStringLiteral("advice"),
							    QStringLiteral("relationship advice"),
							    QStringLiteral("pergunta"),
							    QStringLiteral("comentário"),
							    QStringLiteral("comentario"),
							    QStringLiteral("mensagem"),
							    QStringLiteral("resposta"),
							    QStringLiteral("conselho"),
							    QStringLiteral("espectador"),
							    QStringLiteral("relacionamento"),
							    QStringLiteral("desabafo"),
							    QStringLiteral("comentários atrasados"),
							    QStringLiteral("comentarios atrasados"),
							    QStringLiteral("comentário fixado"),
							    QStringLiteral("comentario fixado"),
							    QStringLiteral("manda mensagem"),
							    QStringLiteral("livepx"),
							    QStringLiteral("seguidores")});

	const QRegularExpression speakerLabelPattern(
		QStringLiteral("(?:^|[\\s\\n])([\\p{L}0-9_][\\p{L}0-9_ .-]{1,28})\\s*[:：]"),
		QRegularExpression::UseUnicodePropertiesOption);

	const int straightQuoteCount = lower.count(QLatin1Char('"'));
	const int curlyQuoteCount = lower.count(QStringLiteral("“")) + lower.count(QStringLiteral("”"));
	const bool quotedMessageSignal = (straightQuoteCount + curlyQuoteCount) >= 4;
	const bool quotedLiveSignal =
		(straightQuoteCount + curlyQuoteCount) >= 2 &&
		containsAnyPhrase(lower, {QStringLiteral("live"), QStringLiteral("gente"), QStringLiteral("mensagem"),
					  QStringLiteral("comentário"), QStringLiteral("comentario"),
					  QStringLiteral("falou"), QStringLiteral("pergunta")});

	return phraseSignal || speakerLabelPattern.match(text).hasMatch() || quotedMessageSignal || quotedLiveSignal;
}

bool transcriptLooksLikeFragmentedViewerChat(const RecordingTranscript &transcript)
{
	if (transcript.segments.size() < 8)
		return false;

	const QString text = joinedTranscriptText(transcript);
	const QString lower = text.toLower();
	if (lower.isEmpty())
		return false;

	int score = 0;
	if (containsAnyPhrase(lower,
			      {QStringLiteral("live"), QStringLiteral("chat"), QStringLiteral("coment"),
			       QStringLiteral("mensag"), QStringLiteral("pergunta"), QStringLiteral("espectador"),
			       QStringLiteral("viewer"), QStringLiteral("comment"), QStringLiteral("message"),
			       QStringLiteral("question"), QStringLiteral("seguidores"), QStringLiteral("livepx")}))
		score += 2;

	if (containsAnyPhrase(lower,
			      {QStringLiteral("gente"), QStringLiteral("obrigado"), QStringLiteral("valeu"),
			       QStringLiteral("sinto muito"), QStringLiteral("meu pai"), QStringLiteral("minha mãe"),
			       QStringLiteral("minha mae"), QStringLiteral("meu amigo"), QStringLiteral("minha escola"),
			       QStringLiteral("meu ex"), QStringLiteral("do ex")}))
		++score;

	const int questionCues =
		substringCount(lower, QStringLiteral("?")) + substringCount(lower, QStringLiteral("como ")) +
		substringCount(lower, QStringLiteral("por que")) + substringCount(lower, QStringLiteral("porque ")) +
		substringCount(lower, QStringLiteral("quantos ")) + substringCount(lower, QStringLiteral("how ")) +
		substringCount(lower, QStringLiteral("why "));
	if (questionCues >= 2)
		++score;

	const int secondPersonCues =
		substringCount(lower, QStringLiteral("você")) + substringCount(lower, QStringLiteral("voce")) +
		substringCount(lower, QStringLiteral(" seu ")) + substringCount(lower, QStringLiteral(" sua ")) +
		substringCount(lower, QStringLiteral(" te ")) + substringCount(lower, QStringLiteral(" you ")) +
		substringCount(lower, QStringLiteral(" your "));
	if (secondPersonCues >= 4)
		++score;

	const double durationSec = transcriptDurationSeconds(transcript);

	qsizetype textChars = 0;
	for (const TranscriptSegment &segment : transcript.segments)
		textChars += segment.text.trimmed().size();

	const double averageSegmentSec = durationSec > 0.0 ? durationSec / transcript.segments.size() : 0.0;
	const double averageChars =
		transcript.segments.isEmpty()
			? 0.0
			: static_cast<double>(textChars) / static_cast<double>(transcript.segments.size());

	if (durationSec >= 90.0 && transcript.segments.size() >= 30 && averageSegmentSec <= 5.5)
		score += 2;
	if (transcript.segments.size() >= 40 && averageChars <= 110.0)
		++score;

	return score >= 3;
}

Signals analyzeSignals(const RecordingTranscript &transcript, const CurationSettings &curationSettings,
		       const QString &generatedPrompt)
{
	Signals result;
	result.segmentCount = static_cast<int>(transcript.segments.size());
	result.selectedDurationSec = selectedDurationSeconds(transcript, curationSettings);
	result.transcriptDurationSec = transcriptDurationSeconds(transcript);

	qsizetype textChars = 0;
	for (const TranscriptSegment &segment : transcript.segments)
		textChars += segment.text.trimmed().size();

	result.averageSegmentSec =
		result.transcriptDurationSec > 0.0 && !transcript.segments.isEmpty()
			? result.transcriptDurationSec / static_cast<double>(transcript.segments.size())
			: 0.0;
	result.averageCharsPerSegment =
		transcript.segments.isEmpty()
			? 0.0
			: static_cast<double>(textChars) / static_cast<double>(transcript.segments.size());

	QString metadata =
		curationSettings.genre + QLatin1Char(' ') + curationSettings.topicKeywords.join(QLatin1Char(' '));
	metadata += QLatin1Char(' ') + generatedPrompt + QLatin1Char(' ') + curationSettings.aiPrompt;

	result.hasMetadataViewerSignals = textHasViewerExchangeSignals(metadata);
	result.hasTranscriptViewerSignals = textHasViewerExchangeSignals(joinedTranscriptText(transcript));
	result.hasFragmentedViewerChatSignals = transcriptLooksLikeFragmentedViewerChat(transcript);
	result.likelyViewerExchange = result.hasMetadataViewerSignals || result.hasTranscriptViewerSignals ||
				      result.hasFragmentedViewerChatSignals;

	return result;
}

bool looksLikeViewerExchange(const RecordingTranscript &transcript, const CurationSettings &curationSettings,
			     const QString &generatedPrompt)
{
	return analyzeSignals(transcript, curationSettings, generatedPrompt).likelyViewerExchange;
}

} // namespace Curation
