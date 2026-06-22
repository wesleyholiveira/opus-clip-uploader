#include "curation/scoring/semantic-reranker.hpp"

#include "curation/scoring/semantic-prototypes.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>

using namespace Curation::Scoring;

namespace {

static double boundedScore(double value)
{
	return std::clamp(value, 0.0, 1.0);
}

static double absoluteRerankerScore(double rawScore)
{
	if (rawScore >= 0.0 && rawScore <= 1.0)
		return boundedScore(rawScore);
	return boundedScore(1.0 / (1.0 + std::exp(-rawScore)));
}

static QVector<double> normalizedScores(const QVector<double> &rawScores)
{
	if (rawScores.isEmpty())
		return {};

	double minScore = rawScores.first();
	double maxScore = rawScores.first();
	for (const double score : rawScores) {
		minScore = std::min(minScore, score);
		maxScore = std::max(maxScore, score);
	}

	QVector<double> result;
	result.reserve(static_cast<long long>(rawScores.size()));
	if (std::fabs(maxScore - minScore) < 0.000001) {
		for (const double score : rawScores)
			result.append(boundedScore(score));
		return result;
	}

	for (const double score : rawScores)
		result.append(boundedScore((score - minScore) / (maxScore - minScore)));
	return result;
}

static double scoreAtOrZero(const QVector<double> &scores, qsizetype index)
{
	if (index < 0 || index >= scores.size())
		return 0.0;
	return absoluteRerankerScore(scores.at(index));
}

static QVector<double> absoluteScores(const QVector<double> &scores)
{
	QVector<double> result;
	result.reserve(static_cast<long long>(scores.size()));
	for (const double score : scores)
		result.append(absoluteRerankerScore(score));
	return result;
}

static bool looksSaturatedDefectBatch(const QVector<double> &absoluteDefects)
{
	if (absoluteDefects.size() < 4)
		return false;
	double minScore = absoluteDefects.first();
	double maxScore = absoluteDefects.first();
	int highCount = 0;
	for (const double score : absoluteDefects) {
		minScore = std::min(minScore, score);
		maxScore = std::max(maxScore, score);
		if (score >= 0.82)
			++highCount;
	}
	const double highRatio = static_cast<double>(highCount) / static_cast<double>(absoluteDefects.size());
	return highRatio >= 0.55 && (maxScore - minScore) <= 0.30;
}

static double calibratedDefectScore(const QVector<double> &absoluteDefects, const QVector<double> &relativeDefects,
	qsizetype index, bool saturatedBatch)
{
	const double absolute = scoreAtOrZero(absoluteDefects, index);
	const double relative = scoreAtOrZero(relativeDefects, index);
	if (!saturatedBatch)
		return absolute;

	// Rerankers are excellent at ordering documents, but their absolute scores are not reliable
	// classifiers. If a defect query marks most of the batch as defective, treat it as a
	// relative signal instead of a hard absolute veto.
	return boundedScore(std::min(absolute, 0.46 + (relative * 0.34)));
}

static bool hasCandidateLevelDefectCollapse(const ClipCandidate &candidate, double rawScore,
	double rawOpeningDefect, double rawEndingDefect, double rawStructureDefect)
{
	if (rawScore < 0.94 || rawOpeningDefect < 0.86 || rawEndingDefect < 0.86 || rawStructureDefect < 0.82)
		return false;

	const double maxDefect = std::max({rawOpeningDefect, rawEndingDefect, rawStructureDefect});
	const double minDefect = std::min({rawOpeningDefect, rawEndingDefect, rawStructureDefect});
	const bool negativeQueriesAgreeTooMuch = maxDefect - minDefect <= 0.16;
	const bool negativeNearlyEqualsPositive = rawScore - maxDefect <= 0.08;
	if (!negativeQueriesAgreeTooMuch && !negativeNearlyEqualsPositive)
		return false;

	if (!candidate.semanticScoringAvailable)
		return true;

	const double semanticPositive = std::max({candidate.scores.semanticClipValue, candidate.scores.semanticEmpathy,
		candidate.scores.semanticHook, candidate.scores.semanticOpeningHook, candidate.scores.semanticResolution,
		candidate.scores.semanticEndingResolution, candidate.scores.topicContinuity});
	const double semanticNegative = std::max({candidate.scores.semanticNoise, candidate.scores.semanticMetaNoise,
		candidate.scores.semanticOpeningMetaNoise, candidate.scores.semanticEndingMetaNoise,
		candidate.scores.semanticEndingTopicShift});
	const bool explicitHumanContext = candidate.scores.semanticDirectAnswer >= 0.64 ||
		candidate.scores.semanticViewerMessage >= 0.64 || candidate.scores.semanticTarget >= 0.64;
	const bool empathyBacked = candidate.scores.semanticEmpathy >= 0.64 &&
		(explicitHumanContext ||
		 (candidate.scores.semanticEmpathy >= semanticNegative + 0.08 &&
		  candidate.scores.semanticClipValue >= candidate.scores.semanticMetaNoise + 0.03));
	const bool socialMetaDominated = !empathyBacked &&
		candidate.scores.semanticOpeningMetaNoise >= candidate.scores.semanticOpeningHook - 0.015 &&
		candidate.scores.semanticMetaNoise >= candidate.scores.semanticClipValue - 0.015;
	if (socialMetaDominated)
		return false;

	// If positive semantic evidence is at least competitive with the negative semantic evidence,
	// then a candidate receiving ~1.0 for both "good clip" and every negative reranker query is
	// more likely a calibration/query-collapse artifact than three independent true defects.
	// Do not apply this rescue to live setup/social/smalltalk candidates that only look positive
	// because empathy or resolution prototypes are too broad.
	const bool positiveEnough = semanticPositive >= (empathyBacked ? 0.66 : 0.68) &&
		semanticPositive + (empathyBacked ? 0.07 : 0.02) >= semanticNegative;
	return positiveEnough && (explicitHumanContext || empathyBacked);
}

} // namespace

