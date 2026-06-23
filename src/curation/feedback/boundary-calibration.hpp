#pragma once

#include <QString>

namespace Curation::Feedback {

struct BoundaryCalibrationProfile {
	double maxQuestionLookbackSec = -1.0;
	double lookaheadSec = -1.0;
	double contextWeight = 1.0;
	double hookWeight = 1.0;
	double developmentWeight = 1.0;
	double resolutionWeight = 1.0;
	double targetWeight = 1.0;
	double defectPenalty = 1.0;
	double minArcConfidence = -1.0;
	bool loaded = false;
};

class BoundaryCalibration {
public:
	static BoundaryCalibrationProfile profileForPreset(const QString &presetId);
};

} // namespace Curation::Feedback
