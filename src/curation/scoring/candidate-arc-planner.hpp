#pragma once

#include "curation/scoring/candidate-generator.hpp"
#include "curation/scoring/transcript-index.hpp"

#include <QString>
#include <QStringList>
#include <QVector>

namespace Curation::Scoring {

struct PlannedArcCandidateRange {
	ClipDuration range;
	QString source;
	QStringList evidence;
	bool startsNearViewerCue = false;
};

class CandidateArcPlanner {
public:
	QVector<PlannedArcCandidateRange> plan(const TranscriptIndex &index,
		const CandidateGenerationOptions &options) const;
};

} // namespace Curation::Scoring
