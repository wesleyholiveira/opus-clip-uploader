#pragma once

#include "curation/scoring/clip-candidate.hpp"
#include "curation/scoring/transcript-index.hpp"

#include <QString>
#include <QVector>

namespace Curation::Scoring {

struct CandidateGenerationOptions {
	ClipDuration searchRange;
	QString presetId;
	QString mainTarget;
	bool reliableMainTarget = false;
	double minDurationSec = 18.0;
	double maxDurationSec = 75.0;
	double anchorPaddingBeforeSec = 0.35;
	double defaultAfterSec = 32.0;
	int maxRawCandidates = 120;
};

class CandidateGenerator {
public:
	QVector<ClipCandidate> generate(const TranscriptIndex &index, const CandidateGenerationOptions &options) const;

private:
	QVector<ClipCandidate> cueAnchoredCandidates(const TranscriptIndex &index,
						       const CandidateGenerationOptions &options) const;
	QVector<ClipCandidate> slidingWindowCandidates(const TranscriptIndex &index,
						       const CandidateGenerationOptions &options) const;
	ClipCandidate buildCandidate(const TranscriptIndex &index, const CandidateGenerationOptions &options,
					     double startSec, double endSec, const QString &source,
					     bool startsNearViewerCue) const;
	ClipDuration trimBeforeNextStrongCue(const TranscriptIndex &index, const CandidateGenerationOptions &options,
					       const ClipDuration &range, int firstSegmentIndex) const;
};

} // namespace Curation::Scoring
