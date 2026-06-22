#include "curation/scoring/exchange-arc-boundary-refiner.hpp"

#include "curation/scoring/semantic-prototypes.hpp"

#include <QStringList>

#include <algorithm>
#include <cmath>
#include <initializer_list>

using namespace Curation::Scoring;

namespace {

static constexpr double VIEWER_PRESET_ANALYSIS_LOOKBACK_SEC = 56.0;
static constexpr double VIEWER_PRESET_ANALYSIS_LOOKAHEAD_SEC = 36.0;
static constexpr int VIEWER_PRESET_MAX_CONTEXT_EXTENSIONS = 12;
static constexpr int DEFAULT_MAX_CONTEXT_EXTENSIONS = 3;

// DFS boundary recovery must be window-based, but still needs hard guards so
// broad semantic themes such as "mental health" do not let the search walk
// across multiple viewer messages/topics. These are independent from the
// existing clip duration limits: one limits origin recovery, the other limits
// answer follow-through.
static constexpr int VIEWER_DFS_MAX_LOOKBACK_WINDOWS = 12;
static constexpr double VIEWER_DFS_MAX_LOOKBACK_SEC = 45.0;
static constexpr int VIEWER_DFS_MAX_FORWARD_WINDOWS = 22;
static constexpr double VIEWER_DFS_MAX_FORWARD_SEC = 82.0;

static double boundedScore(double value)
{
	return std::clamp(value, 0.0, 1.0);
}

enum class ArcChunkRole {
	Unknown,
	OpeningCandidate,
	Development,
	LocalResolution,
	PreviousConclusion,
	SocialOrMetaPrelude,
	TailOrNewTurn
};

static QString roleName(ArcChunkRole role)
{
	switch (role) {
	case ArcChunkRole::OpeningCandidate:
		return QStringLiteral("opening");
	case ArcChunkRole::Development:
		return QStringLiteral("development");
	case ArcChunkRole::LocalResolution:
		return QStringLiteral("resolution");
	case ArcChunkRole::PreviousConclusion:
		return QStringLiteral("previous_conclusion");
	case ArcChunkRole::SocialOrMetaPrelude:
		return QStringLiteral("meta_prelude");
	case ArcChunkRole::TailOrNewTurn:
		return QStringLiteral("tail_or_new_turn");
	case ArcChunkRole::Unknown:
		break;
	}
	return QStringLiteral("unknown");
}

struct ArcChunk {
	int firstIndex = -1;
	int lastIndex = -1;
	double startSec = 0.0;
	double endSec = 0.0;
	QString text;
	SemanticEmbedding embedding;
	double pauseBeforeSec = 0.0;
	double pauseAfterSec = 0.0;
	double target = 0.0;
	double value = 0.0;
	double hook = 0.0;
	double resolution = 0.0;
	double meta = 0.0;
	double shift = 0.0;
	double openingScore = 0.0;
	double developmentScore = 0.0;
	double conclusionScore = 0.0;
	double defectScore = 0.0;
	double explicitOpeningCue = 0.0;
	double explicitDevelopmentCue = 0.0;
	double explicitConclusionCue = 0.0;
	double explicitMetaCue = 0.0;
	double explicitShiftCue = 0.0;
	double contextOpeningScore = 0.0;
	double contextDevelopmentScore = 0.0;
	double contextConclusionScore = 0.0;
	double contextPreviousTopicScore = 0.0;
	double contextMetaScore = 0.0;
	double contextNewTopicScore = 0.0;
	double contextCoherenceScore = 0.0;
	QString roleReason;
	ArcChunkRole role = ArcChunkRole::Unknown;
};

struct ArcSpanScore {
	int first = -1;
	int last = -1;
	double score = 0.0;
	double opening = 0.0;
	double development = 0.0;
	double conclusion = 0.0;
	double boundaryCleanliness = 0.0;
	double tailRisk = 0.0;
	double cohesion = 0.0;
};

struct ContextualArcScore {
	bool valid = false;
	bool implicitOpening = false;
	bool semanticOpeningFallback = false;
	bool terminalFollowThrough = false;
	QString reason;
	int openingIndex = -1;
	int firstDevelopmentIndex = -1;
	int conclusionIndex = -1;
	double score = 0.0;
	double opening = 0.0;
	double development = 0.0;
	double conclusion = 0.0;
	double penalty = 0.0;
};

static bool canUseSpan(const QVector<ArcChunk> &chunks, int first, int last,
	const ExchangeArcBoundaryRefinementOptions &options);
static ArcSpanScore scoreSpan(const QVector<ArcChunk> &chunks, int first, int last,
	const ExchangeArcBoundaryRefinementOptions &options);
static bool isSemanticPrelude(const ArcChunk &chunk);
static bool isBlockingArcRole(ArcChunkRole role);
static bool hasForwardContextualArcSupport(const QVector<ArcChunk> &chunks, int first);
static QString windowDfsGraphEvidence(const QVector<ArcChunk> &chunks, int first, int last);

static QStringList uniqueTexts(QStringList values)
{
	QStringList result;
	for (const QString &value : values) {
		const QString text = value.simplified();
		if (!text.isEmpty() && !result.contains(text))
			result.append(text);
	}
	return result;
}

static QString foldedLowerText(const QString &value)
{
	QString normalized = value.toLower().normalized(QString::NormalizationForm_D);
	QString folded;
	folded.reserve(normalized.size());
	for (const QChar ch : normalized) {
		const QChar::Category category = ch.category();
		if (category == QChar::Mark_NonSpacing || category == QChar::Mark_SpacingCombining ||
		    category == QChar::Mark_Enclosing)
			continue;
		folded.append(ch);
	}
	return folded.simplified();
}

static bool containsAny(const QString &text, std::initializer_list<const char *> needles)
{
	for (const char *needle : needles) {
		if (needle && text.contains(QString::fromUtf8(needle)))
			return true;
	}
	return false;
}


static bool hasQuotedViewerCueShape(const QString &text)
{
	return text.contains(QStringLiteral(": \"")) || text.contains(QStringLiteral(":\"")) ||
		text.contains(QStringLiteral(": “")) || text.contains(QStringLiteral("\"")) ||
		text.contains(QStringLiteral("“")) || text.contains(QStringLiteral("”"));
}

static bool hasPersonalViewerProblemCue(const QString &text)
{
	return containsAny(text, {
		"minha namorada", "meu namorado", "minha esposa", "meu marido", "minha mina", "meu boy",
		"dependencia emocional", "dependência emocional", "emocional de mim", "tentei terminar",
		"terminar mas", "termino", "término", "relacionamento", "ficante", "ex namorado", "ex namorada",
		"meu pai", "minha mae", "minha mãe", "viciado", "viciada", "vicio", "vício", "bet",
		"aposta", "apostas", "psicologo", "psicólogo", "psicologa", "psicóloga", "terapia",
		"ansiedade", "depress", "autoestima", "autoconhecimento", "nao consigo", "não consigo",
		"preciso de ajuda", "o que eu faco", "o que eu faço", "o que devo", "devo terminar",
		"deveria terminar", "ela nao deixou", "ela não deixou", "ele nao deixou", "ele não deixou"
	});
}

static bool hasLikelyNewViewerAttribution(const QString &text)
{
	// Viewer chat lines commonly arrive as `Name: "message"` after WhisperX, but WhisperX
	// can split the attribution and the problem into adjacent windows. Accept a short
	// `Name:` prefix as a boundary cue, while avoiding URLs and long explanatory clauses.
	if (text.contains(QStringLiteral(": \"")) || text.contains(QStringLiteral(":\"")) ||
	    text.contains(QStringLiteral(": “")))
		return true;
	const int colon = text.indexOf(QLatin1Char(':'));
	if (colon < 2 || colon > 36)
		return false;
	const QString prefix = text.left(colon).simplified();
	if (prefix.contains(QStringLiteral("http")) || prefix.contains(QLatin1Char('/')) ||
	    prefix.contains(QLatin1Char('.')) || prefix.size() < 2 || prefix.size() > 36)
		return false;
	int letterCount = 0;
	for (const QChar ch : prefix) {
		if (ch.isLetter())
			++letterCount;
		else if (!ch.isSpace() && ch != QLatin1Char('_') && ch != QLatin1Char('-'))
			return false;
	}
	return letterCount >= 2;
}

static bool looksLikeViewerMessageCueText(const QString &text)
{
	if (text.isEmpty())
		return false;
	const bool quoted = hasQuotedViewerCueShape(text);
	const bool attributed = hasLikelyNewViewerAttribution(text);
	const bool personalProblem = hasPersonalViewerProblemCue(text);
	const bool questionShape = text.contains(QLatin1Char('?')) || containsAny(text, {
		"o que eu", "o que devo", "como eu", "como que", "sera que", "será que", "devo",
		"deveria", "tentei", "nao deixou", "não deixou", "quer meu", "quer o meu"
	});
	return (attributed && (personalProblem || questionShape || quoted)) ||
		(quoted && personalProblem) || (personalProblem && questionShape);
}

static bool looksLikeTargetedViewerMessageCue(const ArcChunk &chunk)
{
	const QString text = foldedLowerText(chunk.text);
	if (!looksLikeViewerMessageCueText(text))
		return false;

	const bool personalProblem = hasPersonalViewerProblemCue(text);
	const double semanticSupport = std::max({chunk.target, chunk.value, chunk.hook, chunk.explicitOpeningCue});
	const double localTopicSupport = std::max(chunk.target, chunk.value);
	const double defect = std::max({chunk.meta, chunk.shift, chunk.defectScore,
		chunk.explicitMetaCue, chunk.explicitShiftCue});
	return personalProblem ||
		(semanticSupport >= 0.60 && localTopicSupport >= 0.56 && defect <= semanticSupport + 0.14);
}

static bool hasViewerAdviceRequestCue(const QString &text)
{
	return containsAny(text, {
		"minha ", "meu ", "meus ", "minhas ", "comigo", "de mim", "pra mim", "para mim",
		"me envolvi", "me sinto", "eu sinto", "eu tenho", "tenho um", "tenho uma",
		"tentei", "quero terminar", "queria terminar", "nao deixou", "não deixou",
		"o que eu", "o que devo", "como eu", "devo ", "deveria ",
		"preciso de ajuda", "nao consigo", "não consigo", "dependencia", "dependência",
		"viciado", "viciada", "vicio", "vício", "bet", "aposta", "apostas",
		"ansiedade", "depress", "terapia", "psicologo", "psicólogo", "relacionamento",
		"namorada", "namorado", "ficante", "pai", "mae", "mãe"
	});
}

static bool looksLikeViewerOriginCue(const ArcChunk &chunk)
{
	const QString text = foldedLowerText(chunk.text);
	if (text.isEmpty())
		return false;
	if (looksLikeTargetedViewerMessageCue(chunk))
		return true;

	const bool personalProblem = hasPersonalViewerProblemCue(text);
	const bool adviceRequest = hasViewerAdviceRequestCue(text);
	const bool attributed = hasLikelyNewViewerAttribution(text);
	const bool quoted = hasQuotedViewerCueShape(text);
	const bool questionShape = text.contains(QLatin1Char('?')) || containsAny(text, {
		"o que eu", "o que devo", "como eu", "devo terminar", "deveria terminar",
		"tentei terminar", "nao deixou", "não deixou", "quer meu", "quer o meu"
	});
	const double positive = std::max({chunk.target, chunk.value, chunk.hook, chunk.openingScore,
		chunk.explicitOpeningCue});
	const double defect = std::max({chunk.meta, chunk.shift, chunk.defectScore,
		chunk.explicitMetaCue, chunk.explicitShiftCue});

	const bool explicitViewerProblem = personalProblem && (attributed || quoted || questionShape || adviceRequest);
	const bool attributedAdvice = attributed && adviceRequest && positive >= 0.46;
	const bool quotedAdvice = quoted && adviceRequest && positive >= 0.50;
	return (explicitViewerProblem || attributedAdvice || quotedAdvice) && defect <= positive + 0.30;
}

static bool isSplitViewerOriginPair(const ArcChunk &previous, const ArcChunk &next)
{
	const QString previousText = foldedLowerText(previous.text);
	const QString nextText = foldedLowerText(next.text);
	if (previousText.isEmpty() || nextText.isEmpty())
		return false;
	const double gapSec = std::max(previous.pauseAfterSec, next.pauseBeforeSec);
	if (gapSec > 3.8)
		return false;
	const bool previousAttribution = hasLikelyNewViewerAttribution(previousText);
	const bool nextAttribution = hasLikelyNewViewerAttribution(nextText);
	const bool previousProblem = hasPersonalViewerProblemCue(previousText) || hasViewerAdviceRequestCue(previousText);
	const bool nextProblem = hasPersonalViewerProblemCue(nextText) || hasViewerAdviceRequestCue(nextText);
	const double nextPositive = std::max({next.target, next.value, next.hook, next.openingScore, next.explicitOpeningCue});
	const double nextDefect = std::max({next.meta, next.shift, next.defectScore, next.explicitMetaCue, next.explicitShiftCue});
	const double previousPositive = std::max({previous.target, previous.value, previous.hook, previous.openingScore, previous.explicitOpeningCue});
	const double previousDefect = std::max({previous.meta, previous.shift, previous.defectScore, previous.explicitMetaCue, previous.explicitShiftCue});

	return (previousAttribution && nextProblem && nextDefect <= nextPositive + 0.34) ||
		(nextAttribution && previousProblem && previousDefect <= previousPositive + 0.34);
}

static bool looksLikeViewerOriginCueAt(const QVector<ArcChunk> &chunks, int index)
{
	if (index < 0 || index >= static_cast<int>(chunks.size()))
		return false;
	if (looksLikeViewerOriginCue(chunks.at(index)))
		return true;
	if (index > 0 && isSplitViewerOriginPair(chunks.at(index - 1), chunks.at(index)))
		return true;
	if (index + 1 < static_cast<int>(chunks.size()) && isSplitViewerOriginPair(chunks.at(index), chunks.at(index + 1)))
		return true;
	return false;
}

static int viewerOriginStartIndexAt(const QVector<ArcChunk> &chunks, int index)
{
	if (index > 0 && isSplitViewerOriginPair(chunks.at(index - 1), chunks.at(index)))
		return index - 1;
	return index;
}

static bool isSameSplitViewerOriginCue(const QVector<ArcChunk> &chunks, int first, int index)
{
	if (first < 0 || index <= first || index >= static_cast<int>(chunks.size()))
		return false;
	return index == first + 1 && isSplitViewerOriginPair(chunks.at(first), chunks.at(index));
}

static bool looksLikeAnyViewerMessageTurn(const ArcChunk &chunk)
{
	const QString text = foldedLowerText(chunk.text);
	if (text.isEmpty())
		return false;
	if (looksLikeViewerOriginCue(chunk))
		return true;
	// Use this only as a boundary/stop cue. It is intentionally broader than
	// looksLikeTargetedViewerMessageCue so an unrelated chat message such as
	// `Yuri: "..."` still stops the previous answer, but a quoted book title does not.
	return hasLikelyNewViewerAttribution(text);
}

static double viewerOriginCueScore(const ArcChunk &chunk)
{
	if (!looksLikeViewerOriginCue(chunk))
		return 0.0;
	const QString text = foldedLowerText(chunk.text);
	const double semantic = std::max({chunk.target, chunk.value, chunk.hook, chunk.openingScore,
		chunk.contextOpeningScore, chunk.explicitOpeningCue});
	const double defect = std::max({chunk.meta, chunk.shift, chunk.defectScore,
		chunk.explicitMetaCue, chunk.explicitShiftCue});
	const double personalBonus = hasPersonalViewerProblemCue(text) ? 0.18 : 0.0;
	const double attributionBonus = hasLikelyNewViewerAttribution(text) ? 0.08 : 0.0;
	const double requestBonus = hasViewerAdviceRequestCue(text) ? 0.08 : 0.0;
	return boundedScore(semantic + personalBonus + attributionBonus + requestBonus - (defect * 0.12));
}

static double viewerOriginCueScoreAt(const QVector<ArcChunk> &chunks, int index)
{
	if (index < 0 || index >= static_cast<int>(chunks.size()))
		return 0.0;
	double score = viewerOriginCueScore(chunks.at(index));
	if (index > 0 && isSplitViewerOriginPair(chunks.at(index - 1), chunks.at(index)))
		score = std::max(score, boundedScore(viewerOriginCueScore(chunks.at(index - 1)) * 0.74 +
			viewerOriginCueScore(chunks.at(index)) * 0.92 + 0.12));
	if (index + 1 < static_cast<int>(chunks.size()) && isSplitViewerOriginPair(chunks.at(index), chunks.at(index + 1)))
		score = std::max(score, boundedScore(viewerOriginCueScore(chunks.at(index)) * 0.74 +
			viewerOriginCueScore(chunks.at(index + 1)) * 0.92 + 0.12));
	return score;
}

static bool hasReliableViewerCueNearStart(const QVector<ArcChunk> &chunks, int first, int last)
{
	if (first < 0 || last < first || first >= static_cast<int>(chunks.size()))
		return false;
	const int safeLast = std::min(last, static_cast<int>(chunks.size()) - 1);
	const double startSec = chunks.at(first).startSec;
	for (int i = first; i <= safeLast && i <= first + 2; ++i) {
		if (chunks.at(i).startSec - startSec > 9.5)
			break;
		if (looksLikeViewerOriginCueAt(chunks, i))
			return true;
	}
	return first > 0 && isSplitViewerOriginPair(chunks.at(first - 1), chunks.at(first));
}

static int bestTargetedViewerCueInsideSpan(const QVector<ArcChunk> &chunks, int first, int last,
	const ExchangeArcBoundaryRefinementOptions &options)
{
	if (options.scoring.presetId != QStringLiteral("viewer_message_response") || first < 0 || last <= first)
		return -1;

	int bestIndex = -1;
	double bestScore = 0.0;
	for (int i = first + 1; i <= last && i < static_cast<int>(chunks.size()); ++i) {
		if (!looksLikeViewerOriginCueAt(chunks, i))
			continue;
		const int cueStart = viewerOriginStartIndexAt(chunks, i);
		if (!canUseSpan(chunks, cueStart, last, options))
			continue;

		const double cueScoreValue = std::max({viewerOriginCueScoreAt(chunks, i), chunks.at(i).explicitOpeningCue, chunks.at(i).contextOpeningScore,
			chunks.at(i).hook, chunks.at(i).target, chunks.at(i).value});
		const double defect = std::max({chunks.at(i).meta, chunks.at(i).shift, chunks.at(i).defectScore});
		const double forward = hasForwardContextualArcSupport(chunks, i) ? 0.18 : 0.0;
		const double score = cueScoreValue + forward - (defect * 0.16) + (i > first ? 0.025 : 0.0);
		if (bestIndex < 0 || score > bestScore + 0.015 ||
		    (std::fabs(score - bestScore) <= 0.015 && cueStart > bestIndex)) {
			bestIndex = cueStart;
			bestScore = score;
		}
	}
	return bestIndex;
}

static int nearestTargetedViewerCueBefore(const QVector<ArcChunk> &chunks, int first, int last,
	const ExchangeArcBoundaryRefinementOptions &options)
{
	if (options.scoring.presetId != QStringLiteral("viewer_message_response") || first <= 0 || last < first)
		return -1;

	const int maxLookbackWindows = VIEWER_DFS_MAX_LOOKBACK_WINDOWS;
	const double maxLookbackSec = VIEWER_DFS_MAX_LOOKBACK_SEC;
	const double maxRecoveredDurationSec = std::min(
		std::max(options.generation.boundaryMinDurationSec, options.generation.maxDurationSec),
		VIEWER_DFS_MAX_FORWARD_SEC);
	int depth = 0;
	for (int i = first - 1; i >= 0 && depth < maxLookbackWindows; --i, ++depth) {
		const double lookbackSec = chunks.at(first).startSec - chunks.at(i).startSec;
		if (lookbackSec > maxLookbackSec)
			break;
		if (looksLikeViewerOriginCueAt(chunks, i)) {
			const int cueStart = viewerOriginStartIndexAt(chunks, i);
			const double durationSec = chunks.at(last).endSec - chunks.at(cueStart).startSec;
			if (durationSec <= maxRecoveredDurationSec + 0.1)
				return cueStart;
		}
		if (looksLikeAnyViewerMessageTurn(chunks.at(i)) || chunks.at(i).role == ArcChunkRole::TailOrNewTurn)
			break;
	}
	return -1;
}

static int nextViewerCueInsideArc(const QVector<ArcChunk> &chunks, int first, int last)
{
	if (first < 0 || last <= first)
		return -1;
	for (int i = first + 1; i <= last && i < static_cast<int>(chunks.size()); ++i) {
		if (isSameSplitViewerOriginCue(chunks, first, i))
			continue;
		if (looksLikeAnyViewerMessageTurn(chunks.at(i)))
			return i;
	}
	return -1;
}

static double cueScore(bool present, double score)
{
	return present ? score : 0.0;
}

static void applyExplicitDiscourseSignals(ArcChunk &chunk)
{
	const QString text = foldedLowerText(chunk.text);
	if (text.isEmpty())
		return;

	const bool questionMark = text.contains(QLatin1Char('?'));
	const bool viewerSetup = containsAny(text, {
		"pergunta", "comentario", "comentário", "mensagem", "duvida", "dúvida", "viewer", "espectador",
		"alguem perguntou", "alguem comentou", "voce perguntou", "voces perguntaram", "o cara perguntou",
		"chat perguntou", "chat ta perguntando", "chat esta perguntando", "responder essa", "respondendo essa"
	});
	const bool questionShape = questionMark || containsAny(text, {
		"por que", "porque eu", "como eu", "como que", "o que eu", "qual e", "qual é", "sera que", "será que",
		"e se", "se eu", "quando eu", "devo", "deveria", "o que fazer", "o que voce acha", "o que você acha"
	});
	const bool explicitProblem = containsAny(text, {
		"problema", "dificuldade", "medo", "ansiedade", "depress", "terapia", "clinica", "clínica",
		"sofr", "triste", "cansado", "culpa", "vergonha", "relacionamento", "termino", "término",
		"namorada", "namorado", "dependencia emocional", "dependência emocional", "viciado", "viciada",
		"vicio", "vício", "bet", "aposta", "meu pai", "minha mae", "minha mãe",
		"rejeicao", "rejeição", "nao consigo", "não consigo", "preciso de ajuda", "conselho"
	});
	const bool curiosityHook = containsAny(text, {
		"sabe qual", "o ponto e", "o ponto é", "o problema e", "o problema é", "a questao e", "a questão é",
		"tem uma coisa", "vou te falar", "presta atencao", "presta atenção", "isso aqui", "a verdade e", "a verdade é"
	});
	const bool developmentCue = containsAny(text, {
		"porque", "por que", "tipo", "quando", "se voce", "se você", "na pratica", "na prática", "isso acontece",
		"a questao", "a questão", "o problema", "o ponto", "quer dizer", "significa", "voce tem", "você tem",
		"voce precisa", "você precisa", "eu acho", "eu penso", "faz sentido", "depende", "exemplo"
	});
	const bool conclusionCue = containsAny(text, {
		"entao", "então", "por isso", "por isso que", "no fim", "no final", "resumindo", "em resumo",
		"basicamente", "conclusao", "conclusão", "e isso", "é isso", "esse e o ponto", "esse é o ponto",
		"moral da historia", "moral da história", "ta ligado", "tá ligado", "sacou", "fechou", "fica tranquilo"
	});
	const bool metaCue = containsAny(text, {
		"obrigado", "obrigada", "valeu", "salve", "boa noite", "bom dia", "boa tarde", "tmj",
		"inscrito", "sub", "prime", "doacao", "doação", "donate", "bits", "gift", "presente",
		"moderador", "mod ", "ban", "timeout", "spam", "regra da live", "live", "stream", "overlay",
		"jogo", "game", "partida", "lobby", "fila", "convite", "youtube", "twitch", "discord"
	});
	const bool shiftCue = containsAny(text, {
		"agora", "proximo", "próximo", "outra coisa", "mudando", "vamos para", "vamos falar", "deixa eu ver",
		"deixa eu ler", "seguinte", "enquanto isso", "voltando aqui", "novo assunto"
	});

	chunk.explicitOpeningCue = std::max({chunk.explicitOpeningCue,
		cueScore(viewerSetup, 0.76), cueScore(questionShape, 0.66), cueScore(explicitProblem, 0.58),
		cueScore(curiosityHook, 0.62)});
	chunk.explicitDevelopmentCue = std::max({chunk.explicitDevelopmentCue,
		cueScore(developmentCue, 0.58), cueScore(explicitProblem, 0.42)});
	chunk.explicitConclusionCue = std::max(chunk.explicitConclusionCue, cueScore(conclusionCue, 0.70));
	chunk.explicitMetaCue = std::max(chunk.explicitMetaCue, cueScore(metaCue, 0.72));
	chunk.explicitShiftCue = std::max(chunk.explicitShiftCue, cueScore(shiftCue, 0.68));

	if (chunk.explicitMetaCue > 0.0)
		chunk.meta = std::max(chunk.meta, chunk.explicitMetaCue);
	if (chunk.explicitShiftCue > 0.0)
		chunk.shift = std::max(chunk.shift, chunk.explicitShiftCue);
}

static double maxPrototypeSimilarityForEmbedding(const SemanticEmbeddingProvider &provider,
	const SemanticEmbedding &embedding, const QStringList &prototypes)
{
	if (!embedding.isValid() || prototypes.isEmpty())
		return 0.0;

	double best = 0.0;
	for (const QString &prototype : prototypes) {
		const QString text = prototype.simplified();
		if (text.isEmpty())
			continue;
		best = std::max(best, cosineSimilarity(embedding, provider.embed(text)));
	}
	return boundedScore(best);
}


static QString clippedRoleText(const QString &text, int maxChars = 360)
{
	QString value = text.simplified();
	if (value.size() <= maxChars)
		return value;
	return value.left(maxChars).trimmed();
}

static QString contextualRoleWindowText(const QVector<ArcChunk> &chunks, int index,
	const ExchangeArcBoundaryRefinementOptions &options)
{
	const int first = std::max(0, index - 2);
	const int last = std::min(static_cast<int>(chunks.size()) - 1, index + 3);
	QStringList window;
	for (int i = first; i <= last; ++i) {
		const QString label = i == index ? QStringLiteral("CURRENT") :
			(i < index ? QStringLiteral("PREVIOUS") : QStringLiteral("NEXT"));
		window.append(QStringLiteral("%1[%2 %3-%4]: %5")
			.arg(label)
			.arg(i - index)
			.arg(QString::number(chunks.at(i).startSec, 'f', 1))
			.arg(QString::number(chunks.at(i).endSec, 'f', 1))
			.arg(clippedRoleText(chunks.at(i).text, i == index ? 520 : 260)));
	}

	QString target = options.scoring.mainTarget.simplified();
	if (target.size() > 320)
		target = target.left(320).trimmed();
	return QStringLiteral(
		"Boundary role classification context. Classify the discourse role of CURRENT using PREVIOUS and NEXT blocks, "
		"not isolated keywords. A valid clip is one continuous viewer response with opening/context, development, and conclusion. "
		"Target/topic: %1. %2")
		.arg(target.isEmpty() ? QStringLiteral("none") : target,
		     window.join(QStringLiteral(" | ")));
}

static QStringList contextualOpeningRolePrototypes(bool portuguese)
{
	if (portuguese) {
		return {
			QStringLiteral("CURRENT é o começo real de um novo corte autossuficiente: apresenta a pergunta, dilema, problema ou contexto do viewer e não depende do bloco anterior."),
			QStringLiteral("CURRENT funciona como gancho inicial: cria curiosidade ou tensão emocional e inicia a resposta a uma única mensagem do chat."),
			QStringLiteral("CURRENT contém contexto inicial suficiente para um espectador entender o assunto sem ouvir os blocos anteriores."),
			QStringLiteral("CURRENT abre um assunto humano de conselho, relacionamento, terapia, saúde mental, empatia ou resposta útil antes da explicação."),
		};
	}
	return {
		QStringLiteral("CURRENT is the real start of a self-contained clip: it introduces the viewer question, dilemma, problem, or context and does not depend on the previous block."),
		QStringLiteral("CURRENT works as the opening hook: it creates curiosity or emotional tension and starts the response to one chat message."),
		QStringLiteral("CURRENT contains enough initial context for an outside viewer to understand the subject without the previous blocks."),
		QStringLiteral("CURRENT opens a human advice, relationship, therapy, mental health, empathy, or useful answer topic before the explanation."),
	};
}

static QStringList contextualDevelopmentRolePrototypes(bool portuguese)
{
	if (portuguese) {
		return {
			QStringLiteral("CURRENT desenvolve o mesmo assunto iniciado antes: explica o raciocínio, dá exemplo, aprofunda o conselho ou constrói a resposta."),
			QStringLiteral("CURRENT é meio do corte: continua a mesma resposta de viewer sem iniciar novo assunto e sem fechar definitivamente."),
			QStringLiteral("CURRENT contém desenvolvimento útil, motivo, consequência, nuance ou orientação prática ligada ao mesmo problema."),
		};
	}
	return {
		QStringLiteral("CURRENT develops the same topic that started before: it explains reasoning, gives an example, deepens the advice, or builds the answer."),
		QStringLiteral("CURRENT is the middle of the clip: it continues the same viewer answer without starting a new topic or fully ending it."),
		QStringLiteral("CURRENT contains useful development, reason, consequence, nuance, or practical guidance tied to the same problem."),
	};
}

static QStringList contextualConclusionRolePrototypes(bool portuguese)
{
	if (portuguese) {
		return {
			QStringLiteral("CURRENT é a conclusão natural da mesma resposta: fecha o raciocínio com resolução, takeaway ou frase final satisfatória."),
			QStringLiteral("CURRENT pode ser o fim limpo do corte porque resolve o ponto e o próximo bloco não é necessário para entender a resposta."),
			QStringLiteral("CURRENT encerra o conselho ou resposta do viewer antes de outro assunto, meta-chat, agradecimento ou nova pergunta começar."),
		};
	}
	return {
		QStringLiteral("CURRENT is the natural conclusion of the same answer: it closes the reasoning with a resolution, takeaway, or satisfying final line."),
		QStringLiteral("CURRENT can be the clean ending of the clip because it resolves the point and the next block is not needed to understand the answer."),
		QStringLiteral("CURRENT ends the advice or viewer response before another topic, meta chat, thank you, or new question begins."),
	};
}

static QStringList contextualPreviousTopicRolePrototypes(bool portuguese)
{
	if (portuguese) {
		return {
			QStringLiteral("CURRENT é sobra do assunto anterior: fecha uma ideia já iniciada antes e não funciona como começo de um novo corte."),
			QStringLiteral("CURRENT depende dos blocos anteriores para fazer sentido e parece continuação ou conclusão de algo que veio antes."),
			QStringLiteral("CURRENT é uma resolução antiga antes do verdadeiro início da próxima resposta."),
		};
	}
	return {
		QStringLiteral("CURRENT is leftover from the previous topic: it closes an idea that started earlier and is not a valid start of a new clip."),
		QStringLiteral("CURRENT depends on previous blocks to make sense and looks like continuation or conclusion of something before it."),
		QStringLiteral("CURRENT is an old resolution before the real start of the next answer."),
	};
}

static QStringList contextualNewTopicRolePrototypes(bool portuguese)
{
	if (portuguese) {
		return {
			QStringLiteral("CURRENT inicia outro assunto, outra pergunta do chat ou outra interação não relacionada; o corte anterior deveria terminar antes dele."),
			QStringLiteral("CURRENT muda de tópico, começa novo viewer ou quebra a continuidade da resposta anterior."),
			QStringLiteral("CURRENT é transição para outro tema, outra mensagem, outro jogo, outro comentário ou outro momento da live."),
		};
	}
	return {
		QStringLiteral("CURRENT starts another topic, another chat question, or an unrelated interaction; the previous clip should end before it."),
		QStringLiteral("CURRENT changes topic, starts a new viewer, or breaks continuity from the previous answer."),
		QStringLiteral("CURRENT is a transition to another theme, message, game, comment, or stream moment."),
	};
}

static QStringList contextualMetaRolePrototypes(bool portuguese)
{
	if (portuguese) {
		return {
			QStringLiteral("CURRENT é meta-chat da live: agradecimento, moderação, jogo, lobby, inscrição, doação, spam, regra, cumprimento ou logística."),
			QStringLiteral("CURRENT é conversa operacional ou casual de stream sem conselho útil, sem arco emocional e sem resposta completa."),
			QStringLiteral("CURRENT fala sobre jogo, moderador, ban, convite, live, Twitch, YouTube, doação, sub, obrigado ou salve."),
		};
	}
	return {
		QStringLiteral("CURRENT is stream meta-chat: thank you, moderation, game, lobby, subscription, donation, spam, rule, greeting, or logistics."),
		QStringLiteral("CURRENT is operational or casual stream talk without useful advice, emotional arc, or complete response."),
		QStringLiteral("CURRENT talks about game, moderator, ban, invite, live, Twitch, YouTube, donation, sub, thanks, or greeting."),
	};
}

static void scoreContextualBoundaryRoles(QVector<ArcChunk> &chunks, const ExchangeArcBoundaryRefinementOptions &options)
{
	if (!options.embeddingProvider || !options.embeddingProvider->isAvailable() || chunks.isEmpty())
		return;

	const QString languageCode = normalizedSemanticLanguageCode(options.scoring.transcriptionLanguage,
		options.scoring.sourceLanguage);
	const bool portuguese = isPortugueseSemanticLanguage(languageCode);
	const QStringList openingPrototypes = contextualOpeningRolePrototypes(portuguese);
	const QStringList developmentPrototypes = contextualDevelopmentRolePrototypes(portuguese);
	const QStringList conclusionPrototypes = contextualConclusionRolePrototypes(portuguese);
	const QStringList previousTopicPrototypes = contextualPreviousTopicRolePrototypes(portuguese);
	const QStringList newTopicPrototypes = contextualNewTopicRolePrototypes(portuguese);
	const QStringList metaPrototypes = contextualMetaRolePrototypes(portuguese);

	for (int i = 0; i < static_cast<int>(chunks.size()); ++i) {
		ArcChunk &chunk = chunks[i];
		const SemanticEmbedding contextEmbedding = options.embeddingProvider->embed(contextualRoleWindowText(chunks, i, options));
		if (!contextEmbedding.isValid())
			continue;

		chunk.contextOpeningScore = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, contextEmbedding, openingPrototypes);
		chunk.contextDevelopmentScore = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, contextEmbedding, developmentPrototypes);
		chunk.contextConclusionScore = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, contextEmbedding, conclusionPrototypes);
		chunk.contextPreviousTopicScore = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, contextEmbedding, previousTopicPrototypes);
		chunk.contextNewTopicScore = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, contextEmbedding, newTopicPrototypes);
		chunk.contextMetaScore = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, contextEmbedding, metaPrototypes);
		if (i > 0)
			chunk.contextCoherenceScore = std::max(chunk.contextCoherenceScore,
				cosineSimilarity(chunks.at(i - 1).embedding, chunk.embedding));
		if (i + 1 < static_cast<int>(chunks.size()))
			chunk.contextCoherenceScore = std::max(chunk.contextCoherenceScore,
				cosineSimilarity(chunk.embedding, chunks.at(i + 1).embedding));

		const double contextPositive = std::max({chunk.contextOpeningScore, chunk.contextDevelopmentScore,
			chunk.contextConclusionScore});
		const double contextDefect = std::max(chunk.contextMetaScore, chunk.contextNewTopicScore);
		chunk.openingScore = boundedScore(std::max(chunk.openingScore,
			(chunk.contextOpeningScore * 0.58) + (chunk.hook * 0.20) + (chunk.target * 0.12) +
			(chunk.explicitOpeningCue * 0.16) - (contextDefect * 0.20)));
		chunk.developmentScore = boundedScore(std::max(chunk.developmentScore,
			(chunk.contextDevelopmentScore * 0.60) + (chunk.value * 0.20) + (chunk.target * 0.08) +
			(chunk.explicitDevelopmentCue * 0.14) - (contextDefect * 0.16)));
		chunk.conclusionScore = boundedScore(std::max(chunk.conclusionScore,
			(chunk.contextConclusionScore * 0.62) + (chunk.resolution * 0.20) +
			(chunk.explicitConclusionCue * 0.18) - (contextDefect * 0.14)));
		chunk.defectScore = boundedScore(std::max(chunk.defectScore,
			(contextDefect * 0.62) + (chunk.explicitMetaCue * 0.24) + (chunk.explicitShiftCue * 0.22) -
			(contextPositive * 0.18)));
		chunk.meta = std::max(chunk.meta, boundedScore((chunk.contextMetaScore * 0.72) + (chunk.explicitMetaCue * 0.20)));
		chunk.shift = std::max(chunk.shift, boundedScore((chunk.contextNewTopicScore * 0.72) + (chunk.explicitShiftCue * 0.20)));
	}
}

