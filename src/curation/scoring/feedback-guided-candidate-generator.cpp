#include "curation/scoring/feedback-guided-candidate-generator.hpp"

#include "curation/curation-signals.hpp"

#include <algorithm>
#include <QElapsedTimer>
#include <QPair>
#include <cmath>

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

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

static bool similarSemanticPrototypePositiveRange(const Curation::Feedback::FeedbackRangeSignal &left,
	const Curation::Feedback::FeedbackRangeSignal &right)
{
	if (!validRange(left.range) || !validRange(right.range))
		return false;
	return FeedbackSimilarityScorer::rangeSimilarity(left.range, right.range) >= 0.82 ||
	       rangeDistance(left.range, right.range) <= 8.0;
}

static bool betterSemanticPrototypeRepresentative(const Curation::Feedback::FeedbackRangeSignal &candidate,
	const Curation::Feedback::FeedbackRangeSignal &current)
{
	if (candidate.sequence != current.sequence)
		return candidate.sequence > current.sequence;
	if (std::fabs(candidate.weight - current.weight) > 0.0001)
		return candidate.weight > current.weight;
	return durationSec(candidate.range) > durationSec(current.range);
}

static QVector<Curation::Feedback::FeedbackRangeSignal> semanticPrototypeEligiblePositiveRanges(
	const Curation::Feedback::FeedbackRangeMemory &memory)
{
	QVector<Curation::Feedback::FeedbackRangeSignal> representatives;
	representatives.reserve(memory.positiveRanges.size());
	for (const Curation::Feedback::FeedbackRangeSignal &positive : memory.positiveRanges) {
		if (!positive.semanticPrototypeEligible || !validRange(positive.range))
			continue;

		bool merged = false;
		for (Curation::Feedback::FeedbackRangeSignal &representative : representatives) {
			if (!similarSemanticPrototypePositiveRange(positive, representative))
				continue;
			if (betterSemanticPrototypeRepresentative(positive, representative))
				representative = positive;
			merged = true;
			break;
		}
		if (!merged)
			representatives.append(positive);
	}

	std::sort(representatives.begin(), representatives.end(), [](const auto &left, const auto &right) {
		if (left.sequence != right.sequence)
			return left.sequence > right.sequence;
		return left.weight > right.weight;
	});
	return representatives;
}

static int effectiveSemanticPrototypeSeedLimit(const FeedbackGuidedCandidateGenerationOptions &options, int clusterCount)
{
	if (clusterCount <= 0)
		return 0;
	if (clusterCount == 1)
		return std::min(options.maxSemanticPrototypeSeeds, 2);
	if (clusterCount == 2)
		return std::min(options.maxSemanticPrototypeSeeds, 4);
	if (clusterCount <= 5)
		return std::min(options.maxSemanticPrototypeSeeds, clusterCount * 2);
	if (clusterCount <= 10)
		return std::min(options.maxSemanticPrototypeSeeds, 18);
	return std::min(options.maxSemanticPrototypeSeeds, 24);
}

static int effectivePatternSeedLimit(const FeedbackGuidedCandidateGenerationOptions &options, int clusterCount)
{
	if (clusterCount <= 0)
		return 0;
	if (clusterCount == 1)
		return std::min(options.maxPatternSeeds, 4);
	if (clusterCount == 2)
		return std::min(options.maxPatternSeeds, 8);
	if (clusterCount <= 5)
		return std::min(options.maxPatternSeeds, 12);
	if (clusterCount <= 10)
		return std::min(options.maxPatternSeeds, 24);
	return std::min(options.maxPatternSeeds, 40);
}


static int feedbackScanStride(const TranscriptIndex &index)
{
	if (index.size() >= 2200)
		return 10;
	if (index.size() >= 1200)
		return 8;
	if (index.size() >= 700)
		return 5;
	return 3;
}

