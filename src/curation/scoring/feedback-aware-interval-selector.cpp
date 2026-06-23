#include "curation/scoring/feedback-aware-interval-selector.hpp"

#include "curation/scoring/interval-dp-selector.hpp"

#include <algorithm>
#include <cmath>
#include <QSet>

namespace Curation::Scoring {

namespace {

static bool hasEvidence(const ClipCandidate &candidate, const QString &needle)
{
	for (const QString &evidence : candidate.evidence) {
		if (evidence == needle || evidence.contains(needle))
			return true;
	}
	return false;
}

static bool isFeedbackPositiveSeed(const ClipCandidate &candidate)
{
	return candidate.source.contains(QStringLiteral("feedback_positive")) ||
		hasEvidence(candidate, QStringLiteral("feedback_positive_seed")) ||
		hasEvidence(candidate, QStringLiteral("feedback_positive_guided"));
}

static bool isLearnedFeedbackAcceptedCandidate(const ClipCandidate &candidate)
{
	return hasEvidence(candidate, QStringLiteral("complete_viewer_arc_gate_passed_by_feedback_trained_ranker")) ||
		hasEvidence(candidate, QStringLiteral("feedback_trained_ranker_strong_accept")) ||
		hasEvidence(candidate, QStringLiteral("feedback_trained_ranker_accept"));
}

} // namespace

double FeedbackAwareIntervalSelector::selectionScore(const ClipCandidate &candidate,
	const FeedbackSimilarityFeatures &features) const
{
	double score = candidate.scores.final;
	score += features.positiveScore * 0.38;
	score += std::max(0.0, features.margin) * 0.32;
	score -= features.negativeScore * 0.34;
	if (features.negativeRangeContamination && !features.explainedByPositiveRange)
		score -= 1.5;
	if (isFeedbackPositiveSeed(candidate))
		score += 0.65;
	if (hasEvidence(candidate, QStringLiteral("complete_viewer_arc_gate_passed")))
		score += 0.20;
	if (isLearnedFeedbackAcceptedCandidate(candidate))
		score += 0.45;
	if (candidate.scores.arcCompleteness >= 0.30)
		score += candidate.scores.arcCompleteness * 0.18;
	return score;
}

QVector<ClipCandidate> FeedbackAwareIntervalSelector::select(QVector<ClipCandidate> candidates,
	const TranscriptIndex &index, const Curation::Feedback::FeedbackRangeMemory &memory,
	const ClipRankerOptions &options) const
{
	if (candidates.isEmpty() || options.maxCandidates <= 0 || !memory.loaded)
		return candidates;

	FeedbackSimilarityScorer scorer;
	QVector<WeightedIntervalCandidate> intervals;
	intervals.reserve(candidates.size());
	for (int i = 0; i < candidates.size(); ++i) {
		const FeedbackSimilarityFeatures features = scorer.score(candidates.at(i), index, memory);
		double score = selectionScore(candidates.at(i), features);
		if (features.negativeRangeContamination && features.negativeScore >= 0.72 && features.margin <= 0.08)
			continue;
		if (isFeedbackPositiveSeed(candidates.at(i)))
			score = std::max(score, 1.0);
		if (isLearnedFeedbackAcceptedCandidate(candidates.at(i)))
			score = std::max(score, 0.92);
		if (candidates.at(i).source == QStringLiteral("feedback_positive_pattern_search"))
			score = std::max(score, 0.62 + std::max(0.0, features.margin));
		if (!std::isfinite(score) || score <= 0.0)
			continue;
		WeightedIntervalCandidate interval;
		interval.sourceIndex = i;
		interval.range = candidates.at(i).range;
		interval.score = score;
		intervals.append(interval);
	}
	if (intervals.isEmpty())
		return {};

	WeightedIntervalSelectionOptions dpOptions;
	dpOptions.maxItems = options.maxCandidates;
	dpOptions.overlapToleranceSec = std::max(0.0, options.overlapToleranceSec);
	dpOptions.minSpacingSec = std::max(0.0, options.minSpacingSec);
	IntervalDpSelector selector;
	const QVector<int> selectedIndices = selector.select(intervals, dpOptions);
	if (selectedIndices.isEmpty())
		return {};

	QVector<ClipCandidate> selected;
	selected.reserve(selectedIndices.size());
	QSet<int> selectedSourceIndexes;
	for (const int sourceIndex : selectedIndices) {
		if (sourceIndex < 0 || sourceIndex >= candidates.size())
			continue;
		ClipCandidate candidate = candidates.at(sourceIndex);
		candidate.evidence.append(QStringLiteral("feedback_aware_interval_dp_selected"));
		candidate.evidence.removeDuplicates();
		selected.append(candidate);
		selectedSourceIndexes.insert(sourceIndex);
	}

	// Weighted interval DP is intentionally conservative. Keep it as the primary
	// selector, but do not let it collapse the whole feedback-guided pool into a
	// single exact seed when there are other non-conflicting feedback-pattern
	// candidates available. The final ClipRanker/MMR still deduplicates and spaces
	// the output.
	if (selected.size() < std::min(options.maxCandidates, 4)) {
		QVector<int> ordered;
		ordered.reserve(intervals.size());
		for (int i = 0; i < intervals.size(); ++i)
			ordered.append(i);
		std::sort(ordered.begin(), ordered.end(), [&intervals](int left, int right) {
			return intervals.at(left).score > intervals.at(right).score;
		});
		for (const int row : ordered) {
			if (selected.size() >= options.maxCandidates || selected.size() >= 12)
				break;
			const int sourceIndex = intervals.at(row).sourceIndex;
			if (selectedSourceIndexes.contains(sourceIndex) || sourceIndex < 0 || sourceIndex >= candidates.size())
				continue;
			ClipCandidate candidate = candidates.at(sourceIndex);
			candidate.evidence.append(QStringLiteral("feedback_aware_interval_dp_supplemental"));
			candidate.evidence.removeDuplicates();
			selected.append(candidate);
			selectedSourceIndexes.insert(sourceIndex);
		}
	}
	return selected;
}

} // namespace Curation::Scoring