QVector<double> SemanticReranker::scoreBatch(const QString &query, const QVector<QString> &candidateTexts) const
{
	QVector<double> scores;
	scores.reserve(static_cast<long long>(candidateTexts.size()));
	for (const QString &candidateText : candidateTexts)
		scores.append(score(query, candidateText));
	return scores;
}

QVector<ClipCandidate> SemanticRerankerStage::apply(QVector<ClipCandidate> candidates,
	const SemanticRerankerContext &context, const SemanticRerankerOptions &options,
	const SemanticReranker *reranker) const
{
	if (!options.enabled) {
		for (ClipCandidate &candidate : candidates)
			candidate.evidence.append(QStringLiteral("reranker_disabled"));
		return candidates;
	}

	if (!reranker || !reranker->isAvailable()) {
		for (ClipCandidate &candidate : candidates)
			candidate.evidence.append(QStringLiteral("reranker_unavailable"));
		return candidates;
	}

	const bool hasValidSemanticEmbeddings = std::any_of(candidates.constBegin(), candidates.constEnd(),
		[](const ClipCandidate &candidate) { return candidate.semanticScoringAvailable; });
	if (!hasValidSemanticEmbeddings) {
		for (ClipCandidate &candidate : candidates)
			candidate.evidence.append(QStringLiteral("reranker_skipped_no_valid_embeddings"));
		return candidates;
	}

	QVector<QString> candidateTexts;
	candidateTexts.reserve(static_cast<long long>(candidates.size()));
	for (const ClipCandidate &candidate : candidates)
		candidateTexts.append(documentForCandidate(candidate, context));

	for (ClipCandidate &candidate : candidates)
		candidate.rerankerAttempted = true;

	const QVector<double> rawPositiveScores = reranker->scoreBatch(queryForContext(context), candidateTexts);
	if (rawPositiveScores.size() != candidates.size()) {
		QString failureReason = reranker->lastError().trimmed();
		if (failureReason.isEmpty())
			failureReason = QStringLiteral("invalid_batch_response");
		for (ClipCandidate &candidate : candidates) {
			candidate.rerankerFailed = true;
			candidate.rerankerFailureReason = failureReason;
			candidate.evidence.append(QStringLiteral("reranker_invalid_batch_response"));
			candidate.evidence.append(QStringLiteral("reranker_failure:%1").arg(failureReason.left(120)));
		}
		return candidates;
	}

	const QVector<double> rawOpeningDefects = reranker->scoreBatch(openingDefectQueryForContext(context), candidateTexts);
	const QVector<double> rawEndingDefects = reranker->scoreBatch(endingDefectQueryForContext(context), candidateTexts);
	const QVector<double> rawStructureDefects = reranker->scoreBatch(structureDefectQueryForContext(context), candidateTexts);
	const bool hasOpeningDefects = rawOpeningDefects.size() == candidates.size();
	const bool hasEndingDefects = rawEndingDefects.size() == candidates.size();
	const bool hasStructureDefects = rawStructureDefects.size() == candidates.size();

	const QVector<double> absoluteOpeningDefects = hasOpeningDefects ? absoluteScores(rawOpeningDefects) : QVector<double>();
	const QVector<double> absoluteEndingDefects = hasEndingDefects ? absoluteScores(rawEndingDefects) : QVector<double>();
	const QVector<double> absoluteStructureDefects = hasStructureDefects ? absoluteScores(rawStructureDefects) : QVector<double>();
	const QVector<double> relativeOpeningDefects = hasOpeningDefects ? normalizedScores(absoluteOpeningDefects) : QVector<double>();
	const QVector<double> relativeEndingDefects = hasEndingDefects ? normalizedScores(absoluteEndingDefects) : QVector<double>();
	const QVector<double> relativeStructureDefects = hasStructureDefects ? normalizedScores(absoluteStructureDefects) : QVector<double>();
	const bool openingDefectsSaturated = looksSaturatedDefectBatch(absoluteOpeningDefects);
	const bool endingDefectsSaturated = looksSaturatedDefectBatch(absoluteEndingDefects);
	const bool structureDefectsSaturated = looksSaturatedDefectBatch(absoluteStructureDefects);
	const bool anyDefectBatchSaturated = openingDefectsSaturated || endingDefectsSaturated || structureDefectsSaturated;

	const QVector<double> normalizedPositiveScores = normalizedScores(rawPositiveScores);
	for (qsizetype i = 0; i < candidates.size(); ++i) {
		ClipCandidate &candidate = candidates[i];
		const double rawScore = absoluteRerankerScore(rawPositiveScores.at(i));
		const double rawOpeningDefect = hasOpeningDefects ? scoreAtOrZero(absoluteOpeningDefects, i) : 0.0;
		const double rawEndingDefect = hasEndingDefects ? scoreAtOrZero(absoluteEndingDefects, i) : 0.0;
		const double rawStructureDefect = hasStructureDefects ? scoreAtOrZero(absoluteStructureDefects, i) : 0.0;
		double openingDefect = hasOpeningDefects ? calibratedDefectScore(absoluteOpeningDefects, relativeOpeningDefects, i, openingDefectsSaturated) : 0.0;
		double endingDefect = hasEndingDefects ? calibratedDefectScore(absoluteEndingDefects, relativeEndingDefects, i, endingDefectsSaturated) : 0.0;
		double structureDefect = hasStructureDefects ? calibratedDefectScore(absoluteStructureDefects, relativeStructureDefects, i, structureDefectsSaturated) : 0.0;
		const bool candidateDefectCollapse = hasCandidateLevelDefectCollapse(candidate, rawScore,
			rawOpeningDefect, rawEndingDefect, rawStructureDefect);
		if (candidateDefectCollapse) {
			// Qwen rerankers can occasionally assign an almost identical near-1.0 score to
			// the positive query and to all defect queries for the same candidate. Keep the
			// defect as a penalty, but do not let that contradictory signal become a hard veto.
			openingDefect = std::min(openingDefect, 0.56);
			endingDefect = std::min(endingDefect, 0.62);
			structureDefect = std::min(structureDefect, 0.56);
		}
		const double defectScore = std::max({openingDefect, endingDefect, structureDefect});
		const double margin = rawScore - defectScore;
		const double rerankerScore = rerankerScoreFromRaw(rawScore, boundedScore(normalizedPositiveScores.at(i)), defectScore);

		candidate.scores.reranker = rerankerScore;
		candidate.scores.rerankerRaw = rawScore;
		candidate.scores.rerankerOpeningDefect = openingDefect;
		candidate.scores.rerankerEndingDefect = endingDefect;
		candidate.scores.rerankerStructureDefect = structureDefect;
		candidate.scores.rerankerBadClip = defectScore;
		candidate.scores.rerankerClipQualityMargin = margin;
		candidate.rerankerAvailable = true;
		candidate.rerankerFailed = false;
		candidate.rerankerFailureReason.clear();
		candidate.scores.final = combineFinalScore(candidate, rerankerScore, options.contributionWeight);

		candidate.evidence.append(QStringLiteral("reranker_model:%1").arg(reranker->modelId().left(80)));
		candidate.evidence.append(QStringLiteral("reranker_language:%1").arg(normalizedSemanticLanguageCode(context.transcriptionLanguage, context.sourceLanguage)));
		candidate.evidence.append(QStringLiteral("reranker_raw:%1").arg(QString::number(rawScore, 'f', 2)));
		candidate.evidence.append(QStringLiteral("reranker_opening_defect:%1").arg(QString::number(openingDefect, 'f', 2)));
		candidate.evidence.append(QStringLiteral("reranker_ending_defect:%1").arg(QString::number(endingDefect, 'f', 2)));
		candidate.evidence.append(QStringLiteral("reranker_structure_defect:%1").arg(QString::number(structureDefect, 'f', 2)));
		candidate.evidence.append(QStringLiteral("reranker_defect:%1").arg(QString::number(defectScore, 'f', 2)));
		if (anyDefectBatchSaturated) {
			candidate.evidence.append(QStringLiteral("reranker_defect_batch_saturated"));
			candidate.evidence.append(QStringLiteral("reranker_raw_defects:%1/%2/%3")
				.arg(QString::number(rawOpeningDefect, 'f', 2), QString::number(rawEndingDefect, 'f', 2),
				     QString::number(rawStructureDefect, 'f', 2)));
		}
		if (candidateDefectCollapse) {
			candidate.evidence.append(QStringLiteral("reranker_candidate_defect_collapse"));
			candidate.evidence.append(QStringLiteral("reranker_candidate_raw_defects:%1/%2/%3")
				.arg(QString::number(rawOpeningDefect, 'f', 2), QString::number(rawEndingDefect, 'f', 2),
				     QString::number(rawStructureDefect, 'f', 2)));
		}
		candidate.evidence.append(QStringLiteral("reranker_margin:%1").arg(QString::number(margin, 'f', 2)));
		if ((rerankerScore >= 0.72 || rawScore >= 0.82) && margin >= 0.12)
			candidate.evidence.append(QStringLiteral("reranker_strong_match"));
		if (rawScore >= 0.90 && defectScore >= 0.70 && margin >= -0.04)
			candidate.evidence.append(QStringLiteral("reranker_defect_disagrees_with_positive"));
		candidate.evidence.removeDuplicates();
	}

	return candidates;
}