static bool cheaplyOverlapsHardNegative(const ClipDuration &range,
	const Curation::Feedback::FeedbackRangeMemory &memory)
{
	for (const Curation::Feedback::FeedbackRangeSignal &negative : memory.negativeRanges) {
		if (!validRange(negative.range))
			continue;
		if (FeedbackSimilarityScorer::negativeRangeContaminatesCandidate(range, negative))
			return true;
	}
	return false;
}

static double cheapViewerTargetSignal(const QString &text)
{
	const double exchange = Curation::textHasViewerExchangeSignals(text) ? 0.32 : 0.0;
	const double emotional = Curation::emotionalScoreForText(text) * 0.26;
	const double advice = Curation::adviceScoreForText(text) * 0.22;
	const double explanation = Curation::explanationScoreForText(text) * 0.12;
	const double opinion = Curation::opinionScoreForText(text) * 0.08;
	return exchange + emotional + advice + explanation + opinion;
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
	const QVector<Curation::Feedback::FeedbackRangeSignal> semanticPositives =
		semanticPrototypeEligiblePositiveRanges(memory);
	const int clusterCount = static_cast<int>(semanticPositives.size());
	const int maxBoundaryVariantsPerPositive = clusterCount <= 1 ? 0 : clusterCount <= 3 ? 1 : 4;
	for (const Curation::Feedback::FeedbackRangeSignal &positive : semanticPositives) {
		if (static_cast<int>(seeds.size()) >= options.maxSeeds)
			return;

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
		int variantsAdded = 0;
		for (const auto &variant : variants) {
			if (variantsAdded >= maxBoundaryVariantsPerPositive)
				break;
			if (duration < 12.0 && variant.first < -4.0)
				continue;
			FeedbackGuidedCandidateSeed seed;
			seed.range = ClipDuration{positive.range.startSec + variant.first, positive.range.endSec + variant.second};
			seed.source = QStringLiteral("feedback_positive_boundary_variant");
			seed.priorScore = 0.72;
			seed.evidence.append(QStringLiteral("feedback_positive_boundary_variant"));
			seed.evidence.append(QStringLiteral("feedback_boundary_variant_delta:%1/%2")
				.arg(QString::number(variant.first, 'f', 1), QString::number(variant.second, 'f', 1)));
			if (appendSeed(seeds, index, memory, options, seed))
				++variantsAdded;
		}
	}
}

