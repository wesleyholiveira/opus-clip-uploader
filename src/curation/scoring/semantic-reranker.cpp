#include "curation/scoring/semantic-reranker.hpp"

#include <algorithm>

using namespace Curation::Scoring;

namespace {

static double boundedScore(double value)
{
	return std::clamp(value, 0.0, 1.0);
}

} // namespace

QVector<ClipCandidate> SemanticRerankerStage::apply(QVector<ClipCandidate> candidates,
						    const SemanticRerankerContext &context,
						    const SemanticRerankerOptions &options,
						    const SemanticReranker *reranker) const
{
	if (!options.enabled) {
		for (ClipCandidate &candidate : candidates)
			candidate.evidence.append(QStringLiteral("reranker_disabled"));
		return candidates;
	}

	if (!reranker || !reranker->isAvailable()) {
		for (ClipCandidate &candidate : candidates)
			candidate.evidence.append(QStringLiteral("reranker_unavailable"));
		return candidates;
	}

	const QString query = queryForContext(context);
	for (ClipCandidate &candidate : candidates) {
		const double rerankerScore = boundedScore(reranker->score(query, candidate.text));
		candidate.scores.reranker = rerankerScore;
		candidate.rerankerAvailable = true;
		candidate.scores.final = combineFinalScore(candidate, rerankerScore, options.contributionWeight);
		candidate.evidence.append(QStringLiteral("reranker_model:%1").arg(reranker->modelId().left(80)));
		if (rerankerScore >= 0.72)
			candidate.evidence.append(QStringLiteral("reranker_strong_match"));
		candidate.evidence.removeDuplicates();
	}

	return candidates;
}

QString SemanticRerankerStage::queryForContext(const SemanticRerankerContext &context) const
{
	const QString target = context.mainTarget.trimmed();
	if (!target.isEmpty()) {
		if (context.presetId == QStringLiteral("viewer_message_response")) {
			return QStringLiteral("one complete response to a single viewer message specifically about %1")
				.arg(target);
		}
		return QStringLiteral("a strong self-contained clip specifically about %1").arg(target);
	}

	if (context.presetId == QStringLiteral("viewer_message_response"))
		return QStringLiteral("one complete answer to a single viewer question without changing topic");
	if (context.presetId == QStringLiteral("advice_answer"))
		return QStringLiteral("a useful practical advice answer with a clear recommendation");
	if (context.presetId == QStringLiteral("emotional_reaction"))
		return QStringLiteral("a strong emotional reaction with a clear payoff");

	return QStringLiteral("a self-contained high quality clip with a clear beginning and resolution");
}

double SemanticRerankerStage::combineFinalScore(const ClipCandidate &candidate, double rerankerScore,
						       double contributionWeight) const
{
	const double weight = std::clamp(contributionWeight, 0.0, 0.45);
	return boundedScore((candidate.scores.final * (1.0 - weight)) + (rerankerScore * weight));
}
