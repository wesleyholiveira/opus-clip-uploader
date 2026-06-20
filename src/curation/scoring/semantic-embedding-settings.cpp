#include "curation/scoring/semantic-embedding-settings.hpp"

#include "utils/config.hpp"

#include <algorithm>

using namespace Curation::Scoring;

namespace {

static int configInt(const QString &key, int defaultValue, int minValue, int maxValue)
{
	bool ok = false;
	const int value = PluginConfig::getValue(key, QString::number(defaultValue)).toInt(&ok);
	if (!ok)
		return defaultValue;
	return std::clamp(value, minValue, maxValue);
}

} // namespace

QString Curation::Scoring::localEmbeddingBackendFromConfig()
{
	QString backend = PluginConfig::getValue(QString::fromLatin1(CONFIG_LOCAL_EMBEDDING_BACKEND),
						  QString::fromLatin1(LOCAL_EMBEDDING_BACKEND_DISABLED))
			  .trimmed();
	if (backend.isEmpty())
		backend = QString::fromLatin1(LOCAL_EMBEDDING_BACKEND_DISABLED);
	return backend;
}


QString Curation::Scoring::localRerankerBackendFromConfig()
{
	QString backend = PluginConfig::getValue(QString::fromLatin1(CONFIG_LOCAL_RERANKER_BACKEND),
						  QString::fromLatin1(LOCAL_RERANKER_BACKEND_DISABLED))
			  .trimmed();
	if (backend.isEmpty())
		backend = QString::fromLatin1(LOCAL_RERANKER_BACKEND_DISABLED);
	return backend;
}

LlamaServerEmbeddingProviderOptions Curation::Scoring::llamaServerEmbeddingOptionsFromConfig()
{
	LlamaServerEmbeddingProviderOptions options;
	const QString backend = localEmbeddingBackendFromConfig();
	options.enabled = backend == QString::fromLatin1(LOCAL_EMBEDDING_BACKEND_LLAMA_SERVER);
	options.endpoint = PluginConfig::getValue(QString::fromLatin1(CONFIG_LOCAL_EMBEDDING_ENDPOINT), options.endpoint).trimmed();
	options.modelId = PluginConfig::getValue(QString::fromLatin1(CONFIG_LOCAL_EMBEDDING_MODEL_ID), options.modelId).trimmed();
	options.timeoutMs = configInt(QString::fromLatin1(CONFIG_LOCAL_EMBEDDING_TIMEOUT_MS), options.timeoutMs, 1000, 60000);
	options.maxTextChars = configInt(QString::fromLatin1(CONFIG_LOCAL_EMBEDDING_MAX_TEXT_CHARS), options.maxTextChars,
					   500, 24000);
	return options;
}

LlamaServerRerankerProviderOptions Curation::Scoring::llamaServerRerankerOptionsFromConfig()
{
	LlamaServerRerankerProviderOptions options;
	const QString backend = localRerankerBackendFromConfig();
	options.enabled = backend == QString::fromLatin1(LOCAL_RERANKER_BACKEND_LLAMA_SERVER);
	options.endpoint = PluginConfig::getValue(QString::fromLatin1(CONFIG_LOCAL_RERANKER_ENDPOINT), options.endpoint).trimmed();
	options.modelId = PluginConfig::getValue(QString::fromLatin1(CONFIG_LOCAL_RERANKER_MODEL_ID), options.modelId).trimmed();
	options.timeoutMs = configInt(QString::fromLatin1(CONFIG_LOCAL_RERANKER_TIMEOUT_MS), options.timeoutMs, 1000, 120000);
	options.maxTextChars = configInt(QString::fromLatin1(CONFIG_LOCAL_RERANKER_MAX_TEXT_CHARS), options.maxTextChars,
					   500, 24000);
	return options;
}