static void assignContextualBoundaryRoles(QVector<ArcChunk> &chunks, const ExchangeArcBoundaryRefinementOptions &options)
{
	if (options.scoring.presetId != QStringLiteral("viewer_message_response") || chunks.isEmpty())
		return;

	for (int i = 0; i < static_cast<int>(chunks.size()); ++i) {
		ArcChunk &chunk = chunks[i];
		const double positive = std::max({chunk.contextOpeningScore, chunk.contextDevelopmentScore,
			chunk.contextConclusionScore, chunk.openingScore, chunk.developmentScore, chunk.conclusionScore});
		const double hardDefect = std::max({chunk.contextMetaScore, chunk.contextNewTopicScore,
			chunk.explicitMetaCue, chunk.explicitShiftCue, chunk.meta, chunk.shift});
		const double previousScore = std::max(chunk.contextPreviousTopicScore,
			(i > 0 ? boundedScore((chunks.at(i - 1).contextConclusionScore * 0.42) +
				(chunk.contextDevelopmentScore * 0.20)) : 0.0));

		if (hardDefect >= positive + 0.045 && hardDefect >= 0.54) {
			chunk.role = chunk.contextNewTopicScore >= chunk.contextMetaScore + 0.03 || chunk.explicitShiftCue >= 0.64
				? ArcChunkRole::TailOrNewTurn
				: ArcChunkRole::SocialOrMetaPrelude;
			chunk.roleReason = QStringLiteral("context_defect");
			continue;
		}

		if (looksLikeTargetedViewerMessageCue(chunk) && hardDefect <= positive + 0.16) {
			chunk.role = ArcChunkRole::OpeningCandidate;
			chunk.roleReason = QStringLiteral("targeted_viewer_message_cue");
			continue;
		}

		if (previousScore >= std::max({chunk.contextOpeningScore, chunk.openingScore}) + 0.040 &&
		    previousScore >= 0.54 && i + 1 < static_cast<int>(chunks.size()) &&
		    std::max(chunks.at(i + 1).contextOpeningScore, chunks.at(i + 1).contextDevelopmentScore) >= previousScore - 0.05) {
			chunk.role = ArcChunkRole::PreviousConclusion;
			chunk.roleReason = QStringLiteral("context_previous_topic");
			continue;
		}

		const double opening = std::max(chunk.contextOpeningScore, chunk.openingScore);
		const double development = std::max(chunk.contextDevelopmentScore, chunk.developmentScore);
		const double conclusion = std::max(chunk.contextConclusionScore, chunk.conclusionScore);
		const double roleMax = std::max({opening, development, conclusion});
		const double roleMin = std::min({opening, development, conclusion});
		const bool viewerPresetTie = options.scoring.presetId == QStringLiteral("viewer_message_response") &&
			roleMax - roleMin <= 0.085 && chunk.explicitConclusionCue < 0.48 &&
			hardDefect <= roleMax + 0.05;
		if (viewerPresetTie) {
			// Multilingual embedding role prompts frequently produce near-identical
			// opening/development/conclusion scores for the same answer. When the
			// scores are tied, do not default to conclusion: that creates the
			// all-resolution plateau seen in dale.mp4. Use local discourse structure
			// and pauses as a tie-breaker instead.
			const bool likelyStart = i == 0 || chunk.pauseBeforeSec >= 1.20 ||
				chunk.explicitOpeningCue >= 0.38 ||
				(i > 0 && chunk.pauseBeforeSec >= 2.40);
			const bool likelyEnd = i + 1 >= static_cast<int>(chunks.size()) ||
				chunk.pauseAfterSec >= 1.20 || chunk.resolution >= 0.62;
			if (likelyStart && chunk.explicitMetaCue < 0.62 && chunk.explicitShiftCue < 0.62) {
				chunk.role = ArcChunkRole::OpeningCandidate;
				chunk.roleReason = QStringLiteral("context_tie_opening");
				continue;
			}
			if (!likelyEnd || development >= conclusion - 0.055 || chunk.explicitDevelopmentCue >= 0.34) {
				chunk.role = ArcChunkRole::Development;
				chunk.roleReason = QStringLiteral("context_tie_development");
				continue;
			}
		}

		if (opening >= development - 0.020 && opening >= conclusion - 0.010 && opening >= hardDefect - 0.02 &&
		    (chunk.explicitOpeningCue >= 0.42 || chunk.contextOpeningScore >= 0.58 || chunk.hook >= 0.64)) {
			chunk.role = ArcChunkRole::OpeningCandidate;
			chunk.roleReason = QStringLiteral("context_opening");
			continue;
		}
		if (development >= hardDefect - 0.04 &&
		    (development >= 0.42 || chunk.value >= 0.54 || chunk.contextDevelopmentScore >= 0.52) &&
		    (chunk.explicitConclusionCue < 0.48 || development >= conclusion - 0.045)) {
			chunk.role = ArcChunkRole::Development;
			chunk.roleReason = QStringLiteral("context_development");
			continue;
		}
		if (conclusion >= development - 0.010 && conclusion >= opening - 0.025 && conclusion >= hardDefect - 0.02 &&
		    (chunk.explicitConclusionCue >= 0.48 || chunk.contextConclusionScore >= 0.58 || chunk.resolution >= 0.58)) {
			chunk.role = ArcChunkRole::LocalResolution;
			chunk.roleReason = QStringLiteral("context_conclusion");
			continue;
		}
	}
}

