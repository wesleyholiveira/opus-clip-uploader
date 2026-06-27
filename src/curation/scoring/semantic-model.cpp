#include "curation/scoring/semantic-model.hpp"

#include <algorithm>
#include <cmath>

using namespace Curation::Scoring;

namespace {

static double boundedScore(double value)
{
	return std::clamp(value, 0.0, 1.0);
}

} // namespace

QVector<SemanticEmbedding> SemanticEmbeddingProvider::embedBatch(const QVector<QString> &texts) const
{
	QVector<SemanticEmbedding> result;
	result.reserve(static_cast<long long>(texts.size()));
	for (const QString &text : texts)
		result.append(embed(text));
	return result;
}

double Curation::Scoring::cosineSimilarity(const SemanticEmbedding &left, const SemanticEmbedding &right)
{
	if (!left.isValid() || !right.isValid() || left.values.size() != right.values.size())
		return 0.0;

	double dot = 0.0;
	double leftNorm = 0.0;
	double rightNorm = 0.0;
	for (int i = 0; i < static_cast<int>(left.values.size()); ++i) {
		const double leftValue = static_cast<double>(left.values.at(i));
		const double rightValue = static_cast<double>(right.values.at(i));
		dot += leftValue * rightValue;
		leftNorm += leftValue * leftValue;
		rightNorm += rightValue * rightValue;
	}

	if (leftNorm <= 0.0 || rightNorm <= 0.0)
		return 0.0;

	// Map cosine from [-1, 1] to [0, 1], because all existing clip scores use a bounded score scale.
	return boundedScore((dot / (std::sqrt(leftNorm) * std::sqrt(rightNorm)) + 1.0) * 0.5);
}
