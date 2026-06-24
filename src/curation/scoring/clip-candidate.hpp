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
	double coarseSemantic = 0.0;
	double semanticTarget = 0.0;
	double embeddingTarget = 0.0;
	double semanticViewerMessage = 0.0;
	double semanticDirectAnswer = 0.0;
	double semanticNoise = 0.0;
	double semanticTopicShift = 0.0;
	double semanticClipValue = 0.0;
	double semanticEmpathy = 0.0;
	double semanticHook = 0.0;
	double semanticResolution = 0.0;
	double semanticMetaNoise = 0.0;
	double semanticOpeningHook = 0.0;
	double semanticOpeningMetaNoise = 0.0;
	double semanticEndingResolution = 0.0;
	double semanticEndingMetaNoise = 0.0;
	double semanticEndingTopicShift = 0.0;
	double topicContinuity = 0.0;
	double semanticFocusContinuity = 0.0;
	double reranker = 0.0;
	double rerankerRaw = 0.0;
	double rerankerBadClip = 0.0;
	double rerankerOpeningDefect = 0.0;
	double rerankerEndingDefect = 0.0;
	double rerankerStructureDefect = 0.0;
	double rerankerClipQualityMargin = 0.0;
	double qualityGate = 0.0;
	double noise = 0.0;
	double pauseBeforeSec = 0.0;
	double pauseAfterSec = 0.0;
	double maxInternalPauseSec = 0.0;
	double pauseBoundary = 0.0;
	double arcOpening = 0.0;
	double arcDevelopment = 0.0;
	double arcConclusion = 0.0;
	double arcBoundaryCleanliness = 0.0;
	double arcTailRisk = 0.0;
	double arcCompleteness = 0.0;
	double final = 0.0;
};

struct ClipCandidate {
	ClipDuration range;
	int firstSegmentIndex = -1;
	int lastSegmentIndex = -1;
	QString text;
	QString timedText;
	QString anchorText;
	QString source;
	QStringList evidence;
	QStringList emotionalCues;
	ClipCandidateScores scores;
	bool startsNearViewerCue = false;
	bool endsBeforeNextCue = false;
	bool hasReliableMainTarget = false;
	bool semanticScoringAvailable = false;
	bool rerankerAvailable = false;
	bool rerankerAttempted = false;
	bool rerankerFailed = false;
	QString rerankerFailureReason;
	int selectedRank = 0;
	double selectedMmrScore = 0.0;
	double selectedMmrSimilarity = 0.0;
	bool qualityGateChecked = false;
	bool rejectedAsNoise = false;
	bool rejectedByQualityGate = false;
	QString rejectionReason;
};

struct ClipScoringResult {
	QVector<ClipCandidate> candidates;
	QVector<ClipCandidate> rejectedCandidateDiagnostics;
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
