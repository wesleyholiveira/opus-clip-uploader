#include "curation/scoring/exchange-arc-boundary-refiner.hpp"

#include "curation/scoring/semantic-prototypes.hpp"

#include <QStringList>

#include <algorithm>
#include <cmath>
#include <initializer_list>

using namespace Curation::Scoring;

namespace {

static double boundedScore(double value)
{
	return std::clamp(value, 0.0, 1.0);
}

enum class ArcChunkRole {
	Unknown,
	OpeningCandidate,
	Development,
	LocalResolution,
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
	if (chunk.conclusionScore >= std::max(chunk.openingScore, chunk.developmentScore) - 0.02 &&
	    chunk.resolution >= 0.52) {
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
	return chunk.role == ArcChunkRole::SocialOrMetaPrelude ||
		(chunk.meta >= 0.56 && chunk.meta >= positive - 0.02 && chunk.openingScore < 0.34);
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
	if (isSemanticPrelude(previous))
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
	const bool tailRisk = isTailRisk(chunks, last);
	score.opening = boundedScore(chunks.at(first).openingScore - (openingPrelude ? 0.18 : 0.0) -
		(weakOpeningPrelude && !openingPrelude ? 0.12 : 0.0));
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
	return score;
}

static int firstLocalResolutionEnd(const QVector<ArcChunk> &chunks, int first, int last,
	const ExchangeArcBoundaryRefinementOptions &options)
{
	if (first < 0 || last <= first || last >= static_cast<int>(chunks.size()))
		return -1;

	const ArcSpanScore fullScore = scoreSpan(chunks, first, last, options);
	int bestEnd = -1;
	double bestScore = 0.0;
	for (int end = first; end < last; ++end) {
		if (!canUseSpan(chunks, first, end, options))
			continue;
		const double trimmedTailSec = chunks.at(last).endSec - chunks.at(end).endSec;
		if (trimmedTailSec < 5.0)
			continue;

		const ArcChunk &ending = chunks.at(end);
		const ArcChunk &next = chunks.at(end + 1);
		const ArcSpanScore shortened = scoreSpan(chunks, first, end, options);
		const double nextPositive = std::max({next.target, next.value, next.hook, next.resolution});
		const double nextDefect = std::max({next.meta, next.shift, next.defectScore});
		const bool nextLooksTail = isPauseSeparatedSemanticTurn(ending, next) || isTailRisk(chunks, end + 1) ||
			next.role == ArcChunkRole::TailOrNewTurn || next.shift >= nextPositive - 0.02 ||
			(next.meta >= nextPositive - 0.01 && next.openingScore < 0.42);
		const bool reachesConclusion = shortened.conclusion >= 0.42 || ending.conclusionScore >= 0.42 ||
			ending.resolution >= 0.58;
		const bool hasDevelopment = shortened.development >= 0.46 || (end - first) >= 2;
		const bool keepsEnoughQuality = shortened.score + 0.035 >= fullScore.score || trimmedTailSec >= 9.0;
		if (!nextLooksTail || !reachesConclusion || !hasDevelopment || !keepsEnoughQuality)
			continue;

		const double candidateScore = shortened.score + (trimmedTailSec >= 10.0 ? 0.05 : 0.0) - (nextDefect * 0.03);
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
	if (originalDurationSec <= std::max(options.generation.minDurationSec + 4.0, 20.0))
		return candidate;
	if (!options.embeddingProvider || !options.embeddingProvider->isAvailable())
		return candidate;

	ClipCandidate analysisCandidate = candidate;
	const bool viewerPreset = options.scoring.presetId == QStringLiteral("viewer_message_response");
	if (viewerPreset) {
		const ClipDuration expandedRange = index.clampRange(
			{std::max(options.generation.searchRange.startSec, candidate.range.startSec - 6.5),
			 std::min(options.generation.searchRange.endSec, candidate.range.endSec + 22.0)},
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
			const bool similarCleanerOpening = std::fabs(candidateScore.score - best.score) <= 0.018 &&
				best.first >= 0 && first > best.first && candidateScore.opening >= best.opening + 0.030 &&
				candidateScore.boundaryCleanliness >= best.boundaryCleanliness - 0.025;
			const bool similarShorterCleanerEnding = std::fabs(candidateScore.score - best.score) <= 0.020 &&
				best.first >= 0 && first == best.first && last < best.last &&
				candidateScore.conclusion >= best.conclusion - 0.010 && candidateScore.tailRisk <= best.tailRisk + 0.015;
			if (betterScore || similarCleanerOpening || similarShorterCleanerEnding)
				best = candidateScore;
		}
	}

	if (best.first < 0 || best.last < best.first || best.score < 0.34)
		return candidate;

	int adjustedFirst = best.first;
	int adjustedLast = best.last;
	bool trimmedOpeningMeta = false;
	bool trimmedWeakOpeningPrelude = false;
	bool extendedTailContinuation = false;
	bool trimmedHardNoisyEnding = false;
	bool trimmedLongPauseTail = false;
	bool blockedTailByLongPause = false;
	bool extendedStartContext = false;
	bool trimmedFirstResolutionTail = false;

	while (adjustedFirst < adjustedLast && isSemanticPrelude(chunks.at(adjustedFirst)) &&
	       canUseSpan(chunks, adjustedFirst + 1, adjustedLast, options)) {
		++adjustedFirst;
		trimmedOpeningMeta = true;
	}
	while (adjustedFirst < adjustedLast && isWeakOpeningPrelude(chunks, adjustedFirst) &&
	       canUseSpan(chunks, adjustedFirst + 1, adjustedLast, options)) {
		++adjustedFirst;
		trimmedWeakOpeningPrelude = true;
	}

	int contextExtensions = 0;
	while (contextExtensions < 3 && adjustedFirst > 0 && shouldPrependOpeningContext(chunks, adjustedFirst) &&
	       isEssentialOpeningContext(chunks.at(adjustedFirst - 1), chunks.at(adjustedFirst)) &&
	       canUseSpan(chunks, adjustedFirst - 1, adjustedLast, options)) {
		--adjustedFirst;
		++contextExtensions;
		extendedStartContext = true;
	}

	int tailExtensions = 0;
	while (tailExtensions < 6 && adjustedLast + 1 < static_cast<int>(chunks.size()) &&
	       isSemanticContinuation(chunks, adjustedLast, adjustedLast + 1) &&
	       canUseSpan(chunks, adjustedFirst, adjustedLast + 1, options)) {
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
	if (extendedTailContinuation)
		refined.evidence.append(QStringLiteral("exchange_arc_tail_continuation"));
	if (trimmedHardNoisyEnding)
		refined.evidence.append(QStringLiteral("exchange_arc_hard_topic_break_trimmed"));
	if (trimmedLongPauseTail)
		refined.evidence.append(QStringLiteral("exchange_arc_pause_tail_trimmed"));
	if (trimmedFirstResolutionTail)
		refined.evidence.append(QStringLiteral("exchange_arc_first_resolution_tail_trimmed"));
	if (blockedTailByLongPause)
		refined.evidence.append(QStringLiteral("exchange_arc_pause_tail_blocked"));
	refined.evidence.append(arcEvidence(adjustedScore));
	refined.evidence.append(roleEvidence(chunks, adjustedFirst, adjustedLast));
	refined.evidence.append(boundaryEvidence(chunks, adjustedFirst, adjustedLast));
	refined.evidence.removeDuplicates();
	return refined;
}
