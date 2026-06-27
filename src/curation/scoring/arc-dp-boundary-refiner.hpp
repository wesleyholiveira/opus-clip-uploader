#pragma once

#include "curation/scoring/clip-candidate.hpp"
#include "curation/scoring/transcript-index.hpp"

#include <QString>

namespace Curation::Scoring {

struct ArcDpBoundaryRefinerOptions {
	ClipDuration searchRange;
	QString presetId;
	QString mainTarget;
	bool reliableMainTarget = false;
	double minDurationSec = 8.0;
	double maxDurationSec = 180.0;
	double lookbackSec = 60.0;
	double lookaheadSec = 42.0;
	double contextWeight = 1.0;
	double hookWeight = 1.0;
	double developmentWeight = 1.0;
	double resolutionWeight = 1.0;
	double targetWeight = 1.0;
	double defectPenalty = 1.0;
	double minArcConfidence = 0.30;
};

class ArcDpBoundaryRefiner {
public:
	ClipCandidate refine(const TranscriptIndex &index, const ClipCandidate &candidate,
			     const ArcDpBoundaryRefinerOptions &options) const;
};

} // namespace Curation::Scoring
