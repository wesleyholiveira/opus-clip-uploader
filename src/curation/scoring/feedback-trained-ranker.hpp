#pragma once

#include "curation/feedback/curation-feedback-store.hpp"
#include "curation/scoring/clip-candidate.hpp"
#include "curation/scoring/feedback-similarity-scorer.hpp"
#include "curation/scoring/transcript-index.hpp"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace Curation::Scoring {

struct FeedbackTrainedRankerModel {
	bool loaded = false;
	QString path;
	QString modelType;
	QString presetId;
	QString trainedAtUtc;
	int records = 0;
	int positives = 0;
	int negatives = 0;
	double intercept = 0.0;
	double rejectBelow = 0.0;
	double acceptAbove = 0.52;
	double strongAcceptAbove = 1.0;
	QStringList featureOrder;
	QHash<QString, double> weights;
};

class FeedbackTrainedRanker {
public:
	QVector<ClipCandidate> apply(QVector<ClipCandidate> candidates, const TranscriptIndex &index,
				     const Curation::Feedback::FeedbackRangeMemory &memory, const QString &presetId,
				     const QString &videoPath) const;

	static QString defaultModelPath();
	static FeedbackTrainedRankerModel loadDefaultModel();

private:
	static FeedbackTrainedRankerModel loadModel(const QString &path);
	static double scoreCandidate(const ClipCandidate &candidate, const TranscriptIndex &index,
				     const Curation::Feedback::FeedbackRangeMemory &memory,
				     const FeedbackTrainedRankerModel &model,
				     FeedbackSimilarityFeatures *feedbackFeatures = nullptr);
	static double featureValue(const QString &name, const ClipCandidate &candidate,
				   const FeedbackSimilarityFeatures &feedbackFeatures);
	static bool hasEvidence(const ClipCandidate &candidate, const QString &needle);
	static bool isPositiveGroundTruthCandidate(const ClipCandidate &candidate);
	static bool isNegativeContaminated(const FeedbackSimilarityFeatures &features);
};

} // namespace Curation::Scoring
