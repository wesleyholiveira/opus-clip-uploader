#include "curation/scoring/semantic-coarse-retriever.hpp"

#include "curation/scoring/text-analysis.hpp"

#include <algorithm>
#include <cmath>

using namespace Curation::Scoring;

namespace {

static double boundedScore(double value)
{
	return std::clamp(value, 0.0, 1.0);
}

static QString preparedWindowText(const QString &text, int maxChars)
{
	QString value = text.simplified();
	if (maxChars > 0 && value.size() > maxChars)
		value = value.left(maxChars);
	return value;
}

static void appendLimited(QStringList &destination, const QStringList &source, int &remaining, int maxItems)
{
	if (remaining <= 0 || maxItems <= 0)
		return;

	for (const QString &item : source) {
		const QString value = item.simplified();
		if (value.isEmpty() || destination.contains(value))
			continue;
		destination.append(value);
		--remaining;
		--maxItems;
		if (remaining <= 0 || maxItems <= 0)
			return;
	}
}

static QVector<SemanticEmbedding> embedPrototypes(const SemanticEmbeddingProvider &provider,
						  const QStringList &prototypes)
{
	QVector<SemanticEmbedding> embeddings;
	embeddings.reserve(prototypes.size());
	for (const QString &prototype : prototypes) {
		const SemanticEmbedding embedding = provider.embed(prototype);
		if (embedding.isValid())
			embeddings.append(embedding);
	}
	return embeddings;
}

static double centerSec(const ClipDuration &range)
{
	return range.startSec + ((range.endSec - range.startSec) * 0.5);
}

} // namespace

QVector<SemanticCoarseRegion> SemanticCoarseRetriever::retrieve(const TranscriptIndex &index,
	const SemanticCoarseRetrievalContext &context, const SemanticCoarseRetrievalOptions &options,
	const SemanticEmbeddingProvider *provider) const
{
	QVector<SemanticCoarseRegion> selected;
	if (!options.enabled || !provider || !provider->isAvailable() || index.isEmpty())
		return selected;

	const SemanticPrototypeSet &defaults = defaultSemanticPrototypes();

	const QVector<SemanticEmbedding> targetEmbeddings = embedPrototypes(*provider, prototypesForContext(context, defaults,
		options.maxPrototypeTexts));
	const QVector<SemanticEmbedding> viewerEmbeddings = embedPrototypes(*provider, defaults.viewerMessage.mid(0, 2));
	const QVector<SemanticEmbedding> directEmbeddings = embedPrototypes(*provider, defaults.directAnswer.mid(0, 2));
	const QVector<SemanticEmbedding> valueEmbeddings = embedPrototypes(*provider, defaults.clipValue.mid(0, 4));

	QStringList noisePrototypes = defaults.greetingNoise.mid(0, 1);
	noisePrototypes.append(defaults.streamManagement.mid(0, 2));
	noisePrototypes.append(defaults.metaNoise.mid(0, 3));
	const QVector<SemanticEmbedding> noiseEmbeddings = embedPrototypes(*provider, noisePrototypes);
	const QVector<SemanticEmbedding> metaNoiseEmbeddings = embedPrototypes(*provider, defaults.metaNoise.mid(0, 4));

	if (targetEmbeddings.isEmpty() && viewerEmbeddings.isEmpty() && directEmbeddings.isEmpty() && valueEmbeddings.isEmpty())
		return selected;

	QVector<SemanticCoarseRegion> scored;
	const QVector<ClipDuration> windows = buildWindows(index, options);
	scored.reserve(windows.size());
	for (const ClipDuration &window : windows) {
		QString text = preparedWindowText(index.textForRange(window), options.maxWindowTextChars);
		if (text.size() < options.minWindowTextChars)
			continue;

		const SemanticEmbedding windowEmbedding = provider->embed(text);
		if (!windowEmbedding.isValid())
			continue;

		SemanticCoarseRegion region;
		region.range = paddedRegion(window, index, options);
		region.targetScore = maxSimilarity(windowEmbedding, targetEmbeddings);
		region.viewerScore = maxSimilarity(windowEmbedding, viewerEmbeddings);
		region.directAnswerScore = maxSimilarity(windowEmbedding, directEmbeddings);
		region.clipValueScore = maxSimilarity(windowEmbedding, valueEmbeddings);
		region.noiseScore = maxSimilarity(windowEmbedding, noiseEmbeddings);
		region.metaNoiseScore = maxSimilarity(windowEmbedding, metaNoiseEmbeddings);
		region.score = scoreForWindow(region.targetScore, region.viewerScore, region.directAnswerScore,
			region.clipValueScore, region.noiseScore, region.metaNoiseScore, context);
		region.textSample = TextAnalysis::sampleForLog(text, 180);
		region.evidence.append(QStringLiteral("semantic_coarse_window"));
		region.evidence.append(QStringLiteral("coarse_score:%1").arg(QString::number(region.score, 'f', 2)));
		if (region.targetScore >= 0.65)
			region.evidence.append(QStringLiteral("coarse_target_match"));
		if (std::max(region.viewerScore, region.directAnswerScore) >= 0.65)
			region.evidence.append(QStringLiteral("coarse_viewer_answer_match"));
		if (region.clipValueScore >= 0.66)
			region.evidence.append(QStringLiteral("coarse_clip_value_match"));
		if (region.metaNoiseScore >= 0.68)
			region.evidence.append(QStringLiteral("coarse_meta_noise_penalty"));
		if (region.noiseScore >= 0.72)
			region.evidence.append(QStringLiteral("coarse_noise_penalty"));
		region.evidence.removeDuplicates();
		scored.append(region);
	}

	std::sort(scored.begin(), scored.end(), [](const SemanticCoarseRegion &left,
						    const SemanticCoarseRegion &right) {
		if (std::fabs(left.score - right.score) > 0.0001)
			return left.score > right.score;
		return centerSec(left.range) < centerSec(right.range);
	});

	const int limit = std::max(1, options.maxRegions);
	selected.reserve(std::min(static_cast<long long>(limit), static_cast<long long>(scored.size())));
	for (const SemanticCoarseRegion &region : scored) {
		bool blocked = false;
		for (const SemanticCoarseRegion &existing : selected) {
			if (overlapsOrTooClose(region.range, existing.range, options.minRegionSpacingSec)) {
				blocked = true;
				break;
			}
		}
		if (blocked)
			continue;

		selected.append(region);
		if (static_cast<int>(selected.size()) >= limit)
			break;
	}

	return selected;
}