static QVector<ArcChunk> chunksForCandidate(const TranscriptIndex &index, const ClipCandidate &candidate,
	const ExchangeArcBoundaryRefinementOptions &options)
{
	QVector<ArcChunk> chunks;
	if (!options.embeddingProvider || !options.embeddingProvider->isAvailable())
		return chunks;

	const QVector<int> indices = index.segmentIndicesForRange(candidate.range);
	if (indices.isEmpty())
		return chunks;

	ArcChunk current;
	double previousEndSec = -1.0;
	auto flushCurrent = [&]() {
		const QString text = current.text.simplified();
		const int minChunkChars = options.scoring.presetId == QStringLiteral("viewer_message_response") ? 8 : 20;
		if (current.firstIndex >= 0 && current.lastIndex >= current.firstIndex && text.size() >= minChunkChars) {
			current.text = text;
			current.pauseBeforeSec = index.silenceBeforeSegment(current.firstIndex);
			current.pauseAfterSec = index.silenceAfterSegment(current.lastIndex);
			current.embedding = options.embeddingProvider->embed(current.text);
			if (current.embedding.isValid())
				chunks.append(current);
		}
		current = {};
	};

	for (const int segmentIndex : indices) {
		const TranscriptSegment *segment = index.segmentAt(segmentIndex);
		if (!segment || segment->text.trimmed().isEmpty())
			continue;

		const QString text = segment->text.trimmed();
		const bool startsNewChunk = current.firstIndex < 0;
		const double gapSec = previousEndSec >= 0.0 ? std::max(0.0, segment->startSec - previousEndSec) : 0.0;
		const double nextDurationSec = startsNewChunk ? std::max(0.0, segment->endSec - segment->startSec)
						 : std::max(0.0, segment->endSec - current.startSec);
		const int nextChars = current.text.size() + text.size() + 1;
		const bool viewerPreset = options.scoring.presetId == QStringLiteral("viewer_message_response");
		const double maxChunkDurationSec = viewerPreset ? 2.6 : 3.75;
		const int maxChunkChars = viewerPreset ? 118 : 190;
		const bool startsNewViewerCueSegment = viewerPreset && !startsNewChunk &&
			looksLikeViewerMessageCueText(foldedLowerText(text));
		const bool shouldFlush = !startsNewChunk &&
			(startsNewViewerCueSegment || gapSec > 0.95 || nextDurationSec > maxChunkDurationSec ||
			 nextChars > maxChunkChars);
		if (shouldFlush)
			flushCurrent();

		if (current.firstIndex < 0) {
			current.firstIndex = segmentIndex;
			current.startSec = segment->startSec;
		}
		current.lastIndex = segmentIndex;
		current.endSec = segment->endSec;
		if (!current.text.isEmpty())
			current.text.append(QLatin1Char(' '));
		current.text.append(text);
		previousEndSec = segment->endSec;
	}
	flushCurrent();
	return chunks;
}

static void classifyChunk(ArcChunk &chunk)
{
	applyExplicitDiscourseSignals(chunk);

	const double positive = std::max({chunk.target, chunk.value, chunk.hook, chunk.resolution,
		chunk.explicitOpeningCue, chunk.explicitDevelopmentCue, chunk.explicitConclusionCue});
	const double defect = std::max({chunk.meta, chunk.shift, chunk.explicitMetaCue, chunk.explicitShiftCue});
	chunk.openingScore = boundedScore((chunk.hook * 0.36) + (chunk.target * 0.24) + (chunk.value * 0.14) +
		(chunk.explicitOpeningCue * 0.34) - (chunk.meta * 0.30) - (chunk.shift * 0.16));
	chunk.developmentScore = boundedScore((chunk.value * 0.38) + (chunk.target * 0.16) +
		(chunk.resolution * 0.12) + (chunk.hook * 0.08) + (chunk.explicitDevelopmentCue * 0.28) -
		(chunk.meta * 0.24) - (chunk.shift * 0.14));
	const double strongestEndingDefect = std::max({chunk.meta, chunk.shift, chunk.explicitMetaCue, chunk.explicitShiftCue});
	const double strictConclusion = boundedScore((chunk.resolution * 0.48) + (chunk.value * 0.18) +
		(chunk.target * 0.06) + (chunk.explicitConclusionCue * 0.34) - (chunk.meta * 0.28) -
		(chunk.shift * 0.26));
	const double semanticConclusionFallback = boundedScore((chunk.resolution * 0.56) + (chunk.value * 0.14) +
		(chunk.target * 0.08) + (chunk.explicitConclusionCue * 0.24) - (strongestEndingDefect * 0.20));
	chunk.conclusionScore = std::max(strictConclusion, semanticConclusionFallback);
	chunk.defectScore = boundedScore((defect * 0.76) + (chunk.shift * 0.18) - (positive * 0.22));

	const bool metaCompetesWithContent = defect >= positive - 0.05;
	const bool explicitMetaDominates = chunk.explicitMetaCue >= 0.70 && chunk.explicitOpeningCue < 0.50 &&
		chunk.explicitDevelopmentCue < 0.48 && chunk.explicitConclusionCue < 0.55;
	const bool weakSelfContainedOpening = chunk.openingScore < 0.48 && chunk.target < 0.58 &&
		chunk.resolution < 0.55 && chunk.explicitOpeningCue < 0.50;
	if (explicitMetaDominates || (chunk.defectScore >= 0.54 && metaCompetesWithContent) ||
	    (weakSelfContainedOpening && chunk.meta >= 0.54 && chunk.meta >= chunk.hook - 0.03)) {
		chunk.role = chunk.shift >= chunk.meta + 0.03 || chunk.explicitShiftCue >= 0.66
			? ArcChunkRole::TailOrNewTurn
			: ArcChunkRole::SocialOrMetaPrelude;
		return;
	}

	const bool explicitOpening = chunk.explicitOpeningCue >= 0.50;
	const bool strongSemanticOpening = chunk.openingScore >= 0.48 && chunk.hook >= 0.62 &&
		chunk.openingScore >= chunk.conclusionScore - 0.04 && chunk.meta < chunk.openingScore + 0.08;
	if (explicitOpening || strongSemanticOpening) {
		chunk.role = ArcChunkRole::OpeningCandidate;
		return;
	}

	const bool explicitConclusion = chunk.explicitConclusionCue >= 0.58;
	const bool strongConclusion = (explicitConclusion || chunk.resolution >= 0.58) &&
		chunk.conclusionScore >= std::max(chunk.openingScore, chunk.developmentScore) - 0.01;
	if (strongConclusion) {
		chunk.role = ArcChunkRole::LocalResolution;
		return;
	}

	const bool explicitDevelopment = chunk.explicitDevelopmentCue >= 0.48;
	const bool strongDevelopment = chunk.developmentScore >= 0.40 &&
		(explicitDevelopment || chunk.value >= 0.52 || positive >= 0.60 ||
		 chunk.developmentScore >= chunk.openingScore - 0.03) &&
		chunk.developmentScore >= chunk.conclusionScore - 0.06;
	if (strongDevelopment) {
		chunk.role = ArcChunkRole::Development;
		return;
	}

	if (chunk.openingScore >= chunk.developmentScore + 0.04 && chunk.openingScore >= 0.38 &&
	    chunk.explicitOpeningCue >= 0.38) {
		chunk.role = ArcChunkRole::OpeningCandidate;
		return;
	}
	if (chunk.developmentScore >= 0.36 || (positive >= 0.60 && chunk.meta < positive + 0.08)) {
		chunk.role = ArcChunkRole::Development;
		return;
	}
	chunk.role = ArcChunkRole::Unknown;
}

static void scoreChunks(QVector<ArcChunk> &chunks, const ExchangeArcBoundaryRefinementOptions &options)
{
	if (!options.embeddingProvider || !options.embeddingProvider->isAvailable())
		return;

	const QString languageCode = normalizedSemanticLanguageCode(options.scoring.transcriptionLanguage,
		options.scoring.sourceLanguage);
	const SemanticPrototypeSet &defaults = semanticPrototypesForLanguage(languageCode);
	const QStringList targetPrototypes = targetPrototypesForPreset(options.scoring.presetId,
		options.scoring.mainTarget, languageCode);
	const QStringList valuePrototypes = uniqueTexts(QStringList(defaults.clipValue) + defaults.empathy + targetPrototypes);
	const QStringList hookPrototypes = uniqueTexts(QStringList(defaults.hook) + defaults.viewerMessage + targetPrototypes);
	const QStringList resolutionPrototypes = uniqueTexts(QStringList(defaults.resolution) + defaults.directAnswer + targetPrototypes);
	const QStringList metaPrototypes = uniqueTexts(QStringList(defaults.metaNoise) + defaults.greetingNoise + defaults.streamManagement);
	const QStringList shiftPrototypes = uniqueTexts(QStringList(defaults.topicShift) + defaults.streamManagement);

	for (ArcChunk &chunk : chunks) {
		chunk.target = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, chunk.embedding, targetPrototypes);
		chunk.value = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, chunk.embedding, valuePrototypes);
		chunk.hook = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, chunk.embedding, hookPrototypes);
		chunk.resolution = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, chunk.embedding, resolutionPrototypes);
		chunk.meta = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, chunk.embedding, metaPrototypes);
		chunk.shift = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, chunk.embedding, shiftPrototypes);
		classifyChunk(chunk);
	}
	scoreContextualBoundaryRoles(chunks, options);
	assignContextualBoundaryRoles(chunks, options);
}

static double averageScore(const QVector<ArcChunk> &chunks, int first, int last, double ArcChunk::*field)
{
	if (first < 0 || last < first || last >= static_cast<int>(chunks.size()))
		return 0.0;
	double sum = 0.0;
	for (int i = first; i <= last; ++i)
		sum += chunks.at(i).*field;
	return boundedScore(sum / static_cast<double>(last - first + 1));
}

static double maxScore(const QVector<ArcChunk> &chunks, int first, int last, double ArcChunk::*field)
{
	if (first < 0 || last < first || last >= static_cast<int>(chunks.size()))
		return 0.0;
	double best = 0.0;
	for (int i = first; i <= last; ++i)
		best = std::max(best, chunks.at(i).*field);
	return boundedScore(best);
}

static double cohesionScore(const QVector<ArcChunk> &chunks, int first, int last)
{
	if (first < 0 || last < first || last >= static_cast<int>(chunks.size()))
		return 0.0;
	if (first == last)
		return 0.62;

	double sum = 0.0;
	double minSimilarity = 1.0;
	int comparisons = 0;
	for (int i = first; i < last; ++i) {
		const double similarity = cosineSimilarity(chunks.at(i).embedding, chunks.at(i + 1).embedding);
		sum += similarity;
		minSimilarity = std::min(minSimilarity, similarity);
		++comparisons;
	}
	if (comparisons <= 0)
		return 0.0;
	return boundedScore(((sum / static_cast<double>(comparisons)) * 0.70) + (minSimilarity * 0.30));
}

static bool canUseSpan(const QVector<ArcChunk> &chunks, int first, int last,
	const ExchangeArcBoundaryRefinementOptions &options)
{
	if (first < 0 || last < first || last >= static_cast<int>(chunks.size()))
		return false;
	const double durationSec = chunks.at(last).endSec - chunks.at(first).startSec;
	const double minDurationSec = std::max(8.0, options.generation.boundaryMinDurationSec);
	const double maxDurationSec = std::max(minDurationSec, options.generation.maxDurationSec);
	return durationSec >= minDurationSec && durationSec <= maxDurationSec + 0.1;
}

static bool isSemanticPrelude(const ArcChunk &chunk)
{
	const double positive = std::max({chunk.target, chunk.value, chunk.hook});
	return chunk.role == ArcChunkRole::SocialOrMetaPrelude || chunk.role == ArcChunkRole::PreviousConclusion ||
		(chunk.meta >= 0.56 && chunk.meta >= positive - 0.02 && chunk.openingScore < 0.34);
}

static bool isBlockingArcRole(ArcChunkRole role)
{
	return role == ArcChunkRole::PreviousConclusion || role == ArcChunkRole::SocialOrMetaPrelude ||
		role == ArcChunkRole::TailOrNewTurn;
}


static bool isWeakOpeningPrelude(const QVector<ArcChunk> &chunks, int index)
{
	if (index < 0 || index + 1 >= static_cast<int>(chunks.size()))
		return false;
	const ArcChunk &current = chunks.at(index);
	const ArcChunk &next = chunks.at(index + 1);
	if (isSemanticPrelude(current))
		return true;

	const double currentPositive = std::max({current.target, current.value, current.hook, current.resolution});
	const double currentDefect = std::max(current.defectScore, current.meta);
	const double nextStartPotential = std::max({next.openingScore, next.developmentScore, next.target, next.hook});
	const double nextPositive = std::max({next.target, next.value, next.hook, next.resolution});
	const bool currentIsSelfContained = current.openingScore >= 0.50 || current.target >= 0.61 ||
		(current.hook >= 0.68 && current.meta <= current.hook - 0.08);
	const bool currentLooksProcedural = current.openingScore <= 0.46 && current.resolution < 0.56 &&
		current.target < 0.58 && currentDefect >= currentPositive - 0.08;
	const bool nextIsBetterStart = nextStartPotential >= current.openingScore + 0.07 ||
		nextPositive >= currentPositive + 0.06 || next.role == ArcChunkRole::OpeningCandidate;
	return !currentIsSelfContained && currentLooksProcedural && nextIsBetterStart;
}

static bool shouldPrependOpeningContext(const QVector<ArcChunk> &chunks, int first)
{
	if (first <= 0 || first >= static_cast<int>(chunks.size()))
		return false;
	const ArcChunk &current = chunks.at(first);
	return current.role == ArcChunkRole::LocalResolution ||
		(current.openingScore < 0.43 && current.conclusionScore >= current.openingScore + 0.06) ||
		(current.openingScore < 0.40 && current.developmentScore >= 0.46);
}

static bool isEssentialOpeningContext(const ArcChunk &previous, const ArcChunk &current)
{
	if (isSemanticPrelude(previous) || previous.role == ArcChunkRole::LocalResolution)
		return false;
	const double previousPositive = std::max({previous.target, previous.value, previous.hook,
		previous.developmentScore, previous.openingScore});
	const double previousDefect = std::max(previous.meta, previous.shift);
	const double continuity = cosineSimilarity(previous.embedding, current.embedding);
	const bool hasContextSignal = previous.role == ArcChunkRole::OpeningCandidate ||
		previous.role == ArcChunkRole::Development || previous.hook >= 0.52 || previous.target >= 0.52 ||
		previous.value >= 0.58 || previous.openingScore >= 0.38;
	const bool notClearlySocial = previousPositive >= previousDefect - 0.04 && previous.shift < 0.72;
	return hasContextSignal && notClearlySocial && continuity >= 0.42 && previous.pauseAfterSec < 3.2;
}


static bool addsUsefulViewerOpeningContext(const QVector<ArcChunk> &chunks, int first, int currentFirst)
{
	if (first < 0 || currentFirst <= first || currentFirst >= static_cast<int>(chunks.size()))
		return false;

	bool foundContext = false;
	for (int i = first; i < currentFirst; ++i) {
		if (isSemanticPrelude(chunks.at(i)) || chunks.at(i).role == ArcChunkRole::TailOrNewTurn)
			return false;
		const ArcChunk &next = chunks.at(std::min(i + 1, currentFirst));
		const double positive = std::max({chunks.at(i).target, chunks.at(i).value, chunks.at(i).hook,
			chunks.at(i).openingScore, chunks.at(i).developmentScore});
		const double defect = std::max(chunks.at(i).meta, chunks.at(i).shift);
		const bool usefulContext = isEssentialOpeningContext(chunks.at(i), next) ||
			(chunks.at(i).hook >= 0.50 && positive >= defect - 0.04) ||
			(chunks.at(i).openingScore >= 0.36 && chunks.at(i).pauseAfterSec < 3.6);
		foundContext = foundContext || usefulContext;
	}
	return foundContext;
}



static bool isModeratePauseSameViewerAnswer(const ArcChunk &previous, const ArcChunk &next)
{
	const double pauseSec = std::max(previous.pauseAfterSec, next.pauseBeforeSec);
	if (pauseSec < 1.45 || pauseSec > 3.65)
		return false;
	if (next.role == ArcChunkRole::TailOrNewTurn || next.role == ArcChunkRole::SocialOrMetaPrelude)
		return false;

	const double continuity = cosineSimilarity(previous.embedding, next.embedding);
	const double nextPositive = std::max({next.target, next.value, next.resolution, next.hook,
		next.developmentScore, next.conclusionScore, next.contextDevelopmentScore, next.contextConclusionScore});
	const double nextDefect = std::max({next.meta, next.shift, next.defectScore,
		next.explicitMetaCue, next.explicitShiftCue, next.contextMetaScore, next.contextNewTopicScore});
	const double previousPositive = std::max({previous.target, previous.value, previous.resolution, previous.hook,
		previous.developmentScore, previous.conclusionScore});
	const bool nextContinuesAnswer = next.role == ArcChunkRole::Development ||
		next.role == ArcChunkRole::LocalResolution || next.resolution >= 0.48 || next.value >= 0.48 ||
		next.developmentScore >= 0.36 || next.conclusionScore >= 0.38;
	const bool notClearlyNewTopic = next.shift < nextPositive + 0.16 && next.contextNewTopicScore < nextPositive + 0.16 &&
		nextDefect < nextPositive + 0.20;
	const bool relatedEnough = continuity >= 0.28 || nextPositive >= previousPositive - 0.18 ||
		(next.target >= 0.50 && next.value >= nextDefect - 0.08);
	return nextContinuesAnswer && notClearlyNewTopic && relatedEnough;
}

static bool isPauseSeparatedSemanticTurn(const ArcChunk &previous, const ArcChunk &next)
{
	const double pauseSec = std::max(previous.pauseAfterSec, next.pauseBeforeSec);
	if (pauseSec < 1.45)
		return false;
	if (isModeratePauseSameViewerAnswer(previous, next))
		return false;
	const double previousResolution = std::max(previous.conclusionScore, previous.resolution);
	const double nextDefect = std::max(next.defectScore, next.shift);
	const double continuity = cosineSimilarity(previous.embedding, next.embedding);
	const double nextPositive = std::max({next.value, next.target, next.resolution, next.hook,
		next.developmentScore, next.conclusionScore});
	const bool nextLooksDifferent = next.role == ArcChunkRole::TailOrNewTurn ||
		next.shift >= nextPositive - 0.02 || continuity < 0.52;
	return previousResolution >= 0.42 && nextLooksDifferent &&
		(pauseSec >= 3.65 || (pauseSec >= 2.65 && nextDefect >= nextPositive + 0.10));
}

static bool isTailRisk(const QVector<ArcChunk> &chunks, int last)
{
	if (last < 0 || last >= static_cast<int>(chunks.size()))
		return false;
	const ArcChunk &tail = chunks.at(last);
	if (tail.role == ArcChunkRole::TailOrNewTurn)
		return true;
	if (last > 0 && isPauseSeparatedSemanticTurn(chunks.at(last - 1), tail))
		return true;
	const double positive = std::max({tail.target, tail.value, tail.resolution, tail.hook});
	return tail.defectScore >= 0.62 && std::max(tail.meta, tail.shift) >= positive - 0.02;
}

static bool isSemanticContinuation(const QVector<ArcChunk> &chunks, int currentLast, int nextIndex)
{
	if (currentLast < 0 || nextIndex <= currentLast || nextIndex >= static_cast<int>(chunks.size()))
		return false;
	const ArcChunk &current = chunks.at(currentLast);
	const ArcChunk &next = chunks.at(nextIndex);
	if (isPauseSeparatedSemanticTurn(current, next) || isTailRisk(chunks, nextIndex))
		return false;

	const double continuity = cosineSimilarity(current.embedding, next.embedding);
	const double nextPositive = std::max({next.target, next.value, next.resolution, next.hook});
	const double nextDefect = std::max(next.meta, next.shift);
	return continuity >= 0.62 && nextPositive >= nextDefect - 0.04 &&
		(next.developmentScore >= 0.34 || next.conclusionScore >= 0.34 || next.resolution >= 0.55);
}

static bool isViewerAnswerContinuation(const QVector<ArcChunk> &chunks, int currentLast, int nextIndex)
{
	if (currentLast < 0 || nextIndex <= currentLast || nextIndex >= static_cast<int>(chunks.size()))
		return false;
	const ArcChunk &current = chunks.at(currentLast);
	const ArcChunk &next = chunks.at(nextIndex);
	if (isBlockingArcRole(next.role))
		return false;

	const double pauseSec = std::max(current.pauseAfterSec, next.pauseBeforeSec);
	const double continuity = cosineSimilarity(current.embedding, next.embedding);
	const double nextPositive = std::max({next.target, next.value, next.resolution, next.hook, next.developmentScore,
		next.conclusionScore, next.contextDevelopmentScore, next.contextConclusionScore});
	const double nextDefect = std::max({next.meta, next.shift, next.defectScore, next.contextMetaScore, next.contextNewTopicScore});
	if ((isPauseSeparatedSemanticTurn(current, next) || isTailRisk(chunks, nextIndex)) &&
	    !isModeratePauseSameViewerAnswer(current, next))
		return false;
	const bool stillAnswerBody = next.role == ArcChunkRole::Development ||
		next.role == ArcChunkRole::LocalResolution || next.developmentScore >= 0.40 ||
		next.conclusionScore >= 0.42 || next.resolution >= 0.50 || next.value >= 0.52;
	const bool notNewTopic = next.shift < nextPositive + 0.09 && nextDefect < nextPositive + 0.13;
	const bool connectedEnough = continuity >= 0.30 || pauseSec <= 1.15 || isModeratePauseSameViewerAnswer(current, next) ||
		(next.target >= 0.55 && next.value >= nextDefect - 0.08);
	return pauseSec <= 3.65 && stillAnswerBody && notNewTopic && connectedEnough;
}

