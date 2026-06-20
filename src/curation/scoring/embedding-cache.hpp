#pragma once

#include "curation/scoring/semantic-model.hpp"

#include <QHash>
#include <QMutex>
#include <QString>

namespace Curation::Scoring {

class EmbeddingCache {
public:
	bool tryGet(const QString &modelId, const QString &text, SemanticEmbedding *embedding) const;
	void put(const QString &modelId, const QString &text, const SemanticEmbedding &embedding);
	void clear();

private:
	QString keyFor(const QString &modelId, const QString &text) const;

	mutable QMutex mutex_;
	QHash<QString, SemanticEmbedding> values_;
};

} // namespace Curation::Scoring