QString SemanticRerankerStage::queryForContext(const SemanticRerankerContext &context) const
{
	const QString target = context.mainTarget.trimmed();
	const QString languageCode = normalizedSemanticLanguageCode(context.transcriptionLanguage, context.sourceLanguage);
	QString task;
	if (isPortugueseSemanticLanguage(languageCode)) {
		task = semanticLanguageInstruction(languageCode) + QStringLiteral(
			"Classifique trechos pela qualidade como corte final. Um bom corte é uma resposta contínua para uma única mensagem do viewer, "
			"com abertura autossuficiente, desenvolvimento e primeira conclusão local. Ignore a tentação de recompensar apenas assunto interessante: "
			"o range completo precisa começar no gancho correto e terminar antes de nova interação. Use [PAUSA Xs] e timestamps como sinais de borda. ");
		if (!target.isEmpty())
			task += QStringLiteral("O melhor trecho deve ser especificamente sobre: %1. Penalize fortemente respostas genéricas de viewer, moderação, jogos, meta-chat, agradecimentos ou conversa casual que não permaneçam nesse tema do começo ao fim. ").arg(target);
		else if (context.presetId == QStringLiteral("viewer_message_response"))
			task += QStringLiteral("Prefira uma resposta completa e valiosa para uma única pergunta/mensagem do chat; aceite também um momento autossuficiente que gere curiosidade, empatia, conselho amoroso/emocional, reflexão ou história, desenvolva a ideia e chegue a um payoff local mesmo sem Q&A explícito. Penalize ranges dominados por agradecimento de presente/item virtual/social, conversa casual, estado físico ou curiosidade trivial sem arco útil. ");
		return task.simplified();
	}

	task = semanticLanguageInstruction(languageCode) + QStringLiteral(
		"Rank transcript excerpts by final clip quality. A good clip is one continuous response to a single viewer message, "
		"with a self-contained opening, development, and first local resolution. Do not reward an interesting topic alone: "
		"the full range must start at the correct hook and end before a new interaction. Use [PAUSE Xs] and timestamps as boundary signals. ");
	if (!target.isEmpty())
		task += QStringLiteral("The best excerpt must be specifically about: %1. Strongly penalize generic viewer answers, moderation, games, meta-chat, thanks, or casual talk that does not stay on that topic from start to finish. ").arg(target);
	else if (context.presetId == QStringLiteral("viewer_message_response"))
		task += QStringLiteral("Prefer one complete valuable answer to a single chat question/message; also accept a self-contained curiosity/empathy/relationship advice/story/opinion moment with development and local payoff even when it is not explicit Q&A. Penalize ranges dominated by virtual gift thanks/social/meta, casual check-ins, body status, or trivial one-off answers without a useful arc. ");
	return task.simplified();
}