static bool isStaleViewerLeadingResolution(const QVector<ArcChunk> &chunks, int first)
{
	if (first < 0 || first + 1 >= static_cast<int>(chunks.size()))
		return false;

	const ArcChunk &current = chunks.at(first);
	const ArcChunk &next = chunks.at(first + 1);
	if (current.role != ArcChunkRole::LocalResolution)
		return false;
	if (isSemanticPrelude(current) || current.role == ArcChunkRole::TailOrNewTurn)
		return true;

	const double continuity = cosineSimilarity(current.embedding, next.embedding);
	const double currentContent = std::max({current.value, current.hook, current.target, current.openingScore});
	const double currentDefect = std::max(current.meta, current.shift);
	const double nextBody = std::max({next.developmentScore, next.openingScore, next.value, next.hook});
	const bool currentLooksLikePriorClose = current.resolution >= 0.56 &&
		current.conclusionScore >= current.openingScore + 0.07 && current.openingScore < 0.44;
	const bool nextLooksLikeActualBody = next.role == ArcChunkRole::Development ||
		next.role == ArcChunkRole::OpeningCandidate || nextBody >= current.openingScore + 0.05;
	const bool currentIsNotStrongHook = current.hook < 0.60 && currentContent >= currentDefect - 0.05;
	return currentLooksLikePriorClose && nextLooksLikeActualBody && currentIsNotStrongHook &&
		continuity >= 0.35 && current.pauseAfterSec < 3.8;
}

static bool isViewerStartBoundaryContaminated(const QVector<ArcChunk> &chunks, int first)
{
	if (first < 0 || first + 1 >= static_cast<int>(chunks.size()))
		return false;

	const ArcChunk &current = chunks.at(first);
	const ArcChunk &next = chunks.at(first + 1);
	if (current.role != ArcChunkRole::LocalResolution)
		return false;

	const double currentDurationSec = std::max(0.0, current.endSec - current.startSec);
	const double currentContent = std::max({current.value, current.hook, current.target, current.openingScore});
	const double currentDefect = std::max({current.meta, current.shift, current.defectScore});
	const double nextBody = std::max({next.developmentScore, next.openingScore, next.value, next.hook});
	const double continuity = cosineSimilarity(current.embedding, next.embedding);
	const bool nextStartsActualBody = next.role == ArcChunkRole::Development ||
		next.role == ArcChunkRole::OpeningCandidate || nextBody >= current.openingScore + 0.03;
	const bool currentLooksLikeLeftoverClose = current.conclusionScore >= current.openingScore + 0.04 ||
		current.resolution >= std::max(current.openingScore, current.developmentScore) + 0.04;
	const bool weakAsOpening = current.openingScore < 0.43 && current.target < 0.62 &&
		(current.hook < 0.66 || current.conclusionScore >= current.openingScore + 0.08);
	const bool notClearlyBetterThanNext = nextBody >= currentContent - 0.03 || next.role == ArcChunkRole::Development;
	const bool notHardTopicBreak = continuity >= 0.30 && current.pauseAfterSec < 4.2 && currentDefect <= currentContent + 0.16;

	return nextStartsActualBody && currentLooksLikeLeftoverClose && weakAsOpening &&
		notClearlyBetterThanNext && notHardTopicBreak && currentDurationSec <= 10.5;
}

static bool shouldExtendViewerConclusionFollowThroughSoft(const QVector<ArcChunk> &chunks, int currentLast, int nextIndex)
{
	if (currentLast < 0 || nextIndex <= currentLast || nextIndex >= static_cast<int>(chunks.size()))
		return false;

	const ArcChunk &current = chunks.at(currentLast);
	const ArcChunk &next = chunks.at(nextIndex);
	const double pauseSec = std::max(current.pauseAfterSec, next.pauseBeforeSec);
	const double nextDurationSec = std::max(0.0, next.endSec - next.startSec);
	const double continuity = cosineSimilarity(current.embedding, next.embedding);
	const double nextPositive = std::max({next.target, next.value, next.resolution, next.hook, next.developmentScore,
		next.conclusionScore, next.contextDevelopmentScore, next.contextConclusionScore});
	const double nextDefect = std::max({next.meta, next.shift, next.defectScore, next.contextMetaScore, next.contextNewTopicScore});
	if ((isPauseSeparatedSemanticTurn(current, next) || isTailRisk(chunks, nextIndex)) &&
	    !isModeratePauseSameViewerAnswer(current, next))
		return false;
	const bool sameAnswerFollowThrough = next.role == ArcChunkRole::LocalResolution ||
		next.role == ArcChunkRole::Development || next.resolution >= 0.48 || next.value >= 0.50;
	const bool notNewTopic = next.shift < nextPositive + 0.08 && nextDefect < nextPositive + 0.12;
	const bool closeEnough = continuity >= 0.30 || pauseSec <= 1.25 || isModeratePauseSameViewerAnswer(current, next);

	return nextDurationSec <= 9.5 && pauseSec <= 3.65 && sameAnswerFollowThrough && notNewTopic && closeEnough;
}

static bool isViewerConclusionFollowThrough(const QVector<ArcChunk> &chunks, int currentLast, int nextIndex)
{
	if (currentLast < 0 || nextIndex <= currentLast || nextIndex >= static_cast<int>(chunks.size()))
		return false;

	const ArcChunk &current = chunks.at(currentLast);
	const ArcChunk &next = chunks.at(nextIndex);
	const double pauseSec = std::max(current.pauseAfterSec, next.pauseBeforeSec);
	const double nextDurationSec = std::max(0.0, next.endSec - next.startSec);
	const double continuity = cosineSimilarity(current.embedding, next.embedding);
	const double nextPositive = std::max({next.target, next.value, next.resolution, next.hook, next.developmentScore,
		next.conclusionScore, next.contextDevelopmentScore, next.contextConclusionScore});
	const double nextDefect = std::max({next.meta, next.shift, next.defectScore, next.contextMetaScore, next.contextNewTopicScore});
	if ((isPauseSeparatedSemanticTurn(current, next) || isTailRisk(chunks, nextIndex)) &&
	    !isModeratePauseSameViewerAnswer(current, next))
		return false;
	const bool nextStillResolvesSameAnswer = next.role == ArcChunkRole::LocalResolution ||
		next.role == ArcChunkRole::Development || next.resolution >= 0.52 || next.value >= 0.54;
	const bool notNewTopicHook = next.shift < nextPositive + 0.10 && nextDefect < nextPositive + 0.16;
	const bool connectedEnough = continuity >= 0.34 || isModeratePauseSameViewerAnswer(current, next);
	return nextDurationSec <= 8.5 && pauseSec <= 3.65 && connectedEnough && nextStillResolvesSameAnswer && notNewTopicHook;
}


static bool looksLikePreviousConclusionBeforeBody(const QVector<ArcChunk> &chunks, int index)
{
	if (index < 0 || index + 1 >= static_cast<int>(chunks.size()))
		return false;

	const ArcChunk &current = chunks.at(index);
	if (current.role != ArcChunkRole::LocalResolution)
		return false;

	int nextBody = -1;
	for (int i = index + 1; i < static_cast<int>(chunks.size()) && i <= index + 3; ++i) {
		if (chunks.at(i).role == ArcChunkRole::OpeningCandidate || chunks.at(i).role == ArcChunkRole::Development) {
			nextBody = i;
			break;
		}
		if (isBlockingArcRole(chunks.at(i).role))
			break;
	}
	if (nextBody < 0)
		return false;

	const ArcChunk &next = chunks.at(nextBody);
	const double continuity = cosineSimilarity(current.embedding, next.embedding);
	const double currentOpening = std::max(current.openingScore, current.hook);
	const double currentBody = std::max({current.value, current.target, current.developmentScore});
	const double nextBodySignal = std::max({next.openingScore, next.developmentScore, next.value, next.hook});
	const bool currentIsClosingShape = current.conclusionScore >= currentOpening + 0.045 ||
		current.resolution >= std::max(current.openingScore, current.developmentScore) + 0.035;
	const bool nextIsBetterStart = nextBodySignal >= std::max(currentOpening, currentBody) - 0.015 ||
		next.role == ArcChunkRole::OpeningCandidate || next.role == ArcChunkRole::Development;
	const bool currentIsWeakHook = currentOpening < 0.58 && current.target < 0.63 &&
		current.openingScore < current.conclusionScore + 0.03;
	const bool boundaryIsNearby = current.pauseAfterSec < 4.5 && continuity >= 0.24;
	return currentIsClosingShape && nextIsBetterStart && currentIsWeakHook && boundaryIsNearby;
}

static void classifyContextualRoles(QVector<ArcChunk> &chunks, const ExchangeArcBoundaryRefinementOptions &options)
{
	if (options.scoring.presetId != QStringLiteral("viewer_message_response") || chunks.size() < 2)
		return;

	// First pass: mark closing residue that appears before the first plausible body/opening.
	// This is the recurrent dale.mp4 failure mode: "resolution, resolution, development..."
	// is not a valid Q&A opening, it is usually the tail of the previous topic.
	for (int i = 0; i + 1 < static_cast<int>(chunks.size()); ++i) {
		if (looksLikePreviousConclusionBeforeBody(chunks, i))
			chunks[i].role = ArcChunkRole::PreviousConclusion;
	}

	// Second pass: if a very short follow-through after a resolution was classified as
	// development, treat it as conclusion continuation. This avoids cutting 2-4 seconds
	// before the natural closing phrase of a viewer answer.
	for (int i = 1; i < static_cast<int>(chunks.size()); ++i) {
		if (chunks.at(i).role != ArcChunkRole::Development)
			continue;
		const ArcChunk &previous = chunks.at(i - 1);
		const ArcChunk &current = chunks.at(i);
		const double durationSec = std::max(0.0, current.endSec - current.startSec);
		const double pauseSec = std::max(previous.pauseAfterSec, current.pauseBeforeSec);
		const double continuity = cosineSimilarity(previous.embedding, current.embedding);
		const double currentPositive = std::max({current.value, current.target, current.resolution, current.hook});
		const double currentDefect = std::max({current.meta, current.shift, current.defectScore});
		const bool followsResolution = previous.role == ArcChunkRole::LocalResolution ||
			previous.role == ArcChunkRole::Development || previous.conclusionScore >= 0.48 || previous.resolution >= 0.54;
		const bool shortSameAnswerTail = durationSec <= 8.5 && pauseSec <= 3.65 &&
			(continuity >= 0.30 || pauseSec <= 1.15 || isModeratePauseSameViewerAnswer(previous, current)) &&
			currentPositive >= currentDefect - 0.10;
		const bool conclusionSignalCompetitive = current.conclusionScore >= current.developmentScore - 0.07 ||
			current.resolution >= 0.48 || current.value >= 0.54;
		if (followsResolution && shortSameAnswerTail && conclusionSignalCompetitive)
			chunks[i].role = ArcChunkRole::LocalResolution;
	}
}

static bool canDevelopmentServeAsExplicitOpening(const ArcChunk &chunk)
{
	if (chunk.role != ArcChunkRole::Development)
		return false;
	const double opening = std::max(chunk.openingScore, chunk.hook);
	const double positive = std::max({chunk.target, chunk.value, chunk.hook, chunk.developmentScore});
	const double defect = std::max({chunk.meta, chunk.shift, chunk.defectScore});

	// A development-looking first block may serve as the opening only when it also
	// carries an explicit hook/context signal. This intentionally removes the old
	// implicit-opening path where any mid-answer body could start a candidate.
	return (chunk.explicitOpeningCue >= 0.54 || (opening >= 0.66 && chunk.hook >= 0.66)) &&
		opening >= chunk.conclusionScore + 0.02 &&
		positive >= defect + 0.04 &&
		chunk.meta < opening + 0.04 && chunk.explicitMetaCue < 0.62;
}

static bool canUseSemanticHookAsOpening(const ArcChunk &chunk)
{
	if (isBlockingArcRole(chunk.role) || chunk.role == ArcChunkRole::LocalResolution ||
	    chunk.role == ArcChunkRole::PreviousConclusion)
		return false;

	const double opening = std::max(chunk.openingScore, chunk.hook);
	const double positive = std::max({chunk.target, chunk.value, chunk.hook, chunk.developmentScore,
		chunk.explicitDevelopmentCue});
	const double defect = std::max({chunk.meta, chunk.shift, chunk.defectScore, chunk.explicitMetaCue,
		chunk.explicitShiftCue});
	const double conclusion = std::max(chunk.conclusionScore, chunk.resolution);

	// This is not the old implicit opening. It is a controlled fallback for real
	// spoken hooks that do not contain lexical question words after Whisper
	// segmentation. It must be clearly hook/value backed and not look like a
	// conclusion, meta-chat, or topic transition.
	const bool strongHook = opening >= 0.64 && std::max(chunk.hook, chunk.contextOpeningScore) >= 0.64;
	const bool targetBackedHook = chunk.target >= 0.60 && chunk.value >= 0.56 && chunk.hook >= 0.58;
	const bool bodyBackedHook = chunk.developmentScore >= 0.46 && chunk.value >= 0.58 && opening >= 0.58;
	const bool notAConclusion = conclusion <= opening + 0.035 || chunk.explicitConclusionCue < 0.45;
	const bool cleanEnough = defect <= positive + 0.04 && chunk.explicitMetaCue < 0.58 &&
		chunk.explicitShiftCue < 0.58 && chunk.shift < positive + 0.06;

	return (strongHook || targetBackedHook || bodyBackedHook) && notAConclusion && cleanEnough;
}

static bool canBeContextualOpening(const ArcChunk &chunk)
{
	if (isBlockingArcRole(chunk.role) || chunk.role == ArcChunkRole::LocalResolution)
		return false;
	const double opening = std::max({chunk.openingScore, chunk.hook, chunk.contextOpeningScore});
	const double positive = std::max({chunk.target, chunk.value, chunk.hook, chunk.developmentScore,
		chunk.contextDevelopmentScore, chunk.explicitOpeningCue});
	const double defect = std::max({chunk.meta, chunk.shift, chunk.defectScore, chunk.explicitMetaCue,
		chunk.contextMetaScore, chunk.contextNewTopicScore});
	if (chunk.role == ArcChunkRole::Development)
		return canDevelopmentServeAsExplicitOpening(chunk);

	const bool explicitOpening = chunk.explicitOpeningCue >= 0.48;
	const bool strongSemanticHook = opening >= 0.66 && std::max(chunk.hook, chunk.contextOpeningScore) >= 0.66 &&
		positive >= defect + 0.07 && chunk.explicitMetaCue < 0.60 && chunk.contextMetaScore < opening + 0.05;
	return (chunk.role == ArcChunkRole::OpeningCandidate && (explicitOpening || strongSemanticHook)) ||
		canUseSemanticHookAsOpening(chunk);
}


static bool canBeContextualDevelopment(const ArcChunk &chunk)
{
	if (isBlockingArcRole(chunk.role))
		return false;
	const double positive = std::max({chunk.value, chunk.target, chunk.developmentScore, chunk.hook,
		chunk.explicitDevelopmentCue});
	const double defect = std::max({chunk.meta, chunk.shift, chunk.defectScore, chunk.explicitMetaCue});
	if (defect >= positive + 0.10)
		return false;
	return chunk.role == ArcChunkRole::Development ||
		(chunk.role == ArcChunkRole::OpeningCandidate && chunk.value >= 0.50 && chunk.explicitMetaCue < 0.60) ||
		(chunk.explicitDevelopmentCue >= 0.48 && positive >= defect - 0.04) ||
		(positive >= 0.62 && chunk.developmentScore >= chunk.conclusionScore - 0.08 && positive >= defect + 0.02);
}


static bool canBeContextualConclusion(const QVector<ArcChunk> &chunks, int first, int index)
{
	if (index < first || index >= static_cast<int>(chunks.size()))
		return false;
	const ArcChunk &chunk = chunks.at(index);
	if (isBlockingArcRole(chunk.role))
		return false;
	const double endingDefect = std::max({chunk.meta, chunk.shift, chunk.defectScore, chunk.explicitMetaCue,
		chunk.explicitShiftCue, chunk.contextMetaScore, chunk.contextNewTopicScore});
	const double endingSignal = std::max({chunk.conclusionScore, chunk.resolution, chunk.explicitConclusionCue,
		chunk.contextConclusionScore});
	const bool explicitConclusion = chunk.explicitConclusionCue >= 0.56;
	const bool directConclusion = explicitConclusion || chunk.role == ArcChunkRole::LocalResolution ||
		(chunk.contextConclusionScore >= 0.58 && chunk.contextConclusionScore >= chunk.contextDevelopmentScore - 0.02) ||
		(chunk.conclusionScore >= 0.50 && chunk.resolution >= 0.54 && endingSignal >= endingDefect - 0.06);
	if (directConclusion && endingDefect < endingSignal + 0.13)
		return true;

	// A final body chunk can be the natural closing phrase if it is short and follows a
	// local resolution from the same answer.
	if (index > first && chunk.role == ArcChunkRole::Development) {
		const ArcChunk &previous = chunks.at(index - 1);
		const double durationSec = std::max(0.0, chunk.endSec - chunk.startSec);
		const double pauseSec = std::max(previous.pauseAfterSec, chunk.pauseBeforeSec);
		const double continuity = cosineSimilarity(previous.embedding, chunk.embedding);
		const bool followsConclusion = previous.role == ArcChunkRole::LocalResolution ||
			previous.conclusionScore >= 0.50 || previous.resolution >= 0.56 || previous.explicitConclusionCue >= 0.56;
		return followsConclusion && durationSec <= 6.5 && pauseSec <= 1.35 &&
			(continuity >= 0.34 || pauseSec <= 0.70) && endingSignal >= endingDefect - 0.08;
	}
	return false;
}


static ContextualArcScore contextualStateMachineScore(const QVector<ArcChunk> &chunks, int first, int last,
	const ExchangeArcBoundaryRefinementOptions &options)
{
	ContextualArcScore score;
	if (options.scoring.presetId != QStringLiteral("viewer_message_response")) {
		score.valid = true;
		score.score = 0.62;
		return score;
	}
	if (first < 0 || last < first || last >= static_cast<int>(chunks.size()))
		return score;

	if (!canBeContextualOpening(chunks.at(first))) {
		score.penalty = 0.52;
		score.reason = QStringLiteral("missing_explicit_opening");
		return score;
	}
	if (!hasReliableViewerCueNearStart(chunks, first, last)) {
		score.penalty = 0.48;
		score.reason = QStringLiteral("missing_viewer_message_cue");
		return score;
	}
	const int nextViewerCue = nextViewerCueInsideArc(chunks, first, last);
	if (nextViewerCue > first) {
		score.penalty = 0.58;
		score.reason = QStringLiteral("multiple_viewer_messages_inside_arc");
		return score;
	}
	score.semanticOpeningFallback = chunks.at(first).explicitOpeningCue < 0.48 &&
		canUseSemanticHookAsOpening(chunks.at(first));

	bool sawDevelopment = false;
	bool sawConclusion = false;
	int developmentCount = 0;
	int conclusionCount = 0;
	int firstConclusionIndex = -1;
	int conclusionIndex = -1;
	for (int i = first; i <= last; ++i) {
		const ArcChunk &chunk = chunks.at(i);
		if (isBlockingArcRole(chunk.role)) {
			score.penalty = 0.56;
			score.reason = QStringLiteral("blocking_role_inside_arc");
			return score;
		}

		if (i == first) {
			score.openingIndex = i;
			continue;
		}

		const bool isConclusion = canBeContextualConclusion(chunks, first, i);
		const bool isDevelopment = canBeContextualDevelopment(chunk);
		if (sawConclusion && isDevelopment && !isConclusion) {
			score.penalty = 0.48;
			score.reason = QStringLiteral("development_after_conclusion");
			return score;
		}
		if (isConclusion) {
			if (firstConclusionIndex < 0)
				firstConclusionIndex = i;
			conclusionIndex = i;
			++conclusionCount;
			sawConclusion = true;
			continue;
		}
		if (isDevelopment) {
			sawDevelopment = true;
			if (score.firstDevelopmentIndex < 0)
				score.firstDevelopmentIndex = i;
			++developmentCount;
			continue;
		}
		score.penalty = 0.34;
		score.reason = QStringLiteral("unclassified_arc_block");
		return score;
	}

	const double spanDurationSec = chunks.at(last).endSec - chunks.at(first).startSec;
	const bool openingHasBody = chunks.at(first).developmentScore >= 0.46 || chunks.at(first).value >= 0.56 ||
		chunks.at(first).explicitDevelopmentCue >= 0.40;
	if (!sawDevelopment && spanDurationSec <= 24.0 && openingHasBody)
		sawDevelopment = true;
	if (!sawDevelopment) {
		score.penalty = 0.42;
		score.reason = QStringLiteral("missing_development");
		return score;
	}
	if (!canBeContextualConclusion(chunks, first, last)) {
		score.penalty = 0.42;
		score.reason = QStringLiteral("missing_conclusion");
		return score;
	}
	if (firstConclusionIndex >= 0) {
		const double conclusionPlateauSec = chunks.at(last).endSec - chunks.at(firstConclusionIndex).startSec;
		if (developmentCount <= 1 && conclusionCount >= 3 && conclusionPlateauSec > 14.0) {
			score.penalty = 0.46;
			score.reason = QStringLiteral("resolution_plateau_without_body");
			return score;
		}
	}
	if (score.semanticOpeningFallback && (spanDurationSec > 42.0 || developmentCount <= 0)) {
		score.penalty = 0.34;
		score.reason = QStringLiteral("weak_semantic_opening_span");
		return score;
	}
	if (spanDurationSec > 68.0) {
		score.penalty = 0.44;
		score.reason = QStringLiteral("overlong_contextual_arc");
		return score;
	}
	if (last + 1 < static_cast<int>(chunks.size()) && isViewerAnswerContinuation(chunks, last, last + 1) &&
	    chunks.at(last + 1).endSec - chunks.at(first).startSec <= 72.0) {
		score.penalty = 0.30;
		score.reason = QStringLiteral("unfinished_viewer_answer");
		return score;
	}

	const ArcChunk &opening = chunks.at(first);
	const ArcChunk &ending = chunks.at(last);
	score.opening = boundedScore(std::max({opening.openingScore, opening.hook, opening.explicitOpeningCue,
		opening.contextOpeningScore}));
	score.development = std::max(averageScore(chunks, first, last, &ArcChunk::developmentScore),
		averageScore(chunks, first, last, &ArcChunk::contextDevelopmentScore));
	if (developmentCount > 0 && score.firstDevelopmentIndex >= 0)
		score.development = std::max(score.development,
			averageScore(chunks, score.firstDevelopmentIndex, std::max(score.firstDevelopmentIndex, last - 1),
				&ArcChunk::developmentScore));
	score.conclusion = boundedScore(std::max({ending.conclusionScore, ending.resolution, ending.explicitConclusionCue,
		ending.contextConclusionScore}));
	const double cohesion = cohesionScore(chunks, first, last);
	const double defect = averageScore(chunks, first, last, &ArcChunk::defectScore);
	if (score.opening < 0.48 || score.development < 0.38 || score.conclusion < 0.48 || cohesion < 0.34) {
		score.penalty = 0.34;
		score.reason = QStringLiteral("weak_contextual_arc_scores");
		return score;
	}

	score.valid = true;
	score.implicitOpening = false;
	score.terminalFollowThrough = ending.role == ArcChunkRole::Development;
	score.conclusionIndex = conclusionIndex >= 0 ? conclusionIndex : last;
	score.score = boundedScore((score.opening * 0.31) + (score.development * 0.30) +
		(score.conclusion * 0.31) + (cohesion * 0.13) - (defect * 0.12) -
		(score.semanticOpeningFallback ? 0.055 : 0.0));
	return score;
}
struct RecoveredSubspan {
	bool valid = false;
	int first = -1;
	int last = -1;
	ArcSpanScore span;
	double stateMachineScore = 0.0;
	double opening = 0.0;
	double development = 0.0;
	double conclusion = 0.0;
	QString reason;
	QString graphEvidence;
	bool semanticOpening = false;
};

