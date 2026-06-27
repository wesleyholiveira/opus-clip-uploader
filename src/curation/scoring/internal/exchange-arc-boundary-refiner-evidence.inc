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

