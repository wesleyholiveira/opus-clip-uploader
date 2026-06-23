#pragma once

#include "curation/scoring/clip-candidate.hpp"
#include "curation/scoring/clip-scoring-pipeline.hpp"

#include <QString>
#include <QVector>

namespace Curation::Scoring::CandidateStageLimiter {

int rerankerLimit(const ClipScoringPipelineOptions &options);
int boundaryDpLimit(const ClipScoringPipelineOptions &options);
QVector<ClipCandidate> limit(QVector<ClipCandidate> candidates, int maxCandidates, const QString &evidence);

} // namespace Curation::Scoring::CandidateStageLimiter