QString SemanticRerankerStage::openingDefectQueryForContext(const SemanticRerankerContext &context) const
{
	const QString languageCode = normalizedSemanticLanguageCode(context.transcriptionLanguage, context.sourceLanguage);
	if (isPortugueseSemanticLanguage(languageCode)) {
		return (semanticLanguageInstruction(languageCode) + QStringLiteral(
			"Classifique somente o defeito de ABERTURA do corte. Nota alta apenas quando a abertura defeituosa domina o início "
			"e o corte deveria começar depois: saudação, agradecimento, conversa casual, pergunta fraca de check-in, comentário sobre horário/estar online/estado físico, meta da live, preparação para ler/procurar mensagem do chat, "
			"suporte técnico sobre internet/conexão/áudio/vídeo/configuração antes do conteúdo útil, agradecimento por presente/item virtual antes do assunto real, leitura de chat sem contexto útil, curiosidade trivial sem arco, ou meio de frase sem contexto. "
			"Nota baixa quando o início já contém a questão, o contexto essencial ou a primeira frase autossuficiente da resposta.")).simplified();
	}
	return (semanticLanguageInstruction(languageCode) + QStringLiteral(
		"Score only the OPENING defect. Give a high score only when the defective opening dominates and the clip should start later: greeting, thanks, casual chat, stream meta, "
		"procedural setup to find/read a chat message, casual check-in, availability/hour/body-status comment, technical troubleshooting about internet/connection/audio/video/setup before useful content, virtual gift/item thank-you before the real subject, unhelpful chat reading, trivial curiosity without an arc, or mid-sentence. Give a low score when the start already contains the issue, essential context, or first self-contained answer sentence.")).simplified();
}

