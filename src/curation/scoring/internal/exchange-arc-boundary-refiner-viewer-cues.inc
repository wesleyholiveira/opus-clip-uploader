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

