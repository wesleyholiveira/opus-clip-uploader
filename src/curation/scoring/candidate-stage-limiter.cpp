#include "curation/scoring/candidate-stage-limiter.hpp"

#include <algorithm>
#include <cmath>

namespace Curation::Scoring::CandidateStageLimiter {

namespace {

double boundedScore(double value)
{
	return std::clamp(value, 0.0, 1.0);
}

static bool hasEvidence(const ClipCandidate &candidate, const QString &needle)
{
	for (const QString &evidence : candidate.evidence) {
		if (evidence == needle || evidence.contains(needle))
			return true;
	}
	return false;
}

double expensiveStagePriority(const ClipCandidate &candidate)
{
	if (candidate.source.contains(QStringLiteral("feedback_positive_exact_seed")) ||
	    hasEvidence(candidate, QStringLiteral("feedback_positive_exact_seed_preserved")) ||
	    hasEvidence(candidate, QStringLiteral("complete_viewer_arc_gate_passed_by_user_feedback")))
		return 1.0;
	if (hasEvidence(candidate, QStringLiteral("complete_viewer_arc_gate_passed_by_feedback_trained_ranker")) ||
	    hasEvidence(candidate, QStringLiteral("feedback_trained_ranker_strong_accept")) ||
		hasEvidence(candidate, QStringLiteral("feedback_trained_ranker_accept")))
		return std::max(0.94, boundedScore(candidate.scores.final + 0.12));
	if (candidate.source.contains(QStringLiteral("feedback_positive")) ||
	    hasEvidence(candidate, QStringLiteral("feedback_positive_guided")) ||
	    hasEvidence(candidate, QStringLiteral("feedback_positive_boundary_variant")))
		return std::max(0.88, boundedScore(candidate.scores.final + 0.18));

	const double semanticPositive = std::max({candidate.scores.semanticTarget, candidate.scores.semanticClipValue,
		candidate.scores.semanticViewerMessage, candidate.scores.semanticDirectAnswer, candidate.scores.semanticEmpathy,
		candidate.scores.semanticResolution, candidate.scores.semanticOpeningHook, candidate.scores.semanticEndingResolution});
	const double rerankerSignal = candidate.rerankerAttempted ? candidate.scores.rerankerRaw : candidate.scores.reranker;
	return boundedScore((candidate.scores.final * 0.44) + (semanticPositive * 0.32) +
		(candidate.scores.coarseSemantic * 0.14) + (rerankerSignal * 0.10));
}

} // namespace

int rerankerLimit(const ClipScoringPipelineOptions &options)
{
	return std::clamp(options.budget.maxCandidatesBeforeEmbedding * 2, options.ranking.maxCandidates * 3,
		std::max(options.ranking.maxCandidates * 4, 48));
}

int boundaryDpLimit(const ClipScoringPipelineOptions &options)
{
	return std::clamp(options.budget.maxCandidatesBeforeEmbedding * 2, options.ranking.maxCandidates * 3,
		std::max(options.ranking.maxCandidates * 5, 60));
}

QVector<ClipCandidate> limit(QVector<ClipCandidate> candidates, int maxCandidates, const QString &evidence)
{
	if (maxCandidates <= 0 || candidates.size() <= maxCandidates)
		return candidates;
	std::stable_sort(candidates.begin(), candidates.end(), [](const ClipCandidate &left, const ClipCandidate &right) {
		const double leftScore = expensiveStagePriority(left);
		const double rightScore = expensiveStagePriority(right);
		if (std::fabs(leftScore - rightScore) > 0.0001)
			return leftScore > rightScore;
		return left.range.startSec < right.range.startSec;
	});
	candidates.resize(maxCandidates);
	for (ClipCandidate &candidate : candidates) {
		candidate.evidence.append(evidence);
		candidate.evidence.append(QStringLiteral("expensive_stage_priority:%1").arg(QString::number(expensiveStagePriority(candidate), 'f', 2)));
	}
	return candidates;
}

} // namespace Curation::Scoring::CandidateStageLimiter
