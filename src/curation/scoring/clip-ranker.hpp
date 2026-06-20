#pragma once

#include "curation/scoring/clip-candidate.hpp"

#include <QVector>

namespace Curation::Scoring {

struct ClipRankerOptions {
	int maxCandidates = 5;
	double minFinalScore = 0.42;
	double overlapToleranceSec = 8.0;
	double minSpacingSec = 0.0;
	bool useMmr = true;
	double mmrRelevanceWeight = 0.72;
	double mmrTextSimilarityWeight = 0.35;
	double mmrTemporalSimilarityWeight = 0.65;
};

class ClipRanker {
public:
	QVector<ClipCandidate> rank(QVector<ClipCandidate> candidates, const ClipRankerOptions &options) const;

private:
	QVector<ClipCandidate> rankGreedy(QVector<ClipCandidate> candidates, const ClipRankerOptions &options) const;
	QVector<ClipCandidate> rankWithMmr(QVector<ClipCandidate> candidates, const ClipRankerOptions &options) const;
	bool rangesOverlapTooMuch(const ClipDuration &left, const ClipDuration &right,
				   double overlapToleranceSec) const;
	bool rangesAreTooClose(const ClipDuration &left, const ClipDuration &right, double minSpacingSec) const;
	bool candidateConflictsWithSelected(const ClipCandidate &candidate, const QVector<ClipCandidate> &selected,
					  const ClipRankerOptions &options) const;
	double relevanceScore(const ClipCandidate &candidate) const;
	double mmrScore(const ClipCandidate &candidate, const QVector<ClipCandidate> &selected,
			 const ClipRankerOptions &options, double *maxSimilarity) const;
	double candidateSimilarity(const ClipCandidate &left, const ClipCandidate &right,
				   const ClipRankerOptions &options) const;
	double temporalSimilarity(const ClipDuration &left, const ClipDuration &right,
				  const ClipRankerOptions &options) const;
	double textSimilarity(const QString &left, const QString &right) const;
};

} // namespace Curation::Scoring
