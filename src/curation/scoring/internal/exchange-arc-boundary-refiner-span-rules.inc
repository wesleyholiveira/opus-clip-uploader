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

	// This is not the old implicit opening. It is a controlled support path for real
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


