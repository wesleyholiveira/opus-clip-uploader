#pragma once

#include "curation/scoring/clip-candidate.hpp"
#include "curation/scoring/transcript-index.hpp"

#include <QString>
#include <QStringList>

namespace Curation::Scoring {

struct CheapScoringContext {
	QString presetId;
	QString mainTarget;
	bool reliableMainTarget = false;
};

class CheapClipScorer {
public:
	ClipCandidate score(const TranscriptIndex &index, const ClipCandidate &candidate,
			    const CheapScoringContext &context) const;

	bool hasStrongLocalCue(const QString &text) const;
	bool looksLikeQuestionOrViewerMessage(const QString &text) const;
	double targetKeywordScore(const QString &text, const QString &target) const;

private:
	double durationScore(double durationSec) const;
	double boundaryScore(const TranscriptIndex &index, const ClipCandidate &candidate) const;
	double hookScore(const QString &text) const;
	double topicContinuityScore(const QString &text) const;
	double viewerResponseScore(const QString &text, bool startsNearViewerCue, bool endsBeforeNextCue) const;
	double finalScore(const ClipCandidate &candidate, const CheapScoringContext &context) const;
	QStringList targetTerms(const QString &target) const;
};

} // namespace Curation::Scoring
