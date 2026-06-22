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
	bool terminalFollowThrough = false;
	int openingIndex = -1;
	int firstDevelopmentIndex = -1;
	int conclusionIndex = -1;
	double score = 0.0;
	double opening = 0.0;
	double development = 0.0;
	double conclusion = 0.0;
	double penalty = 0.0;
};

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
		if (current.firstIndex >= 0 && current.lastIndex >= current.firstIndex && text.size() >= 20) {
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
		const bool shouldFlush = !startsNewChunk &&
			(gapSec > 0.95 || nextDurationSec > 3.75 || nextChars > 190);
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
	const double positive = std::max({chunk.target, chunk.value, chunk.hook, chunk.resolution});
	const double defect = std::max(chunk.meta, chunk.shift);
	chunk.openingScore = boundedScore((chunk.hook * 0.44) + (chunk.target * 0.30) + (chunk.value * 0.20) -
		(chunk.meta * 0.32) - (chunk.shift * 0.12));
	chunk.developmentScore = boundedScore((chunk.value * 0.46) + (chunk.target * 0.18) +
		(chunk.resolution * 0.18) + (chunk.hook * 0.10) - (chunk.meta * 0.24) - (chunk.shift * 0.10));
	const double strongestEndingDefect = std::max(chunk.meta, chunk.shift);
	const double strictConclusion = boundedScore((chunk.resolution * 0.54) + (chunk.value * 0.22) +
		(chunk.target * 0.08) - (chunk.meta * 0.26) - (chunk.shift * 0.24));
	const double semanticConclusionFallback = boundedScore((chunk.resolution * 0.62) + (chunk.value * 0.18) +
		(chunk.target * 0.10) - (strongestEndingDefect * 0.18));
	chunk.conclusionScore = std::max(strictConclusion, semanticConclusionFallback);
	chunk.defectScore = boundedScore((defect * 0.72) + (chunk.shift * 0.18) - (positive * 0.22));

	const bool metaCompetesWithContent = defect >= positive - 0.04;
	const bool weakSelfContainedOpening = chunk.openingScore < 0.46 && chunk.target < 0.58 && chunk.resolution < 0.55;
	if ((chunk.defectScore >= 0.54 && metaCompetesWithContent) ||
	    (weakSelfContainedOpening && chunk.meta >= 0.54 && chunk.meta >= chunk.hook - 0.03)) {
		chunk.role = chunk.shift >= chunk.meta + 0.03 ? ArcChunkRole::TailOrNewTurn : ArcChunkRole::SocialOrMetaPrelude;
		return;
	}

	// Resolution prototypes are intentionally broad, especially in Portuguese. If we
	// classify before checking hook/body signals, an entire answer can collapse into
	// "resolution, resolution, resolution" and the boundary gate has no way to tell
	// whether the viewer setup or development was preserved. Prefer opening/body roles
	// whenever they are competitive with the conclusion signal.
	const bool strongOpening = chunk.openingScore >= 0.40 &&
		(chunk.hook >= 0.52 || chunk.target >= 0.52 || chunk.openingScore >= chunk.developmentScore - 0.04) &&
		chunk.openingScore >= chunk.conclusionScore - 0.07;
	if (strongOpening) {
		chunk.role = ArcChunkRole::OpeningCandidate;
		return;
	}

	const bool strongDevelopment = chunk.developmentScore >= 0.38 &&
		(chunk.value >= 0.52 || positive >= 0.58 || chunk.developmentScore >= chunk.openingScore - 0.04) &&
		chunk.developmentScore >= chunk.conclusionScore - 0.06;
	if (strongDevelopment) {
		chunk.role = ArcChunkRole::Development;
		return;
	}

	const bool strongConclusion = chunk.resolution >= 0.56 &&
		chunk.conclusionScore >= std::max(chunk.openingScore, chunk.developmentScore) - 0.02;
	if (strongConclusion) {
		chunk.role = ArcChunkRole::LocalResolution;
		return;
	}

	if (chunk.openingScore >= chunk.developmentScore + 0.03 && chunk.openingScore >= 0.34) {
		chunk.role = ArcChunkRole::OpeningCandidate;
		return;
	}
	if (chunk.developmentScore >= 0.34 || positive >= 0.58) {
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


static bool isPauseSeparatedSemanticTurn(const ArcChunk &previous, const ArcChunk &next)
{
	const double pauseSec = std::max(previous.pauseAfterSec, next.pauseBeforeSec);
	if (pauseSec < 1.45)
		return false;
	const double previousResolution = std::max(previous.conclusionScore, previous.resolution);
	const double nextDefect = std::max(next.defectScore, next.shift);
	const double continuity = cosineSimilarity(previous.embedding, next.embedding);
	const bool nextLooksDifferent = next.role == ArcChunkRole::TailOrNewTurn ||
		next.shift >= std::max({next.value, next.target, next.resolution}) - 0.02 || continuity < 0.52;
	return previousResolution >= 0.42 && nextLooksDifferent &&
		(pauseSec >= 1.85 || (pauseSec >= 1.45 && nextDefect >= 0.58));
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
	if (isPauseSeparatedSemanticTurn(current, next) || isTailRisk(chunks, nextIndex) || isBlockingArcRole(next.role))
		return false;

	const double pauseSec = std::max(current.pauseAfterSec, next.pauseBeforeSec);
	const double continuity = cosineSimilarity(current.embedding, next.embedding);
	const double nextPositive = std::max({next.target, next.value, next.resolution, next.hook, next.developmentScore});
	const double nextDefect = std::max({next.meta, next.shift, next.defectScore});
	const bool stillAnswerBody = next.role == ArcChunkRole::Development ||
		next.role == ArcChunkRole::LocalResolution || next.developmentScore >= 0.40 ||
		next.conclusionScore >= 0.42 || next.resolution >= 0.50 || next.value >= 0.52;
	const bool notNewTopic = next.shift < nextPositive + 0.09 && nextDefect < nextPositive + 0.13;
	const bool connectedEnough = continuity >= 0.32 || pauseSec <= 0.85 ||
		(next.target >= 0.55 && next.value >= nextDefect - 0.08);
	return pauseSec <= 1.70 && stillAnswerBody && notNewTopic && connectedEnough;
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
	if (isPauseSeparatedSemanticTurn(current, next) || isTailRisk(chunks, nextIndex))
		return false;

	const double pauseSec = std::max(current.pauseAfterSec, next.pauseBeforeSec);
	const double nextDurationSec = std::max(0.0, next.endSec - next.startSec);
	const double continuity = cosineSimilarity(current.embedding, next.embedding);
	const double nextPositive = std::max({next.target, next.value, next.resolution, next.hook, next.developmentScore});
	const double nextDefect = std::max({next.meta, next.shift, next.defectScore});
	const bool sameAnswerFollowThrough = next.role == ArcChunkRole::LocalResolution ||
		next.role == ArcChunkRole::Development || next.resolution >= 0.48 || next.value >= 0.50;
	const bool notNewTopic = next.shift < nextPositive + 0.08 && nextDefect < nextPositive + 0.12;
	const bool closeEnough = continuity >= 0.34 || pauseSec <= 0.75;

	return nextDurationSec <= 7.5 && pauseSec <= 1.25 && sameAnswerFollowThrough && notNewTopic && closeEnough;
}

static bool isViewerConclusionFollowThrough(const QVector<ArcChunk> &chunks, int currentLast, int nextIndex)
{
	if (currentLast < 0 || nextIndex <= currentLast || nextIndex >= static_cast<int>(chunks.size()))
		return false;

	const ArcChunk &current = chunks.at(currentLast);
	const ArcChunk &next = chunks.at(nextIndex);
	if (isPauseSeparatedSemanticTurn(current, next) || isTailRisk(chunks, nextIndex))
		return false;

	const double nextDurationSec = std::max(0.0, next.endSec - next.startSec);
	const double continuity = cosineSimilarity(current.embedding, next.embedding);
	const double nextPositive = std::max({next.target, next.value, next.resolution, next.hook, next.developmentScore});
	const double nextDefect = std::max(next.meta, next.shift);
	const bool nextStillResolvesSameAnswer = next.role == ArcChunkRole::LocalResolution ||
		next.role == ArcChunkRole::Development || next.resolution >= 0.52 || next.value >= 0.54;
	const bool notNewTopicHook = next.shift < nextPositive + 0.03 && nextDefect < nextPositive + 0.08;
	return nextDurationSec <= 5.8 && continuity >= 0.46 && nextStillResolvesSameAnswer && notNewTopicHook;
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
		const bool shortSameAnswerTail = durationSec <= 5.8 && pauseSec <= 1.30 &&
			(continuity >= 0.36 || pauseSec <= 0.65) && currentPositive >= currentDefect - 0.08;
		const bool conclusionSignalCompetitive = current.conclusionScore >= current.developmentScore - 0.07 ||
			current.resolution >= 0.48 || current.value >= 0.54;
		if (followsResolution && shortSameAnswerTail && conclusionSignalCompetitive)
			chunks[i].role = ArcChunkRole::LocalResolution;
	}
}

static bool canBeContextualOpening(const ArcChunk &chunk)
{
	if (isBlockingArcRole(chunk.role) || chunk.role == ArcChunkRole::LocalResolution)
		return false;
	const double opening = std::max(chunk.openingScore, chunk.hook);
	const double positive = std::max({chunk.target, chunk.value, chunk.hook, chunk.developmentScore});
	const double defect = std::max({chunk.meta, chunk.shift, chunk.defectScore});
	return chunk.role == ArcChunkRole::OpeningCandidate ||
		(opening >= 0.38 && positive >= defect - 0.08) ||
		(chunk.role == ArcChunkRole::Development && chunk.openingScore >= 0.34 && positive >= 0.54);
}

static bool canBeContextualDevelopment(const ArcChunk &chunk)
{
	if (isBlockingArcRole(chunk.role))
		return false;
	const double positive = std::max({chunk.value, chunk.target, chunk.developmentScore, chunk.hook});
	const double defect = std::max({chunk.meta, chunk.shift, chunk.defectScore});
	return chunk.role == ArcChunkRole::Development ||
		(chunk.role == ArcChunkRole::OpeningCandidate && chunk.value >= 0.48) ||
		(positive >= 0.56 && chunk.developmentScore >= chunk.conclusionScore - 0.10 && positive >= defect - 0.08);
}

static bool canBeContextualConclusion(const QVector<ArcChunk> &chunks, int first, int index)
{
	if (index < first || index >= static_cast<int>(chunks.size()))
		return false;
	const ArcChunk &chunk = chunks.at(index);
	if (isBlockingArcRole(chunk.role))
		return false;
	const double endingDefect = std::max({chunk.meta, chunk.shift, chunk.defectScore});
	const double endingSignal = std::max({chunk.conclusionScore, chunk.resolution, chunk.value * 0.92});
	const bool directConclusion = chunk.role == ArcChunkRole::LocalResolution ||
		(chunk.conclusionScore >= 0.46 && chunk.resolution >= 0.50) ||
		(endingSignal >= 0.54 && endingSignal >= endingDefect - 0.10);
	if (directConclusion)
		return true;

	// A final body chunk can be the natural closing phrase if it is short and follows a
	// local resolution from the same answer.
	if (index > first && chunk.role == ArcChunkRole::Development) {
		const ArcChunk &previous = chunks.at(index - 1);
		const double durationSec = std::max(0.0, chunk.endSec - chunk.startSec);
		const double pauseSec = std::max(previous.pauseAfterSec, chunk.pauseBeforeSec);
		const double continuity = cosineSimilarity(previous.embedding, chunk.embedding);
		const bool followsConclusion = previous.role == ArcChunkRole::LocalResolution ||
			previous.conclusionScore >= 0.48 || previous.resolution >= 0.54;
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
		score.penalty = 0.42;
		return score;
	}

	bool sawDevelopment = false;
	bool sawConclusion = false;
	bool sawPrematureConclusion = false;
	int developmentCount = 0;
	int conclusionIndex = -1;
	for (int i = first; i <= last; ++i) {
		const ArcChunk &chunk = chunks.at(i);
		if (isBlockingArcRole(chunk.role)) {
			score.penalty = 0.50;
			return score;
		}

		if (i == first) {
			score.openingIndex = i;
			continue;
		}

		const bool isConclusion = canBeContextualConclusion(chunks, first, i);
		const bool isDevelopment = canBeContextualDevelopment(chunk);
		if (sawConclusion && isDevelopment && !isConclusion) {
			score.penalty = 0.36;
			return score;
		}
		if (isConclusion) {
			if (i < last)
				sawPrematureConclusion = true;
			sawConclusion = true;
			conclusionIndex = i;
			continue;
		}
		if (isDevelopment) {
			sawDevelopment = true;
			if (score.firstDevelopmentIndex < 0)
				score.firstDevelopmentIndex = i;
			++developmentCount;
			continue;
		}
		score.penalty = 0.28;
		return score;
	}

	if (!sawDevelopment && developmentCount <= 0) {
		// Allow a compact two-block answer where the opening block also contains body.
		const ArcChunk &opening = chunks.at(first);
		sawDevelopment = opening.developmentScore >= 0.46 || opening.value >= 0.58;
	}
	if (!sawDevelopment || !canBeContextualConclusion(chunks, first, last)) {
		score.penalty = 0.32;
		return score;
	}

	const ArcChunk &opening = chunks.at(first);
	const ArcChunk &ending = chunks.at(last);
	score.valid = true;
	score.implicitOpening = opening.role == ArcChunkRole::Development;
	score.terminalFollowThrough = ending.role == ArcChunkRole::Development;
	score.conclusionIndex = conclusionIndex >= 0 ? conclusionIndex : last;
	score.opening = boundedScore(std::max(opening.openingScore, opening.hook) -
		(score.implicitOpening ? 0.045 : 0.0));
	score.development = averageScore(chunks, first, last, &ArcChunk::developmentScore);
	if (developmentCount > 0 && score.firstDevelopmentIndex >= 0)
		score.development = std::max(score.development,
			averageScore(chunks, score.firstDevelopmentIndex, std::max(score.firstDevelopmentIndex, last - 1),
				&ArcChunk::developmentScore));
	score.conclusion = boundedScore(std::max(ending.conclusionScore, ending.resolution) -
		(score.terminalFollowThrough ? 0.025 : 0.0));
	const double cohesion = cohesionScore(chunks, first, last);
	const double defect = averageScore(chunks, first, last, &ArcChunk::defectScore);
	const double prematurePenalty = sawPrematureConclusion && !score.terminalFollowThrough ? 0.06 : 0.0;
	score.score = boundedScore((score.opening * 0.30) + (score.development * 0.30) +
		(score.conclusion * 0.30) + (cohesion * 0.14) - (defect * 0.12) - prematurePenalty);
	return score;
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
		return QStringLiteral("exchange_arc_state_machine:invalid penalty:%1")
			.arg(QString::number(score.penalty, 'f', 2));
	QStringList flags;
	if (score.implicitOpening)
		flags.append(QStringLiteral("implicit_opening"));
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
	if (!viewerPreset && originalDurationSec <= std::max(options.generation.minDurationSec + 4.0, 20.0))
		return candidate;
	if (!options.embeddingProvider || !options.embeddingProvider->isAvailable())
		return candidate;

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
		return candidate;
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

	if (best.first < 0 || best.last < best.first || best.score < 0.34)
		return candidate;

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
		const bool normalContinuation = isSemanticContinuation(chunks, adjustedLast, adjustedLast + 1);
		const bool viewerContinuation = viewerPreset && isViewerAnswerContinuation(chunks, adjustedLast, adjustedLast + 1) &&
			viewerTailExtensionSec + std::max(0.0, chunks.at(adjustedLast + 1).endSec - chunks.at(adjustedLast + 1).startSec) <= 16.0;
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
		       (isViewerConclusionFollowThrough(chunks, adjustedLast, adjustedLast + 1) ||
			shouldExtendViewerConclusionFollowThroughSoft(chunks, adjustedLast, adjustedLast + 1) ||
			isViewerAnswerContinuation(chunks, adjustedLast, adjustedLast + 1)) &&
		       canUseSpan(chunks, adjustedFirst, adjustedLast + 1, options) &&
		       chunks.at(adjustedLast + 1).endSec - chunks.at(earlyConclusionEnd).endSec <= 16.0) {
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

	ClipDuration refinedRange{chunks.at(adjustedFirst).startSec, chunks.at(adjustedLast).endSec};
	refinedRange = index.clampRange(refinedRange, options.generation.searchRange);
	bool snappedToWordBoundary = false;
	if (index.hasWordTimings()) {
		const ClipDuration wordSnapped = index.snapRangeToWordBoundaries(refinedRange, options.generation.searchRange);
		snappedToWordBoundary = std::fabs(wordSnapped.startSec - refinedRange.startSec) > 0.08 ||
			std::fabs(wordSnapped.endSec - refinedRange.endSec) > 0.08;
		refinedRange = wordSnapped;
	}
	const double refinedDurationSec = refinedRange.endSec - refinedRange.startSec;
	if (refinedDurationSec < minDurationSec || refinedDurationSec > maxDurationSec + 0.1)
		return candidate;

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
	refined.evidence.append(arcEvidence(adjustedScore));
	refined.evidence.append(roleEvidence(chunks, adjustedFirst, adjustedLast));
	refined.evidence.append(stateMachineEvidence(chunks, adjustedFirst, adjustedLast, options));
	refined.evidence.append(boundaryEvidence(chunks, adjustedFirst, adjustedLast));
	refined.evidence.removeDuplicates();
	return refined;
}
