#include "curation/scoring/clip-ranker.hpp"

#include <algorithm>
#include <cmath>

namespace Curation::Scoring {

QVector<ClipCandidate> ClipRanker::rank(QVector<ClipCandidate> candidates, const ClipRankerOptions &options) const
{
	candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&options](const ClipCandidate &candidate) {
		return candidate.range.endSec <= candidate.range.startSec || candidate.text.trimmed().isEmpty() ||
		       candidate.rejectedAsNoise || candidate.rejectedByQualityGate || candidate.scores.final < options.minFinalScore;
	}), candidates.end());

	std::sort(candidates.begin(), candidates.end(), [](const ClipCandidate &left, const ClipCandidate &right) {
		if (std::fabs(left.scores.final - right.scores.final) > 0.0001)
			return left.scores.final > right.scores.final;
		if (std::fabs(left.scores.boundary - right.scores.boundary) > 0.0001)
			return left.scores.boundary > right.scores.boundary;
		return left.range.startSec < right.range.startSec;
	});

	QVector<ClipCandidate> selected;
	selected.reserve(std::min(static_cast<long long>(options.maxCandidates), static_cast<long long>(candidates.size())));
	for (const ClipCandidate &candidate : candidates) {
		bool overlaps = false;
		for (const ClipCandidate &selectedCandidate : selected) {
			if (rangesOverlapTooMuch(candidate.range, selectedCandidate.range, options.overlapToleranceSec)) {
				overlaps = true;
				break;
			}
		}
		if (overlaps)
			continue;

		selected.append(candidate);
		if (static_cast<int>(selected.size()) >= options.maxCandidates)
			break;
	}

	return selected;
}

bool ClipRanker::rangesOverlapTooMuch(const ClipDuration &left, const ClipDuration &right,
					      double overlapToleranceSec) const
{
	const double overlap = std::min(left.endSec, right.endSec) - std::max(left.startSec, right.startSec);
	if (overlap <= overlapToleranceSec)
		return false;

	const double leftDuration = std::max(0.0, left.endSec - left.startSec);
	const double rightDuration = std::max(0.0, right.endSec - right.startSec);
	const double shorter = std::min(leftDuration, rightDuration);
	return shorter <= 0.0 || overlap >= shorter * 0.45;
}

} // namespace Curation::Scoring
