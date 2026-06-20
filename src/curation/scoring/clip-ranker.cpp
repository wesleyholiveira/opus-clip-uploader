#include "curation/scoring/clip-ranker.hpp"

#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cmath>

using namespace Curation::Scoring;

namespace {

static double boundedScore(double value)
{
	return std::clamp(value, 0.0, 1.0);
}

static bool candidateComesBefore(const ClipCandidate &left, const ClipCandidate &right)
{
	if (std::fabs(left.scores.final - right.scores.final) > 0.0001)
		return left.scores.final > right.scores.final;
	if (std::fabs(left.scores.reranker - right.scores.reranker) > 0.0001)
		return left.scores.reranker > right.scores.reranker;
	if (std::fabs(left.scores.boundary - right.scores.boundary) > 0.0001)
		return left.scores.boundary > right.scores.boundary;
	return left.range.startSec < right.range.startSec;
}

static void removeUnusableCandidates(QVector<ClipCandidate> &candidates, const ClipRankerOptions &options)
{
	candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&options](const ClipCandidate &candidate) {
		return candidate.range.endSec <= candidate.range.startSec || candidate.text.trimmed().isEmpty() ||
		       candidate.rejectedAsNoise || candidate.rejectedByQualityGate ||
		       candidate.scores.final < options.minFinalScore;
	}), candidates.end());
}

static QSet<QString> tokenSetForText(const QString &text)
{
	static const QRegularExpression splitExpression(QStringLiteral("[^\\p{L}\\p{N}_]+"));
	QSet<QString> tokens;
	const QStringList parts = text.toCaseFolded().split(splitExpression, Qt::SkipEmptyParts);
	for (const QString &part : parts) {
		const QString token = part.trimmed();
		if (token.size() >= 3)
			tokens.insert(token.left(48));
	}
	return tokens;
}

} // namespace

QVector<ClipCandidate> ClipRanker::rank(QVector<ClipCandidate> candidates, const ClipRankerOptions &options) const
{
	removeUnusableCandidates(candidates, options);
	if (candidates.isEmpty() || options.maxCandidates <= 0)
		return {};

	std::sort(candidates.begin(), candidates.end(), candidateComesBefore);
	if (!options.useMmr)
		return rankGreedy(std::move(candidates), options);

	return rankWithMmr(std::move(candidates), options);
}

QVector<ClipCandidate> ClipRanker::rankGreedy(QVector<ClipCandidate> candidates,
	const ClipRankerOptions &options) const
{
	QVector<ClipCandidate> selected;
	selected.reserve(std::min(static_cast<long long>(options.maxCandidates), static_cast<long long>(candidates.size())));
	for (const ClipCandidate &candidate : candidates) {
		if (candidateConflictsWithSelected(candidate, selected, options))
			continue;

		ClipCandidate selectedCandidate = candidate;
		selectedCandidate.selectedRank = static_cast<int>(selected.size()) + 1;
		selectedCandidate.evidence.append(QStringLiteral("selected_rank:%1").arg(selectedCandidate.selectedRank));
		selectedCandidate.evidence.removeDuplicates();
		selected.append(selectedCandidate);
		if (static_cast<int>(selected.size()) >= options.maxCandidates)
			break;
	}


	return selected;
}

QVector<ClipCandidate> ClipRanker::rankWithMmr(QVector<ClipCandidate> candidates,
	const ClipRankerOptions &options) const
{
	QVector<ClipCandidate> selected;
	selected.reserve(std::min(static_cast<long long>(options.maxCandidates), static_cast<long long>(candidates.size())));

	while (!candidates.isEmpty() && static_cast<int>(selected.size()) < options.maxCandidates) {
		int bestIndex = -1;
		double bestMmrScore = -2.0;
		double bestSimilarity = 0.0;
		for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
			const ClipCandidate &candidate = candidates.at(i);
			if (candidateConflictsWithSelected(candidate, selected, options))
				continue;

			double similarity = 0.0;
			const double score = mmrScore(candidate, selected, options, &similarity);
			if (bestIndex < 0 || score > bestMmrScore + 0.0001 ||
			    (std::fabs(score - bestMmrScore) <= 0.0001 &&
			     candidateComesBefore(candidate, candidates.at(bestIndex)))) {
				bestIndex = i;
				bestMmrScore = score;
				bestSimilarity = similarity;
			}
		}

		if (bestIndex < 0)
			break;

		ClipCandidate selectedCandidate = candidates.takeAt(bestIndex);
		selectedCandidate.selectedRank = static_cast<int>(selected.size()) + 1;
		selectedCandidate.selectedMmrScore = bestMmrScore;
		selectedCandidate.selectedMmrSimilarity = bestSimilarity;
		selectedCandidate.evidence.append(QStringLiteral("mmr_diversity_selected"));
		selectedCandidate.evidence.append(QStringLiteral("selected_rank:%1").arg(selectedCandidate.selectedRank));
		selectedCandidate.evidence.append(QStringLiteral("mmr:%1").arg(QString::number(bestMmrScore, 'f', 2)));
		if (!selected.isEmpty())
			selectedCandidate.evidence.append(QStringLiteral("mmr_similarity:%1")
							.arg(QString::number(bestSimilarity, 'f', 2)));
		selectedCandidate.evidence.removeDuplicates();
		selected.append(selectedCandidate);
	}

	std::sort(selected.begin(), selected.end(), [](const ClipCandidate &left, const ClipCandidate &right) {
		return left.range.startSec < right.range.startSec;
	});
	return selected;
}