static bool canStartRecoveredSubspan(const ArcChunk &chunk)
{
	if (isBlockingArcRole(chunk.role) || chunk.role == ArcChunkRole::LocalResolution ||
	    chunk.role == ArcChunkRole::PreviousConclusion)
		return false;

	const double opening = std::max(chunk.openingScore, chunk.hook);
	const double positive = std::max({chunk.target, chunk.value, chunk.hook, chunk.developmentScore,
		chunk.explicitOpeningCue, chunk.explicitDevelopmentCue});
	const double defect = std::max({chunk.meta, chunk.shift, chunk.defectScore,
		chunk.explicitMetaCue, chunk.explicitShiftCue});
	const double conclusion = std::max(chunk.conclusionScore, chunk.resolution);
	const bool explicitContext = chunk.explicitOpeningCue >= 0.44;
	const bool semanticHook = opening >= 0.64 && std::max(chunk.hook, chunk.contextOpeningScore) >= 0.62 && chunk.value >= 0.54;
	const bool targetBacked = chunk.target >= 0.60 && chunk.value >= 0.55 && opening >= 0.55;
	const bool notConclusionTail = conclusion <= opening + 0.08 || chunk.explicitConclusionCue < 0.42;
	const bool notMeta = defect <= positive + 0.055 && chunk.explicitMetaCue < 0.60 && chunk.explicitShiftCue < 0.60;
	return (explicitContext || semanticHook || targetBacked || canUseSemanticHookAsOpening(chunk) ||
		canDevelopmentServeAsExplicitOpening(chunk)) && notConclusionTail && notMeta;
}

static bool canContinueRecoveredSubspan(const ArcChunk &chunk)
{
	if (isBlockingArcRole(chunk.role))
		return false;
	const double positive = std::max({chunk.value, chunk.target, chunk.developmentScore, chunk.hook,
		chunk.resolution, chunk.explicitDevelopmentCue});
	const double defect = std::max({chunk.meta, chunk.shift, chunk.defectScore,
		chunk.explicitMetaCue, chunk.explicitShiftCue});
	const bool conclusionLike = chunk.role == ArcChunkRole::LocalResolution ||
		chunk.resolution >= 0.52 || chunk.conclusionScore >= 0.42 || chunk.contextConclusionScore >= 0.52 ||
		chunk.explicitConclusionCue >= 0.46;
	return positive >= defect - 0.08 && (canBeContextualDevelopment(chunk) || conclusionLike ||
		chunk.value >= 0.52 || chunk.developmentScore >= 0.36);
}

static bool canEndRecoveredSubspan(const QVector<ArcChunk> &chunks, int first, int last)
{
	if (last <= first || last >= static_cast<int>(chunks.size()))
		return false;
	const ArcChunk &ending = chunks.at(last);
	if (isBlockingArcRole(ending.role) || isTailRisk(chunks, last))
		return false;
	const double signal = std::max({ending.conclusionScore, ending.resolution, ending.explicitConclusionCue});
	const double defect = std::max({ending.meta, ending.shift, ending.defectScore,
		ending.explicitMetaCue, ending.explicitShiftCue});
	if (canBeContextualConclusion(chunks, first, last) && signal >= defect - 0.12)
		return true;
	return (ending.role == ArcChunkRole::LocalResolution || ending.resolution >= 0.60 ||
		ending.conclusionScore >= 0.49 || ending.contextConclusionScore >= 0.56 ||
		ending.explicitConclusionCue >= 0.50) && signal >= defect - 0.08;
}

static double contextualOpeningSignal(const ArcChunk &chunk)
{
	const double context = std::max(chunk.contextOpeningScore, chunk.openingScore);
	const double semantic = std::max({chunk.hook, chunk.target * 0.92, chunk.value * 0.72});
	const double explicitCue = chunk.explicitOpeningCue;
	const double defect = std::max({chunk.contextPreviousTopicScore * 0.72, chunk.contextMetaScore * 0.82,
		chunk.contextNewTopicScore * 0.82, chunk.meta * 0.66, chunk.shift * 0.70, chunk.explicitMetaCue,
		chunk.explicitShiftCue});
	const double conclusion = std::max({chunk.contextConclusionScore, chunk.conclusionScore, chunk.resolution});
	return boundedScore(std::max({context, semantic, explicitCue}) - (defect * 0.18) - (conclusion * 0.07));
}

static double contextualDevelopmentSignal(const ArcChunk &chunk)
{
	const double context = std::max(chunk.contextDevelopmentScore, chunk.developmentScore);
	const double semantic = std::max({chunk.value, chunk.target * 0.82, chunk.hook * 0.54,
		chunk.explicitDevelopmentCue});
	const double defect = std::max({chunk.contextMetaScore, chunk.contextNewTopicScore, chunk.meta, chunk.shift,
		chunk.explicitMetaCue, chunk.explicitShiftCue});
	return boundedScore(std::max(context, semantic) - (defect * 0.14));
}

static double contextualConclusionSignal(const ArcChunk &chunk)
{
	const double context = std::max(chunk.contextConclusionScore, chunk.conclusionScore);
	const double semantic = std::max({chunk.resolution, chunk.value * 0.64, chunk.explicitConclusionCue});
	const double defect = std::max({chunk.contextMetaScore, chunk.contextNewTopicScore, chunk.meta, chunk.shift,
		chunk.explicitMetaCue, chunk.explicitShiftCue});
	return boundedScore(std::max(context, semantic) - (defect * 0.14));
}

static double contextualBadSignal(const ArcChunk &chunk)
{
	const double positive = std::max({chunk.contextOpeningScore, chunk.contextDevelopmentScore,
		chunk.contextConclusionScore, chunk.openingScore, chunk.developmentScore, chunk.conclusionScore,
		chunk.target, chunk.value, chunk.hook, chunk.resolution});
	const double defect = std::max({chunk.contextPreviousTopicScore, chunk.contextMetaScore,
		chunk.contextNewTopicScore, chunk.meta, chunk.shift, chunk.defectScore, chunk.explicitMetaCue,
		chunk.explicitShiftCue});
	return boundedScore(defect - (positive * 0.10));
}

static bool hasForwardContextualArcSupport(const QVector<ArcChunk> &chunks, int first)
{
	if (first < 0 || first + 1 >= static_cast<int>(chunks.size()))
		return false;

	bool sawDevelopment = false;
	double bestDevelopment = 0.0;
	double bestConclusion = 0.0;
	const double maxLookaheadSec = 46.0;
	for (int i = first + 1; i < static_cast<int>(chunks.size()); ++i) {
		if (chunks.at(i).endSec - chunks.at(first).startSec > maxLookaheadSec)
			break;
		if (i > first + 1 && isPauseSeparatedSemanticTurn(chunks.at(i - 1), chunks.at(i)))
			break;
		if (chunks.at(i).role == ArcChunkRole::TailOrNewTurn || chunks.at(i).role == ArcChunkRole::SocialOrMetaPrelude)
			break;

		const double development = contextualDevelopmentSignal(chunks.at(i));
		const double conclusion = contextualConclusionSignal(chunks.at(i));
		const double positive = std::max({development, conclusion, chunks.at(i).value, chunks.at(i).target,
			chunks.at(i).hook, chunks.at(i).resolution});
		const double defect = std::max({chunks.at(i).contextMetaScore, chunks.at(i).contextNewTopicScore,
			chunks.at(i).meta, chunks.at(i).shift, chunks.at(i).explicitMetaCue, chunks.at(i).explicitShiftCue});
		if (defect >= positive + 0.20)
			break;

		bestDevelopment = std::max(bestDevelopment, development);
		bestConclusion = std::max(bestConclusion, conclusion);
		if (development >= conclusion - 0.08 || chunks.at(i).role == ArcChunkRole::Development ||
		    chunks.at(i).value >= 0.50)
			sawDevelopment = true;
		if (sawDevelopment && (canBeContextualConclusion(chunks, first, i) ||
		    conclusion >= 0.43 || chunks.at(i).resolution >= 0.56))
			return true;
	}

	return sawDevelopment && bestDevelopment >= 0.34 && bestConclusion >= 0.40;
}

static bool canStartContextualDpSubspan(const QVector<ArcChunk> &chunks, int first)
{
	if (first < 0 || first >= static_cast<int>(chunks.size()))
		return false;
	const ArcChunk &chunk = chunks.at(first);
	if (chunk.role == ArcChunkRole::SocialOrMetaPrelude || chunk.role == ArcChunkRole::TailOrNewTurn)
		return false;
	if (!looksLikeViewerOriginCue(chunk))
		return false;

	const double opening = contextualOpeningSignal(chunk);
	const double bad = contextualBadSignal(chunk);
	const double conclusion = contextualConclusionSignal(chunk);
	const bool forwardSupport = hasForwardContextualArcSupport(chunks, first);
	const bool explicitOpening = chunk.explicitOpeningCue >= 0.42 || chunk.role == ArcChunkRole::OpeningCandidate;
	const double hardDefect = std::max({chunk.contextMetaScore, chunk.contextNewTopicScore,
		chunk.meta, chunk.shift, chunk.explicitMetaCue, chunk.explicitShiftCue});
	const double previousTopic = chunk.contextPreviousTopicScore;
	const double semanticPositive = std::max({chunk.hook, chunk.value, chunk.target, opening});
	const bool contextualOpening = chunk.contextOpeningScore >= 0.46 && forwardSupport &&
		(chunk.explicitOpeningCue >= 0.42 || chunk.contextOpeningScore >= hardDefect - 0.06 ||
		 chunk.hook >= 0.58 || chunk.value >= 0.52 || chunk.target >= 0.54);
	const bool semanticOpening = chunk.hook >= 0.55 && chunk.value >= 0.44 && forwardSupport &&
		semanticPositive >= hardDefect - 0.10;
	const bool targetOpening = chunk.target >= 0.52 && chunk.value >= 0.44 && opening >= 0.40 && forwardSupport;
	const bool resolutionCanBeViewerQuestion = chunk.role == ArcChunkRole::LocalResolution && forwardSupport &&
		chunk.explicitOpeningCue >= 0.42 &&
		std::max({chunk.hook, chunk.target, chunk.value, chunk.contextOpeningScore}) >= hardDefect - 0.06 &&
		chunk.explicitConclusionCue < 0.52;
	const bool previousTopicAllowed = previousTopic <= opening + 0.28 || explicitOpening || semanticOpening ||
		targetOpening || resolutionCanBeViewerQuestion;
	const bool notOldConclusion = conclusion <= opening + (forwardSupport ? 0.26 : 0.14) || explicitOpening ||
		semanticOpening || targetOpening || resolutionCanBeViewerQuestion || chunk.explicitConclusionCue < 0.40;
	const bool cleanEnough = bad <= opening + 0.24 && hardDefect <= semanticPositive + 0.24 &&
		chunk.explicitMetaCue < 0.70 && chunk.explicitShiftCue < 0.70;

	return opening >= 0.34 && cleanEnough && notOldConclusion && previousTopicAllowed &&
		(explicitOpening || contextualOpening || semanticOpening || targetOpening || resolutionCanBeViewerQuestion);
}

static bool canUseContextualDpBodyBlock(const ArcChunk &chunk)
{
	if (chunk.role == ArcChunkRole::SocialOrMetaPrelude || chunk.role == ArcChunkRole::TailOrNewTurn)
		return false;
	const double positive = std::max({contextualDevelopmentSignal(chunk), contextualConclusionSignal(chunk),
		chunk.target, chunk.value, chunk.hook, chunk.resolution});
	const double bad = contextualBadSignal(chunk);
	return positive >= bad - 0.12 && chunk.explicitMetaCue < 0.70 && chunk.explicitShiftCue < 0.70;
}


struct ViewerOriginDfsResult {
	bool valid = false;
	int index = -1;
	double score = 0.0;
	int depth = 0;
};

struct ViewerWindowDfsState {
	int index = -1;
	int depth = 0;
};

static bool canUseViewerDfsSeed(const ArcChunk &chunk)
{
	if (isBlockingArcRole(chunk.role))
		return false;
	const double positive = std::max({chunk.target, chunk.value, chunk.hook, chunk.resolution,
		chunk.openingScore, chunk.developmentScore, chunk.conclusionScore,
		chunk.contextOpeningScore, chunk.contextDevelopmentScore, chunk.contextConclusionScore});
	const double defect = std::max({chunk.meta, chunk.shift, chunk.defectScore,
		chunk.explicitMetaCue, chunk.explicitShiftCue, chunk.contextMetaScore, chunk.contextNewTopicScore});
	return positive >= defect - 0.08 &&
		(chunk.role == ArcChunkRole::OpeningCandidate || chunk.role == ArcChunkRole::Development ||
		 chunk.role == ArcChunkRole::LocalResolution || chunk.target >= 0.52 || chunk.value >= 0.50 ||
		 chunk.hook >= 0.52 || chunk.resolution >= 0.54);
}

static bool isHardWindowTopicDrift(const ArcChunk &previous, const ArcChunk &next)
{
	const double continuity = cosineSimilarity(previous.embedding, next.embedding);
	const double previousPositive = std::max({previous.target, previous.value, previous.hook, previous.resolution,
		previous.developmentScore, previous.conclusionScore, previous.contextDevelopmentScore,
		previous.contextConclusionScore});
	const double nextPositive = std::max({next.target, next.value, next.hook, next.resolution,
		next.developmentScore, next.conclusionScore, next.contextDevelopmentScore, next.contextConclusionScore});
	const double nextDefect = std::max({next.meta, next.shift, next.defectScore, next.explicitMetaCue,
		next.explicitShiftCue, next.contextMetaScore, next.contextNewTopicScore});
	const double previousDefect = std::max({previous.meta, previous.shift, previous.defectScore,
		previous.explicitMetaCue, previous.explicitShiftCue, previous.contextMetaScore, previous.contextNewTopicScore});
	const double pauseSec = std::max(previous.pauseAfterSec, next.pauseBeforeSec);
	const bool nextLooksNewTopic = next.contextNewTopicScore >= nextPositive + 0.10 ||
		next.shift >= nextPositive + 0.08 || next.explicitShiftCue >= 0.58;
	const bool nextLooksMeta = next.contextMetaScore >= nextPositive + 0.12 || next.meta >= nextPositive + 0.12 ||
		next.explicitMetaCue >= 0.62;
	const bool previousLooksClosedOrDifferent = previous.role == ArcChunkRole::TailOrNewTurn ||
		previous.contextNewTopicScore >= previousPositive + 0.12 || previousDefect >= previousPositive + 0.20;

	if (next.role == ArcChunkRole::TailOrNewTurn || next.role == ArcChunkRole::SocialOrMetaPrelude)
		return true;
	if ((nextLooksNewTopic || nextLooksMeta) && continuity < 0.42)
		return true;
	if (previousLooksClosedOrDifferent && continuity < 0.38 && pauseSec > 1.8)
		return true;
	if (pauseSec > 4.2 && continuity < 0.34 && nextDefect >= nextPositive - 0.02)
		return true;
	if (pauseSec > 6.0 && continuity < 0.48)
		return true;
	return false;
}

static bool canDfsCrossBackwardWindow(const QVector<ArcChunk> &chunks, int previousIndex, int currentIndex)
{
	if (previousIndex < 0 || currentIndex <= previousIndex || currentIndex >= static_cast<int>(chunks.size()))
		return false;
	const ArcChunk &previous = chunks.at(previousIndex);
	const ArcChunk &current = chunks.at(currentIndex);
	if (looksLikeViewerOriginCueAt(chunks, previousIndex))
		return true;
	if (looksLikeAnyViewerMessageTurn(previous))
		return false;
	if (previous.role == ArcChunkRole::TailOrNewTurn)
		return false;
	if (isHardWindowTopicDrift(previous, current))
		return false;

	const double continuity = cosineSimilarity(previous.embedding, current.embedding);
	const double previousPositive = std::max({previous.target, previous.value, previous.hook, previous.resolution,
		previous.developmentScore, previous.conclusionScore});
	const double previousDefect = std::max({previous.meta, previous.shift, previous.defectScore,
		previous.explicitMetaCue, previous.explicitShiftCue});
	const double pauseSec = std::max(previous.pauseAfterSec, current.pauseBeforeSec);
	if (previous.role == ArcChunkRole::SocialOrMetaPrelude && previousDefect >= previousPositive + 0.12)
		return false;
	if (pauseSec > 5.8 && continuity < 0.24 && previousPositive < previousDefect + 0.08)
		return false;
	return true;
}

static ViewerOriginDfsResult findViewerOriginByWindowDfs(const QVector<ArcChunk> &chunks, int seed,
	const ExchangeArcBoundaryRefinementOptions &options)
{
	ViewerOriginDfsResult best;
	if (options.scoring.presetId != QStringLiteral("viewer_message_response") || seed < 0 ||
	    seed >= static_cast<int>(chunks.size()))
		return best;

	const int maxDepth = VIEWER_DFS_MAX_LOOKBACK_WINDOWS;
	const double maxOriginToSeedSec = std::min(
		std::max(options.generation.boundaryMinDurationSec, options.generation.maxDurationSec),
		VIEWER_DFS_MAX_LOOKBACK_SEC);
	QVector<ViewerWindowDfsState> stack;
	stack.append(ViewerWindowDfsState{seed, 0});
	QVector<int> bestDepthAt(chunks.size(), 9999);

	while (!stack.isEmpty()) {
		const ViewerWindowDfsState state = stack.takeLast();
		if (state.index < 0 || state.index >= static_cast<int>(chunks.size()) || state.depth > maxDepth)
			continue;
		if (state.depth >= bestDepthAt.at(state.index))
			continue;
		bestDepthAt[state.index] = state.depth;

		const double originToSeedSec = chunks.at(seed).endSec - chunks.at(state.index).startSec;
		if (originToSeedSec > maxOriginToSeedSec + 0.1)
			continue;
		if (looksLikeViewerOriginCueAt(chunks, state.index)) {
			const int cueStart = viewerOriginStartIndexAt(chunks, state.index);
			const double cue = viewerOriginCueScoreAt(chunks, state.index);
			const double forward = hasForwardContextualArcSupport(chunks, cueStart) ? 0.16 : 0.0;
			const double distancePenalty = static_cast<double>(state.depth) * 0.025;
			const double score = boundedScore(cue + forward - distancePenalty);
			if (!best.valid || score > best.score + 0.010 ||
			    (std::fabs(score - best.score) <= 0.010 && cueStart > best.index)) {
				best.valid = true;
				best.index = cueStart;
				best.score = score;
				best.depth = state.depth;
			}
			// Keep searching a little farther back: sometimes Whisper splits a viewer
			// attribution and the actual problem into adjacent windows. The score tie-breaker
			// will still prefer the closest strong origin.
		}

		if (state.depth >= maxDepth || state.index <= 0)
			continue;
		const int previous = state.index - 1;
		if (chunks.at(seed).endSec - chunks.at(previous).startSec > maxOriginToSeedSec + 0.1)
			continue;
		if (!canDfsCrossBackwardWindow(chunks, previous, state.index))
			continue;
		stack.append(ViewerWindowDfsState{previous, state.depth + 1});
	}
	return best;
}

