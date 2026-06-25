#include "curation/scoring/embedding-cache.hpp"

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#include <util/platform.h>
#ifdef __cplusplus
}
#endif

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMutexLocker>

#include <cstring>

using namespace Curation::Scoring;

namespace {

static constexpr int EMBEDDING_CACHE_VERSION = 2;
static constexpr qsizetype MAX_CACHE_BYTES_TO_LOAD = 96 * 1024 * 1024;

static QString obsConfigPath(const char *relativePath)
{
	char *path = obs_module_config_path(relativePath);
	if (!path)
		return {};
	const QString result = QString::fromUtf8(path);
	bfree(path);
	return result;
}

static QByteArray serializeEmbeddingValues(const SemanticEmbedding &embedding)
{
	QByteArray raw;
	if (!embedding.isValid())
		return raw;
	raw.resize(static_cast<qsizetype>(embedding.values.size() * static_cast<int>(sizeof(float))));
	std::memcpy(raw.data(), embedding.values.constData(), static_cast<size_t>(raw.size()));
	return raw.toBase64(QByteArray::Base64Encoding);
}

static SemanticEmbedding deserializeEmbeddingValues(const QJsonObject &object)
{
	SemanticEmbedding embedding;
	const int dim = object.value(QStringLiteral("dim")).toInt();
	if (dim <= 0 || dim > 32768)
		return embedding;
	const QByteArray raw = QByteArray::fromBase64(object.value(QStringLiteral("values")).toString().toLatin1());
	if (raw.size() != static_cast<qsizetype>(dim * static_cast<int>(sizeof(float))))
		return embedding;
	embedding.values.resize(dim);
	std::memcpy(embedding.values.data(), raw.constData(), static_cast<size_t>(raw.size()));
	return embedding;
}

} // namespace

bool EmbeddingCache::tryGet(const QString &modelId, const QString &text, SemanticEmbedding *embedding) const
{
	if (!embedding)
		return false;

	const QString key = keyFor(modelId, text);
	QMutexLocker locker(&mutex_);
	ensureLoadedLocked();
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
	ensureLoadedLocked();
	values_.insert(key, embedding);
	if (!persistedKeys_.contains(key))
		appendLocked(key, embedding);
}

void EmbeddingCache::clear()
{
	QMutexLocker locker(&mutex_);
	values_.clear();
	persistedKeys_.clear();
	loaded_ = true;
	QFile::remove(cacheFilePath());
}

QString EmbeddingCache::keyFor(const QString &modelId, const QString &text) const
{
	QByteArray payload;
	payload.append("embedding-cache-v2");
	payload.append('\0');
	payload.append(modelId.trimmed().toUtf8());
	payload.append('\0');
	payload.append(text.simplified().toUtf8());
	return QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
}

void EmbeddingCache::ensureLoadedLocked() const
{
	if (loaded_)
		return;
	loaded_ = true;

	QFile file(cacheFilePath());
	if (!file.exists() || file.size() <= 0 || file.size() > MAX_CACHE_BYTES_TO_LOAD || !file.open(QIODevice::ReadOnly))
		return;

	while (!file.atEnd()) {
		const QByteArray line = file.readLine().trimmed();
		if (line.isEmpty())
			continue;
		QJsonParseError error;
		const QJsonDocument document = QJsonDocument::fromJson(line, &error);
		if (error.error != QJsonParseError::NoError || !document.isObject())
			continue;
		const QJsonObject object = document.object();
		if (object.value(QStringLiteral("version")).toInt() != EMBEDDING_CACHE_VERSION)
			continue;
		const QString key = object.value(QStringLiteral("key")).toString().trimmed();
		if (key.isEmpty())
			continue;
		SemanticEmbedding embedding = deserializeEmbeddingValues(object);
		if (!embedding.isValid())
			continue;
		values_.insert(key, embedding);
		persistedKeys_.insert(key);
	}
}

void EmbeddingCache::appendLocked(const QString &key, const SemanticEmbedding &embedding) const
{
	const QString dirPath = cacheDirectoryPath();
	if (dirPath.trimmed().isEmpty())
		return;
	QDir().mkpath(dirPath);

	QFile file(cacheFilePath());
	if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
		return;

	QJsonObject object;
	object.insert(QStringLiteral("version"), EMBEDDING_CACHE_VERSION);
	object.insert(QStringLiteral("key"), key);
	object.insert(QStringLiteral("dim"), embedding.values.size());
	object.insert(QStringLiteral("values"), QString::fromLatin1(serializeEmbeddingValues(embedding)));
	file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
	file.write("\n");
	persistedKeys_.insert(key);
}

QString EmbeddingCache::cacheDirectoryPath() const
{
	const QString dir = obsConfigPath("semantic-embedding-cache");
	if (!dir.trimmed().isEmpty())
		return dir;
	return QDir::temp().filePath(QStringLiteral("clip-cropper-semantic-embedding-cache"));
}

QString EmbeddingCache::cacheFilePath() const
{
	return QDir(cacheDirectoryPath()).filePath(QStringLiteral("embeddings-v2.jsonl"));
}
