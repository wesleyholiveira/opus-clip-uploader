#include "curation/curation-signals.hpp"

#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

namespace {

static int phraseHitCount(const QString &text, const QStringList &phrases, QStringList *matchedPhrases = nullptr)
{
	int hits = 0;
	for (const QString &phrase : phrases) {
		if (!phrase.trimmed().isEmpty() && text.contains(phrase, Qt::CaseInsensitive)) {
			++hits;
			if (matchedPhrases)
				matchedPhrases->append(phrase);
		}
	}
	return hits;
}

static double boundedScore(double value)
{
	return std::clamp(value, 0.0, 1.0);
}

static bool containsAnyPhrase(const QString &text, const QStringList &phrases)
{
	for (const QString &phrase : phrases) {
		if (text.contains(phrase, Qt::CaseInsensitive))
			return true;
	}
	return false;
}

} // namespace

namespace Curation {

double emotionalScoreForText(const QString &text, QStringList *matchedCues)
{
	const QString lower = text.toLower();
	QStringList localCues;
	QStringList *cues = matchedCues ? matchedCues : &localCues;

	const int griefHits = phraseHitCount(
		lower,
		{QStringLiteral("perdi meu pai"), QStringLiteral("perdi minha mãe"), QStringLiteral("perdi minha mae"),
		 QStringLiteral("perdeu o pai"), QStringLiteral("perdeu a mãe"), QStringLiteral("perdeu a mae"),
		 QStringLiteral("meu pai morreu"), QStringLiteral("minha mãe morreu"),
		 QStringLiteral("minha mae morreu"), QStringLiteral("faleceu"), QStringLiteral("luto"),
		 QStringLiteral("grief"), QStringLiteral("lost my father"), QStringLiteral("lost my mother")},
		cues);
	const int highStakesHits =
		phraseHitCount(lower,
			       {QStringLiteral("aposta"), QStringLiteral("apostas"), QStringLiteral("vício"),
				QStringLiteral("vicio"), QStringLiteral("gambling"), QStringLiteral("betting"),
				QStringLiteral("addiction"), QStringLiteral("depress"), QStringLiteral("ansiedade"),
				QStringLiteral("trauma"), QStringLiteral("sorry for your loss")},
			       cues);
	const int vulnerableHits = phraseHitCount(
		lower,
		{QStringLiteral("nunca falei"), QStringLiteral("nunca pedi"), QStringLiteral("não sei como"),
		 QStringLiteral("nao sei como"), QStringLiteral("tenho medo"), QStringLiteral("me sinto"),
		 QStringLiteral("sinto muita falta"), QStringLiteral("sinto falta"), QStringLiteral("não consigo"),
		 QStringLiteral("nao consigo"), QStringLiteral("i don't know"), QStringLiteral("i feel")},
		cues);

	const double griefWeight = griefHits > 0 ? 0.58 : 0.0;
	const double highStakesWeight = std::min(0.32, highStakesHits * 0.12);
	const double vulnerableWeight = std::min(0.24, vulnerableHits * 0.08);
	const double comboWeight = griefHits > 0 && highStakesHits > 0 ? 0.18 : 0.0;
	return boundedScore(griefWeight + highStakesWeight + vulnerableWeight + comboWeight);
}

double adviceScoreForText(const QString &text)
{
	const QString lower = text.toLower();
	const int hits = phraseHitCount(
		lower, {QStringLiteral("conselho"), QStringLiteral("advice"), QStringLiteral("como eu posso"),
			QStringLiteral("como posso"), QStringLiteral("o que eu faço"), QStringLiteral("o que eu faco"),
			QStringLiteral("devo"), QStringLiteral("should i"), QStringLiteral("how can i"),
			QStringLiteral("relacionamento"), QStringLiteral("relationship")});
	return boundedScore(static_cast<double>(hits) / 4.0);
}

double explanationScoreForText(const QString &text)
{
	const QString lower = text.toLower();
	const int hits = phraseHitCount(
		lower, {QStringLiteral("explica"), QStringLiteral("explicar"), QStringLiteral("por que"),
			QStringLiteral("porque"), QStringLiteral("conceito"), QStringLiteral("funciona"),
			QStringLiteral("método"), QStringLiteral("metodo"), QStringLiteral("method"),
			QStringLiteral("explains"), QStringLiteral("because"), QStringLiteral("means that")});
	return boundedScore(static_cast<double>(hits) / 5.0);
}

double storyScoreForText(const QString &text)
{
	const QString lower = text.toLower();
	const int hits = phraseHitCount(lower, {QStringLiteral("uma vez"), QStringLiteral("aconteceu"),
						QStringLiteral("quando eu"), QStringLiteral("na época"),
						QStringLiteral("na epoca"), QStringLiteral("lembro"),
						QStringLiteral("história"), QStringLiteral("historia"),
						QStringLiteral("story"), QStringLiteral("when i was")});
	return boundedScore(static_cast<double>(hits) / 4.0);
}

double opinionScoreForText(const QString &text)
{
	const QString lower = text.toLower();
	const int hits = phraseHitCount(lower, {QStringLiteral("eu acho"), QStringLiteral("minha opinião"),
						QStringLiteral("minha opiniao"), QStringLiteral("na minha visão"),
						QStringLiteral("na minha visao"), QStringLiteral("i think"),
						QStringLiteral("my take"), QStringLiteral("opinion")});
	return boundedScore(static_cast<double>(hits) / 3.0);
}

double tutorialScoreForText(const QString &text)
{
	const QString lower = text.toLower();
	const int hits =
		phraseHitCount(lower, {QStringLiteral("passo"), QStringLiteral("primeiro"), QStringLiteral("depois"),
				       QStringLiteral("tutorial"), QStringLiteral("como fazer"), QStringLiteral("faça"),
				       QStringLiteral("faca"), QStringLiteral("step"), QStringLiteral("walk through"),
				       QStringLiteral("do this")});
	return boundedScore(static_cast<double>(hits) / 4.0);
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

} // namespace Curation