void FeedbackGuidedCandidateGenerator::appendPrototypeSimilaritySeeds(QVector<FeedbackGuidedCandidateSeed> &seeds,
	const TranscriptIndex &index, const Curation::Feedback::FeedbackRangeMemory &memory,
	const FeedbackGuidedCandidateGenerationOptions &options) const
{
	if (memory.positiveRanges.isEmpty() || index.isEmpty())
		return;

	QElapsedTimer timer;
	timer.start();
	const QVector<Curation::Feedback::FeedbackRangeSignal> semanticPositives =
		semanticPrototypeEligiblePositiveRanges(memory);
	if (semanticPositives.isEmpty())
		return;

	QVector<QPair<QString, Curation::Feedback::FeedbackRangeSignal>> prototypes;
	for (const Curation::Feedback::FeedbackRangeSignal &positive : semanticPositives) {
		const QString text = index.textForRange(positive.range).trimmed();
		if (text.size() >= 40)
			prototypes.append(qMakePair(text.left(1600), positive));
		if (prototypes.size() >= 16)
			break;
	}
	if (prototypes.isEmpty())
		return;

	struct ScoredSeed {
		FeedbackGuidedCandidateSeed seed;
		double score = 0.0;
	};
	QVector<ScoredSeed> lexicalCandidates;
	const QVector<int> windowSizes = index.size() >= 1200 ? QVector<int>{7, 12} : QVector<int>{4, 7, 10, 14};
	const int stride = feedbackScanStride(index);
	int scanned = 0;
	for (int first = 0; first < index.size(); first += stride) {
		for (const int windowSize : windowSizes) {
			const int last = std::min(index.size() - 1, first + windowSize - 1);
			if (last <= first)
				continue;
			ClipDuration range = rangeForSegmentWindow(index, first, last);
			range = clampToGeneration(index, options.generation, range);
			if (!validRange(range) || !rangeFitsGeneration(range, options.generation))
				continue;
			if (cheaplyOverlapsHardNegative(range, memory))
				continue;
			const QString text = index.textForRange(range).left(1800);
			if (text.size() < 40)
				continue;
			++scanned;

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

			FeedbackGuidedCandidateSeed seed;
			seed.range = range;
			seed.source = QStringLiteral("feedback_positive_semantic_prototype");
			seed.priorScore = 0.58 + std::min(0.24, std::max(0.0, bestPositive) * 0.42);
			seed.evidence.append(QStringLiteral("feedback_positive_semantic_prototype"));
			seed.evidence.append(QStringLiteral("feedback_prototype_similarity:%1").arg(QString::number(bestPositive, 'f', 2)));
			seed.evidence.append(QStringLiteral("feedback_prototype_reason:%1").arg(bestReason.left(96)));
			lexicalCandidates.append(ScoredSeed{seed, bestPositive});
		}
	}

	std::sort(lexicalCandidates.begin(), lexicalCandidates.end(), [](const ScoredSeed &left, const ScoredSeed &right) {
		return left.score > right.score;
	});

	const int semanticPrototypeLimit = effectiveSemanticPrototypeSeedLimit(options, static_cast<int>(semanticPositives.size()));
	const int maxVerified = std::min(static_cast<int>(lexicalCandidates.size()), std::max(semanticPrototypeLimit * 4, 24));
	QVector<ScoredSeed> verified;
	verified.reserve(maxVerified);
	FeedbackSimilarityScorer scorer;
	for (int i = 0; i < maxVerified; ++i) {
		ScoredSeed candidate = lexicalCandidates.at(i);
		const QString text = index.textForRange(candidate.seed.range).left(1800);
		const FeedbackSimilarityFeatures features = scorer.scoreRange(candidate.seed.range, text, index, memory);
		const double score = std::max(candidate.score, features.positiveScore) - (features.negativeScore * 0.68);
		if (score < 0.24 || features.negativeRangeContamination)
			continue;
		candidate.score = score;
		candidate.seed.priorScore = std::max(candidate.seed.priorScore, 0.58 + std::min(0.24, std::max(0.0, score) * 0.42));
		candidate.seed.evidence.append(features.evidence);
		candidate.seed.evidence.removeDuplicates();
		verified.append(candidate);
	}

	std::sort(verified.begin(), verified.end(), [](const ScoredSeed &left, const ScoredSeed &right) {
		return left.score > right.score;
	});
	int added = 0;
	for (const ScoredSeed &candidate : verified) {
		if (static_cast<int>(seeds.size()) >= options.maxSeeds || added >= semanticPrototypeLimit)
			break;
		if (appendSeed(seeds, index, memory, options, candidate.seed))
			++added;
	}
	blog(LOG_INFO,
	     "[clip-cropper] Feedback-guided prototype scan finished. scanned=%d lexical=%d verified=%d added=%d stride=%d elapsedMs=%lld",
	     scanned, static_cast<int>(lexicalCandidates.size()), static_cast<int>(verified.size()), added, stride,
	     static_cast<long long>(timer.elapsed()));
}


