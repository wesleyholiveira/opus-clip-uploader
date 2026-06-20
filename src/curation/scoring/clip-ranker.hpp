#pragma once

#include "curation/scoring/clip-candidate.hpp"

#include <QVector>

namespace Curation::Scoring {

struct ClipRankerOptions {
	int maxCandidates = 5;
	double minFinalScore = 0.42;
	double overlapToleranceSec = 8.0;
};

class ClipRanker {
public:
	QVector<ClipCandidate> rank(QVector<ClipCandidate> candidates, const ClipRankerOptions &options) const;

private:
	bool rangesOverlapTooMuch(const ClipDuration &left, const ClipDuration &right,
				   double overlapToleranceSec) const;
};

} // namespace Curation::Scoring
