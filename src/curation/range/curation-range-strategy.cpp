#include "curation/range/curation-range-strategy.hpp"

#include "curation/range/viewer-message-range-strategy.hpp"

#include <algorithm>

namespace {

QVector<ClipDuration> validResolutionRanges(const CurationRangeStrategyResolution &resolution)
{
	QVector<ClipDuration> ranges;
	ranges.reserve(resolution.ranges.size());

	for (ClipDuration range : resolution.ranges) {
		if (range.endSec <= range.startSec)
			continue;
		range.startSec = std::max(0.0, range.startSec);
		range.endSec = std::max(range.startSec, range.endSec);
		if (range.endSec > range.startSec)
			ranges.append(range);
	}

	return ranges;
}

} // namespace

CurationRangeStrategyResolver::CurationRangeStrategyResolver()
{
	strategies.append(std::make_shared<ViewerMessageResponseRangeStrategy>());
}

CurationRangeStrategyResolution CurationRangeStrategyResolver::resolve(const RecordingTranscript &transcript,
								       const CurationSettings &settings,
								       const QString &opusPrompt) const
{
	for (const std::shared_ptr<CurationRangeStrategy> &strategy : strategies) {
		if (!strategy)
			continue;

		CurationRangeStrategyResolution resolution = strategy->resolve(transcript, settings, opusPrompt);
		if (resolution.applied() || !resolution.reason.trimmed().isEmpty())
			return resolution;
	}

	CurationRangeStrategyResolution resolution;
	resolution.reason = QStringLiteral("no_range_strategy_matched");
	return resolution;
}

CurationSettings CurationRangeStrategyResolver::apply(const CurationSettings &settings,
						      const CurationRangeStrategyResolution &resolution) const
{
	const QVector<ClipDuration> ranges = validResolutionRanges(resolution);
	if (ranges.isEmpty())
		return settings;

	CurationSettings adjusted = settings;
	adjusted.clipDurations = ranges;
	double minStartSec = ranges.first().startSec;
	double maxEndSec = ranges.first().endSec;
	for (const ClipDuration &range : ranges) {
		minStartSec = std::min(minStartSec, range.startSec);
		maxEndSec = std::max(maxEndSec, range.endSec);
	}
	adjusted.rangeStartSec = minStartSec;
	adjusted.rangeEndSec = maxEndSec;
	return adjusted;
}
