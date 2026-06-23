#pragma once

#include "curation/scoring/embedding-cache.hpp"
#include "curation/scoring/semantic-model.hpp"

#include <QMutex>
#include <QUrl>

#include <functional>

namespace Curation::Scoring {

struct LlamaServerEmbeddingProviderOptions {
	QString endpoint = QStringLiteral("http://127.0.0.1:8080/v1/embeddings");
	QString modelId = QStringLiteral("qwen3-embedding-0.6b-q8_0");
	int timeoutMs = 10000;
	int maxTextChars = 6000;
	int maxBatchSize = 64;
	bool enabled = false;
	std::function<bool()> cancellationCallback = {};
};

class LlamaServerEmbeddingProvider final : public SemanticEmbeddingProvider {
public:
	explicit LlamaServerEmbeddingProvider(LlamaServerEmbeddingProviderOptions options = {});

	bool isAvailable() const override;
	QString modelId() const override;
	SemanticEmbedding embed(const QString &text) const override;
	QVector<SemanticEmbedding> embedBatch(const QVector<QString> &texts) const override;

	QString endpoint() const;
	QString lastError() const;

private:
	QUrl normalizedEndpoint(const QString &value) const;
	QByteArray buildRequestPayload(const QString &text) const;
	QByteArray buildBatchRequestPayload(const QVector<QString> &texts) const;
	SemanticEmbedding parseEmbeddingResponse(const QByteArray &payload) const;
	QVector<SemanticEmbedding> parseEmbeddingBatchResponse(const QByteArray &payload, qsizetype expectedSize) const;
	QVector<SemanticEmbedding> postEmbeddingBatch(const QVector<QString> &preparedTexts) const;
	QString preparedText(const QString &text) const;
	void markFailure(const QString &message, bool fatal = false) const;
	void markSuccess() const;
	void setLastError(const QString &message) const;

	LlamaServerEmbeddingProviderOptions options_;
	mutable EmbeddingCache cache_;
	mutable QMutex stateMutex_;
	mutable bool failed_ = false;
	mutable int consecutiveFailures_ = 0;
	mutable QString lastError_;
};

} // namespace Curation::Scoring
