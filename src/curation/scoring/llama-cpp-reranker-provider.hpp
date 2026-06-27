#pragma once

#include "curation/scoring/semantic-reranker.hpp"

#include <QMutex>
#include <QString>

#include <functional>
#include <memory>

namespace Curation::Scoring {

struct LlamaCppRerankerProviderOptions {
	QString modelPathOrId = QStringLiteral("qwen3-reranker-0.6b-q8_0");
	int maxTextChars = 3200;
	int maxBatchSize = 4;
	int contextSize = 4096;
	int batchSize = 512;
	int threads = 0;
	int batchThreads = 0;
	int gpuLayers = -1;
	bool enabled = false;
	std::function<bool()> cancellationCallback = {};
};

class LlamaCppRerankerProvider final : public SemanticReranker {
public:
	explicit LlamaCppRerankerProvider(LlamaCppRerankerProviderOptions options = {});
	~LlamaCppRerankerProvider() override;

	bool isAvailable() const override;
	QString modelId() const override;
	double score(const QString &query, const QString &candidateText) const override;
	QVector<double> scoreBatch(const QString &query, const QVector<QString> &candidateTexts) const override;
	QString lastError() const override;
	QString resolvedModelPath() const;

private:
	class Engine;
	bool ensureLoaded() const;
	QString preparedPairText(const QString &query, const QString &candidateText) const;
	static double normalizeRankScore(double rawScore);

	LlamaCppRerankerProviderOptions options_;
	mutable QMutex mutex_;
	mutable std::unique_ptr<Engine> engine_;
	mutable bool loadAttempted_ = false;
	mutable bool available_ = false;
	mutable QString resolvedModelPath_;
	mutable QString lastError_;
};

} // namespace Curation::Scoring
