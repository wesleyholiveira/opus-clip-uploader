#include "curation/scoring/feedback-guided-candidate-stage.hpp"

#include "curation/scoring/candidate-range-utils.hpp"

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

#include <algorithm>
#include <cmath>
#include <QElapsedTimer>

using namespace Curation::Scoring::CandidateRangeUtils;

namespace Curation::Scoring {

namespace {

static bool validFeedbackRange(const ClipDuration &range)
{
	return std::isfinite(range.startSec) && std::isfinite(range.endSec) && range.endSec > range.startSec;
}

static double feedbackRangeDistance(const ClipDuration &left, const ClipDuration &right)
{
	return std::fabs(left.startSec - right.startSec) + std::fabs(left.endSec - right.endSec);
}

static int semanticPrototypeClusterCount(const Curation::Feedback::FeedbackRangeMemory &memory)
{
	QVector<ClipDuration> clusters;
	for (const Curation::Feedback::FeedbackRangeSignal &positive : memory.positiveRanges) {
		if (!positive.semanticPrototypeEligible || !validFeedbackRange(positive.range))
			continue;
		bool matched = false;
		for (const ClipDuration &cluster : clusters) {
			if (FeedbackSimilarityScorer::rangeSimilarity(cluster, positive.range) >= 0.82 ||
			    feedbackRangeDistance(cluster, positive.range) <= 8.0) {
				matched = true;
				break;
			}
		}
		if (!matched)
			clusters.append(positive.range);
	}
	return static_cast<int>(clusters.size());
}

} // namespace

QVector<ClipCandidate>
FeedbackGuidedCandidateStage::appendCandidates(const TranscriptIndex &index, QVector<ClipCandidate> candidates,
					       const Curation::Feedback::FeedbackRangeMemory &memory,
					       const FeedbackGuidedCandidateStageOptions &options) const
{
	if (!memory.loaded || memory.positiveRanges.isEmpty())
		return candidates;

	const int semanticPositiveClusters = semanticPrototypeClusterCount(memory);
	FeedbackGuidedCandidateGenerationOptions feedbackGenerationOptions;
	feedbackGenerationOptions.generation = options.generation;
	const int clusterLimitedSeedBudget = semanticPositiveClusters <= 0    ? 0
					     : semanticPositiveClusters == 1  ? 8
					     : semanticPositiveClusters == 2  ? 14
					     : semanticPositiveClusters <= 5  ? 24
					     : semanticPositiveClusters <= 10 ? 40
									      : 64;
	feedbackGenerationOptions.maxSeeds =
		std::min(std::clamp(options.ranking.maxCandidates * 2, 12, 64), clusterLimitedSeedBudget);
	feedbackGenerationOptions.maxSemanticPrototypeSeeds = semanticPositiveClusters <= 1   ? 2
							      : semanticPositiveClusters == 2 ? 4
							      : semanticPositiveClusters <= 5
								      ? std::min(10, semanticPositiveClusters * 2)
								      : std::min(18, semanticPositiveClusters * 2);
	feedbackGenerationOptions.maxPatternSeeds = semanticPositiveClusters <= 1    ? 4
						    : semanticPositiveClusters == 2  ? 8
						    : semanticPositiveClusters <= 5  ? 12
						    : semanticPositiveClusters <= 10 ? 24
										     : 40;

	const bool saturatedSameVideoMemory = memory.recordsRead >= 80 || memory.ignoredDiagnosticSignals >= 40 ||
					      memory.approvedAdjustedPositiveSignals >= 20 ||
					      semanticPositiveClusters >= 16;
	if (saturatedSameVideoMemory) {
		feedbackGenerationOptions.maxExactPositiveSeeds = 0;
		feedbackGenerationOptions.maxBoundaryVariantSeeds = 4;
		feedbackGenerationOptions.maxSemanticPrototypeSeeds =
			std::max(feedbackGenerationOptions.maxSemanticPrototypeSeeds, 24);
		feedbackGenerationOptions.maxPatternSeeds = std::max(feedbackGenerationOptions.maxPatternSeeds, 40);
		blog(LOG_INFO,
		     "[clip-cropper] Feedback-guided repetition guard active. video=%s records=%d ignored=%d approvedAdjusted=%d semanticClusters=%d maxExact=%d maxBoundary=%d",
		     options.videoPath.toUtf8().constData(), memory.recordsRead, memory.ignoredDiagnosticSignals,
		     memory.approvedAdjustedPositiveSignals, semanticPositiveClusters,
		     feedbackGenerationOptions.maxExactPositiveSeeds,
		     feedbackGenerationOptions.maxBoundaryVariantSeeds);
	} else {
		feedbackGenerationOptions.maxExactPositiveSeeds = semanticPositiveClusters <= 3 ? 4 : 2;
		feedbackGenerationOptions.maxBoundaryVariantSeeds = semanticPositiveClusters <= 3 ? 8 : 6;
	}

	QElapsedTimer generatorTimer;
	generatorTimer.start();
	FeedbackGuidedCandidateGenerator feedbackGenerator;
	const QVector<FeedbackGuidedCandidateSeed> feedbackSeeds =
		feedbackGenerator.generate(index, memory, feedbackGenerationOptions);
	const qint64 seedGenerationMs = generatorTimer.elapsed();
	if (feedbackSeeds.isEmpty())
		return candidates;
	QElapsedTimer buildTimer;
	buildTimer.start();

	SemanticCoarseRegion feedbackRegion;
	feedbackRegion.score = 0.78;
	feedbackRegion.evidence.append(QStringLiteral("feedback_guided_candidate_region"));

	int addedFeedbackCandidates = 0;
	int exactSeedCount = 0;
	int boundaryVariantCount = 0;
	int prototypeSeedCount = 0;
	int patternSeedCount = 0;
	int semanticPositiveCount = 0;
	for (const Curation::Feedback::FeedbackRangeSignal &positive : memory.positiveRanges) {
		if (positive.semanticPrototypeEligible)
			++semanticPositiveCount;
	}
	for (const FeedbackGuidedCandidateSeed &seed : feedbackSeeds) {
		if (seed.source == QStringLiteral("feedback_positive_exact_seed"))
			++exactSeedCount;
		else if (seed.source == QStringLiteral("feedback_positive_boundary_variant"))
			++boundaryVariantCount;
		else if (seed.source == QStringLiteral("feedback_positive_semantic_prototype"))
			++prototypeSeedCount;
		else if (seed.source == QStringLiteral("feedback_positive_pattern_search"))
			++patternSeedCount;
		if (hasSimilarRange(candidates, seed.range, 1.0))
			continue;
		ClipCandidate candidate = CandidateBuilder::buildForRange(
			index, options.generation, options.scoring, options.qualityGate, seed.range, feedbackRegion);
		if (candidate.text.trimmed().isEmpty())
			continue;
		candidate.source = seed.source.trimmed().isEmpty() ? QStringLiteral("feedback_guided_candidate")
								   : seed.source;
		candidate.scores.final = std::max(candidate.scores.final, seed.priorScore);
		candidate.scores.semanticTarget = std::max(candidate.scores.semanticTarget, 0.62);
		candidate.scores.semanticClipValue = std::max(candidate.scores.semanticClipValue, 0.62);
		if (candidate.source == QStringLiteral("feedback_positive_pattern_search")) {
			candidate.scores.final = std::max(candidate.scores.final, 0.58);
			candidate.scores.viewerResponse = std::max(candidate.scores.viewerResponse, 0.60);
			candidate.scores.semanticDirectAnswer = std::max(candidate.scores.semanticDirectAnswer, 0.58);
		}
		candidate.evidence.append(seed.evidence);
		candidate.evidence.append(QStringLiteral("feedback_guided_candidate_generated_before_ranking"));
		candidate.evidence.removeDuplicates();
		candidates.append(candidate);
		++addedFeedbackCandidates;
	}

	if (addedFeedbackCandidates > 0) {
		blog(LOG_INFO,
		     "[clip-cropper] Added feedback-guided candidates before ranking. video=%s seeds=%d added=%d exact=%d boundary=%d prototype=%d pattern=%d positive=%d semanticPositive=%d semanticClusters=%d negative=%d seedGenerationMs=%lld buildMs=%lld",
		     options.videoPath.toUtf8().constData(), static_cast<int>(feedbackSeeds.size()),
		     addedFeedbackCandidates, exactSeedCount, boundaryVariantCount, prototypeSeedCount,
		     patternSeedCount, static_cast<int>(memory.positiveRanges.size()), semanticPositiveCount,
		     semanticPositiveClusters, static_cast<int>(memory.negativeRanges.size()),
		     static_cast<long long>(seedGenerationMs), static_cast<long long>(buildTimer.elapsed()));
	}

	return candidates;
}

} // namespace Curation::Scoring
