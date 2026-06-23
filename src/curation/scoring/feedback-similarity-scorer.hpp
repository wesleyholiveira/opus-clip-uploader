#pragma once

#include "curation/feedback/curation-feedback-store.hpp"
#include "curation/scoring/clip-candidate.hpp"
#include "curation/scoring/transcript-index.hpp"

#include <QString>
#include <QStringList>

namespace Curation::Scoring {

struct FeedbackSimilarityFeatures {
	double positiveRangeSimilarity = 0.0;
	double negativeRangeSimilarity = 0.0;
	double positiveTextSimilarity = 0.0;
	double negativeTextSimilarity = 0.0;
	double positiveScore = 0.0;
	double negativeScore = 0.0;
	double margin = 0.0;
	double negativeOverlapSec = 0.0;
	double positiveOverlapSec = 0.0;
	bool negativeRangeContamination = false;
	bool explainedByPositiveRange = false;
	QString positiveReason;
	QString negativeReason;
	QString positiveDecision;
	QString negativeDecision;
	QStringList evidence;
};

class FeedbackSimilarityScorer {
public:
	FeedbackSimilarityFeatures score(const ClipCandidate &candidate, const TranscriptIndex &index,
					 const Curation::Feedback::FeedbackRangeMemory &memory) const;
	FeedbackSimilarityFeatures scoreRange(const ClipDuration &range, const QString &text,
					      const TranscriptIndex &index,
					      const Curation::Feedback::FeedbackRangeMemory &memory) const;

	static double rangeDurationSec(const ClipDuration &range);
	static double rangeCenterSec(const ClipDuration &range);
	static double overlapSec(const ClipDuration &left, const ClipDuration &right);
	static double rangeSimilarity(const ClipDuration &candidate, const ClipDuration &feedback);
	static bool negativeRangeContaminatesCandidate(const ClipDuration &candidate,
						       const Curation::Feedback::FeedbackRangeSignal &negative);
	static bool positiveRangeExplainsCandidate(const ClipDuration &candidate,
						   const Curation::Feedback::FeedbackRangeSignal &positive);
	static double lexicalSimilarity(const QString &left, const QString &right);

private:
	double bestRangeSimilarity(const ClipDuration &range,
				   const QVector<Curation::Feedback::FeedbackRangeSignal> &results,
				   Curation::Feedback::FeedbackRangeSignal *bestSignal = nullptr) const;
	double bestOverlapSeconds(const ClipDuration &range,
				  const QVector<Curation::Feedback::FeedbackRangeSignal> &results,
				  Curation::Feedback::FeedbackRangeSignal *bestSignal = nullptr) const;
	double bestTextSimilarity(const QString &text, const TranscriptIndex &index,
				  const QVector<Curation::Feedback::FeedbackRangeSignal> &results,
				  Curation::Feedback::FeedbackRangeSignal *bestSignal = nullptr) const;
};

} // namespace Curation::Scoring