QString SemanticRerankerStage::endingDefectQueryForContext(const SemanticRerankerContext &context) const
{
	const QString languageCode = normalizedSemanticLanguageCode(context.transcriptionLanguage, context.sourceLanguage);
	if (isPortugueseSemanticLanguage(languageCode)) {
		return (semanticLanguageInstruction(languageCode) + QStringLiteral(
			"Classifique somente o defeito de FINAL do corte. Nota alta apenas quando o final defeituoso domina a cauda e o corte deveria terminar antes: "
			"continua após a primeira conclusão local, corta antes da conclusão real, inclui pausa seguida de nova pergunta/vocativo, outro comentário do chat, suporte técnico/meta/social, agradecimento de presente, sermão ou assunto diferente depois do payoff. "
			"Nota baixa quando termina na resolução natural da mesma troca ou contém apenas palavras finais necessárias.")).simplified();
	}
	return (semanticLanguageInstruction(languageCode) + QStringLiteral(
		"Score only the ENDING defect. Give a high score only when the defective ending dominates the tail and the clip should end earlier: it continues after the first local resolution, cuts before the real conclusion, includes tail material, "
		"a pause followed by a new question/vocative, another chat comment, technical troubleshooting, social/meta, a rant/sermon or different topic after the payoff. Give a low score when it ends at the natural resolution or only includes necessary closing words.")).simplified();
}

