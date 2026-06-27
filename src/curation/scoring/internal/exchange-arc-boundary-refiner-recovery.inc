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



