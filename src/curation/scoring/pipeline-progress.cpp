#include "curation/scoring/pipeline-progress.hpp"

#include <algorithm>
#include <utility>

namespace Curation::Scoring::PipelineProgress {

void report(const ClipScoringPipelineOptions &options, const QString &message, int value, int maximum)
{
	if (!options.progressCallback)
		return;
	ClipScoringPipelineProgressUpdate update;
	update.message = message;
	update.value = std::clamp(value, 0, std::max(0, maximum));
	update.maximum = std::max(0, maximum);
	options.progressCallback(std::move(update));
}

bool isCanceled(const ClipScoringPipelineOptions &options)
{
	return options.cancellationCallback && options.cancellationCallback();
}

bool stopIfCanceled(const ClipScoringPipelineOptions &options, ClipScoringResult &result, const QString &summary)
{
	if (!isCanceled(options))
		return false;
	report(options, QStringLiteral("Clip suggestion analysis canceled."), 100);
	result.summary = summary;
	return true;
}

QString candidateMessage(const QString &phase, const ClipCandidate &candidate, int index, int total)
{
	return QStringLiteral("%1 %2/%3 (%4-%5s)")
		.arg(phase)
		.arg(index)
		.arg(total)
		.arg(QString::number(candidate.range.startSec, 'f', 2))
		.arg(QString::number(candidate.range.endSec, 'f', 2));
}

} // namespace Curation::Scoring::PipelineProgress
