#include "curation/scoring/cheap-clip-scorer.hpp"

#include "curation/scoring/text-analysis.hpp"

#include "curation/curation-signals.hpp"

#include <algorithm>
#include <initializer_list>

namespace {

static double boundedScore(double value)
{
	return std::clamp(value, 0.0, 1.0);
}

bool containsAny(const QString &text, const QStringList &phrases)
{
	for (const QString &phrase : phrases) {
		if (text.contains(phrase, Qt::CaseInsensitive))
			return true;
	}
	return false;
}

int phraseHitCount(const QString &text, const QStringList &phrases)
{
	int hits = 0;
	for (const QString &phrase : phrases) {
		if (!phrase.trimmed().isEmpty() && text.contains(phrase, Qt::CaseInsensitive))
			++hits;
	}
	return hits;
}

} // namespace

namespace Curation::Scoring {

ClipCandidate CheapClipScorer::score(const TranscriptIndex &index, const ClipCandidate &candidate,
				     const CheapScoringContext &context) const
{
	ClipCandidate scored = candidate;
	scored.text = scored.text.trimmed().isEmpty() ? index.textForRange(scored.range) : scored.text.simplified();
	if (scored.timedText.trimmed().isEmpty())
		scored.timedText = index.timedTextForRange(scored.range);
	scored.firstSegmentIndex = scored.firstSegmentIndex >= 0 ? scored.firstSegmentIndex
								 : index.firstSegmentIndexOverlapping(scored.range);
	scored.lastSegmentIndex = scored.lastSegmentIndex >= 0 ? scored.lastSegmentIndex
							       : index.lastSegmentIndexOverlapping(scored.range);
	scored.hasReliableMainTarget = context.reliableMainTarget && !context.mainTarget.trimmed().isEmpty();

	QStringList cues;
	scored.scores.emotional = Curation::emotionalScoreForText(scored.text, &cues);
	scored.emotionalCues = cues;
	scored.scores.advice = Curation::adviceScoreForText(scored.text);
	scored.scores.explanation = Curation::explanationScoreForText(scored.text);
	scored.scores.story = Curation::storyScoreForText(scored.text);
	scored.scores.opinion = Curation::opinionScoreForText(scored.text);
	scored.scores.tutorial = Curation::tutorialScoreForText(scored.text);
	scored.scores.duration = durationScore(scored.range.endSec - scored.range.startSec);
	scored.scores.pauseBeforeSec = index.silenceBeforeRange(scored.range);
	scored.scores.pauseAfterSec = index.silenceAfterRange(scored.range);
	scored.scores.maxInternalPauseSec = index.maxInternalSilenceInRange(scored.range);
	scored.scores.pauseBoundary = boundedScore((std::min(scored.scores.pauseBeforeSec, 4.0) * 0.12) +
						   (std::min(scored.scores.pauseAfterSec, 4.0) * 0.18));
	scored.scores.boundary = boundedScore(boundaryScore(index, scored) + (scored.scores.pauseBoundary * 0.18));
	scored.scores.hook = hookScore(scored.text);
	scored.scores.topicContinuity = topicContinuityScore(scored.text);
	scored.scores.noise = TextAnalysis::isBacklogOrGreetingText(scored.text) ||
					      TextAnalysis::hasNoiseOnlySemanticTopic(scored.text)
				      ? 1.0
				      : 0.0;
	scored.rejectedAsNoise = scored.scores.noise >= 1.0;
	scored.scores.viewerResponse =
		viewerResponseScore(scored.text, scored.startsNearViewerCue, scored.endsBeforeNextCue);
	scored.scores.semanticTarget = targetKeywordScore(scored.text, context.mainTarget);
	scored.scores.final = finalScore(scored, context);

	if (scored.scores.emotional > 0.0)
		scored.evidence.append(QStringLiteral("emotional_cue"));
	if (scored.scores.advice > 0.0)
		scored.evidence.append(QStringLiteral("advice_cue"));
	if (scored.scores.explanation > 0.0)
		scored.evidence.append(QStringLiteral("explanation_cue"));
	if (scored.scores.viewerResponse > 0.0)
		scored.evidence.append(QStringLiteral("viewer_response_cue"));
	if (scored.scores.pauseAfterSec >= 3.0)
		scored.evidence.append(
			QStringLiteral("pause_after:%1").arg(QString::number(scored.scores.pauseAfterSec, 'f', 1)));
	if (scored.scores.maxInternalPauseSec >= 2.0)
		scored.evidence.append(QStringLiteral("internal_pause:%1")
					       .arg(QString::number(scored.scores.maxInternalPauseSec, 'f', 1)));
	if (scored.scores.semanticTarget > 0.0)
		scored.evidence.append(QStringLiteral("target_keyword_match"));
	if (scored.scores.boundary >= 0.7)
		scored.evidence.append(QStringLiteral("clean_boundary"));
	if (scored.scores.noise > 0.0)
		scored.evidence.append(QStringLiteral("noise_penalty"));

	scored.evidence.removeDuplicates();
	return scored;
}

bool CheapClipScorer::hasStrongLocalCue(const QString &text) const
{
	return !TextAnalysis::isBacklogOrGreetingText(text) &&
	       (Curation::emotionalScoreForText(text) >= 0.20 || Curation::adviceScoreForText(text) >= 0.20 ||
		Curation::explanationScoreForText(text) >= 0.20 || Curation::storyScoreForText(text) >= 0.20 ||
		Curation::opinionScoreForText(text) >= 0.20 || Curation::tutorialScoreForText(text) >= 0.20 ||
		looksLikeQuestionOrViewerMessage(text));
}

bool CheapClipScorer::looksLikeQuestionOrViewerMessage(const QString &text) const
{
	return TextAnalysis::looksLikeQuestionOrViewerMessage(text) || Curation::textHasViewerExchangeSignals(text);
}

double CheapClipScorer::targetKeywordScore(const QString &text, const QString &target) const
{
	const QString normalized = TextAnalysis::normalized(text);
	const QStringList terms = targetTerms(target);
	if (normalized.isEmpty() || terms.isEmpty())
		return 0.0;

	int hits = 0;
	for (const QString &term : terms) {
		if (normalized.contains(term, Qt::CaseInsensitive))
			++hits;
	}

	return boundedScore(static_cast<double>(hits) /
			    static_cast<double>(std::max(2, static_cast<int>(terms.size()))));
}

double CheapClipScorer::durationScore(double durationSec) const
{
	if (durationSec <= 0.0)
		return 0.0;
	if (durationSec < 12.0)
		return 0.15;
	if (durationSec < 18.0)
		return 0.45;
	if (durationSec <= 75.0)
		return 1.0;
	if (durationSec <= 90.0)
		return 0.82;
	if (durationSec <= 150.0)
		return 0.62;
	return 0.30;
}

double CheapClipScorer::boundaryScore(const TranscriptIndex &index, const ClipCandidate &candidate) const
{
	double score = 0.0;
	const double silenceBefore = index.silenceBeforeRange(candidate.range);
	const double silenceAfter = index.silenceAfterRange(candidate.range);

	if (silenceBefore >= 0.35)
		score += 0.25;
	if (silenceBefore >= 0.75)
		score += 0.10;
	if (silenceAfter >= 0.45)
		score += 0.30;
	if (silenceAfter >= 0.85)
		score += 0.10;

	const QString trimmed = candidate.text.trimmed();
	if (trimmed.endsWith(QLatin1Char('.')) || trimmed.endsWith(QLatin1Char('!')) ||
	    trimmed.endsWith(QLatin1Char('?')))
		score += 0.25;

	return boundedScore(score);
}

double CheapClipScorer::hookScore(const QString &text) const
{
	const QString lower = text.left(320).toLower();
	const int hits = phraseHitCount(
		lower, {QStringLiteral("olha"), QStringLiteral("honestly"), QStringLiteral("the thing is"),
			QStringLiteral("o ponto é"), QStringLiteral("o ponto e"), QStringLiteral("a verdade"),
			QStringLiteral("vou te falar"), QStringLiteral("deixa eu"), QStringLiteral("listen"),
			QStringLiteral("here's"), QStringLiteral("primeiro"), QStringLiteral("first")});
	return boundedScore(static_cast<double>(hits) / 2.0);
}

double CheapClipScorer::topicContinuityScore(const QString &text) const
{
	const QString lower = text.toLower();
	const int hardShifts =
		phraseHitCount(lower, {QStringLiteral("mudando de assunto"), QStringLiteral("anyway"),
				       QStringLiteral("by the way"), QStringLiteral("next question"),
				       QStringLiteral("próxima pergunta"), QStringLiteral("proxima pergunta"),
				       QStringLiteral("outra coisa"), QStringLiteral("agora vamos")});
	return boundedScore(1.0 - (static_cast<double>(hardShifts) * 0.35));
}

double CheapClipScorer::viewerResponseScore(const QString &text, bool startsNearViewerCue, bool endsBeforeNextCue) const
{
	const QString lower = text.toLower();
	double score = 0.0;
	if (startsNearViewerCue || looksLikeQuestionOrViewerMessage(text))
		score += 0.35;
	if (Curation::textHasViewerExchangeSignals(text))
		score += 0.20;
	if (containsAny(lower, {QStringLiteral("eu faria"), QStringLiteral("você deveria"),
				QStringLiteral("voce deveria"), QStringLiteral("o que eu recomendo"),
				QStringLiteral("my advice"), QStringLiteral("you should"), QStringLiteral("i would")}))
		score += 0.25;
	if (endsBeforeNextCue)
		score += 0.20;
	return boundedScore(score);
}

double CheapClipScorer::finalScore(const ClipCandidate &candidate, const CheapScoringContext &context) const
{
	const ClipCandidateScores &s = candidate.scores;
	if (context.reliableMainTarget && !context.mainTarget.trimmed().isEmpty()) {
		return boundedScore((s.semanticTarget * 0.42) + (s.viewerResponse * 0.24) + (s.boundary * 0.18) +
				    (s.topicContinuity * 0.10) + (s.emotional * 0.06) - (s.noise * 0.35));
	}

	if (context.presetId == QStringLiteral("viewer_message_response")) {
		return boundedScore((s.viewerResponse * 0.34) + (std::max(s.advice, s.explanation) * 0.18) +
				    (s.boundary * 0.18) + (s.emotional * 0.14) + (s.hook * 0.08) + (s.duration * 0.08) -
				    (s.noise * 0.40));
	}

	return boundedScore((s.boundary * 0.20) + (s.duration * 0.15) + (s.hook * 0.12) +
			    (std::max({s.emotional, s.advice, s.explanation, s.story, s.opinion, s.tutorial}) * 0.34) +
			    (s.topicContinuity * 0.19) - (s.noise * 0.35));
}

QStringList CheapClipScorer::targetTerms(const QString &target) const
{
	return TextAnalysis::meaningfulTerms(target);
}

} // namespace Curation::Scoring
