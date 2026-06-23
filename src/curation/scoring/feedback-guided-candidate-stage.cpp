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

using namespace Curation::Scoring::CandidateRangeUtils;

namespace Curation::Scoring {

QVector<ClipCandidate> FeedbackGuidedCandidateStage::appendCandidates(const TranscriptIndex &index,
	QVector<ClipCandidate> candidates,
	const Curation::Feedback::FeedbackRangeMemory &memory,
	const FeedbackGuidedCandidateStageOptions &options) const
{
	if (!memory.loaded || memory.positiveRanges.isEmpty())
		return candidates;

	FeedbackGuidedCandidateGenerationOptions feedbackGenerationOptions;
	feedbackGenerationOptions.generation = options.generation;
	feedbackGenerationOptions.maxSeeds = std::clamp(options.ranking.maxCandidates * 4, 24, 96);
	feedbackGenerationOptions.maxSemanticPrototypeSeeds = std::clamp(options.ranking.maxCandidates * 2, 12, 48);
	feedbackGenerationOptions.maxPatternSeeds = std::clamp(options.ranking.maxCandidates * 3, 18, 72);

	FeedbackGuidedCandidateGenerator feedbackGenerator;
	const QVector<FeedbackGuidedCandidateSeed> feedbackSeeds = feedbackGenerator.generate(index, memory, feedbackGenerationOptions);
	if (feedbackSeeds.isEmpty())
		return candidates;

	SemanticCoarseRegion feedbackRegion;
	feedbackRegion.score = 0.78;
	feedbackRegion.evidence.append(QStringLiteral("feedback_guided_candidate_region"));

	int addedFeedbackCandidates = 0;
	int exactSeedCount = 0;
	int boundaryVariantCount = 0;
	int prototypeSeedCount = 0;
	int patternSeedCount = 0;
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
		ClipCandidate candidate = CandidateBuilder::buildForRange(index, options.generation, options.scoring,
			options.qualityGate, seed.range, feedbackRegion);
		if (candidate.text.trimmed().isEmpty())
			continue;
		candidate.source = seed.source.trimmed().isEmpty() ? QStringLiteral("feedback_guided_candidate") : seed.source;
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
		     "[clip-cropper] Added feedback-guided candidates before ranking. video=%s seeds=%d added=%d exact=%d boundary=%d prototype=%d pattern=%d positive=%d negative=%d",
		     options.videoPath.toUtf8().constData(), static_cast<int>(feedbackSeeds.size()), addedFeedbackCandidates,
		     exactSeedCount, boundaryVariantCount, prototypeSeedCount, patternSeedCount,
		     static_cast<int>(memory.positiveRanges.size()), static_cast<int>(memory.negativeRanges.size()));
	}

	return candidates;
}

} // namespace Curation::Scoring
