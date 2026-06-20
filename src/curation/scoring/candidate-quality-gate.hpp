#pragma once

#include "curation/scoring/clip-candidate.hpp"

#include <QString>
#include <QVector>

namespace Curation::Scoring {

struct CandidateQualityGateOptions {
	QString presetId;
	QString mainTarget;
	bool reliableMainTarget = false;
	double minDurationSec = 12.0;
	int minTextChars = 40;
	double minFinalScore = 0.34;
	double maxNoiseScore = 0.82;
	double minSemanticTargetWhenAvailable = 0.54;
	double minViewerResponseForViewerPreset = 0.32;
	bool requireSemanticTargetWhenAvailable = true;
};

class CandidateQualityGate {
public:
	QVector<ClipCandidate> apply(QVector<ClipCandidate> candidates, const CandidateQualityGateOptions &options) const;
	ClipCandidate apply(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;

private:
	QString rejectionReason(const ClipCandidate &candidate, const CandidateQualityGateOptions &options) const;
};

} // namespace Curation::Scoring
