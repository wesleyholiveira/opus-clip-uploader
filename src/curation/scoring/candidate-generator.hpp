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
	double maxDurationSec = 180.0;
	double anchorPaddingBeforeSec = 0.35;
	double defaultAfterSec = 90.0;
	double emotionalAfterSec = 120.0;
	double adviceAfterSec = 150.0;
	double boundaryMinDurationSec = 8.0;
	double slidingWindowStepSec = 15.0;
	int maxRawCandidates = 120;
};

class CandidateGenerator {
public:
	QVector<ClipCandidate> generate(const TranscriptIndex &index, const CandidateGenerationOptions &options) const;

private:
	QVector<ClipCandidate> cueAnchoredCandidates(const TranscriptIndex &index,
						     const CandidateGenerationOptions &options) const;
	QVector<ClipCandidate> arcDpCandidates(const TranscriptIndex &index,
					       const CandidateGenerationOptions &options) const;
	QVector<ClipCandidate> slidingWindowCandidates(const TranscriptIndex &index,
						       const CandidateGenerationOptions &options) const;
	ClipCandidate buildCandidate(const TranscriptIndex &index, const CandidateGenerationOptions &options,
				     double startSec, double endSec, const QString &source,
				     bool startsNearViewerCue) const;
	ClipDuration expandStartForViewerContext(const TranscriptIndex &index,
						 const CandidateGenerationOptions &options, const ClipDuration &range,
						 int anchorSegmentIndex) const;
	ClipDuration extendForOpenExplanation(const TranscriptIndex &index, const CandidateGenerationOptions &options,
					      const ClipDuration &range) const;
	ClipDuration trimBeforeNextTopicShift(const TranscriptIndex &index, const CandidateGenerationOptions &options,
					      const ClipDuration &range, int anchorSegmentIndex) const;
};

} // namespace Curation::Scoring
