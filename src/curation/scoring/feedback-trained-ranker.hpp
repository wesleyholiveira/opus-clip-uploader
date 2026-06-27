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

inline constexpr const char *CONFIG_FEEDBACK_RANKER_MODEL_PATH = "feedback_ranker_model_path";

struct FeedbackTrainedRankerTreeNode {
	QString feature;
	double threshold = 0.0;
	int left = -1;
	int right = -1;
	double value = 0.0;
	bool leaf = false;
};

struct FeedbackTrainedRankerTree {
	double weight = 1.0;
	QVector<FeedbackTrainedRankerTreeNode> nodes;
};

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
	double baseScore = 0.0;
	double rejectBelow = 0.0;
	double acceptAbove = 0.52;
	double strongAcceptAbove = 1.0;
	QStringList featureOrder;
	QHash<QString, double> weights;
	QVector<FeedbackTrainedRankerTree> trees;
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
	static double logisticScore(const ClipCandidate &candidate, const FeedbackSimilarityFeatures &feedbackFeatures,
				    const FeedbackTrainedRankerModel &model);
	static double gbdtScore(const ClipCandidate &candidate, const FeedbackSimilarityFeatures &feedbackFeatures,
				 const FeedbackTrainedRankerModel &model);
	static double featureValue(const QString &name, const ClipCandidate &candidate,
				   const FeedbackSimilarityFeatures &feedbackFeatures);
	static bool hasEvidence(const ClipCandidate &candidate, const QString &needle);
	static bool isPositiveGroundTruthCandidate(const ClipCandidate &candidate);
	static bool isNegativeContaminated(const FeedbackSimilarityFeatures &features);
};

} // namespace Curation::Scoring
