#include "curation/range/viewer-message-range-strategy.hpp"

#include "curation/range/viewer-message-focus-range.hpp"

#include <QStringList>

#include <algorithm>

namespace {
static constexpr int MAX_VIEWER_MESSAGE_CANDIDATE_PROJECTS = 5;

bool shouldApplyCandidateRanges(const ViewerMessageFocusRangeResult &focus)
{
	const QString reason = focus.reason.trimmed();
	return reason == QStringLiteral("no_reliable_target") ||
	       reason == QStringLiteral("target_not_explicitly_requested") ||
	       reason == QStringLiteral("unreliable_target");
}

QVector<ClipDuration> cappedCandidateRanges(const QVector<ClipDuration> &ranges)
{
	QVector<ClipDuration> capped;
	capped.reserve(std::min(static_cast<long long>(MAX_VIEWER_MESSAGE_CANDIDATE_PROJECTS),
				      static_cast<long long>(ranges.size())));

	for (const ClipDuration &range : ranges) {
		if (range.endSec <= range.startSec)
			continue;
		capped.append(range);
		if (capped.size() >= MAX_VIEWER_MESSAGE_CANDIDATE_PROJECTS)
			break;
	}

	return capped;
}

} // namespace

QString ViewerMessageResponseRangeStrategy::name() const
{
	return QStringLiteral("viewer_message_response");
}

CurationRangeStrategyResolution ViewerMessageResponseRangeStrategy::resolve(const RecordingTranscript &transcript,
								    const CurationSettings &settings,
								    const QString &opusPrompt) const
{
	const ViewerMessageFocusRangeResolver focusResolver;
	const ViewerMessageFocusRangeResult focus = focusResolver.resolve(transcript, settings, opusPrompt);

	CurationRangeStrategyResolution resolution;
	resolution.strategyName = name();
	resolution.reason = focus.reason;
	resolution.target = focus.target;
	resolution.anchorText = focus.anchorText;
	resolution.manualRange = focus.manualRange;
	resolution.confidence = focus.confidence;
	resolution.startAdjusted = focus.startAdjusted;
	resolution.details = focus.candidateSummary;

	if (focus.applied) {
		resolution.mode = CurationRangeStrategyResolution::Mode::FocusRange;
		resolution.ranges.append(focus.focusRange);
		return resolution;
	}

	if (!focus.candidateRanges.isEmpty() && shouldApplyCandidateRanges(focus)) {
		resolution.ranges = cappedCandidateRanges(focus.candidateRanges);
		if (!resolution.ranges.isEmpty()) {
			resolution.mode = CurationRangeStrategyResolution::Mode::CandidateRanges;
			resolution.reason = QStringLiteral("candidate_ranges_for_viewer_message_discovery");
			return resolution;
		}
	}

	resolution.mode = CurationRangeStrategyResolution::Mode::Unchanged;
	return resolution;
}
