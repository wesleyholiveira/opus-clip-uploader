#include "curation/scoring/embedding-semantic-reranker.hpp"

using namespace Curation::Scoring;

EmbeddingSemanticReranker::EmbeddingSemanticReranker(const SemanticEmbeddingProvider *provider) : provider_(provider) {}

bool EmbeddingSemanticReranker::isAvailable() const
{
	return provider_ && provider_->isAvailable();
}

QString EmbeddingSemanticReranker::modelId() const
{
	return provider_ ? provider_->modelId() : QStringLiteral("disabled");
}

double EmbeddingSemanticReranker::score(const QString &query, const QString &candidateText) const
{
	if (!isAvailable())
		return 0.0;

	return cosineSimilarity(provider_->embed(query), provider_->embed(candidateText));
}
