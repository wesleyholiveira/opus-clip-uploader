#pragma once

#include "models/curation-settings.hpp"

#include <QString>
#include <QStringList>
#include <QVector>

namespace Curation::Scoring {

struct ClipCandidateScores {
	double duration = 0.0;
	double boundary = 0.0;
	double hook = 0.0;
	double emotional = 0.0;
	double advice = 0.0;
	double explanation = 0.0;
	double story = 0.0;
	double opinion = 0.0;
	double tutorial = 0.0;
	double viewerResponse = 0.0;
	double semanticTarget = 0.0;
	double topicContinuity = 0.0;
	double noise = 0.0;
	double final = 0.0;
};

struct ClipCandidate {
	ClipDuration range;
	int firstSegmentIndex = -1;
	int lastSegmentIndex = -1;
	QString text;
	QString anchorText;
	QString source;
	QStringList evidence;
	QStringList emotionalCues;
	ClipCandidateScores scores;
	bool startsNearViewerCue = false;
	bool endsBeforeNextCue = false;
	bool hasReliableMainTarget = false;
	bool rejectedAsNoise = false;
};

struct ClipScoringResult {
	QVector<ClipCandidate> candidates;
	QString summary;

	QVector<ClipDuration> ranges() const
	{
		QVector<ClipDuration> result;
		result.reserve(candidates.size());
		for (const ClipCandidate &candidate : candidates) {
			if (candidate.range.endSec > candidate.range.startSec)
				result.append(candidate.range);
		}
		return result;
	}
};

} // namespace Curation::Scoring