bool ClipRanker::rangesOverlapTooMuch(const ClipDuration &left, const ClipDuration &right,
					      double overlapToleranceSec) const
{
	const double overlap = std::min(left.endSec, right.endSec) - std::max(left.startSec, right.startSec);
	if (overlap <= overlapToleranceSec)
		return false;

	const double leftDuration = std::max(0.0, left.endSec - left.startSec);
	const double rightDuration = std::max(0.0, right.endSec - right.startSec);
	const double shorter = std::min(leftDuration, rightDuration);
	return shorter <= 0.0 || overlap >= shorter * 0.45;
}

bool ClipRanker::rangesAreTooClose(const ClipDuration &left, const ClipDuration &right, double minSpacingSec) const
{
	if (minSpacingSec <= 0.0)
		return false;

	const double leftCenter = left.startSec + ((left.endSec - left.startSec) * 0.5);
	const double rightCenter = right.startSec + ((right.endSec - right.startSec) * 0.5);
	return std::fabs(leftCenter - rightCenter) < minSpacingSec;
}

bool ClipRanker::candidateConflictsWithSelected(const ClipCandidate &candidate,
	const QVector<ClipCandidate> &selected, const ClipRankerOptions &options) const
{
	for (const ClipCandidate &selectedCandidate : selected) {
		if (rangesOverlapTooMuch(candidate.range, selectedCandidate.range, options.overlapToleranceSec) ||
		    rangesAreTooClose(candidate.range, selectedCandidate.range, options.minSpacingSec))
			return true;
	}
	return false;
}

double ClipRanker::relevanceScore(const ClipCandidate &candidate) const
{
	return boundedScore(candidate.scores.final);
}

double ClipRanker::mmrScore(const ClipCandidate &candidate, const QVector<ClipCandidate> &selected,
	const ClipRankerOptions &options, double *maxSimilarity) const
{
	const double relevanceWeight = std::clamp(options.mmrRelevanceWeight, 0.05, 0.95);
	const double relevance = relevanceScore(candidate);
	double similarity = 0.0;
	for (const ClipCandidate &selectedCandidate : selected)
		similarity = std::max(similarity, candidateSimilarity(candidate, selectedCandidate, options));

	if (maxSimilarity)
		*maxSimilarity = similarity;
	return (relevanceWeight * relevance) - ((1.0 - relevanceWeight) * similarity);
}

double ClipRanker::candidateSimilarity(const ClipCandidate &left, const ClipCandidate &right,
	const ClipRankerOptions &options) const
{
	const double temporalWeight = std::clamp(options.mmrTemporalSimilarityWeight, 0.0, 1.0);
	const double textWeight = std::clamp(options.mmrTextSimilarityWeight, 0.0, 1.0);
	const double weightTotal = std::max(0.000001, temporalWeight + textWeight);
	const double temporal = temporalSimilarity(left.range, right.range, options);
	const double text = textSimilarity(left.text, right.text);
	return boundedScore(((temporal * temporalWeight) + (text * textWeight)) / weightTotal);
}

double ClipRanker::temporalSimilarity(const ClipDuration &left, const ClipDuration &right,
	const ClipRankerOptions &options) const
{
	const double leftDuration = std::max(0.0, left.endSec - left.startSec);
	const double rightDuration = std::max(0.0, right.endSec - right.startSec);
	const double shorter = std::min(leftDuration, rightDuration);
	const double overlap = std::max(0.0, std::min(left.endSec, right.endSec) - std::max(left.startSec, right.startSec));
	double overlapSimilarity = 0.0;
	if (shorter > 0.0)
		overlapSimilarity = boundedScore(overlap / shorter);

	const double leftCenter = left.startSec + (leftDuration * 0.5);
	const double rightCenter = right.startSec + (rightDuration * 0.5);
	const double centerDistance = std::fabs(leftCenter - rightCenter);
	const double spacingReference = options.minSpacingSec > 0.0 ? options.minSpacingSec :
		std::max(30.0, (leftDuration + rightDuration) * 0.75);
	const double proximitySimilarity = boundedScore(1.0 - (centerDistance / spacingReference));
	return std::max(overlapSimilarity, proximitySimilarity * 0.65);
}

double ClipRanker::textSimilarity(const QString &left, const QString &right) const
{
	const QSet<QString> leftTokens = tokenSetForText(left);
	const QSet<QString> rightTokens = tokenSetForText(right);
	if (leftTokens.isEmpty() || rightTokens.isEmpty())
		return 0.0;

	int intersection = 0;
	for (const QString &token : leftTokens) {
		if (rightTokens.contains(token))
			++intersection;
	}
	const qsizetype unionSize = leftTokens.size() + rightTokens.size() - static_cast<qsizetype>(intersection);
	if (unionSize <= 0)
		return 0.0;
	return boundedScore(static_cast<double>(intersection) / static_cast<double>(unionSize));
}
