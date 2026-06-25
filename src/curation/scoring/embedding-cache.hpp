#pragma once

#include "curation/scoring/semantic-model.hpp"

#include <QHash>
#include <QMutex>
#include <QSet>
#include <QString>

namespace Curation::Scoring {

class EmbeddingCache {
public:
	bool tryGet(const QString &modelId, const QString &text, SemanticEmbedding *embedding) const;
	void put(const QString &modelId, const QString &text, const SemanticEmbedding &embedding);
	void clear();

private:
	QString keyFor(const QString &modelId, const QString &text) const;
	void ensureLoadedLocked() const;
	void appendLocked(const QString &key, const SemanticEmbedding &embedding) const;
	QString cacheDirectoryPath() const;
	QString cacheFilePath() const;

	mutable QMutex mutex_;
	mutable bool loaded_ = false;
	mutable QHash<QString, SemanticEmbedding> values_;
	mutable QSet<QString> persistedKeys_;
};

} // namespace Curation::Scoring
