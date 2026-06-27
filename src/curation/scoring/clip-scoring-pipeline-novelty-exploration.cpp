#include "curation/scoring/clip-scoring-pipeline-detail.hpp"

#include "curation/feedback/curation-feedback-store.hpp"
#include "curation/scoring/candidate-builder.hpp"
#include "curation/scoring/candidate-source-builder.hpp"
#include "curation/scoring/cheap-clip-scorer.hpp"
#include "curation/scoring/feedback-similarity-scorer.hpp"
#include "curation/scoring/parallel-chunk-map.hpp"
#include "curation/scoring/semantic-coarse-retriever.hpp"

#include <QStringList>
#include <QSet>
#include <QElapsedTimer>

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace Curation::Scoring::ClipScoringPipelineDetail {

double localExplorationScore(const ClipCandidate &candidate, const FeedbackSimilarityFeatures &feedback)
{
	const double viewer = std::max({candidate.scores.viewerResponse, candidate.scores.semanticViewerMessage,
					candidate.scores.semanticDirectAnswer});
	const double structure = std::max({candidate.scores.arcCompleteness, candidate.scores.arcOpening,
					   candidate.scores.arcDevelopment, candidate.scores.arcConclusion});
	const double hook = std::max(candidate.scores.hook, candidate.scores.semanticHook);
	const double resolution =
		std::max(candidate.scores.semanticResolution, candidate.scores.semanticEndingResolution);
	const double positive = std::max(0.0, feedback.positiveScore) * 0.16;
	const double noveltyPenalty = std::max(0.0, feedback.negativeScore) * 0.22;
	return (viewer * 0.30) + (structure * 0.22) + (hook * 0.18) + (resolution * 0.18) + positive - noveltyPenalty;
}

bool hasSimilarExplorationRange(const QVector<ClipCandidate> &candidates, const ClipDuration &range)
{
	for (const ClipCandidate &candidate : candidates) {
		if (FeedbackSimilarityScorer::rangeSimilarity(candidate.range, range) >= 0.70)
			return true;
		const double boundaryDistance = std::fabs(candidate.range.startSec - range.startSec) +
						std::fabs(candidate.range.endSec - range.endSec);
		if (boundaryDistance <= 8.0)
			return true;
	}
	return false;
}

QString noveltyRangeKey(const ClipDuration &range)
{
	return QStringLiteral("%1:%2")
		.arg(static_cast<int>(std::round(range.startSec * 2.0)))
		.arg(static_cast<int>(std::round(range.endSec * 2.0)));
}

QVector<double> fastNoveltyDurations(const CandidateGenerationOptions &generation)
{
	QVector<double> durations{
		std::clamp(24.0, generation.minDurationSec, generation.maxDurationSec),
		std::clamp(38.0, generation.minDurationSec, generation.maxDurationSec),
		std::clamp(58.0, generation.minDurationSec, generation.maxDurationSec),
		std::clamp(82.0, generation.minDurationSec, generation.maxDurationSec),
	};
	durations.erase(std::unique(durations.begin(), durations.end(),
				    [](double left, double right) { return std::fabs(left - right) < 0.25; }),
			durations.end());
	return durations;
}

QVector<ClipCandidate> buildFastNoveltyRawCandidates(const TranscriptIndex &index,
						     const ClipScoringPipelineOptions &options, int maxRawCandidates)
{
	QVector<ClipCandidate> candidates;
	if (index.isEmpty() || maxRawCandidates <= 0)
		return candidates;

	CandidateSourceBuilderOptions sourceOptions = candidateSourceOptionsFromOptions(options);
	sourceOptions.generation.maxRawCandidates = maxRawCandidates;
	CheapClipScorer cueScorer;

	struct Anchor {
		int segmentIndex = -1;
		double score = 0.0;
		bool targetHit = false;
		bool viewerCue = false;
	};

	QVector<Anchor> anchors;
	anchors.reserve(96);
	const ClipDuration searchRange = sourceOptions.generation.searchRange;
	const int segmentStride = index.size() >= 2200 ? 2 : 1;
	for (int i = 0; i < index.size(); i += segmentStride) {
		const TranscriptSegment *segment = index.segmentAt(i);
		if (!segment || !TranscriptIndex::segmentOverlapsRange(*segment, searchRange) ||
		    segment->text.trimmed().isEmpty())
			continue;
		const bool targetHit =
			sourceOptions.generation.reliableMainTarget &&
			cueScorer.targetKeywordScore(segment->text, sourceOptions.generation.mainTarget) >= 0.30;
		const bool strongCue = cueScorer.hasStrongLocalCue(segment->text);
		if (!targetHit && !strongCue)
			continue;
		const bool viewerCue = cueScorer.looksLikeQuestionOrViewerMessage(segment->text);
		double score = 0.0;
		if (targetHit)
			score += 0.44;
		if (viewerCue)
			score += 0.24;
		if (strongCue)
			score += 0.18;
		score += std::min(0.18, static_cast<double>(segment->text.trimmed().size()) / 700.0);
		anchors.append(Anchor{i, score, targetHit, viewerCue});
	}

	std::sort(anchors.begin(), anchors.end(), [](const Anchor &left, const Anchor &right) {
		if (std::fabs(left.score - right.score) > 0.0001)
			return left.score > right.score;
		return left.segmentIndex < right.segmentIndex;
	});
	const int maxAnchors = std::min(static_cast<int>(anchors.size()), std::max(16, maxRawCandidates / 3));
	QVector<Anchor> spacedAnchors;
	spacedAnchors.reserve(maxAnchors);
	const auto anchorStartSec = [&index](const Anchor &anchor) {
		const TranscriptSegment *segment = index.segmentAt(anchor.segmentIndex);
		return segment ? segment->startSec : 0.0;
	};
	const auto alreadySelectedNear = [&spacedAnchors, &anchorStartSec](const Anchor &anchor, double minSpacingSec) {
		const double start = anchorStartSec(anchor);
		for (const Anchor &selected : spacedAnchors) {
			if (std::fabs(anchorStartSec(selected) - start) <= minSpacingSec)
				return true;
		}
		return false;
	};
	for (const double minSpacingSec : QVector<double>{300.0, 180.0, 90.0, 0.0}) {
		for (const Anchor &anchor : anchors) {
			if (spacedAnchors.size() >= maxAnchors)
				break;
			bool alreadySelected = false;
			for (const Anchor &selected : spacedAnchors) {
				if (selected.segmentIndex == anchor.segmentIndex) {
					alreadySelected = true;
					break;
				}
			}
			if (alreadySelected)
				continue;
			if (minSpacingSec > 0.0 && alreadySelectedNear(anchor, minSpacingSec))
				continue;
			spacedAnchors.append(anchor);
		}
		if (spacedAnchors.size() >= maxAnchors)
			break;
	}
	const QVector<double> durations = fastNoveltyDurations(sourceOptions.generation);
	const QVector<double> startOffsets = options.scoring.presetId == QStringLiteral("viewer_message_response")
						     ? QVector<double>{-44.0, -32.0, -18.0, -8.0}
						     : QVector<double>{-18.0, -8.0, 0.0};
	QSet<QString> seen;
	seen.reserve(maxRawCandidates);
	candidates.reserve(maxRawCandidates);

	SemanticCoarseRegion syntheticRegion;
	syntheticRegion.score = 0.58;
	syntheticRegion.evidence.append(QStringLiteral("feedback_novelty_fast_generation"));

	for (int anchorPosition = 0;
	     anchorPosition < spacedAnchors.size() && static_cast<int>(candidates.size()) < maxRawCandidates;
	     ++anchorPosition) {
		const Anchor &anchor = spacedAnchors.at(anchorPosition);
		const TranscriptSegment *segment = index.segmentAt(anchor.segmentIndex);
		if (!segment)
			continue;
		for (const double duration : durations) {
			for (const double offset : startOffsets) {
				ClipDuration range{segment->startSec + offset, segment->startSec + offset + duration};
				range = index.clampRange(range, searchRange);
				if (range.endSec - range.startSec < sourceOptions.generation.minDurationSec)
					continue;
				const QString key = noveltyRangeKey(range);
				if (seen.contains(key))
					continue;
				seen.insert(key);
				ClipCandidate candidate = CandidateBuilder::buildForRange(
					index, sourceOptions.generation, sourceOptions.scoring,
					sourceOptions.qualityGate, range, syntheticRegion);
				candidate.source = anchor.targetHit ? QStringLiteral("target_anchor")
								    : QStringLiteral("cue_anchor");
				candidate.startsNearViewerCue = anchor.viewerCue;
				candidate.anchorText = segment->text.trimmed().left(220);
				candidate.evidence.append(QStringLiteral("feedback_novelty_fast_anchor_generation"));
				candidate.evidence.removeDuplicates();
				if (!CandidateBuilder::isStructurallyViable(candidate, sourceOptions.generation,
									    sourceOptions.qualityGate))
					continue;
				candidates.append(candidate);
				if (static_cast<int>(candidates.size()) >= maxRawCandidates)
					break;
			}
			if (static_cast<int>(candidates.size()) >= maxRawCandidates)
				break;
		}
	}
	return candidates;
}

QVector<ClipCandidate> buildNoveltyExplorationCandidates(const TranscriptIndex &index,
							 const ClipScoringPipelineOptions &options,
							 const Curation::Feedback::FeedbackRangeMemory &memory,
							 NoveltyExplorationBuildStats *stats)
{
	NoveltyExplorationBuildStats localStats;
	QElapsedTimer stageTimer;
	stageTimer.start();

	CandidateSourceBuilderOptions sourceOptions = candidateSourceOptionsFromOptions(options);
	const int semanticPositiveCount = semanticPrototypePositiveRangeCount(memory);
	const int explorationRawLimit =
		semanticPositiveCount > 0
			? std::min(220, std::max(120, options.budget.maxCandidatesBeforeEmbedding))
			: std::min(260, std::max(160, options.budget.maxCandidatesBeforeEmbedding * 2));
	sourceOptions.maxRawCandidates = explorationRawLimit;
	sourceOptions.generation.maxRawCandidates = explorationRawLimit;
	sourceOptions.generation.slidingWindowStepSec = std::max(30.0, options.generation.slidingWindowStepSec);

	QVector<ClipCandidate> raw = buildFastNoveltyRawCandidates(index, options, explorationRawLimit);
	localStats.generationMs = stageTimer.elapsed();
	localStats.rawCandidates = static_cast<int>(raw.size());
	if (memory.loaded && !raw.isEmpty()) {
		QVector<ClipCandidate> unreviewedRaw;
		unreviewedRaw.reserve(raw.size());
		for (const ClipCandidate &candidate : raw) {
			if (rangeWasAlreadyReviewedByFeedbackForExploration(candidate.range, memory)) {
				++localStats.reviewedSuppressed;
				continue;
			}
			unreviewedRaw.append(candidate);
		}
		raw = std::move(unreviewedRaw);
	}

	if (raw.isEmpty()) {
		if (stats)
			*stats = localStats;
		return {};
	}

	stageTimer.restart();
	const int preferredWorkers = std::clamp(static_cast<int>(raw.size() / 24) + 1, 1, 6);
	localStats.workerCount = ParallelChunkMap::boundedWorkerCount(static_cast<int>(raw.size()), preferredWorkers);

	struct LocalScoredCandidate {
		ClipCandidate candidate;
		double score = 0.0;
	};
	const auto scoreRange = [&index, &raw, &memory, &sourceOptions](int first, int last) {
		FeedbackSimilarityScorer feedbackScorer;
		QVector<LocalScoredCandidate> accepted;
		accepted.reserve(std::max(0, last - first));
		for (int i = first; i < last; ++i) {
			ClipCandidate candidate = raw.at(i);
			if (rangeWasAlreadyReviewedByFeedbackForExploration(candidate.range, memory))
				continue;
			if (candidate.range.endSec <= candidate.range.startSec || candidate.text.trimmed().size() < 80)
				continue;
			if (!CandidateBuilder::isStructurallyViable(candidate, sourceOptions.generation,
								    sourceOptions.qualityGate))
				continue;

			const FeedbackSimilarityFeatures feedback = feedbackScorer.score(candidate, index, memory);
			const double duration = candidate.range.endSec - candidate.range.startSec;
			// Exact/near-exact reviewed ranges are already filtered above. For novelty
			// review we should not discard every nearby candidate just because it is
			// close to a previous bad boundary. Treat moderately contaminated ranges as
			// diagnostics instead of hard-dropping them, otherwise the review table dries
			// up after a few negative/adjusted decisions.
			if (feedback.negativeRangeSimilarity >= 0.86 ||
			    feedback.negativeOverlapSec >= std::max(20.0, duration * 0.55))
				continue;
			const bool diagnosticOnly = feedback.negativeRangeContamination ||
						    feedback.negativeRangeSimilarity >= 0.58 ||
						    feedback.negativeOverlapSec >= 10.0 ||
						    (feedback.negativeScore >= 0.68 && feedback.margin <= 0.02);

			candidate.source =
				QStringLiteral("feedback_novelty_exploration:%1").arg(candidate.source.left(80));
			candidate.evidence.append(QStringLiteral("feedback_novelty_exploration"));
			candidate.evidence.append(QStringLiteral("feedback_novelty_outside_suppressed_ranges"));
			if (diagnosticOnly)
				candidate.evidence.append(
					QStringLiteral("feedback_novelty_relaxed_negative_review_probe"));
			candidate.evidence.append(feedback.evidence);
			candidate.evidence.append(QStringLiteral("feedback_novelty_parallel_local_score"));
			candidate.evidence.removeDuplicates();
			const double localScore = localExplorationScore(candidate, feedback);
			candidate.scores.final =
				std::max(candidate.scores.final, 0.34 + std::max(0.0, localScore) * 0.10);
			if (diagnosticOnly) {
				candidate.rejectedByQualityGate = true;
				candidate.rejectionReason = QStringLiteral("novelty_exploration_review_required");
			}
			accepted.append(LocalScoredCandidate{std::move(candidate), localScore});
		}
		return accepted;
	};

	QVector<LocalScoredCandidate> scored =
		ParallelChunkMap::runIndexed(static_cast<int>(raw.size()), preferredWorkers, scoreRange);
	localStats.localScoreMs = stageTimer.elapsed();
	localStats.localAccepted = static_cast<int>(scored.size());

	stageTimer.restart();
	std::sort(scored.begin(), scored.end(), [](const LocalScoredCandidate &left, const LocalScoredCandidate &right) {
		if (std::fabs(left.score - right.score) > 0.0001)
			return left.score > right.score;
		if (std::fabs(left.candidate.scores.viewerResponse - right.candidate.scores.viewerResponse) > 0.0001)
			return left.candidate.scores.viewerResponse > right.candidate.scores.viewerResponse;
		if (std::fabs(left.candidate.scores.hook - right.candidate.scores.hook) > 0.0001)
			return left.candidate.scores.hook > right.candidate.scores.hook;
		return left.candidate.range.startSec < right.candidate.range.startSec;
	});

	QVector<ClipCandidate> exploration;
	// This fallback only needs a small diagnostic pool. Keeping it tight avoids
	// spending tens of seconds reranking and boundary-refining candidates that are
	// likely to be shown as review diagnostics instead of applied markers.
	const int expensiveLimit = semanticPositiveCount > 0 ? 12 : 14;
	exploration.reserve(expensiveLimit);
	for (const LocalScoredCandidate &candidate : scored) {
		if (exploration.size() >= expensiveLimit)
			break;
		if (hasSimilarExplorationRange(exploration, candidate.candidate.range))
			continue;
		exploration.append(candidate.candidate);
	}
	localStats.diversityMs = stageTimer.elapsed();
	localStats.selectedForExpensiveScoring = static_cast<int>(exploration.size());
	if (stats)
		*stats = localStats;
	return exploration;
}

QVector<ClipCandidate> buildExhaustionCoverageDiagnostics(const TranscriptIndex &index,
							  const ClipScoringPipelineOptions &options,
							  const Curation::Feedback::FeedbackRangeMemory &memory,
							  int limit)
{
	QVector<ClipCandidate> diagnostics;
	if (index.isEmpty() || limit <= 0)
		return diagnostics;

	CandidateSourceBuilderOptions sourceOptions = candidateSourceOptionsFromOptions(options);
	const ClipDuration searchRange = sourceOptions.generation.searchRange;
	const double searchStart = std::max(0.0, searchRange.startSec);
	const double searchEnd = searchRange.endSec > searchStart ? searchRange.endSec : searchStart;
	const double span = searchEnd - searchStart;
	if (span <= 1.0)
		return diagnostics;

	CheapClipScorer cueScorer;
	const QVector<double> durations = fastNoveltyDurations(sourceOptions.generation);
	const QVector<double> startOffsets = options.scoring.presetId == QStringLiteral("viewer_message_response")
						     ? QVector<double>{-58.0, -44.0, -28.0, -12.0, 0.0}
						     : QVector<double>{-24.0, -12.0, 0.0};
	const int bucketCount = std::clamp(limit * 6, 12, 48);
	const double bucketSize = span / static_cast<double>(bucketCount);

	struct BucketAnchor {
		int segmentIndex = -1;
		double score = -1.0;
		double bucketCenter = 0.0;
	};

	QVector<BucketAnchor> anchors;
	anchors.reserve(bucketCount);
	for (int bucket = 0; bucket < bucketCount; ++bucket) {
		const double bucketStart = searchStart + bucketSize * static_cast<double>(bucket);
		const double bucketEnd = bucket == bucketCount - 1 ? searchEnd : bucketStart + bucketSize;
		const double bucketCenter = (bucketStart + bucketEnd) * 0.5;
		BucketAnchor best;
		best.bucketCenter = bucketCenter;
		for (int i = 0; i < index.size(); ++i) {
			const TranscriptSegment *segment = index.segmentAt(i);
			if (!segment || segment->text.trimmed().isEmpty())
				continue;
			if (segment->endSec < bucketStart || segment->startSec > bucketEnd)
				continue;
			const QString text = segment->text.trimmed();
			const bool targetHit =
				sourceOptions.generation.reliableMainTarget &&
				cueScorer.targetKeywordScore(text, sourceOptions.generation.mainTarget) >= 0.22;
			const bool strongCue = cueScorer.hasStrongLocalCue(text);
			const bool viewerCue = cueScorer.looksLikeQuestionOrViewerMessage(text);
			const double segmentCenter = (segment->startSec + segment->endSec) * 0.5;
			const double distancePenalty = std::min(0.20, std::fabs(segmentCenter - bucketCenter) /
									      std::max(1.0, bucketSize) * 0.20);
			double score =
				0.08 + std::min(0.16, static_cast<double>(text.size()) / 900.0) - distancePenalty;
			if (targetHit)
				score += 0.34;
			if (viewerCue)
				score += 0.24;
			if (strongCue)
				score += 0.18;
			if (score > best.score)
				best = BucketAnchor{i, score, bucketCenter};
		}
		if (best.segmentIndex >= 0)
			anchors.append(best);
	}

	std::sort(anchors.begin(), anchors.end(), [](const BucketAnchor &left, const BucketAnchor &right) {
		if (std::fabs(left.score - right.score) > 0.0001)
			return left.score > right.score;
		return left.bucketCenter < right.bucketCenter;
	});

	SemanticCoarseRegion syntheticRegion;
	syntheticRegion.score = 0.42;
	syntheticRegion.evidence.append(QStringLiteral("feedback_exhaustion_coverage_fallback"));
	QSet<QString> seen;
	seen.reserve(limit * 4);
	const int desiredMinimum = std::min(limit, 3);
	for (int pass = 0; pass < 2 && diagnostics.size() < desiredMinimum; ++pass) {
		const bool forceBeyondIgnoredNeighborhoods = pass > 0;
		for (const BucketAnchor &anchor : anchors) {
			if (diagnostics.size() >= limit)
				break;
			const TranscriptSegment *segment = index.segmentAt(anchor.segmentIndex);
			if (!segment)
				continue;
			for (const double duration : durations) {
				if (diagnostics.size() >= limit)
					break;
				for (const double offset : startOffsets) {
					ClipDuration range{segment->startSec + offset,
							   segment->startSec + offset + duration};
					range = index.clampRange(range, searchRange);
					if (range.endSec - range.startSec < sourceOptions.generation.minDurationSec)
						continue;
					if (rangeWasExactlyReviewedByFeedback(range, memory))
						continue;
					if (!forceBeyondIgnoredNeighborhoods &&
					    rangeWasAlreadyReviewedByFeedbackForExploration(range, memory))
						continue;
					if (hasSimilarDiagnosticRange(diagnostics, range) ||
					    hasNearbyDiagnosticReviewCluster(diagnostics, range))
						continue;
					const QString key =
						QStringLiteral("%1:%2").arg(pass).arg(noveltyRangeKey(range));
					if (seen.contains(key))
						continue;
					seen.insert(key);
					ClipCandidate candidate = CandidateBuilder::buildForRange(
						index, sourceOptions.generation, sourceOptions.scoring,
						sourceOptions.qualityGate, range, syntheticRegion);
					if (candidate.range.endSec <= candidate.range.startSec ||
					    candidate.text.trimmed().size() < 60)
						continue;
					candidate.source =
						forceBeyondIgnoredNeighborhoods
							? QStringLiteral("feedback_exhaustion_forced_coverage_probe")
							: QStringLiteral("feedback_exhaustion_coverage_probe");
					candidate.rejectedByQualityGate = true;
					candidate.qualityGateChecked = true;
					candidate.rejectionReason =
						QStringLiteral("novelty_exploration_review_required");
					candidate.scores.final = std::max(candidate.scores.final, 0.18);
					candidate.evidence.append(QStringLiteral("feedback_exhaustion_coverage_probe"));
					candidate.evidence.append(
						QStringLiteral("feedback_exhaustion_after_reviewed_neighborhoods"));
					candidate.evidence.append(
						QStringLiteral("not_training_signal_until_user_decision"));
					if (forceBeyondIgnoredNeighborhoods)
						candidate.evidence.append(QStringLiteral(
							"feedback_exhaustion_forced_after_all_unreviewed_regions_exhausted"));
					candidate.evidence.append(QStringLiteral("coverage_bucket_center:%1")
									  .arg(anchor.bucketCenter, 0, 'f', 1));
					candidate.evidence.removeDuplicates();
					diagnostics.append(candidate);
					break;
				}
			}
		}
	}
	std::sort(diagnostics.begin(), diagnostics.end(), [](const ClipCandidate &left, const ClipCandidate &right) {
		return left.range.startSec < right.range.startSec;
	});
	return diagnostics;
}

QVector<ClipCandidate> explorationDiagnosticsFromCandidates(const QVector<ClipCandidate> &candidates,
							    const Curation::Feedback::FeedbackRangeMemory &memory,
							    int limit)
{
	QVector<ClipCandidate> diagnostics = topRejectedCandidatesForDiagnostics(candidates, limit, &memory);
	if (diagnostics.size() >= limit)
		return diagnostics;

	for (ClipCandidate candidate : candidates) {
		if (diagnostics.size() >= limit)
			break;
		if (candidate.range.endSec <= candidate.range.startSec ||
		    isFeedbackSuppressedRejectedCandidate(candidate))
			continue;
		if (rangeWasAlreadyReviewedByFeedbackForExploration(candidate.range, memory))
			continue;
		if (isRejectedCandidate(candidate))
			continue;
		if (hasSimilarExplorationRange(diagnostics, candidate.range))
			continue;

		candidate.rejectedByQualityGate = true;
		candidate.rejectionReason = QStringLiteral("novelty_exploration_review_required");
		candidate.evidence.append(QStringLiteral("feedback_novelty_exploration_review_required"));
		candidate.evidence.removeDuplicates();
		diagnostics.append(candidate);
	}
	return diagnostics;
}

QVector<ClipCandidate> noveltyTimelineCandidatesFromCandidates(const QVector<ClipCandidate> &candidates,
							       const Curation::Feedback::FeedbackRangeMemory &memory,
							       int limit)
{
	QVector<ClipCandidate> promotable;
	const int maxItems = std::max(1, limit);
	for (ClipCandidate candidate : candidates) {
		if (promotable.size() >= maxItems)
			break;
		if (candidate.range.endSec <= candidate.range.startSec || candidate.text.trimmed().isEmpty())
			continue;
		if (isRejectedCandidate(candidate) || isFeedbackSuppressedRejectedCandidate(candidate))
			continue;
		if (rangeWasAlreadyReviewedByFeedback(candidate.range, memory) &&
		    !candidateHasEvidence(candidate,
					  QStringLiteral("complete_viewer_arc_gate_passed_by_user_feedback")) &&
		    !candidateHasEvidence(
			    candidate, QStringLiteral("complete_viewer_arc_gate_passed_by_direct_positive_feedback")))
			continue;
		if (hasSimilarExplorationRange(promotable, candidate.range))
			continue;
		candidate.evidence.append(QStringLiteral("feedback_novelty_exploration_promoted_to_timeline"));
		candidate.evidence.removeDuplicates();
		promotable.append(candidate);
	}
	return promotable;
}
} // namespace Curation::Scoring::ClipScoringPipelineDetail
