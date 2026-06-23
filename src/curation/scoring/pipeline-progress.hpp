#pragma once

#include "curation/scoring/clip-scoring-pipeline.hpp"

namespace Curation::Scoring::PipelineProgress {

void report(const ClipScoringPipelineOptions &options, const QString &message, int value, int maximum = 100);
bool isCanceled(const ClipScoringPipelineOptions &options);
bool stopIfCanceled(const ClipScoringPipelineOptions &options, ClipScoringResult &result, const QString &summary);
QString candidateMessage(const QString &phase, const ClipCandidate &candidate, int index, int total);

} // namespace Curation::Scoring::PipelineProgress
