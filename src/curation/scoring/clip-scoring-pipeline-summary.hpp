#pragma once

#include "curation/scoring/clip-candidate.hpp"

namespace Curation::Scoring::ClipScoringPipelineSummary {

QString build(const QVector<ClipCandidate> &candidates);
QString rejection(const QVector<ClipCandidate> &candidates);
QString noCandidate(const QString &reason, const QVector<ClipCandidate> &candidates);

} // namespace Curation::Scoring::ClipScoringPipelineSummary
