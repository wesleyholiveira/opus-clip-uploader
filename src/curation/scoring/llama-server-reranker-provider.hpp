#pragma once

#include "curation/scoring/semantic-reranker.hpp"

#include <QUrl>

#include <functional>

namespace Curation::Scoring {

struct LlamaServerRerankerProviderOptions {
	QString endpoint = QStringLiteral("http://127.0.0.1:8081/v1/rerank");
	QString modelId = QStringLiteral("qwen3-reranker-0.6b-q8_0");
	int timeoutMs = 20000;
	int maxTextChars = 900;
	bool enabled = false;
	std::function<bool()> cancellationCallback = {};
};

class LlamaServerRerankerProvider final : public SemanticReranker {
public:
	explicit LlamaServerRerankerProvider(LlamaServerRerankerProviderOptions options = {});

	bool isAvailable() const override;
	QString modelId() const override;
	double score(const QString &query, const QString &candidateText) const override;
	QVector<double> scoreBatch(const QString &query, const QVector<QString> &candidateTexts) const override;

	QString endpoint() const;
	QString lastError() const override;

private:
	QUrl normalizedEndpoint(const QString &value) const;
	QByteArray buildRequestPayload(const QString &query, const QVector<QString> &candidateTexts) const;
	QVector<double> parseRerankResponse(const QByteArray &payload, qsizetype expectedSize) const;
	QString preparedText(const QString &text) const;
	void markFailure(const QString &message) const;

	LlamaServerRerankerProviderOptions options_;
	mutable bool failed_ = false;
	mutable QString lastError_;
};

} // namespace Curation::Scoring