QString SemanticRerankerStage::structureDefectQueryForContext(const SemanticRerankerContext &context) const
{
	const QString languageCode = normalizedSemanticLanguageCode(context.transcriptionLanguage, context.sourceLanguage);
	if (isPortugueseSemanticLanguage(languageCode)) {
		return (semanticLanguageInstruction(languageCode) + QStringLiteral(
			"Classifique defeitos estruturais do corte inteiro. Nota alta apenas quando há defeito estrutural dominante: ausência clara de gancho, desenvolvimento ou conclusão, "
			"mistura de múltiplas interações, trecho dominado por agradecimento de presente/social/meta, resposta curta/trivial sem desenvolvimento, gancho isolado sem conclusão, ou ausência de empatia/conselho/curiosidade real. "
			"Nota baixa significa arco único aceitável com abertura, desenvolvimento e conclusão local, inclusive história, opinião ou momento de curiosidade sem Q&A explícito.")).simplified();
	}
	return (semanticLanguageInstruction(languageCode) + QStringLiteral(
		"Score structural defects for the whole clip. Give a high score only when there is a dominant structural defect: clearly no hook, no development, no conclusion, mixed interactions, "
		"mostly gift thanks/social/meta instead of a valuable answer, a short/trivial answer without development, an isolated hook without conclusion, or no real empathy/advice/curiosity. Give a low score when there is an acceptable single arc with opening, development, and local conclusion, including a story, opinion, or curiosity moment without explicit Q&A.")).simplified();
}

QString SemanticRerankerStage::documentForCandidate(const ClipCandidate &candidate,
	const SemanticRerankerContext &context) const
{
	const double durationSec = candidate.range.endSec - candidate.range.startSec;
	const QString languageCode = normalizedSemanticLanguageCode(context.transcriptionLanguage, context.sourceLanguage);
	const QString transcript = candidate.timedText.trimmed().isEmpty() ? candidate.text.simplified() : candidate.timedText.simplified();
	QString document = QStringLiteral("%1Duration: %2s. Pausa antes: %3s. Pausa depois: %4s. Maior pausa interna: %5s. Transcript with timestamps and pauses: %6")
		.arg(semanticDocumentPrefix(languageCode), QString::number(durationSec, 'f', 1),
		     QString::number(candidate.scores.pauseBeforeSec, 'f', 1),
		     QString::number(candidate.scores.pauseAfterSec, 'f', 1),
		     QString::number(candidate.scores.maxInternalPauseSec, 'f', 1), transcript);
	return document.simplified();
}

double SemanticRerankerStage::rerankerScoreFromRaw(double rawScore, double normalizedScore, double defectScore) const
{
	const double absolute = boundedScore(rawScore);
	const double relative = boundedScore(normalizedScore);
	const double defect = boundedScore(defectScore);
	const double marginBonus = std::max(0.0, absolute - defect) * 0.16;
	return boundedScore((absolute * 0.74) + (relative * 0.16) + marginBonus - (defect * 0.14));
}

double SemanticRerankerStage::combineFinalScore(const ClipCandidate &candidate, double rerankerScore,
	double contributionWeight) const
{
	const double weight = std::clamp(contributionWeight, 0.0, 0.55);
	return boundedScore((candidate.scores.final * (1.0 - weight)) + (rerankerScore * weight));
}