static bool canDfsFollowViewerAnswerWindow(const QVector<ArcChunk> &chunks, int currentIndex, int nextIndex)
{
	if (currentIndex < 0 || nextIndex <= currentIndex || nextIndex >= static_cast<int>(chunks.size()))
		return false;
	const ArcChunk &current = chunks.at(currentIndex);
	const ArcChunk &next = chunks.at(nextIndex);
	if (looksLikeAnyViewerMessageTurn(next))
		return false;
	if (next.role == ArcChunkRole::TailOrNewTurn || next.role == ArcChunkRole::SocialOrMetaPrelude)
		return false;
	const bool moderateSameAnswer = isModeratePauseSameViewerAnswer(current, next);
	if ((isPauseSeparatedSemanticTurn(current, next) || isTailRisk(chunks, nextIndex)) && !moderateSameAnswer)
		return false;
	if (isHardWindowTopicDrift(current, next) && !moderateSameAnswer)
		return false;

	const double continuity = cosineSimilarity(current.embedding, next.embedding);
	const double nextPositive = std::max({next.target, next.value, next.hook, next.resolution,
		next.developmentScore, next.conclusionScore, next.contextDevelopmentScore, next.contextConclusionScore});
	const double nextDefect = std::max({next.meta, next.shift, next.defectScore,
		next.explicitMetaCue, next.explicitShiftCue, next.contextMetaScore, next.contextNewTopicScore});
	const double pauseSec = std::max(current.pauseAfterSec, next.pauseBeforeSec);
	const bool answerLike = canUseContextualDpBodyBlock(next) || canBeContextualDevelopment(next) ||
		canBeContextualConclusion(chunks, std::max(0, currentIndex - 4), nextIndex) || next.value >= 0.48 ||
		next.target >= 0.50 || next.resolution >= 0.50;
	const bool connected = continuity >= 0.24 || pauseSec <= 1.25 || isModeratePauseSameViewerAnswer(current, next) ||
		next.target >= nextDefect - 0.04;
	return answerLike && connected && nextDefect <= nextPositive + 0.20;
}

static bool canEndViewerDfsArc(const QVector<ArcChunk> &chunks, int first, int last,
	const ExchangeArcBoundaryRefinementOptions &options)
{
	if (!canUseSpan(chunks, first, last, options) || last <= first)
		return false;
	const ArcChunk &ending = chunks.at(last);
	if (isBlockingArcRole(ending.role) || isTailRisk(chunks, last))
		return false;
	const double conclusion = contextualConclusionSignal(ending);
	const double defect = contextualBadSignal(ending);
	const bool conclusionLike = canBeContextualConclusion(chunks, first, last) || ending.role == ArcChunkRole::LocalResolution ||
		ending.resolution >= 0.54 || ending.conclusionScore >= 0.46 || ending.explicitConclusionCue >= 0.46;
	if (!conclusionLike || conclusion < defect - 0.10)
		return false;
	if (last + 1 < static_cast<int>(chunks.size()) && !looksLikeAnyViewerMessageTurn(chunks.at(last + 1)) &&
	    canDfsFollowViewerAnswerWindow(chunks, last, last + 1) &&
	    chunks.at(last + 1).endSec - chunks.at(first).startSec <= std::min(options.generation.maxDurationSec, VIEWER_DFS_MAX_FORWARD_SEC))
		return false;
	return true;
}

static ArcSpanScore scoreViewerDfsArc(const QVector<ArcChunk> &chunks, int first, int last,
	const ExchangeArcBoundaryRefinementOptions &options)
{
	ArcSpanScore score = scoreSpan(chunks, first, last, options);
	if (!canUseSpan(chunks, first, last, options))
		return score;

	const ArcChunk &opening = chunks.at(first);
	const ArcChunk &ending = chunks.at(last);
	const double openingScore = std::max({score.opening, viewerOriginCueScoreAt(chunks, first), contextualOpeningSignal(opening),
		opening.openingScore, opening.hook, opening.explicitOpeningCue});
	const double developmentScore = std::max({score.development,
		averageScore(chunks, first, last, &ArcChunk::developmentScore),
		averageScore(chunks, first, last, &ArcChunk::contextDevelopmentScore),
		maxScore(chunks, first, last, &ArcChunk::value)});
	const double conclusionScore = std::max({score.conclusion, contextualConclusionSignal(ending), ending.conclusionScore,
		ending.resolution, ending.explicitConclusionCue});
	const double cohesion = std::max(score.cohesion, cohesionScore(chunks, first, last));
	const double bad = std::max(contextualBadSignal(opening), averageScore(chunks, first, last, &ArcChunk::defectScore));
	const double tailRisk = isTailRisk(chunks, last) ? 0.78 : std::max(0.0, contextualBadSignal(ending) * 0.56);

	score.first = first;
	score.last = last;
	score.opening = boundedScore(openingScore);
	score.development = boundedScore(developmentScore);
	score.conclusion = boundedScore(conclusionScore);
	score.cohesion = boundedScore(cohesion);
	score.tailRisk = tailRisk;
	score.boundaryCleanliness = boundedScore((score.opening * 0.36) + (score.conclusion * 0.38) +
		(score.cohesion * 0.16) + 0.08 - (tailRisk * 0.08));
	score.score = boundedScore(std::max(score.score,
		(score.opening * 0.29) + (score.development * 0.28) + (score.conclusion * 0.30) +
		(score.cohesion * 0.16) - (bad * 0.10) - (tailRisk * 0.06)));
	return score;
}

static RecoveredSubspan recoverViewerMessageArcByWindowDfs(const QVector<ArcChunk> &chunks,
	const ExchangeArcBoundaryRefinementOptions &options)
{
	RecoveredSubspan best;
	if (options.scoring.presetId != QStringLiteral("viewer_message_response") || chunks.size() < 2)
		return best;

	const int maxForwardDepth = VIEWER_DFS_MAX_FORWARD_WINDOWS;
	const double maxDurationSec = std::min(
		std::max(options.generation.boundaryMinDurationSec, options.generation.maxDurationSec),
		VIEWER_DFS_MAX_FORWARD_SEC);
	QVector<int> usedOrigins;
	for (int seed = 0; seed < static_cast<int>(chunks.size()); ++seed) {
		if (!canUseViewerDfsSeed(chunks.at(seed)))
			continue;
		const ViewerOriginDfsResult origin = findViewerOriginByWindowDfs(chunks, seed, options);
		if (!origin.valid || origin.index < 0 || usedOrigins.contains(origin.index))
			continue;
		usedOrigins.append(origin.index);

		QVector<ViewerWindowDfsState> stack;
		stack.append(ViewerWindowDfsState{origin.index, 0});
		while (!stack.isEmpty()) {
			const ViewerWindowDfsState state = stack.takeLast();
			if (state.index < origin.index || state.index >= static_cast<int>(chunks.size()) || state.depth > maxForwardDepth)
				continue;
			const double durationSec = chunks.at(state.index).endSec - chunks.at(origin.index).startSec;
			if (durationSec > maxDurationSec)
				continue;

			if (canEndViewerDfsArc(chunks, origin.index, state.index, options)) {
				const ArcSpanScore span = scoreViewerDfsArc(chunks, origin.index, state.index, options);
				const ContextualArcScore machine = contextualStateMachineScore(chunks, origin.index, state.index, options);
				const double machineScore = machine.valid ? machine.score : 0.0;
				const double score = boundedScore(std::max(span.score, machineScore) + (origin.score * 0.05) -
					(static_cast<double>(origin.depth) * 0.006));
				const bool laterViewerOriginWins = best.valid && origin.index > best.first &&
					origin.score >= 0.50 && score + 0.040 >= best.stateMachineScore &&
					span.opening >= best.opening - 0.045 && span.development >= best.development - 0.060 &&
					span.conclusion >= best.conclusion - 0.070;
				if (!best.valid || score > best.stateMachineScore + 0.008 || laterViewerOriginWins ||
				    (std::fabs(score - best.stateMachineScore) <= 0.008 &&
				     durationSec < chunks.at(best.last).endSec - chunks.at(best.first).startSec)) {
					best.valid = true;
					best.first = origin.index;
					best.last = state.index;
					best.span = span;
					best.span.score = score;
					best.stateMachineScore = score;
					best.opening = span.opening;
					best.development = span.development;
					best.conclusion = span.conclusion;
					best.semanticOpening = false;
					best.reason = QStringLiteral("viewer_message_window_dfs_arc");
					best.graphEvidence = windowDfsGraphEvidence(chunks, origin.index, state.index);
				}
			}

			if (state.depth >= maxForwardDepth)
				continue;
			const int next = state.index + 1;
			if (next >= static_cast<int>(chunks.size()))
				continue;
			if (!canDfsFollowViewerAnswerWindow(chunks, state.index, next))
				continue;
			const double nextDurationSec = chunks.at(next).endSec - chunks.at(origin.index).startSec;
			if (nextDurationSec > maxDurationSec + 0.1)
				continue;
			if (!canUseSpan(chunks, origin.index, next, options) && nextDurationSec > maxDurationSec)
				continue;
			stack.append(ViewerWindowDfsState{next, state.depth + 1});
		}
	}
	return best;
}

static RecoveredSubspan recoverSemanticHookSubspan(const QVector<ArcChunk> &chunks,
	const ExchangeArcBoundaryRefinementOptions &options)
{
	RecoveredSubspan best;
	if (options.scoring.presetId != QStringLiteral("viewer_message_response") || chunks.size() < 2)
		return best;

	const double minDurationSec = std::max(8.0, options.generation.boundaryMinDurationSec);
	const double maxDurationSec = std::min(std::max(minDurationSec, options.generation.maxDurationSec), 64.0);
	for (int first = 0; first < static_cast<int>(chunks.size()); ++first) {
		if (!canStartContextualDpSubspan(chunks, first))
			continue;

		double developmentSum = 0.0;
		double maxDevelopment = 0.0;
		double maxBad = contextualBadSignal(chunks.at(first));
		int developmentBlocks = 0;
		int usefulBlocks = 1;
		for (int last = first + 1; last < static_cast<int>(chunks.size()); ++last) {
			const double durationSec = chunks.at(last).endSec - chunks.at(first).startSec;
			if (durationSec > maxDurationSec)
				break;
			if (last > first && looksLikeAnyViewerMessageTurn(chunks.at(last)))
				break;
			if (last > first && ((isPauseSeparatedSemanticTurn(chunks.at(last - 1), chunks.at(last)) &&
				!isModeratePauseSameViewerAnswer(chunks.at(last - 1), chunks.at(last))) ||
				chunks.at(last).role == ArcChunkRole::TailOrNewTurn))
				break;
			if (!canUseContextualDpBodyBlock(chunks.at(last)))
				break;

			const double development = contextualDevelopmentSignal(chunks.at(last));
			const double conclusion = contextualConclusionSignal(chunks.at(last));
			const double bad = contextualBadSignal(chunks.at(last));
			maxBad = std::max(maxBad, bad);
			++usefulBlocks;
			const double bodySignal = std::max({development, chunks.at(last).value, chunks.at(last).target * 0.82,
				chunks.at(last).resolution * 0.58});
			const bool countsAsAnswerBody = development >= conclusion - 0.12 ||
				chunks.at(last).role == ArcChunkRole::Development || chunks.at(last).value >= 0.48 ||
				chunks.at(last).target >= 0.52 || (chunks.at(last).resolution >= 0.56 && last < first + 3);
			if (countsAsAnswerBody) {
				developmentSum += bodySignal;
				maxDevelopment = std::max(maxDevelopment, bodySignal);
				if (bodySignal >= 0.32 || chunks.at(last).role == ArcChunkRole::Development || chunks.at(last).value >= 0.48)
					++developmentBlocks;
			}

			if (durationSec < minDurationSec)
				continue;

			const bool openingHasBody = contextualDevelopmentSignal(chunks.at(first)) >= 0.38 ||
				chunks.at(first).value >= 0.50 || chunks.at(first).target >= 0.52 ||
				chunks.at(first).explicitDevelopmentCue >= 0.34;
			if (developmentBlocks <= 0 && !openingHasBody)
				continue;

			const double ending = contextualConclusionSignal(chunks.at(last));
			const double endingBad = contextualBadSignal(chunks.at(last));
			const bool explicitEnding = chunks.at(last).explicitConclusionCue >= 0.46 ||
				chunks.at(last).role == ArcChunkRole::LocalResolution;
			const bool contextualEnding = chunks.at(last).contextConclusionScore >= 0.49 &&
				chunks.at(last).contextConclusionScore >= chunks.at(last).contextDevelopmentScore - 0.04;
			const bool semanticEnding = chunks.at(last).resolution >= 0.58 && ending >= endingBad - 0.05;
			if (ending < 0.38 || !(explicitEnding || contextualEnding || semanticEnding ||
				(chunks.at(last).resolution >= 0.52 && durationSec >= minDurationSec + 3.0)))
				continue;
			if (endingBad >= ending + 0.24)
				continue;
			if (last + 1 < static_cast<int>(chunks.size()) && isViewerAnswerContinuation(chunks, last, last + 1) &&
			    chunks.at(last + 1).endSec - chunks.at(first).startSec <= maxDurationSec)
				continue;

			const double opening = contextualOpeningSignal(chunks.at(first));
			const double developmentAvg = developmentBlocks > 0
				? boundedScore(developmentSum / static_cast<double>(std::max(1, usefulBlocks - 1)))
				: contextualDevelopmentSignal(chunks.at(first));
			const double developmentScore = std::max(developmentAvg, maxDevelopment);
			const double cohesion = cohesionScore(chunks, first, last);
			const double avgBad = averageScore(chunks, first, last, &ArcChunk::defectScore);
			const double meta = averageScore(chunks, first, last, &ArcChunk::meta);
			if (opening < 0.34 || developmentScore < 0.28 || ending < 0.38 || cohesion < 0.18)
				continue;
			if (maxBad >= std::max({opening, developmentScore, ending}) + 0.28)
				continue;

			const bool semanticOpening = chunks.at(first).explicitOpeningCue < 0.42 &&
				chunks.at(first).role != ArcChunkRole::OpeningCandidate;
			if (semanticOpening && durationSec > 42.0)
				continue;
			if (durationSec > 52.0 && cohesion < 0.42)
				continue;

			const double score = boundedScore((opening * 0.30) + (developmentScore * 0.27) +
				(ending * 0.31) + (cohesion * 0.18) - (avgBad * 0.10) - (meta * 0.05) -
				(maxBad * 0.08) - (semanticOpening ? 0.035 : 0.0));
			if (score < 0.32)
				continue;

			if (!best.valid || score > best.span.score + 0.006 ||
			    (std::fabs(score - best.span.score) <= 0.006 &&
			     durationSec < chunks.at(best.last).endSec - chunks.at(best.first).startSec)) {
				best.valid = true;
				best.first = first;
				best.last = last;
				best.semanticOpening = semanticOpening;
				best.opening = opening;
				best.development = developmentScore;
				best.conclusion = ending;
				best.stateMachineScore = score;
				best.reason = QStringLiteral("contextual_role_dp_subspan");
				best.span.first = first;
				best.span.last = last;
				best.span.opening = opening;
				best.span.development = developmentScore;
				best.span.conclusion = ending;
				best.span.cohesion = cohesion;
				best.span.tailRisk = std::max(0.0, maxBad * 0.62);
				best.span.boundaryCleanliness = boundedScore((opening * 0.42) + (ending * 0.44) + 0.10 - (maxBad * 0.11));
				best.span.score = score;
			}
		}
	}
	return best;
}



static double durationFit(double durationSec, const ExchangeArcBoundaryRefinementOptions &options)
{
	const double minSec = std::max(1.0, options.generation.minDurationSec);
	const double maxSec = std::max(minSec, options.generation.maxDurationSec);
	if (durationSec < minSec || durationSec > maxSec)
		return 0.0;
	if (durationSec >= 22.0 && durationSec <= 120.0)
		return 1.0;
	if (durationSec < 22.0)
		return boundedScore(0.72 + ((durationSec - minSec) / std::max(1.0, 22.0 - minSec)) * 0.28);
	return boundedScore(1.0 - ((durationSec - 120.0) / std::max(1.0, maxSec - 120.0)) * 0.22);
}

static ArcSpanScore scoreSpan(const QVector<ArcChunk> &chunks, int first, int last,
	const ExchangeArcBoundaryRefinementOptions &options)
{
	ArcSpanScore score;
	score.first = first;
	score.last = last;
	if (!canUseSpan(chunks, first, last, options))
		return score;

	const double durationSec = chunks.at(last).endSec - chunks.at(first).startSec;
	const double value = std::max(averageScore(chunks, first, last, &ArcChunk::value),
		maxScore(chunks, first, last, &ArcChunk::target));
	const double development = first < last ? averageScore(chunks, std::min(first + 1, last), last,
		&ArcChunk::developmentScore) : chunks.at(first).developmentScore;
	const double averageDefect = averageScore(chunks, first, last, &ArcChunk::defectScore);
	score.cohesion = cohesionScore(chunks, first, last);

	const bool openingPrelude = isSemanticPrelude(chunks.at(first));
	const bool weakOpeningPrelude = isWeakOpeningPrelude(chunks, first);
	const bool staleViewerLeadingResolution =
		options.scoring.presetId == QStringLiteral("viewer_message_response") &&
		(isStaleViewerLeadingResolution(chunks, first) || isViewerStartBoundaryContaminated(chunks, first));
	const bool tailRisk = isTailRisk(chunks, last);
	score.opening = boundedScore(chunks.at(first).openingScore - (openingPrelude ? 0.18 : 0.0) -
		(weakOpeningPrelude && !openingPrelude ? 0.12 : 0.0) -
		(staleViewerLeadingResolution ? 0.18 : 0.0));
	score.development = boundedScore((value * 0.30) + (development * 0.30) + (score.cohesion * 0.24) +
		(durationFit(durationSec, options) * 0.10) - (averageDefect * 0.16));
	const double endingDefect = std::max(chunks.at(last).meta, chunks.at(last).shift);
	const double semanticEndingSupport = boundedScore((chunks.at(last).resolution * 0.66) +
		(chunks.at(last).value * 0.18) + (chunks.at(last).target * 0.08) - (endingDefect * 0.16));
	score.conclusion = boundedScore(std::max(chunks.at(last).conclusionScore, semanticEndingSupport) -
		(tailRisk ? 0.20 : 0.0));
	score.tailRisk = tailRisk ? std::max(chunks.at(last).defectScore, 0.72) : chunks.at(last).defectScore * 0.55;
	score.boundaryCleanliness = boundedScore((score.opening * 0.42) + (score.conclusion * 0.44) +
		((openingPrelude || weakOpeningPrelude || tailRisk) ? 0.0 : 0.14));
	score.score = boundedScore((score.opening * 0.28) + (score.development * 0.32) +
		(score.conclusion * 0.30) + (score.boundaryCleanliness * 0.10) - (score.tailRisk * 0.10));

	if (options.scoring.presetId == QStringLiteral("viewer_message_response")) {
		const ContextualArcScore contextual = contextualStateMachineScore(chunks, first, last, options);
		if (contextual.valid) {
			score.opening = boundedScore((score.opening * 0.55) + (contextual.opening * 0.45));
			score.development = boundedScore((score.development * 0.55) + (contextual.development * 0.45));
			score.conclusion = boundedScore((score.conclusion * 0.55) + (contextual.conclusion * 0.45));
			score.boundaryCleanliness = boundedScore(std::max(score.boundaryCleanliness, contextual.score * 0.92));
			score.score = boundedScore((score.score * 0.42) + (contextual.score * 0.58) + 0.035 -
				(contextual.implicitOpening ? 0.015 : 0.0));
		} else {
			score.opening = boundedScore(score.opening * 0.62);
			score.boundaryCleanliness = boundedScore(score.boundaryCleanliness * 0.52);
			score.tailRisk = std::max(score.tailRisk, 0.58 + contextual.penalty);
			score.score = boundedScore((score.score * 0.38) - contextual.penalty);
		}
	}
	return score;
}

static int firstLocalResolutionEnd(const QVector<ArcChunk> &chunks, int first, int last,
	const ExchangeArcBoundaryRefinementOptions &options)
{
	if (first < 0 || last <= first || last >= static_cast<int>(chunks.size()))
		return -1;

	const bool viewerPreset = options.scoring.presetId == QStringLiteral("viewer_message_response");
	const ArcSpanScore fullScore = scoreSpan(chunks, first, last, options);
	int bestEnd = -1;
	double bestScore = 0.0;
	for (int end = first; end < last; ++end) {
		if (!canUseSpan(chunks, first, end, options))
			continue;
		const double trimmedTailSec = chunks.at(last).endSec - chunks.at(end).endSec;

		const ArcChunk &ending = chunks.at(end);
		const ArcChunk &next = chunks.at(end + 1);
		const ArcSpanScore shortened = scoreSpan(chunks, first, end, options);
		const double nextPositive = std::max({next.target, next.value, next.hook, next.resolution});
		const double nextDefect = std::max({next.meta, next.shift, next.defectScore});
		const bool nextLooksTail = isPauseSeparatedSemanticTurn(ending, next) || isTailRisk(chunks, end + 1) ||
			next.role == ArcChunkRole::TailOrNewTurn || next.shift >= nextPositive - 0.02 ||
			(next.meta >= nextPositive - 0.01 && next.openingScore < 0.42);
		const bool clearTopicBreak = nextLooksTail &&
			(isPauseSeparatedSemanticTurn(ending, next) || next.role == ArcChunkRole::TailOrNewTurn ||
			 next.shift >= nextPositive + 0.02);
		const double minSoftTailTrimSec = viewerPreset ? 10.5 : 7.0;
		if (trimmedTailSec < (clearTopicBreak ? 3.0 : minSoftTailTrimSec))
			continue;
		const bool reachesConclusion = shortened.conclusion >= 0.42 || ending.conclusionScore >= 0.42 ||
			ending.resolution >= 0.58;
		const bool hasDevelopment = shortened.development >= 0.46 || (end - first) >= 2;
		const bool keepsEnoughQuality = shortened.score + 0.035 >= fullScore.score || trimmedTailSec >= 9.0;
		const double plateauTrimMinSec = viewerPreset ? 12.0 : 9.0;
		const bool conclusionPlateauTail = reachesConclusion && trimmedTailSec >= plateauTrimMinSec &&
			ending.conclusionScore >= 0.48 && ending.resolution >= 0.56 &&
			shortened.development >= fullScore.development - 0.05 &&
			shortened.conclusion >= fullScore.conclusion - 0.07 &&
			shortened.tailRisk <= fullScore.tailRisk + 0.08;
		if ((!nextLooksTail && !conclusionPlateauTail) || !reachesConclusion || !hasDevelopment || !keepsEnoughQuality)
			continue;

		const double candidateScore = shortened.score + (trimmedTailSec >= 10.0 ? 0.05 : 0.0) +
			(conclusionPlateauTail ? 0.035 : 0.0) - (nextDefect * 0.03);
		if (bestEnd < 0 || candidateScore > bestScore) {
			bestEnd = end;
			bestScore = candidateScore;
		}
	}
	return bestEnd;
}


