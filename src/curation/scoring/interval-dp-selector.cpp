#include "curation/scoring/interval-dp-selector.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace Curation::Scoring {

namespace {

static double centerSec(const ClipDuration &range)
{
	return range.startSec + ((range.endSec - range.startSec) * 0.5);
}

static bool betterScore(double left, double right)
{
	return left > right + 0.000001;
}

} // namespace

bool IntervalDpSelector::compatible(const WeightedIntervalCandidate &left, const WeightedIntervalCandidate &right,
				    const WeightedIntervalSelectionOptions &options) const
{
	const double overlap =
		std::min(left.range.endSec, right.range.endSec) - std::max(left.range.startSec, right.range.startSec);
	if (overlap > options.overlapToleranceSec)
		return false;
	if (options.minSpacingSec > 0.0 &&
	    std::fabs(centerSec(left.range) - centerSec(right.range)) < options.minSpacingSec)
		return false;
	return true;
}

QVector<int> IntervalDpSelector::select(const QVector<WeightedIntervalCandidate> &intervals,
					const WeightedIntervalSelectionOptions &options) const
{
	QVector<int> result;
	if (intervals.isEmpty() || options.maxItems <= 0)
		return result;

	QVector<WeightedIntervalCandidate> sorted = intervals;
	sorted.erase(std::remove_if(sorted.begin(), sorted.end(),
				    [](const WeightedIntervalCandidate &candidate) {
					    return candidate.sourceIndex < 0 ||
						   candidate.range.endSec <= candidate.range.startSec ||
						   !std::isfinite(candidate.score);
				    }),
		     sorted.end());
	if (sorted.isEmpty())
		return result;

	std::sort(sorted.begin(), sorted.end(),
		  [](const WeightedIntervalCandidate &left, const WeightedIntervalCandidate &right) {
			  if (std::fabs(left.range.endSec - right.range.endSec) > 0.0001)
				  return left.range.endSec < right.range.endSec;
			  if (std::fabs(left.range.startSec - right.range.startSec) > 0.0001)
				  return left.range.startSec < right.range.startSec;
			  return left.score > right.score;
		  });

	const int n = static_cast<int>(sorted.size());
	const int limit = std::min(std::max(1, options.maxItems), n);
	QVector<int> previous(n, -1);
	for (int i = 0; i < n; ++i) {
		for (int j = i - 1; j >= 0; --j) {
			if (compatible(sorted.at(j), sorted.at(i), options)) {
				previous[i] = j;
				break;
			}
		}
	}

	QVector<QVector<double>> dp(n + 1, QVector<double>(limit + 1, 0.0));
	QVector<QVector<bool>> take(n + 1, QVector<bool>(limit + 1, false));
	for (int i = 1; i <= n; ++i) {
		const WeightedIntervalCandidate &candidate = sorted.at(i - 1);
		const int previousRow = previous.at(i - 1) + 1;
		for (int k = 1; k <= limit; ++k) {
			const double skipScore = dp[i - 1][k];
			const double takeScore = dp[previousRow][k - 1] + std::max(0.0, candidate.score);
			if (betterScore(takeScore, skipScore)) {
				dp[i][k] = takeScore;
				take[i][k] = true;
			} else {
				dp[i][k] = skipScore;
			}
		}
	}

	int bestK = 1;
	for (int k = 2; k <= limit; ++k) {
		if (betterScore(dp[n][k], dp[n][bestK]))
			bestK = k;
	}

	QVector<int> selectedSortedRows;
	for (int i = n, k = bestK; i > 0 && k > 0;) {
		if (take[i][k]) {
			selectedSortedRows.prepend(i - 1);
			i = previous.at(i - 1) + 1;
			--k;
		} else {
			--i;
		}
	}

	for (const int row : selectedSortedRows)
		result.append(sorted.at(row).sourceIndex);
	return result;
}

} // namespace Curation::Scoring
