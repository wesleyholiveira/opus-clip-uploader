#pragma once

#include "curation/scoring/semantic-model.hpp"
#include "curation/scoring/semantic-reranker.hpp"

namespace Curation::Scoring {

class EmbeddingSemanticReranker final : public SemanticReranker {
public:
	explicit EmbeddingSemanticReranker(const SemanticEmbeddingProvider *provider);

	bool isAvailable() const override;
	QString modelId() const override;
	double score(const QString &query, const QString &candidateText) const override;

private:
	const SemanticEmbeddingProvider *provider_ = nullptr;
};

} // namespace Curation::Scoring
