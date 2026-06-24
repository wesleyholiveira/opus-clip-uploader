#pragma once

#include "curation/scoring/embedding-cache.hpp"
#include "curation/scoring/semantic-model.hpp"

#include <QMutex>
#include <QString>

#include <functional>
#include <memory>

namespace Curation::Scoring {

struct LlamaCppEmbeddingProviderOptions {
	QString modelPathOrId = QStringLiteral("qwen3-embedding-0.6b-q8_0");
	int maxTextChars = 6000;
	int maxBatchSize = 8;
	int contextSize = 2048;
	int batchSize = 512;
	int threads = 0;
	int batchThreads = 0;
	int gpuLayers = -1;
	bool enabled = false;
	std::function<bool()> cancellationCallback = {};
};

class LlamaCppEmbeddingProvider final : public SemanticEmbeddingProvider {
public:
	explicit LlamaCppEmbeddingProvider(LlamaCppEmbeddingProviderOptions options = {});
	~LlamaCppEmbeddingProvider() override;

	bool isAvailable() const override;
	QString modelId() const override;
	SemanticEmbedding embed(const QString &text) const override;
	QVector<SemanticEmbedding> embedBatch(const QVector<QString> &texts) const override;
	QString lastError() const;
	QString resolvedModelPath() const;

private:
	class Engine;
	bool ensureLoaded() const;
	QString preparedText(const QString &text) const;
	void setLastError(const QString &message) const;

	LlamaCppEmbeddingProviderOptions options_;
	mutable EmbeddingCache cache_;
	mutable QMutex mutex_;
	mutable std::unique_ptr<Engine> engine_;
	mutable bool loadAttempted_ = false;
	mutable bool available_ = false;
	mutable QString resolvedModelPath_;
	mutable QString lastError_;
};

} // namespace Curation::Scoring