static QString compactDfsGraphText(QString text)
{
	text = text.simplified();
	text.replace(QLatin1Char('|'), QLatin1Char('/'));
	text.replace(QLatin1Char(';'), QLatin1Char(','));
	text.replace(QLatin1Char('\n'), QLatin1Char(' '));
	if (text.size() > 44)
		text = text.left(41).trimmed() + QStringLiteral("...");
	return text;
}

static QString windowDfsGraphEvidence(const QVector<ArcChunk> &chunks, int first, int last)
{
	if (first < 0 || last < first || chunks.isEmpty())
		return {};
	const int safeFirst = std::clamp(first, 0, static_cast<int>(chunks.size()) - 1);
	const int safeLast = std::clamp(last, safeFirst, static_cast<int>(chunks.size()) - 1);
	const int graphFirst = std::max(0, safeFirst - 3);
	const int graphLast = std::min(static_cast<int>(chunks.size()) - 1, safeLast + 3);

	QStringList nodes;
	for (int i = graphFirst; i <= graphLast; ++i) {
		const ArcChunk &chunk = chunks.at(i);
		QStringList flags;
		if (i == safeFirst)
			flags.append(QStringLiteral("S"));
		if (i == safeLast)
			flags.append(QStringLiteral("E"));
		if (looksLikeViewerOriginCueAt(chunks, i))
			flags.append(QStringLiteral("origin"));
		else if (looksLikeAnyViewerMessageTurn(chunk))
			flags.append(QStringLiteral("viewer"));
		if (i > graphFirst && isHardWindowTopicDrift(chunks.at(i - 1), chunk))
			flags.append(QStringLiteral("drift"));
		if (isBlockingArcRole(chunk.role))
			flags.append(QStringLiteral("block"));
		if (flags.isEmpty())
			flags.append(QStringLiteral("-"));

		nodes.append(QStringLiteral("%1:%2[%3-%4]role=%5 cue=%6 val=%7 meta=%8 flags=%9 text='%10'")
			.arg(i - safeFirst)
			.arg(i)
			.arg(QString::number(chunk.startSec, 'f', 1))
			.arg(QString::number(chunk.endSec, 'f', 1))
			.arg(roleName(chunk.role))
			.arg(QString::number(viewerOriginCueScoreAt(chunks, i), 'f', 2))
			.arg(QString::number(std::max(chunk.value, chunk.target), 'f', 2))
			.arg(QString::number(std::max(chunk.meta, chunk.shift), 'f', 2))
			.arg(flags.join(QLatin1Char('+')))
			.arg(compactDfsGraphText(chunk.text)));
		if (nodes.size() >= 10)
			break;
	}
	return QStringLiteral("exchange_arc_window_dfs_graph:%1").arg(nodes.join(QStringLiteral(" -> ")));
}


static bool isLikelyViewerAttributionWord(const QString &word)
{
	QString cleaned = foldedLowerText(word).trimmed();
	cleaned.remove(QLatin1Char('"'));
	cleaned.remove(QStringLiteral("“"));
	cleaned.remove(QStringLiteral("”"));
	cleaned.remove(QStringLiteral("'"));
	if (!cleaned.endsWith(QLatin1Char(':')))
		return false;
	cleaned.chop(1);
	cleaned = cleaned.trimmed();
	if (cleaned.size() < 2 || cleaned.size() > 28)
		return false;
	if (cleaned.contains(QStringLiteral("http")) || cleaned.contains(QLatin1Char('/')) ||
	    cleaned.contains(QLatin1Char('.')))
		return false;
	int letters = 0;
	for (const QChar ch : cleaned) {
		if (ch.isLetter())
			++letters;
		else if (!ch.isSpace() && ch != QLatin1Char('_') && ch != QLatin1Char('-'))
			return false;
	}
	return letters >= 2;
}

static QString wordsWindowText(const QVector<WordTiming> &words, int first, int last)
{
	QStringList parts;
	const int safeFirst = std::max(0, first);
	const int safeLast = std::min(last, static_cast<int>(words.size()) - 1);
	for (int i = safeFirst; i <= safeLast; ++i) {
		const QString word = words.at(i).word.trimmed();
		if (!word.isEmpty())
			parts.append(word);
	}
	return foldedLowerText(parts.join(QLatin1Char(' ')));
}

static bool hasViewerOriginProblemInFollowingWords(const QVector<WordTiming> &words, int index)
{
	if (index < 0 || index >= static_cast<int>(words.size()))
		return false;
	const QString following = wordsWindowText(words, index, index + 28);
	const QString nearFollowing = wordsWindowText(words, index, index + 12);
	return hasPersonalViewerProblemCue(following) || hasViewerAdviceRequestCue(following) ||
		containsAny(nearFollowing, {
			"o que eu", "o que devo", "como eu", "devo", "deveria", "tentei",
			"nao deixou", "não deixou", "quer meu", "quer o meu"
		});
}

static bool wordStartsViewerProblemPhrase(const QVector<WordTiming> &words, int index)
{
	const QString phrase = wordsWindowText(words, index, index + 8);
	return containsAny(phrase, {
		"minha namorada", "meu namorado", "minha esposa", "meu marido",
		"me envolvi", "me sinto", "eu sinto", "eu tenho", "tenho um", "tenho uma",
		"meu pai", "minha mae", "minha mãe", "dependencia emocional", "dependência emocional",
		"tentei terminar", "queria terminar", "quero terminar", "preciso de ajuda",
		"nao consigo", "não consigo", "o que eu", "o que devo", "como eu"
	});
}

static double findIntraWindowViewerOriginStartSec(const TranscriptIndex &index, const ClipDuration &range)
{
	if (!index.hasWordTimings() || range.endSec <= range.startSec)
		return -1.0;
	const QVector<WordTiming> words = index.wordsForRange(range);
	if (words.isEmpty())
		return -1.0;

	double bestStart = -1.0;
	double bestScore = 0.0;
	for (int i = 0; i < static_cast<int>(words.size()); ++i) {
		const WordTiming &word = words.at(i);
		const bool attribution = isLikelyViewerAttributionWord(word.word) && hasViewerOriginProblemInFollowingWords(words, i);
		const bool problemPhrase = wordStartsViewerProblemPhrase(words, i);
		if (!attribution && !problemPhrase)
			continue;
		const QString following = wordsWindowText(words, i, i + 28);
		double score = attribution ? 0.78 : 0.62;
		if (hasPersonalViewerProblemCue(following))
			score += 0.14;
		if (hasViewerAdviceRequestCue(following))
			score += 0.08;
		// Prefer the latest reliable origin inside a mixed viewer range. This fixes cases
		// where a previous off-topic chat line sits before the actual advice question.
		if (bestStart < 0.0 || score > bestScore + 0.025 ||
		    (std::fabs(score - bestScore) <= 0.025 && word.startSec > bestStart + 1.0)) {
			bestStart = word.startSec;
			bestScore = score;
		}
	}
	return bestStart;
}

static bool looksLikeViewerTurnText(const QString &text)
{
	const QString folded = foldedLowerText(text);
	return hasLikelyNewViewerAttribution(folded) || looksLikeViewerMessageCueText(folded);
}

static bool looksLikeHardTopicDriftText(const QString &previousText, const QString &nextText)
{
	const QString prev = foldedLowerText(previousText);
	const QString next = foldedLowerText(nextText);
	if (next.isEmpty())
		return false;
	if (looksLikeViewerTurnText(next))
		return true;
	const bool meta = containsAny(next, {
		"obrigado", "obrigada", "valeu", "salve", "boa noite", "bom dia", "link", "livro",
		"vou ler", "preciso terminar", "te vi", "pessoalmente", "chat", "live"
	});
	const bool prevClosing = containsAny(prev, {
		"cuidado com isso", "procura um psic", "na sua vida", "melhor coisa", "concorda comigo",
		"não necessariamente", "nao necessariamente", "dependência emocional", "dependencia emocional"
	});
	return meta && prevClosing;
}

static double extendViewerRangeEndByTranscriptWindows(const TranscriptIndex &index, const ClipDuration &range,
	const ExchangeArcBoundaryRefinementOptions &options)
{
	if (range.endSec <= range.startSec)
		return range.endSec;
	const int firstSegment = index.firstSegmentIndexOverlapping(range);
	const int currentLast = index.lastSegmentIndexOverlapping(range);
	if (firstSegment < 0 || currentLast < firstSegment)
		return range.endSec;

	const double maxEnd = std::min({options.generation.searchRange.endSec,
		range.startSec + VIEWER_DFS_MAX_FORWARD_SEC,
		range.endSec + 24.0});
	double bestEnd = range.endSec;
	QString previousText = index.textForRange(range).right(260);
	for (int i = currentLast + 1; i < index.size(); ++i) {
		const TranscriptSegment *segment = index.segmentAt(i);
		if (!segment || segment->text.trimmed().isEmpty())
			continue;
		if (segment->startSec > maxEnd + 0.1)
			break;
		const double gapSec = std::max(0.0, segment->startSec - bestEnd);
		const QString segmentText = segment->text.trimmed();
		if (gapSec > 4.4 && !containsAny(foldedLowerText(segmentText), {
			"cuidado", "procura", "psic", "terapia", "vício", "vicio", "dependência", "dependencia",
			"relacionamento", "possessiv", "não necessariamente", "nao necessariamente"
		}))
			break;
		if (looksLikeHardTopicDriftText(previousText, segmentText))
			break;
		if (looksLikeViewerTurnText(segmentText) && segment->startSec > range.startSec + 5.0)
			break;
		bestEnd = std::min(segment->endSec, maxEnd);
		previousText = segmentText;
		if (segment->endSec >= maxEnd - 0.05)
			break;
	}
	return bestEnd;
}

static QString arcEvidence(const ArcSpanScore &score)
{
	return QStringLiteral("exchange_arc opening:%1 development:%2 conclusion:%3 clean:%4 tailRisk:%5 score:%6")
		.arg(QString::number(score.opening, 'f', 2), QString::number(score.development, 'f', 2),
		     QString::number(score.conclusion, 'f', 2), QString::number(score.boundaryCleanliness, 'f', 2),
		     QString::number(score.tailRisk, 'f', 2), QString::number(score.score, 'f', 2));
}

static QString roleEvidence(const QVector<ArcChunk> &chunks, int first, int last)
{
	QStringList roles;
	for (int i = first; i <= last && i < static_cast<int>(chunks.size()); ++i) {
		roles.append(QStringLiteral("%1:%2").arg(i - first).arg(roleName(chunks.at(i).role)));
		if (roles.size() >= 8)
			break;
	}
	return QStringLiteral("exchange_arc_roles:%1").arg(roles.join(QLatin1Char(',')));
}

static QString roleReasonEvidence(const QVector<ArcChunk> &chunks, int first, int last)
{
	QStringList reasons;
	for (int i = first; i <= last && i < static_cast<int>(chunks.size()); ++i) {
		const QString reason = chunks.at(i).roleReason.isEmpty() ? QStringLiteral("none") : chunks.at(i).roleReason;
		reasons.append(QStringLiteral("%1:%2").arg(i - first).arg(reason));
		if (reasons.size() >= 8)
			break;
	}
	return QStringLiteral("exchange_arc_role_reasons:%1").arg(reasons.join(QLatin1Char(',')));
}

static QString contextualRoleScoreEvidence(const QVector<ArcChunk> &chunks, int first, int last)
{
	QStringList roles;
	for (int i = first; i <= last && i < static_cast<int>(chunks.size()); ++i) {
		const ArcChunk &chunk = chunks.at(i);
		roles.append(QStringLiteral("%1:%2/o%3/d%4/c%5/prev%6/meta%7/new%8")
			.arg(i - first).arg(roleName(chunk.role), QString::number(chunk.contextOpeningScore, 'f', 2),
			     QString::number(chunk.contextDevelopmentScore, 'f', 2),
			     QString::number(chunk.contextConclusionScore, 'f', 2),
			     QString::number(chunk.contextPreviousTopicScore, 'f', 2),
			     QString::number(chunk.contextMetaScore, 'f', 2),
			     QString::number(chunk.contextNewTopicScore, 'f', 2)));
		if (roles.size() >= 6)
			break;
	}
	return QStringLiteral("exchange_arc_contextual_role_scores:%1").arg(roles.join(QLatin1Char(',')));
}

static QString boundaryEvidence(const QVector<ArcChunk> &chunks, int first, int last)
{
	if (first < 0 || last < first || last >= static_cast<int>(chunks.size()))
		return {};
	return QStringLiteral("exchange_arc_boundary opening:%1 openingDefect:%2 ending:%3 endingDefect:%4 shift:%5 pauseBefore:%6 pauseAfter:%7")
		.arg(QString::number(chunks.at(first).openingScore, 'f', 2), QString::number(chunks.at(first).defectScore, 'f', 2),
		     QString::number(chunks.at(last).conclusionScore, 'f', 2), QString::number(chunks.at(last).defectScore, 'f', 2),
		     QString::number(chunks.at(last).shift, 'f', 2), QString::number(chunks.at(first).pauseBeforeSec, 'f', 1),
		     QString::number(chunks.at(last).pauseAfterSec, 'f', 1));
}

static QString stateMachineEvidence(const QVector<ArcChunk> &chunks, int first, int last,
	const ExchangeArcBoundaryRefinementOptions &options)
{
	const ContextualArcScore score = contextualStateMachineScore(chunks, first, last, options);
	if (!score.valid)
		return QStringLiteral("exchange_arc_state_machine:invalid penalty:%1 reason:%2")
			.arg(QString::number(score.penalty, 'f', 2),
			     score.reason.isEmpty() ? QStringLiteral("unspecified") : score.reason);
	QStringList flags;
	if (score.implicitOpening)
		flags.append(QStringLiteral("implicit_opening"));
	if (score.semanticOpeningFallback)
		flags.append(QStringLiteral("semantic_opening"));
	if (score.terminalFollowThrough)
		flags.append(QStringLiteral("terminal_followthrough"));
	return QStringLiteral("exchange_arc_state_machine:valid score:%1 opening:%2 development:%3 conclusion:%4 flags:%5")
		.arg(QString::number(score.score, 'f', 2), QString::number(score.opening, 'f', 2),
		     QString::number(score.development, 'f', 2), QString::number(score.conclusion, 'f', 2),
		     flags.isEmpty() ? QStringLiteral("none") : flags.join(QLatin1Char(',')));
}

static void writeArcScores(ClipCandidate &candidate, const ArcSpanScore &score)
{
	candidate.scores.arcOpening = score.opening;
	candidate.scores.arcDevelopment = score.development;
	candidate.scores.arcConclusion = score.conclusion;
	candidate.scores.arcBoundaryCleanliness = score.boundaryCleanliness;
	candidate.scores.arcTailRisk = score.tailRisk;
	candidate.scores.arcCompleteness = score.score;
}

} // namespace

