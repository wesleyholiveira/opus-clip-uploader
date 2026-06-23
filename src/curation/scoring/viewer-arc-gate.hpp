#pragma once

#include "curation/scoring/clip-candidate.hpp"
#include "curation/scoring/transcript-index.hpp"

#include <QString>
#include <QVector>

namespace Curation::Scoring {

struct ViewerArcGateOptions {
	bool enabled = false;
	QString presetId;
	ClipDuration searchRange;
	double minDurationSec = 0.0;
	double maxDurationSec = 180.0;
};

class ViewerArcGate {
public:
	void recoverMissingOpenings(const TranscriptIndex &index, QVector<ClipCandidate> &candidates,
		const ViewerArcGateOptions &options) const;
	QVector<ClipCandidate> apply(QVector<ClipCandidate> candidates, const ViewerArcGateOptions &options) const;
	bool hasCompleteArc(const ClipCandidate &candidate, const ViewerArcGateOptions &options) const;

private:
	bool recoverMissingOpening(const TranscriptIndex &index, ClipCandidate &candidate,
		const ViewerArcGateOptions &options) const;
	bool shouldAttemptOpeningRecovery(const ClipCandidate &candidate, const ViewerArcGateOptions &options) const;
	bool looksLikeViewerOriginText(const QString &rawText) const;
	bool isUserAcceptedFeedbackSeed(const ClipCandidate &candidate) const;
	bool isFeedbackSuppressed(const ClipCandidate &candidate) const;
	bool hasValidStateMachine(const ClipCandidate &candidate) const;
	bool hasInvalidStateMachine(const ClipCandidate &candidate) const;
	bool hasExplicitViewerOrigin(const ClipCandidate &candidate) const;
	bool hasLearnedFeedbackArcSupport(const ClipCandidate &candidate) const;
	QString invalidReason(const ClipCandidate &candidate) const;
	double calibratedMinArcConfidence(const ViewerArcGateOptions &options) const;
};

} // namespace Curation::Scoring
