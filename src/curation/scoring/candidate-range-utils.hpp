#pragma once

#include "curation/scoring/clip-candidate.hpp"
#include "curation/scoring/candidate-generator.hpp"
#include "curation/scoring/transcript-index.hpp"

#include <QString>
#include <QVector>

namespace Curation::Scoring::CandidateRangeUtils {

QString rangeKey(const ClipDuration &range);
double rangeCenterSec(const ClipDuration &range);
double overlapSec(const ClipDuration &left, const ClipDuration &right);
bool substantiallyOverlapsFocus(const ClipDuration &candidate, const ClipDuration &focus);
void appendUniqueStart(QVector<double> &starts, double startSec);
void appendUniqueValue(QVector<double> &values, double value, double tolerance = 0.25);
void appendUniqueRange(QVector<ClipDuration> &ranges, const ClipDuration &range, double toleranceSec = 0.75);
bool rangeIsWithinDurationLimits(const ClipDuration &range, const CandidateGenerationOptions &options);
ClipDuration normalizedVariantRange(const TranscriptIndex &index,
	const CandidateGenerationOptions &options,
	const ClipDuration &candidateRange,
	const ClipDuration &localSearchRange);
bool hasSimilarRange(const QVector<ClipCandidate> &candidates, const ClipDuration &range, double toleranceSec = 1.25);

} // namespace Curation::Scoring::CandidateRangeUtils
