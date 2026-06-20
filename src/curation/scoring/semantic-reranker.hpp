#pragma once

#include "curation/scoring/clip-candidate.hpp"

#include <QString>
#include <QVector>

namespace Curation::Scoring {

struct SemanticRerankerOptions {
	bool enabled = true;
	double contributionWeight = 0.18;
};

struct SemanticRerankerContext {
	QString presetId;
	QString mainTarget;
	bool reliableMainTarget = false;
};

class SemanticReranker {
public:
	virtual ~SemanticReranker() = default;

	virtual bool isAvailable() const = 0;
	virtual QString modelId() const = 0;
	virtual double score(const QString &query, const QString &candidateText) const = 0;
	virtual QVector<double> scoreBatch(const QString &query, const QVector<QString> &candidateTexts) const;
	virtual QString lastError() const { return {}; }
};

class SemanticRerankerStage {
public:
	QVector<ClipCandidate> apply(QVector<ClipCandidate> candidates, const SemanticRerankerContext &context,
				       const SemanticRerankerOptions &options, const SemanticReranker *reranker) const;

private:
	QString queryForContext(const SemanticRerankerContext &context) const;
	QString badClipQueryForContext(const SemanticRerankerContext &context) const;
	QString documentForCandidate(const ClipCandidate &candidate) const;
	double rerankerScoreFromRaw(double rawScore, double normalizedScore, double badClipScore) const;
	double combineFinalScore(const ClipCandidate &candidate, double rerankerScore, double contributionWeight) const;
};

} // namespace Curation::Scoring
