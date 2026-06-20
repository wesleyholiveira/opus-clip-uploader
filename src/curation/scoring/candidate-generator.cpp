#include "curation/scoring/candidate-generator.hpp"

#include "curation/scoring/cheap-clip-scorer.hpp"

#include "curation/curation-signals.hpp"

#include <algorithm>
#include <cmath>

namespace {

static constexpr double NEXT_CUE_MIN_GAP_SEC = 10.0;
static constexpr double NEXT_CUE_PADDING_SEC = 0.35;

bool validRange(const ClipDuration &range)
{
	return std::isfinite(range.startSec) && std::isfinite(range.endSec) && range.endSec > range.startSec;
}

} // namespace

namespace Curation::Scoring {

QVector<ClipCandidate> CandidateGenerator::generate(const TranscriptIndex &index,
							     const CandidateGenerationOptions &options) const
{
	QVector<ClipCandidate> candidates;
	if (index.isEmpty() || !validRange(options.searchRange))
		return candidates;

	candidates = cueAnchoredCandidates(index, options);
	const QVector<ClipCandidate> windows = slidingWindowCandidates(index, options);
	for (const ClipCandidate &candidate : windows) {
		candidates.append(candidate);
		if (static_cast<int>(candidates.size()) >= options.maxRawCandidates)
			break;
	}
	return candidates;
}

QVector<ClipCandidate> CandidateGenerator::cueAnchoredCandidates(const TranscriptIndex &index,
							 const CandidateGenerationOptions &options) const
{
	QVector<ClipCandidate> candidates;
	CheapClipScorer cueScorer;

	for (int i = 0; i < index.size(); ++i) {
		const TranscriptSegment *segment = index.segmentAt(i);
		if (!segment || !TranscriptIndex::segmentOverlapsRange(*segment, options.searchRange) ||
		    segment->text.trimmed().isEmpty())
			continue;

		const bool targetHit = options.reliableMainTarget &&
			cueScorer.targetKeywordScore(segment->text, options.mainTarget) >= 0.35;
		const bool strongCue = cueScorer.hasStrongLocalCue(segment->text);
		if (!strongCue && !targetHit)
			continue;

		double afterSec = options.defaultAfterSec;
		const double emotional = Curation::emotionalScoreForText(segment->text);
		const double advice = Curation::adviceScoreForText(segment->text);
		if (emotional >= 0.25)
			afterSec = std::max(afterSec, 38.0);
		if (advice >= 0.25 || targetHit)
			afterSec = std::max(afterSec, 42.0);

		const double startSec = std::max(options.searchRange.startSec, segment->startSec - options.anchorPaddingBeforeSec);
		const double desiredEndSec = std::min(options.searchRange.endSec, segment->startSec + afterSec);
		const double minEndSec = std::min(options.searchRange.endSec, startSec + options.minDurationSec);
		double endSec = std::max(desiredEndSec, minEndSec);
		if ((endSec - startSec) > options.maxDurationSec)
			endSec = startSec + options.maxDurationSec;

		ClipCandidate candidate = buildCandidate(index, options, startSec, endSec,
						      targetHit ? QStringLiteral("target_anchor") : QStringLiteral("cue_anchor"),
						      cueScorer.looksLikeQuestionOrViewerMessage(segment->text));
		candidate.anchorText = segment->text.trimmed().left(220);
		candidate.range = trimBeforeNextStrongCue(index, options, candidate.range, i);
		candidate.endsBeforeNextCue = candidate.range.endSec < endSec - 0.01;
		candidate.text = index.textForRange(candidate.range);
		candidate.firstSegmentIndex = index.firstSegmentIndexOverlapping(candidate.range);
		candidate.lastSegmentIndex = index.lastSegmentIndexOverlapping(candidate.range);
		if (candidate.range.endSec > candidate.range.startSec)
			candidates.append(candidate);

		if (static_cast<int>(candidates.size()) >= options.maxRawCandidates)
			break;
	}

	return candidates;
}

QVector<ClipCandidate> CandidateGenerator::slidingWindowCandidates(const TranscriptIndex &index,
							   const CandidateGenerationOptions &options) const
{
	QVector<ClipCandidate> candidates;
	const QVector<double> durations{30.0, 45.0, 60.0};
	const double stepSec = 15.0;

	for (double startSec = options.searchRange.startSec; startSec < options.searchRange.endSec;
	     startSec += stepSec) {
		for (const double duration : durations) {
			if (duration < options.minDurationSec || duration > options.maxDurationSec)
				continue;
			const double endSec = std::min(options.searchRange.endSec, startSec + duration);
			if ((endSec - startSec) < options.minDurationSec)
				continue;
			candidates.append(buildCandidate(index, options, startSec, endSec, QStringLiteral("sliding_window"), false));
			if (static_cast<int>(candidates.size()) >= options.maxRawCandidates)
				return candidates;
		}
	}

	return candidates;
}

ClipCandidate CandidateGenerator::buildCandidate(const TranscriptIndex &index, const CandidateGenerationOptions &options,
						 double startSec, double endSec, const QString &source,
						 bool startsNearViewerCue) const
{
	ClipCandidate candidate;
	candidate.range = index.clampRange({startSec, endSec}, options.searchRange);
	candidate.firstSegmentIndex = index.firstSegmentIndexOverlapping(candidate.range);
	candidate.lastSegmentIndex = index.lastSegmentIndexOverlapping(candidate.range);
	candidate.text = index.textForRange(candidate.range);
	candidate.anchorText = candidate.text.left(220);
	candidate.source = source;
	candidate.startsNearViewerCue = startsNearViewerCue;
	return candidate;
}

ClipDuration CandidateGenerator::trimBeforeNextStrongCue(const TranscriptIndex &index,
							 const CandidateGenerationOptions &options,
							 const ClipDuration &range, int firstSegmentIndex) const
{
	CheapClipScorer cueScorer;
	const TranscriptSegment *first = index.segmentAt(firstSegmentIndex);
	if (!first)
		return range;

	for (int i = firstSegmentIndex + 1; i < index.size(); ++i) {
		const TranscriptSegment *segment = index.segmentAt(i);
		if (!segment || !TranscriptIndex::segmentOverlapsRange(*segment, range) || segment->text.trimmed().isEmpty())
			continue;
		if (segment->startSec <= first->startSec + NEXT_CUE_MIN_GAP_SEC)
			continue;
		if (!cueScorer.hasStrongLocalCue(segment->text))
			continue;

		const double trimmedEndSec = segment->startSec - NEXT_CUE_PADDING_SEC;
		if (trimmedEndSec > range.startSec + options.minDurationSec * 0.50)
			return {range.startSec, std::min(range.endSec, trimmedEndSec)};
	}

	return range;
}

} // namespace Curation::Scoring
