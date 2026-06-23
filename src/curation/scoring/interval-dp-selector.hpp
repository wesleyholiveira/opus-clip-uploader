#pragma once

#include "models/curation-settings.hpp"

#include <QVector>

namespace Curation::Scoring {

struct WeightedIntervalCandidate {
	int sourceIndex = -1;
	ClipDuration range;
	double score = 0.0;
};

struct WeightedIntervalSelectionOptions {
	int maxItems = 5;
	double overlapToleranceSec = 0.0;
	double minSpacingSec = 0.0;
};

class IntervalDpSelector {
public:
	QVector<int> select(const QVector<WeightedIntervalCandidate> &intervals,
		const WeightedIntervalSelectionOptions &options) const;

private:
	bool compatible(const WeightedIntervalCandidate &left, const WeightedIntervalCandidate &right,
		const WeightedIntervalSelectionOptions &options) const;
};

} // namespace Curation::Scoring