void FeedbackGuidedCandidateGenerator::appendPatternSearchSeeds(QVector<FeedbackGuidedCandidateSeed> &seeds,
	const TranscriptIndex &index, const Curation::Feedback::FeedbackRangeMemory &memory,
	const FeedbackGuidedCandidateGenerationOptions &options) const
{
	if (memory.positiveRanges.isEmpty() || index.isEmpty())
		return;

	QElapsedTimer timer;
	timer.start();
	const QVector<Curation::Feedback::FeedbackRangeSignal> semanticPositives =
		semanticPrototypeEligiblePositiveRanges(memory);
	if (semanticPositives.isEmpty())
		return;

	QVector<double> durations;
	durations.reserve(semanticPositives.size());
	for (const Curation::Feedback::FeedbackRangeSignal &positive : semanticPositives) {
		const double duration = durationSec(positive.range);
		if (duration >= 8.0 && duration <= options.generation.maxDurationSec + 1.0)
			durations.append(duration);
	}
	if (durations.isEmpty())
		return;
	std::sort(durations.begin(), durations.end());
	const double medianDuration = durations.at(durations.size() / 2);
	QVector<double> templateDurations = {
		medianDuration,
		std::max(options.generation.minDurationSec, medianDuration * 0.75),
		std::min(options.generation.maxDurationSec, medianDuration * 1.25)
	};
	if (durations.size() <= 3) {
		templateDurations.append(durations.front());
		templateDurations.append(durations.back());
	}
	std::sort(templateDurations.begin(), templateDurations.end());
	templateDurations.erase(std::unique(templateDurations.begin(), templateDurations.end(), [](double a, double b) {
		return std::fabs(a - b) < 2.0;
	}), templateDurations.end());

	struct ScoredPatternSeed {
		FeedbackGuidedCandidateSeed seed;
		double score = 0.0;
		QString text;
	};
	QVector<ScoredPatternSeed> cheapCandidates;
	const int stride = feedbackScanStride(index);
	int scanned = 0;
	for (int segmentIndex = 0; segmentIndex < index.size(); segmentIndex += stride) {
		const TranscriptSegment *segment = index.segmentAt(segmentIndex);
		if (!segment)
			continue;
		for (const double templateDuration : templateDurations) {
			ClipDuration range{segment->startSec - 2.0, segment->startSec + templateDuration};
			range = clampToGeneration(index, options.generation, range);
			if (!validRange(range) || !rangeFitsGeneration(range, options.generation))
				continue;
			if (cheaplyOverlapsHardNegative(range, memory))
				continue;
			const QString text = index.textForRange(range).trimmed();
			if (text.size() < 48)
				continue;
			++scanned;

			const double cheap = cheapViewerTargetSignal(text);
			if (cheap < 0.22)
				continue;

			FeedbackGuidedCandidateSeed seed;
			seed.range = range;
			seed.source = QStringLiteral("feedback_positive_pattern_search");
			seed.priorScore = 0.54 + std::min(0.20, cheap * 0.25);
			seed.evidence.append(QStringLiteral("feedback_positive_pattern_search"));
			seed.evidence.append(QStringLiteral("feedback_pattern_cheap_score:%1").arg(QString::number(cheap, 'f', 2)));
			seed.evidence.append(QStringLiteral("feedback_pattern_template_duration:%1").arg(QString::number(templateDuration, 'f', 1)));
			cheapCandidates.append(ScoredPatternSeed{seed, cheap, text.left(1800)});
		}
	}

	std::sort(cheapCandidates.begin(), cheapCandidates.end(), [](const ScoredPatternSeed &left, const ScoredPatternSeed &right) {
		return left.score > right.score;
	});

	const int patternLimit = effectivePatternSeedLimit(options, static_cast<int>(semanticPositives.size()));
	const int maxVerified = std::min(static_cast<int>(cheapCandidates.size()), std::max(patternLimit * 5, 32));
	QVector<ScoredPatternSeed> verified;
	verified.reserve(maxVerified);
	FeedbackSimilarityScorer scorer;
	for (int i = 0; i < maxVerified; ++i) {
		ScoredPatternSeed candidate = cheapCandidates.at(i);
		const FeedbackSimilarityFeatures features = scorer.scoreRange(candidate.seed.range, candidate.text, index, memory);
		if (features.negativeRangeContamination || (features.negativeScore >= 0.55 && features.margin <= 0.02))
			continue;

		const double exchange = Curation::textHasViewerExchangeSignals(candidate.text) ? 0.28 : 0.0;
		const double emotional = Curation::emotionalScoreForText(candidate.text) * 0.22;
		const double advice = Curation::adviceScoreForText(candidate.text) * 0.18;
		const double explanation = Curation::explanationScoreForText(candidate.text) * 0.12;
		const double opinion = Curation::opinionScoreForText(candidate.text) * 0.08;
		const double positive = features.positiveScore * 0.42;
		const double margin = std::max(0.0, features.margin) * 0.22;
		const double score = exchange + emotional + advice + explanation + opinion + positive + margin;
		if (score < 0.34)
			continue;

		candidate.score = score;
		candidate.seed.priorScore = 0.54 + std::min(0.28, score * 0.34);
		candidate.seed.evidence.append(QStringLiteral("feedback_pattern_score:%1").arg(QString::number(score, 'f', 2)));
		if (exchange > 0.0)
			candidate.seed.evidence.append(QStringLiteral("feedback_pattern_viewer_exchange_signal"));
		if (features.positiveScore > 0.0)
			candidate.seed.evidence.append(QStringLiteral("feedback_pattern_positive_similarity:%1").arg(QString::number(features.positiveScore, 'f', 2)));
		candidate.seed.evidence.append(features.evidence);
		candidate.seed.evidence.removeDuplicates();
		verified.append(candidate);
	}

	std::sort(verified.begin(), verified.end(), [](const ScoredPatternSeed &left, const ScoredPatternSeed &right) {
		return left.score > right.score;
	});
	int added = 0;
	for (const ScoredPatternSeed &candidate : verified) {
		if (static_cast<int>(seeds.size()) >= options.maxSeeds || added >= patternLimit)
			break;
		if (appendSeed(seeds, index, memory, options, candidate.seed))
			++added;
	}
	blog(LOG_INFO,
	     "[clip-cropper] Feedback-guided pattern scan finished. scanned=%d cheap=%d verified=%d added=%d stride=%d durations=%d elapsedMs=%lld",
	     scanned, static_cast<int>(cheapCandidates.size()), static_cast<int>(verified.size()), added, stride,
	     static_cast<int>(templateDurations.size()), static_cast<long long>(timer.elapsed()));
}


