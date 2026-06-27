#pragma once

#include <QString>
#include <QVector>

namespace Curation::Scoring {

struct SemanticEmbedding {
	QVector<float> values;

	bool isValid() const { return !values.isEmpty(); }
};

class SemanticEmbeddingProvider {
public:
	virtual ~SemanticEmbeddingProvider() = default;

	virtual bool isAvailable() const = 0;
	virtual QString modelId() const = 0;
	virtual SemanticEmbedding embed(const QString &text) const = 0;
	virtual QVector<SemanticEmbedding> embedBatch(const QVector<QString> &texts) const;
};

double cosineSimilarity(const SemanticEmbedding &left, const SemanticEmbedding &right);

} // namespace Curation::Scoring
