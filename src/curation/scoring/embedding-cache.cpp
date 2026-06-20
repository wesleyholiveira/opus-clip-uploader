#include "curation/scoring/embedding-cache.hpp"

#include <QCryptographicHash>
#include <QMutexLocker>

using namespace Curation::Scoring;

bool EmbeddingCache::tryGet(const QString &modelId, const QString &text, SemanticEmbedding *embedding) const
{
	if (!embedding)
		return false;

	const QString key = keyFor(modelId, text);
	QMutexLocker locker(&mutex_);
	const auto it = values_.constFind(key);
	if (it == values_.constEnd())
		return false;

	*embedding = it.value();
	return true;
}

void EmbeddingCache::put(const QString &modelId, const QString &text, const SemanticEmbedding &embedding)
{
	if (!embedding.isValid())
		return;

	const QString key = keyFor(modelId, text);
	QMutexLocker locker(&mutex_);
	values_.insert(key, embedding);
}

void EmbeddingCache::clear()
{
	QMutexLocker locker(&mutex_);
	values_.clear();
}

QString EmbeddingCache::keyFor(const QString &modelId, const QString &text) const
{
	QByteArray payload;
	payload.append(modelId.trimmed().toUtf8());
	payload.append('\0');
	payload.append(text.simplified().toUtf8());
	return QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
}