ClipCandidate ExchangeArcBoundaryRefiner::refine(const TranscriptIndex &index, const ClipCandidate &candidate,
	const ExchangeArcBoundaryRefinementOptions &options) const
{
	const double originalDurationSec = candidate.range.endSec - candidate.range.startSec;
	const bool viewerPreset = options.scoring.presetId == QStringLiteral("viewer_message_response");
	auto skipped = [&candidate](const QString &reason) {
		ClipCandidate diagnostic = candidate;
		diagnostic.evidence.append(QStringLiteral("exchange_arc_role_classifier:skipped"));
		diagnostic.evidence.append(QStringLiteral("exchange_arc_refiner_skipped:%1").arg(reason));
		diagnostic.evidence.removeDuplicates();
		return diagnostic;
	};
	if (!viewerPreset && originalDurationSec <= std::max(options.generation.minDurationSec + 4.0, 20.0))
		return skipped(QStringLiteral("short_non_viewer_candidate"));
	if (!options.embeddingProvider)
		return skipped(QStringLiteral("missing_embedding_provider"));
	if (!options.embeddingProvider->isAvailable())
		return skipped(QStringLiteral("embedding_provider_unavailable"));

	ClipCandidate analysisCandidate = candidate;
	if (viewerPreset) {
		const ClipDuration expandedRange = index.clampRange(
			{std::max(options.generation.searchRange.startSec,
				 candidate.range.startSec - VIEWER_PRESET_ANALYSIS_LOOKBACK_SEC),
			 std::min(options.generation.searchRange.endSec,
				 candidate.range.endSec + VIEWER_PRESET_ANALYSIS_LOOKAHEAD_SEC)},
			options.generation.searchRange);
		if (expandedRange.endSec > expandedRange.startSec &&
		    (expandedRange.startSec < candidate.range.startSec - 0.5 ||
		     expandedRange.endSec > candidate.range.endSec + 0.5)) {
			analysisCandidate.range = expandedRange;
			analysisCandidate.firstSegmentIndex = index.firstSegmentIndexOverlapping(expandedRange);
			analysisCandidate.lastSegmentIndex = index.lastSegmentIndexOverlapping(expandedRange);
			analysisCandidate.text = index.textForRange(expandedRange).simplified();
		}
	}

	QVector<ArcChunk> chunks = chunksForCandidate(index, analysisCandidate, options);
	if (chunks.size() < 2)
		return skipped(QStringLiteral("not_enough_contextual_chunks"));
	scoreChunks(chunks, options);
	classifyContextualRoles(chunks, options);

	ArcSpanScore best;
	const double minDurationSec = std::max(8.0, options.generation.boundaryMinDurationSec);
	const double maxDurationSec = std::max(minDurationSec, options.generation.maxDurationSec);
	for (int first = 0; first < static_cast<int>(chunks.size()); ++first) {
		for (int last = first; last < static_cast<int>(chunks.size()); ++last) {
			const double durationSec = chunks.at(last).endSec - chunks.at(first).startSec;
			if (durationSec < minDurationSec)
				continue;
			if (durationSec > maxDurationSec)
				break;
			const QString spanText = index.textForSegmentWindow(chunks.at(first).firstIndex, chunks.at(last).lastIndex);
			if (spanText.trimmed().size() < std::max(36, options.qualityGate.minTextChars / 2))
				continue;
			const ArcSpanScore candidateScore = scoreSpan(chunks, first, last, options);
			const bool betterScore = candidateScore.score > best.score + 0.012;
			const bool bestHasBadViewerStart = viewerPreset && best.first >= 0 &&
				(isSemanticPrelude(chunks.at(best.first)) ||
				 isStaleViewerLeadingResolution(chunks, best.first) ||
				 isViewerStartBoundaryContaminated(chunks, best.first));
			const bool canPreferLaterOpening = !viewerPreset || bestHasBadViewerStart;
			const bool similarCleanerOpening = canPreferLaterOpening &&
				std::fabs(candidateScore.score - best.score) <= (bestHasBadViewerStart ? 0.055 : 0.018) &&
				best.first >= 0 && first > best.first &&
				(candidateScore.opening >= best.opening + (bestHasBadViewerStart ? 0.010 : 0.030) ||
				 candidateScore.boundaryCleanliness >= best.boundaryCleanliness + 0.020) &&
				candidateScore.development >= best.development - 0.040 &&
				candidateScore.conclusion >= best.conclusion - 0.050 &&
				candidateScore.tailRisk <= best.tailRisk + 0.050;
			const bool similarShorterCleanerEnding = std::fabs(candidateScore.score - best.score) <= 0.020 &&
				best.first >= 0 && first == best.first && last < best.last &&
				candidateScore.conclusion >= best.conclusion - 0.010 && candidateScore.tailRisk <= best.tailRisk + 0.015;
			const bool addsCleanViewerContext = viewerPreset && best.first >= 0 && first < best.first &&
				!isStaleViewerLeadingResolution(chunks, first) &&
				!isViewerStartBoundaryContaminated(chunks, first) &&
				addsUsefulViewerOpeningContext(chunks, first, best.first);
			const bool similarEarlierViewerContext = viewerPreset && best.first >= 0 && first < best.first &&
				last >= best.last && candidateScore.score + 0.032 >= best.score &&
				candidateScore.development >= best.development - 0.035 &&
				candidateScore.conclusion >= best.conclusion - 0.045 &&
				candidateScore.tailRisk <= best.tailRisk + 0.050 && addsCleanViewerContext;
			if (betterScore || similarCleanerOpening || similarShorterCleanerEnding || similarEarlierViewerContext)
				best = candidateScore;
		}
	}

	RecoveredSubspan recoveredSubspan;
	const bool bestHasValidContextualArc = !viewerPreset ||
		(best.first >= 0 && best.last >= best.first && contextualStateMachineScore(chunks, best.first, best.last, options).valid);
	if (best.first < 0 || best.last < best.first || best.score < 0.34 || !bestHasValidContextualArc) {
		recoveredSubspan = recoverViewerMessageArcByWindowDfs(chunks, options);
		if (!recoveredSubspan.valid)
			recoveredSubspan = recoverSemanticHookSubspan(chunks, options);
		if (recoveredSubspan.valid) {
			best = recoveredSubspan.span;
		} else {
			ArcSpanScore diagnosticBest;
			for (int first = 0; first < static_cast<int>(chunks.size()); ++first) {
				for (int last = first; last < static_cast<int>(chunks.size()); ++last) {
					if (!canUseSpan(chunks, first, last, options))
						continue;
					const ArcSpanScore probe = scoreSpan(chunks, first, last, options);
					if (diagnosticBest.first < 0 || probe.score > diagnosticBest.score)
						diagnosticBest = probe;
				}
			}
			ClipCandidate diagnostic = candidate;
			if (diagnosticBest.first >= 0 && diagnosticBest.last >= diagnosticBest.first) {
				diagnostic.evidence.append(QStringLiteral("exchange_arc_role_classifier:v32_window_dfs"));
				diagnostic.evidence.append(QStringLiteral("exchange_arc_window_dfs_limits:back12_45s_forward22_82s_split_origin_graph_intra"));
				diagnostic.evidence.append(QStringLiteral("exchange_arc_role_classifier:v23_context_window_dp"));
				diagnostic.evidence.append(roleEvidence(chunks, diagnosticBest.first, diagnosticBest.last));
				diagnostic.evidence.append(roleReasonEvidence(chunks, diagnosticBest.first, diagnosticBest.last));
				diagnostic.evidence.append(contextualRoleScoreEvidence(chunks, diagnosticBest.first, diagnosticBest.last));
				diagnostic.evidence.append(windowDfsGraphEvidence(chunks, diagnosticBest.first, diagnosticBest.last));
				diagnostic.evidence.append(stateMachineEvidence(chunks, diagnosticBest.first, diagnosticBest.last, options));
				diagnostic.evidence.append(QStringLiteral("exchange_arc_window_dfs_no_valid_origin_or_answer"));
				diagnostic.evidence.append(QStringLiteral("exchange_arc_no_valid_subspan"));
				diagnostic.evidence.append(arcEvidence(diagnosticBest));
				diagnostic.evidence.append(boundaryEvidence(chunks, diagnosticBest.first, diagnosticBest.last));
				diagnostic.evidence.removeDuplicates();
			} else {
				diagnostic.evidence.append(QStringLiteral("exchange_arc_role_classifier:v32_window_dfs"));
				diagnostic.evidence.append(QStringLiteral("exchange_arc_window_dfs_limits:back12_45s_forward22_82s_split_origin_graph_intra"));
				diagnostic.evidence.append(QStringLiteral("exchange_arc_role_classifier:v23_context_window_dp"));
				diagnostic.evidence.append(QStringLiteral("exchange_arc_no_scoreable_span"));
				diagnostic.evidence.append(QStringLiteral("exchange_arc_state_machine:invalid penalty:0.99 reason:no_scoreable_span"));
				diagnostic.evidence.removeDuplicates();
			}
			return diagnostic;
		}
	}

	int adjustedFirst = best.first;
	int adjustedLast = best.last;
	bool trimmedOpeningMeta = false;
	bool trimmedWeakOpeningPrelude = false;
	bool trimmedStaleOpeningResolution = false;
	bool extendedTailContinuation = false;
	bool trimmedHardNoisyEnding = false;
	bool trimmedLongPauseTail = false;
	bool blockedTailByLongPause = false;
	bool extendedStartContext = false;
	bool trimmedFirstResolutionTail = false;
	bool extendedViewerConclusionFollowThrough = false;
	const bool usedRecoveredSubspan = recoveredSubspan.valid;

	while (adjustedFirst < adjustedLast && isSemanticPrelude(chunks.at(adjustedFirst)) &&
	       canUseSpan(chunks, adjustedFirst + 1, adjustedLast, options)) {
		++adjustedFirst;
		trimmedOpeningMeta = true;
	}
	while (!viewerPreset && adjustedFirst < adjustedLast && isWeakOpeningPrelude(chunks, adjustedFirst) &&
	       canUseSpan(chunks, adjustedFirst + 1, adjustedLast, options)) {
		++adjustedFirst;
		trimmedWeakOpeningPrelude = true;
	}
	while (viewerPreset && adjustedFirst < adjustedLast &&
	       (isStaleViewerLeadingResolution(chunks, adjustedFirst) ||
		isViewerStartBoundaryContaminated(chunks, adjustedFirst)) &&
	       canUseSpan(chunks, adjustedFirst + 1, adjustedLast, options)) {
		++adjustedFirst;
		trimmedStaleOpeningResolution = true;
	}

	bool trimmedToViewerMessageBoundary = false;
	bool prependedViewerMessageCue = false;
	bool trimmedAtNextViewerMessage = false;
	if (viewerPreset) {
		const int targetedCue = bestTargetedViewerCueInsideSpan(chunks, adjustedFirst, adjustedLast, options);
		if (targetedCue > adjustedFirst && canUseSpan(chunks, targetedCue, adjustedLast, options)) {
			adjustedFirst = targetedCue;
			trimmedToViewerMessageBoundary = true;
		}
		if (!hasReliableViewerCueNearStart(chunks, adjustedFirst, adjustedLast)) {
			const int previousCue = nearestTargetedViewerCueBefore(chunks, adjustedFirst, adjustedLast, options);
			if (previousCue >= 0) {
				adjustedFirst = previousCue;
				prependedViewerMessageCue = true;
			}
		}
		const int nextCue = nextViewerCueInsideArc(chunks, adjustedFirst, adjustedLast);
		if (nextCue > adjustedFirst && canUseSpan(chunks, adjustedFirst, nextCue - 1, options)) {
			adjustedLast = nextCue - 1;
			trimmedAtNextViewerMessage = true;
		}
	}

	int contextExtensions = 0;
	const int maxContextExtensions = viewerPreset ? VIEWER_PRESET_MAX_CONTEXT_EXTENSIONS : DEFAULT_MAX_CONTEXT_EXTENSIONS;
	while (contextExtensions < maxContextExtensions && adjustedFirst > 0 &&
	       (shouldPrependOpeningContext(chunks, adjustedFirst) ||
		(viewerPreset && addsUsefulViewerOpeningContext(chunks, adjustedFirst - 1, adjustedFirst))) &&
	       isEssentialOpeningContext(chunks.at(adjustedFirst - 1), chunks.at(adjustedFirst)) &&
	       (!viewerPreset || (!isStaleViewerLeadingResolution(chunks, adjustedFirst - 1) &&
				  !isViewerStartBoundaryContaminated(chunks, adjustedFirst - 1))) &&
	       canUseSpan(chunks, adjustedFirst - 1, adjustedLast, options)) {
		--adjustedFirst;
		++contextExtensions;
		extendedStartContext = true;
	}

	int tailExtensions = 0;
	double viewerTailExtensionSec = 0.0;
	while (tailExtensions < (viewerPreset ? 10 : 6) && adjustedLast + 1 < static_cast<int>(chunks.size())) {
		if (viewerPreset && looksLikeAnyViewerMessageTurn(chunks.at(adjustedLast + 1)))
			break;
		if (viewerPreset && isHardWindowTopicDrift(chunks.at(adjustedLast), chunks.at(adjustedLast + 1)) &&
		    !isModeratePauseSameViewerAnswer(chunks.at(adjustedLast), chunks.at(adjustedLast + 1)))
			break;
		const bool normalContinuation = isSemanticContinuation(chunks, adjustedLast, adjustedLast + 1);
		const bool viewerContinuation = viewerPreset && isViewerAnswerContinuation(chunks, adjustedLast, adjustedLast + 1) &&
			viewerTailExtensionSec + std::max(0.0, chunks.at(adjustedLast + 1).endSec - chunks.at(adjustedLast + 1).startSec) <= 22.0;
		if ((!normalContinuation && !viewerContinuation) ||
		    !canUseSpan(chunks, adjustedFirst, adjustedLast + 1, options))
			break;
		viewerTailExtensionSec += std::max(0.0, chunks.at(adjustedLast + 1).endSec - chunks.at(adjustedLast + 1).startSec);
		++adjustedLast;
		++tailExtensions;
		extendedTailContinuation = true;
	}

	while (adjustedLast > adjustedFirst && isPauseSeparatedSemanticTurn(chunks.at(adjustedLast - 1), chunks.at(adjustedLast)) &&
	       canUseSpan(chunks, adjustedFirst, adjustedLast - 1, options)) {
		--adjustedLast;
		trimmedLongPauseTail = true;
	}
	while (adjustedLast > adjustedFirst && isTailRisk(chunks, adjustedLast) &&
	       canUseSpan(chunks, adjustedFirst, adjustedLast - 1, options)) {
		--adjustedLast;
		trimmedHardNoisyEnding = true;
	}
	const int earlyConclusionEnd = firstLocalResolutionEnd(chunks, adjustedFirst, adjustedLast, options);
	if (earlyConclusionEnd >= adjustedFirst && earlyConclusionEnd < adjustedLast) {
		adjustedLast = earlyConclusionEnd;
		trimmedFirstResolutionTail = true;
		while (viewerPreset && adjustedLast + 1 < static_cast<int>(chunks.size()) &&
		       !looksLikeAnyViewerMessageTurn(chunks.at(adjustedLast + 1)) &&
		       (isViewerConclusionFollowThrough(chunks, adjustedLast, adjustedLast + 1) ||
			shouldExtendViewerConclusionFollowThroughSoft(chunks, adjustedLast, adjustedLast + 1) ||
			isViewerAnswerContinuation(chunks, adjustedLast, adjustedLast + 1)) &&
		       canUseSpan(chunks, adjustedFirst, adjustedLast + 1, options) &&
		       chunks.at(adjustedLast + 1).endSec - chunks.at(earlyConclusionEnd).endSec <= 22.0) {
			++adjustedLast;
			extendedViewerConclusionFollowThrough = true;
		}
	}
	if (adjustedLast + 1 < static_cast<int>(chunks.size()) &&
	    isPauseSeparatedSemanticTurn(chunks.at(adjustedLast), chunks.at(adjustedLast + 1)))
		blockedTailByLongPause = true;

	ArcSpanScore adjustedScore = scoreSpan(chunks, adjustedFirst, adjustedLast, options);
	if (adjustedScore.first < 0 || adjustedScore.last < adjustedScore.first)
		adjustedScore = best;
	if (usedRecoveredSubspan) {
		// The regular span scorer calls the strict contextual state machine again. A DP-recovered
		// subspan is already a validated contextual arc, so do not let the strict scorer overwrite
		// its arc metrics with zero and make the quality gate reject it as missing_contextual_arc.
		// Keep any improved boundary/tail numbers from the regular scorer, but preserve the
		// recovered opening/development/conclusion/completeness contract expected by the gate.
		adjustedScore.first = adjustedFirst;
		adjustedScore.last = adjustedLast;
		adjustedScore.opening = std::max(adjustedScore.opening, recoveredSubspan.opening);
		adjustedScore.development = std::max(adjustedScore.development, recoveredSubspan.development);
		adjustedScore.conclusion = std::max(adjustedScore.conclusion, recoveredSubspan.conclusion);
		adjustedScore.cohesion = std::max(adjustedScore.cohesion, recoveredSubspan.span.cohesion);
		adjustedScore.boundaryCleanliness = std::max(adjustedScore.boundaryCleanliness,
			recoveredSubspan.span.boundaryCleanliness);
		adjustedScore.tailRisk = adjustedScore.tailRisk > 0.0
			? std::min(adjustedScore.tailRisk, recoveredSubspan.span.tailRisk)
			: recoveredSubspan.span.tailRisk;
		adjustedScore.score = std::max(adjustedScore.score, recoveredSubspan.stateMachineScore);
	}

	ClipDuration refinedRange{chunks.at(adjustedFirst).startSec, chunks.at(adjustedLast).endSec};
	refinedRange = index.clampRange(refinedRange, options.generation.searchRange);
	bool intraWindowViewerOriginTrimmed = false;
	bool transcriptWindowTailExtended = false;
	if (viewerPreset) {
		const ClipDuration originSearchRange = index.clampRange(
			{std::max(options.generation.searchRange.startSec, refinedRange.startSec - 1.5),
			 std::min(options.generation.searchRange.endSec, refinedRange.endSec + 2.0)},
			options.generation.searchRange);
		const double intraOriginStart = findIntraWindowViewerOriginStartSec(index, originSearchRange);
		if (intraOriginStart >= refinedRange.startSec + 1.0 &&
		    refinedRange.endSec - intraOriginStart >= std::max(6.0, minDurationSec - 4.0)) {
			refinedRange.startSec = intraOriginStart;
			intraWindowViewerOriginTrimmed = true;
		}
		if (intraWindowViewerOriginTrimmed || usedRecoveredSubspan) {
			const double extendedEnd = extendViewerRangeEndByTranscriptWindows(index, refinedRange, options);
			if (extendedEnd > refinedRange.endSec + 1.0) {
				refinedRange.endSec = extendedEnd;
				transcriptWindowTailExtended = true;
			}
		}
	}
	bool snappedToWordBoundary = false;
	if (index.hasWordTimings()) {
		const ClipDuration wordSnapped = index.snapRangeToWordBoundaries(refinedRange, options.generation.searchRange);
		snappedToWordBoundary = std::fabs(wordSnapped.startSec - refinedRange.startSec) > 0.08 ||
			std::fabs(wordSnapped.endSec - refinedRange.endSec) > 0.08;
		refinedRange = wordSnapped;
	}
	const double refinedDurationSec = refinedRange.endSec - refinedRange.startSec;
	if (refinedDurationSec < minDurationSec || refinedDurationSec > maxDurationSec + 0.1) {
		ClipCandidate diagnostic = candidate;
		diagnostic.evidence.append(QStringLiteral("exchange_arc_role_classifier:v23_context_window_dp"));
		diagnostic.evidence.append(QStringLiteral("exchange_arc_refiner_skipped:refined_duration_out_of_bounds"));
		diagnostic.evidence.append(arcEvidence(adjustedScore));
		diagnostic.evidence.append(roleEvidence(chunks, adjustedFirst, adjustedLast));
		diagnostic.evidence.append(roleReasonEvidence(chunks, adjustedFirst, adjustedLast));
		diagnostic.evidence.append(contextualRoleScoreEvidence(chunks, adjustedFirst, adjustedLast));
		diagnostic.evidence.append(windowDfsGraphEvidence(chunks, adjustedFirst, adjustedLast));
		diagnostic.evidence.append(stateMachineEvidence(chunks, adjustedFirst, adjustedLast, options));
		diagnostic.evidence.removeDuplicates();
		return diagnostic;
	}

	ClipCandidate refined = candidate;
	writeArcScores(refined, adjustedScore);
	const bool materiallyChanged = std::fabs(refinedRange.startSec - candidate.range.startSec) > 1.0 ||
		std::fabs(refinedRange.endSec - candidate.range.endSec) > 1.0;
	if (materiallyChanged) {
		refined.range = refinedRange;
		refined.firstSegmentIndex = index.firstSegmentIndexOverlapping(refined.range);
		refined.lastSegmentIndex = index.lastSegmentIndexOverlapping(refined.range);
		refined.text = index.textForRange(refined.range).simplified();
		refined.timedText = index.timedTextForRange(refined.range);
		refined.anchorText = refined.text.left(220);
		refined.evidence.append(QStringLiteral("exchange_arc_refined"));
		if (std::fabs(refinedRange.startSec - candidate.range.startSec) > 1.0)
			refined.evidence.append(QStringLiteral("exchange_arc_start_refined"));
		if (std::fabs(refinedRange.endSec - candidate.range.endSec) > 1.0)
			refined.evidence.append(QStringLiteral("exchange_arc_end_refined"));
		if (refinedRange.startSec < candidate.range.startSec - 1.0)
			refined.evidence.append(QStringLiteral("exchange_arc_context_start_extended"));
		if (extendedStartContext && refinedRange.startSec < candidate.range.startSec - 0.5)
			refined.evidence.append(QStringLiteral("exchange_arc_essential_context_prepended"));
		if (refinedRange.endSec > candidate.range.endSec + 1.0)
			refined.evidence.append(QStringLiteral("exchange_arc_conclusion_extended"));
		if (snappedToWordBoundary)
			refined.evidence.append(QStringLiteral("exchange_arc_word_boundary_snapped"));
	} else {
		refined.evidence.append(QStringLiteral("exchange_arc_kept"));
		if (snappedToWordBoundary)
			refined.evidence.append(QStringLiteral("exchange_arc_word_boundary_snapped"));
	}

	if (trimmedOpeningMeta)
		refined.evidence.append(QStringLiteral("exchange_arc_opening_meta_trimmed"));
	if (trimmedWeakOpeningPrelude)
		refined.evidence.append(QStringLiteral("exchange_arc_weak_opening_prelude_trimmed"));
	if (trimmedStaleOpeningResolution)
		refined.evidence.append(QStringLiteral("exchange_arc_stale_opening_resolution_trimmed"));
	if (trimmedToViewerMessageBoundary)
		refined.evidence.append(QStringLiteral("exchange_arc_viewer_message_boundary_trimmed"));
	if (prependedViewerMessageCue)
		refined.evidence.append(QStringLiteral("exchange_arc_viewer_message_cue_prepended"));
	if (trimmedAtNextViewerMessage)
		refined.evidence.append(QStringLiteral("exchange_arc_next_viewer_message_trimmed"));
	if (extendedTailContinuation)
		refined.evidence.append(QStringLiteral("exchange_arc_tail_continuation"));
	if (trimmedHardNoisyEnding)
		refined.evidence.append(QStringLiteral("exchange_arc_hard_topic_break_trimmed"));
	if (trimmedLongPauseTail)
		refined.evidence.append(QStringLiteral("exchange_arc_pause_tail_trimmed"));
	if (trimmedFirstResolutionTail)
		refined.evidence.append(QStringLiteral("exchange_arc_first_resolution_tail_trimmed"));
	if (extendedViewerConclusionFollowThrough)
		refined.evidence.append(QStringLiteral("exchange_arc_viewer_conclusion_followthrough_extended"));
	if (blockedTailByLongPause)
		refined.evidence.append(QStringLiteral("exchange_arc_pause_tail_blocked"));
	if (intraWindowViewerOriginTrimmed)
		refined.evidence.append(QStringLiteral("exchange_arc_intra_window_viewer_origin_trimmed"));
	if (transcriptWindowTailExtended)
		refined.evidence.append(QStringLiteral("exchange_arc_transcript_window_tail_extended"));
	refined.evidence.append(QStringLiteral("exchange_arc_refined_candidate_range:%1-%2")
		.arg(QString::number(refinedRange.startSec, 'f', 2), QString::number(refinedRange.endSec, 'f', 2)));
	refined.evidence.append(QStringLiteral("exchange_arc_role_classifier:v32_window_dfs"));
	refined.evidence.append(QStringLiteral("exchange_arc_window_dfs_limits:back12_45s_forward22_82s_split_origin_graph_intra"));
	refined.evidence.append(QStringLiteral("exchange_arc_role_classifier:v23_context_window_dp"));
	if (usedRecoveredSubspan) {
		refined.evidence.append(QStringLiteral("exchange_arc_semantic_hook_subspan_recovered"));
		if (recoveredSubspan.reason == QStringLiteral("contextual_role_dp_subspan"))
			refined.evidence.append(QStringLiteral("exchange_arc_contextual_dp_subspan_recovered"));
		if (recoveredSubspan.reason == QStringLiteral("viewer_message_window_dfs_arc"))
			refined.evidence.append(QStringLiteral("exchange_arc_viewer_message_window_dfs_recovered"));
		if (!recoveredSubspan.graphEvidence.isEmpty())
			refined.evidence.append(recoveredSubspan.graphEvidence);
		QStringList recoveryFlags;
		if (recoveredSubspan.semanticOpening)
			recoveryFlags.append(QStringLiteral("semantic_opening"));
		recoveryFlags.append(recoveredSubspan.reason.isEmpty() ? QStringLiteral("subspan_recovered") : recoveredSubspan.reason);
		recoveryFlags.append(QStringLiteral("subspan_recovered"));
		refined.evidence.append(QStringLiteral("exchange_arc_state_machine:valid score:%1 opening:%2 development:%3 conclusion:%4 flags:%5")
			.arg(QString::number(recoveredSubspan.stateMachineScore, 'f', 2),
			     QString::number(recoveredSubspan.opening, 'f', 2),
			     QString::number(recoveredSubspan.development, 'f', 2),
			     QString::number(recoveredSubspan.conclusion, 'f', 2),
			     recoveryFlags.join(QLatin1Char(','))));
	} else {
		refined.evidence.append(stateMachineEvidence(chunks, adjustedFirst, adjustedLast, options));
	}
	refined.evidence.append(arcEvidence(adjustedScore));
	refined.evidence.append(roleEvidence(chunks, adjustedFirst, adjustedLast));
	refined.evidence.append(roleReasonEvidence(chunks, adjustedFirst, adjustedLast));
	refined.evidence.append(contextualRoleScoreEvidence(chunks, adjustedFirst, adjustedLast));
	if (viewerPreset)
		refined.evidence.append(windowDfsGraphEvidence(chunks, adjustedFirst, adjustedLast));
	refined.evidence.append(boundaryEvidence(chunks, adjustedFirst, adjustedLast));
	if (viewerPreset && hasReliableViewerCueNearStart(chunks, adjustedFirst, adjustedLast))
		refined.evidence.append(QStringLiteral("exchange_arc_viewer_message_cue_confirmed"));
	refined.evidence.removeDuplicates();
	return refined;
}
