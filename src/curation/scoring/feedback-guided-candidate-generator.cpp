#include "curation/scoring/feedback-guided-candidate-generator.hpp"

#include "curation/curation-signals.hpp"

#include <algorithm>
#include <QPair>
#include <cmath>

namespace Curation::Scoring {

namespace {

static bool validRange(const ClipDuration &range)
{
	return std::isfinite(range.startSec) && std::isfinite(range.endSec) && range.endSec > range.startSec;
}

static double durationSec(const ClipDuration &range)
{
	return std::max(0.0, range.endSec - range.startSec);
}

static ClipDuration clampToGeneration(const TranscriptIndex &index, const CandidateGenerationOptions &options,
	ClipDuration range)
{
	range = index.clampRange(range, options.searchRange);
	const double duration = durationSec(range);
	if (duration <= 0.0)
		return {};
	if (duration < options.minDurationSec) {
		const double missing = options.minDurationSec - duration;
		range.startSec -= missing * 0.35;
		range.endSec += missing * 0.65;
		range = index.clampRange(range, options.searchRange);
	}
	if (durationSec(range) > options.maxDurationSec + 1.0)
		range.endSec = range.startSec + options.maxDurationSec;
	return index.clampRange(range, options.searchRange);
}

static bool rangeFitsGeneration(const ClipDuration &range, const CandidateGenerationOptions &options)
{
	const double duration = durationSec(range);
	return duration >= std::max(4.0, options.minDurationSec * 0.45) && duration <= options.maxDurationSec + 1.0;
}

static double rangeDistance(const ClipDuration &a, const ClipDuration &b)
{
	return std::fabs(a.startSec - b.startSec) + std::fabs(a.endSec - b.endSec);
}

static ClipDuration rangeForSegmentWindow(const TranscriptIndex &index, int first, int last)
{
	const TranscriptSegment *firstSegment = index.segmentAt(first);
	const TranscriptSegment *lastSegment = index.segmentAt(last);
	if (!firstSegment || !lastSegment)
		return {};
	return ClipDuration{firstSegment->startSec, lastSegment->endSec};
}

} // namespace

bool FeedbackGuidedCandidateGenerator::similarSeedExists(const QVector<FeedbackGuidedCandidateSeed> &seeds,
	const ClipDuration &range) const
{
	for (const FeedbackGuidedCandidateSeed &seed : seeds) {
		if (FeedbackSimilarityScorer::rangeSimilarity(seed.range, range) >= 0.72 || rangeDistance(seed.range, range) <= 5.0)
			return true;
	}
	return false;
}

bool FeedbackGuidedCandidateGenerator::appendSeed(QVector<FeedbackGuidedCandidateSeed> &seeds,
	const TranscriptIndex &index, const Curation::Feedback::FeedbackRangeMemory &memory,
	const FeedbackGuidedCandidateGenerationOptions &options,
	FeedbackGuidedCandidateSeed seed) const
{
	seed.range = clampToGeneration(index, options.generation, seed.range);
	if (!validRange(seed.range) || !rangeFitsGeneration(seed.range, options.generation))
		return false;
	if (similarSeedExists(seeds, seed.range))
		return false;

	FeedbackSimilarityScorer scorer;
	const FeedbackSimilarityFeatures features = scorer.scoreRange(seed.range, index.textForRange(seed.range), index, memory);
	if (features.negativeRangeContamination && features.negativeScore >= 0.68 && features.margin <= 0.08)
		return false;
	if (features.negativeRangeContamination && !features.explainedByPositiveRange && features.negativeScore >= features.positiveScore + 0.14)
		return false;
	if (features.negativeScore >= 0.62 && features.margin < -0.10)
		return false;

	seed.priorScore = std::max(seed.priorScore, 0.50 + (std::max(0.0, features.margin) * 0.25));
	seed.evidence.append(QStringLiteral("feedback_guided_seed"));
	seed.evidence.append(features.evidence);
	seed.evidence.removeDuplicates();
	seeds.append(seed);
	return true;
}

void FeedbackGuidedCandidateGenerator::appendPositiveRangeSeeds(QVector<FeedbackGuidedCandidateSeed> &seeds,
	const TranscriptIndex &index, const Curation::Feedback::FeedbackRangeMemory &memory,
	const FeedbackGuidedCandidateGenerationOptions &options) const
{
	for (const Curation::Feedback::FeedbackRangeSignal &positive : memory.positiveRanges) {
		if (static_cast<int>(seeds.size()) >= options.maxSeeds)
			return;
		if (!validRange(positive.range))
			continue;

		FeedbackGuidedCandidateSeed exact;
		exact.range = positive.range;
		exact.source = QStringLiteral("feedback_positive_exact_seed");
		exact.priorScore = 0.86 + std::min(0.10, positive.weight * 0.04);
		exact.evidence.append(QStringLiteral("feedback_positive_exact_seed"));
		exact.evidence.append(QStringLiteral("feedback_positive_decision:%1").arg(positive.decision.left(32)));
		exact.evidence.append(QStringLiteral("feedback_positive_reason:%1").arg(positive.reason.left(96)));
		appendSeed(seeds, index, memory, options, exact);

		const double duration = durationSec(positive.range);
		const QVector<QPair<double, double>> variants = {
			{ -3.0, 3.0 }, { -6.0, 4.0 }, { -4.0, 8.0 }, { 0.0, 6.0 }
		};
		for (const auto &variant : variants) {
			if (duration < 12.0 && variant.first < -4.0)
				continue;
			FeedbackGuidedCandidateSeed seed;
			seed.range = ClipDuration{positive.range.startSec + variant.first, positive.range.endSec + variant.second};
			seed.source = QStringLiteral("feedback_positive_boundary_variant");
			seed.priorScore = 0.72;
			seed.evidence.append(QStringLiteral("feedback_positive_boundary_variant"));
			seed.evidence.append(QStringLiteral("feedback_boundary_variant_delta:%1/%2")
				.arg(QString::number(variant.first, 'f', 1), QString::number(variant.second, 'f', 1)));
			appendSeed(seeds, index, memory, options, seed);
		}
	}
}

void FeedbackGuidedCandidateGenerator::appendPrototypeSimilaritySeeds(QVector<FeedbackGuidedCandidateSeed> &seeds,
	const TranscriptIndex &index, const Curation::Feedback::FeedbackRangeMemory &memory,
	const FeedbackGuidedCandidateGenerationOptions &options) const
{
	if (memory.positiveRanges.isEmpty() || index.isEmpty())
		return;

	QVector<QPair<QString, Curation::Feedback::FeedbackRangeSignal>> prototypes;
	for (const Curation::Feedback::FeedbackRangeSignal &positive : memory.positiveRanges) {
		const QString text = index.textForRange(positive.range).trimmed();
		if (text.size() >= 40)
			prototypes.append(qMakePair(text.left(1600), positive));
		if (prototypes.size() >= 24)
			break;
	}
	if (prototypes.isEmpty())
		return;

	struct ScoredSeed {
		FeedbackGuidedCandidateSeed seed;
		double score = 0.0;
	};
	QVector<ScoredSeed> scored;
	const QVector<int> windowSizes = { 4, 7, 10, 14 };
	for (int first = 0; first < index.size(); first += 2) {
		for (const int windowSize : windowSizes) {
			const int last = std::min(index.size() - 1, first + windowSize - 1);
			if (last <= first)
				continue;
			ClipDuration range = rangeForSegmentWindow(index, first, last);
			range = clampToGeneration(index, options.generation, range);
			if (!validRange(range) || !rangeFitsGeneration(range, options.generation))
				continue;
			const QString text = index.textForRange(range).left(1800);
			if (text.size() < 40)
				continue;

			double bestPositive = 0.0;
			QString bestReason;
			for (const auto &prototype : prototypes) {
				const double score = FeedbackSimilarityScorer::lexicalSimilarity(text, prototype.first) *
					std::max(0.05, prototype.second.weight);
				if (score > bestPositive) {
					bestPositive = score;
					bestReason = prototype.second.reason;
				}
			}
			if (bestPositive < 0.30)
				continue;

			FeedbackSimilarityScorer scorer;
			const FeedbackSimilarityFeatures features = scorer.scoreRange(range, text, index, memory);
			const double score = std::max(bestPositive, features.positiveScore) - (features.negativeScore * 0.68);
			if (score < 0.24 || features.negativeRangeContamination)
				continue;

			FeedbackGuidedCandidateSeed seed;
			seed.range = range;
			seed.source = QStringLiteral("feedback_positive_semantic_prototype");
			seed.priorScore = 0.58 + std::min(0.24, std::max(0.0, score) * 0.42);
			seed.evidence.append(QStringLiteral("feedback_positive_semantic_prototype"));
			seed.evidence.append(QStringLiteral("feedback_prototype_similarity:%1").arg(QString::number(bestPositive, 'f', 2)));
			seed.evidence.append(QStringLiteral("feedback_prototype_reason:%1").arg(bestReason.left(96)));
			scored.append(ScoredSeed{seed, score});
		}
	}

	std::sort(scored.begin(), scored.end(), [](const ScoredSeed &left, const ScoredSeed &right) {
		return left.score > right.score;
	});
	int added = 0;
	for (const ScoredSeed &candidate : scored) {
		if (static_cast<int>(seeds.size()) >= options.maxSeeds || added >= options.maxSemanticPrototypeSeeds)
			break;
		if (appendSeed(seeds, index, memory, options, candidate.seed))
			++added;
	}
}


void FeedbackGuidedCandidateGenerator::appendPatternSearchSeeds(QVector<FeedbackGuidedCandidateSeed> &seeds,
	const TranscriptIndex &index, const Curation::Feedback::FeedbackRangeMemory &memory,
	const FeedbackGuidedCandidateGenerationOptions &options) const
{
	if (memory.positiveRanges.isEmpty() || index.isEmpty())
		return;

	QVector<double> durations;
	durations.reserve(memory.positiveRanges.size());
	for (const Curation::Feedback::FeedbackRangeSignal &positive : memory.positiveRanges) {
		const double duration = durationSec(positive.range);
		if (duration >= 8.0 && duration <= options.generation.maxDurationSec + 1.0)
			durations.append(duration);
	}
	if (durations.isEmpty())
		return;
	std::sort(durations.begin(), durations.end());
	const double medianDuration = durations.at(durations.size() / 2);
	QVector<double> templateDurations = {medianDuration, std::max(options.generation.minDurationSec, medianDuration * 0.75),
		std::min(options.generation.maxDurationSec, medianDuration * 1.25)};
	templateDurations.append(durations.front());
	templateDurations.append(durations.back());

	struct ScoredPatternSeed {
		FeedbackGuidedCandidateSeed seed;
		double score = 0.0;
	};
	QVector<ScoredPatternSeed> scored;
	const int stride = index.size() > 1000 ? 2 : 1;
	for (int segmentIndex = 0; segmentIndex < index.size(); segmentIndex += stride) {
		const TranscriptSegment *segment = index.segmentAt(segmentIndex);
		if (!segment)
			continue;
		for (const double templateDuration : templateDurations) {
			ClipDuration range{segment->startSec - 2.0, segment->startSec + templateDuration};
			range = clampToGeneration(index, options.generation, range);
			if (!validRange(range) || !rangeFitsGeneration(range, options.generation))
				continue;
			const QString text = index.textForRange(range).trimmed();
			if (text.size() < 48)
				continue;

			FeedbackSimilarityScorer scorer;
			const FeedbackSimilarityFeatures features = scorer.scoreRange(range, text, index, memory);
			if (features.negativeRangeContamination || (features.negativeScore >= 0.55 && features.margin <= 0.02))
				continue;

			const double exchange = Curation::textHasViewerExchangeSignals(text) ? 0.28 : 0.0;
			const double emotional = Curation::emotionalScoreForText(text) * 0.22;
			const double advice = Curation::adviceScoreForText(text) * 0.18;
			const double explanation = Curation::explanationScoreForText(text) * 0.12;
			const double opinion = Curation::opinionScoreForText(text) * 0.08;
			const double positive = features.positiveScore * 0.42;
			const double margin = std::max(0.0, features.margin) * 0.22;
			const double score = exchange + emotional + advice + explanation + opinion + positive + margin;
			if (score < 0.34)
				continue;

			FeedbackGuidedCandidateSeed seed;
			seed.range = range;
			seed.source = QStringLiteral("feedback_positive_pattern_search");
			seed.priorScore = 0.54 + std::min(0.28, score * 0.34);
			seed.evidence.append(QStringLiteral("feedback_positive_pattern_search"));
			seed.evidence.append(QStringLiteral("feedback_pattern_score:%1").arg(QString::number(score, 'f', 2)));
			seed.evidence.append(QStringLiteral("feedback_pattern_template_duration:%1").arg(QString::number(templateDuration, 'f', 1)));
			if (exchange > 0.0)
				seed.evidence.append(QStringLiteral("feedback_pattern_viewer_exchange_signal"));
			if (features.positiveScore > 0.0)
				seed.evidence.append(QStringLiteral("feedback_pattern_positive_similarity:%1").arg(QString::number(features.positiveScore, 'f', 2)));
			scored.append(ScoredPatternSeed{seed, score});
		}
	}

	std::sort(scored.begin(), scored.end(), [](const ScoredPatternSeed &left, const ScoredPatternSeed &right) {
		return left.score > right.score;
	});
	int added = 0;
	for (const ScoredPatternSeed &candidate : scored) {
		if (static_cast<int>(seeds.size()) >= options.maxSeeds || added >= options.maxPatternSeeds)
			break;
		if (appendSeed(seeds, index, memory, options, candidate.seed))
			++added;
	}
}

QVector<FeedbackGuidedCandidateSeed> FeedbackGuidedCandidateGenerator::generate(const TranscriptIndex &index,
	const Curation::Feedback::FeedbackRangeMemory &memory,
	const FeedbackGuidedCandidateGenerationOptions &options) const
{
	QVector<FeedbackGuidedCandidateSeed> seeds;
	if (!memory.loaded || memory.positiveRanges.isEmpty() || options.maxSeeds <= 0)
		return seeds;
	seeds.reserve(std::min(options.maxSeeds, 128));
	appendPositiveRangeSeeds(seeds, index, memory, options);
	appendPrototypeSimilaritySeeds(seeds, index, memory, options);
	appendPatternSearchSeeds(seeds, index, memory, options);
	return seeds;
}

} // namespace Curation::Scoring