QVector<FeedbackGuidedCandidateSeed> FeedbackGuidedCandidateGenerator::generate(const TranscriptIndex &index,
	const Curation::Feedback::FeedbackRangeMemory &memory,
	const FeedbackGuidedCandidateGenerationOptions &options) const
{
	QElapsedTimer timer;
	timer.start();
	QVector<FeedbackGuidedCandidateSeed> seeds;
	if (!memory.loaded || memory.positiveRanges.isEmpty() || options.maxSeeds <= 0)
		return seeds;
	seeds.reserve(std::min(options.maxSeeds, 128));
	appendPositiveRangeSeeds(seeds, index, memory, options);
	const int afterExact = static_cast<int>(seeds.size());
	appendPrototypeSimilaritySeeds(seeds, index, memory, options);
	const int afterPrototype = static_cast<int>(seeds.size());
	appendPatternSearchSeeds(seeds, index, memory, options);
	blog(LOG_INFO,
	     "[clip-cropper] Feedback-guided seed generation finished. seeds=%d exactOrBoundary=%d prototype=%d pattern=%d maxSeeds=%d elapsedMs=%lld",
	     static_cast<int>(seeds.size()), afterExact, std::max(0, afterPrototype - afterExact),
	     std::max(0, static_cast<int>(seeds.size()) - afterPrototype), options.maxSeeds,
	     static_cast<long long>(timer.elapsed()));
	return seeds;
}

} // namespace Curation::Scoring