QVector<ClipDuration> SemanticCoarseRetriever::buildWindows(const TranscriptIndex &index,
	const SemanticCoarseRetrievalOptions &options) const
{
	QVector<ClipDuration> windows;
	ClipDuration searchRange = options.searchRange;
	if (searchRange.endSec <= searchRange.startSec)
		searchRange = {0.0, index.durationSec()};
	searchRange = index.clampRange(searchRange, {0.0, std::max(index.durationSec(), searchRange.endSec)});
	if (searchRange.endSec <= searchRange.startSec)
		return windows;

	const double durationSec = searchRange.endSec - searchRange.startSec;
	const double windowSec = std::clamp(options.windowSec, 20.0, std::max(20.0, durationSec));
	const int maxWindows = std::max(1, options.maxWindowsToEmbed);
	const double requestedStride = std::max(5.0, options.strideSec);
	const double cappedStride = durationSec > windowSec && maxWindows > 1
		? std::max(requestedStride, (durationSec - windowSec) / static_cast<double>(maxWindows - 1))
		: requestedStride;

	for (double startSec = searchRange.startSec; startSec < searchRange.endSec; startSec += cappedStride) {
		const double endSec = std::min(searchRange.endSec, startSec + windowSec);
		if (endSec - startSec < std::min(12.0, windowSec * 0.5))
			break;
		windows.append({startSec, endSec});
		if (static_cast<int>(windows.size()) >= maxWindows)
			break;
	}

	if (windows.isEmpty())
		windows.append(searchRange);
	return windows;
}

QStringList SemanticCoarseRetriever::prototypesForContext(const SemanticCoarseRetrievalContext &context,
	const SemanticPrototypeSet &prototypes, int maxPrototypeTexts) const
{
	QStringList result;
	int remaining = std::clamp(maxPrototypeTexts, 0, 24);
	if (remaining <= 0)
		return result;

	appendLimited(result, targetPrototypesForPreset(context.presetId, context.mainTarget), remaining, 4);
	appendLimited(result, prototypes.clipValue, remaining, 5);
	appendLimited(result, prototypes.viewerMessage, remaining, 2);
	appendLimited(result, prototypes.directAnswer, remaining, 2);
	return result;
}

double SemanticCoarseRetriever::maxSimilarity(const SemanticEmbedding &embedding,
	const QVector<SemanticEmbedding> &prototypes) const
{
	double best = 0.0;
	for (const SemanticEmbedding &prototype : prototypes)
		best = std::max(best, cosineSimilarity(embedding, prototype));
	return boundedScore(best);
}

double SemanticCoarseRetriever::scoreForWindow(double targetScore, double viewerScore, double directAnswerScore,
	double clipValueScore, double noiseScore, double metaNoiseScore, const SemanticCoarseRetrievalContext &context) const
{
	const double answerScore = std::max(viewerScore, directAnswerScore);
	const double noisePenalty = (noiseScore * 0.26) + (metaNoiseScore * 0.36);
	if (context.reliableMainTarget && !context.mainTarget.trimmed().isEmpty())
		return boundedScore((targetScore * 0.42) + (clipValueScore * 0.30) + (answerScore * 0.22) - noisePenalty);
	if (context.presetId == QStringLiteral("viewer_message_response"))
		return boundedScore((clipValueScore * 0.40) + (viewerScore * 0.18) + (directAnswerScore * 0.24) +
				    (targetScore * 0.12) - noisePenalty);
	return boundedScore((clipValueScore * 0.34) + (targetScore * 0.34) + (answerScore * 0.22) - noisePenalty);
}

ClipDuration SemanticCoarseRetriever::paddedRegion(const ClipDuration &window, const TranscriptIndex &index,
	const SemanticCoarseRetrievalOptions &options) const
{
	ClipDuration searchRange = options.searchRange;
	if (searchRange.endSec <= searchRange.startSec)
		searchRange = {0.0, index.durationSec()};
	const double paddingSec = std::max(0.0, options.regionPaddingSec);
	return index.clampRange({window.startSec - paddingSec, window.endSec + paddingSec}, searchRange);
}

bool SemanticCoarseRetriever::overlapsOrTooClose(const ClipDuration &left, const ClipDuration &right,
	double minSpacingSec) const
{
	const double overlap = std::min(left.endSec, right.endSec) - std::max(left.startSec, right.startSec);
	if (overlap > 0.0)
		return true;
	if (minSpacingSec <= 0.0)
		return false;
	return std::fabs(centerSec(left) - centerSec(right)) < minSpacingSec;
}
