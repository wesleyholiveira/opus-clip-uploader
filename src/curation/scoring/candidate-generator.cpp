#include "curation/scoring/candidate-generator.hpp"

#include "curation/scoring/cheap-clip-scorer.hpp"
#include "curation/scoring/text-analysis.hpp"

#include "curation/curation-signals.hpp"

#include <algorithm>
#include <cmath>

using namespace Curation::Scoring;

namespace {

static constexpr double NEXT_CUE_MIN_GAP_SEC = 10.0;
static constexpr double NEXT_CUE_PADDING_SEC = 0.35;
static constexpr double VIEWER_CONTEXT_LOOKBACK_SEC = 18.0;
static constexpr double MENTAL_HEALTH_CONTEXT_LOOKBACK_SEC = 36.0;

bool validRange(const ClipDuration &range)
{
	return std::isfinite(range.startSec) && std::isfinite(range.endSec) && range.endSec > range.startSec;
}

double candidateAfterSeconds(const CandidateGenerationOptions &options, double emotionalScore, double adviceScore,
			     bool targetHit)
{
	if (targetHit || adviceScore >= 0.35)
		return std::max(options.defaultAfterSec, options.adviceAfterSec);
	if (emotionalScore >= 0.35)
		return std::max(options.defaultAfterSec, options.emotionalAfterSec);
	return options.defaultAfterSec;
}

bool shouldExtendCandidate(const QString &text)
{
	return TextAnalysis::looksLikeSameExchangeContinuation(text) || TextAnalysis::looksLikeGamblingContext(text) ||
	       TextAnalysis::looksLikeMentalHealthContext(text);
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

		const double emotional = Curation::emotionalScoreForText(segment->text);
		const double advice = Curation::adviceScoreForText(segment->text);
		const double afterSec = candidateAfterSeconds(options, emotional, advice, targetHit);

		const double startSec =
			std::max(options.searchRange.startSec, segment->startSec - options.anchorPaddingBeforeSec);
		const double desiredEndSec = std::min(options.searchRange.endSec, segment->startSec + afterSec);
		const double minEndSec = std::min(options.searchRange.endSec, startSec + options.minDurationSec);
		double endSec = std::max(desiredEndSec, minEndSec);
		if ((endSec - startSec) > options.maxDurationSec)
			endSec = startSec + options.maxDurationSec;

		ClipDuration range = {startSec, endSec};
		range = expandStartForViewerContext(index, options, range, i);
		range = extendForOpenExplanation(index, options, range);
		const double endBeforeTrimSec = range.endSec;
		range = trimBeforeNextTopicShift(index, options, range, i);
		range = extendForOpenExplanation(index, options, range);

		ClipCandidate candidate =
			buildCandidate(index, options, range.startSec, range.endSec,
				       targetHit ? QStringLiteral("target_anchor") : QStringLiteral("cue_anchor"),
				       cueScorer.looksLikeQuestionOrViewerMessage(segment->text));
		candidate.anchorText = segment->text.trimmed().left(220);
		candidate.endsBeforeNextCue = candidate.range.endSec < endBeforeTrimSec - 0.01;
		if (candidate.endsBeforeNextCue)
			candidate.evidence.append(QStringLiteral("trimmed_before_topic_shift"));
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
	const double stepSec = std::max(5.0, options.slidingWindowStepSec);

	for (double startSec = options.searchRange.startSec; startSec < options.searchRange.endSec;
	     startSec += stepSec) {
		for (const double duration : durations) {
			if (duration < options.minDurationSec || duration > options.maxDurationSec)
				continue;
			const double endSec = std::min(options.searchRange.endSec, startSec + duration);
			if ((endSec - startSec) < options.minDurationSec)
				continue;

			ClipCandidate candidate = buildCandidate(index, options, startSec, endSec,
								 QStringLiteral("sliding_window"), false);
			if (TextAnalysis::hasNoiseOnlySemanticTopic(candidate.text) ||
			    TextAnalysis::isBacklogOrGreetingText(candidate.text))
				continue;
			candidates.append(candidate);
			if (static_cast<int>(candidates.size()) >= options.maxRawCandidates)
				return candidates;
		}
	}

	return candidates;
}

ClipCandidate CandidateGenerator::buildCandidate(const TranscriptIndex &index,
						 const CandidateGenerationOptions &options, double startSec,
						 double endSec, const QString &source, bool startsNearViewerCue) const
{
	ClipCandidate candidate;
	candidate.range = index.clampRange({startSec, endSec}, options.searchRange);
	candidate.firstSegmentIndex = index.firstSegmentIndexOverlapping(candidate.range);
	candidate.lastSegmentIndex = index.lastSegmentIndexOverlapping(candidate.range);
	candidate.text = index.textForRange(candidate.range);
	candidate.anchorText = TextAnalysis::sampleForLog(candidate.text.left(220), 220);
	candidate.source = source;
	candidate.startsNearViewerCue = startsNearViewerCue;
	candidate.hasReliableMainTarget = options.reliableMainTarget;
	return candidate;
}

ClipDuration CandidateGenerator::expandStartForViewerContext(const TranscriptIndex &index,
							     const CandidateGenerationOptions &options,
							     const ClipDuration &range, int anchorSegmentIndex) const
{
	const TranscriptSegment *anchor = index.segmentAt(anchorSegmentIndex);
	if (!anchor)
		return range;

	double startSec = range.startSec;
	const double lookbackSec = TextAnalysis::looksLikeMentalHealthContext(anchor->text)
					   ? MENTAL_HEALTH_CONTEXT_LOOKBACK_SEC
					   : VIEWER_CONTEXT_LOOKBACK_SEC;

	for (int i = anchorSegmentIndex - 1; i >= 0; --i) {
		const TranscriptSegment *segment = index.segmentAt(i);
		if (!segment || segment->endSec < range.startSec - lookbackSec)
			break;
		if (segment->startSec > range.startSec + 0.75)
			continue;
		if (!TextAnalysis::looksLikeViewerContextPrelude(segment->text, anchor->text))
			continue;
		startSec = std::min(startSec, std::max(options.searchRange.startSec, segment->startSec - 0.10));
	}

	return {startSec, range.endSec};
}

ClipDuration CandidateGenerator::extendForOpenExplanation(const TranscriptIndex &index,
							  const CandidateGenerationOptions &options,
							  const ClipDuration &range) const
{
	QString context = index.textForRange(range);
	if (!shouldExtendCandidate(context))
		return range;

	double endSec = range.endSec;
	const double maxEndSec = std::min(options.searchRange.endSec, range.startSec + options.maxDurationSec);
	for (int i = index.lastSegmentIndexOverlapping(range) + 1; i < index.size(); ++i) {
		const TranscriptSegment *segment = index.segmentAt(i);
		if (!segment || segment->startSec > maxEndSec)
			break;

		const bool closeEnough = segment->startSec < endSec + 1.25;
		const bool continuation = !TextAnalysis::looksLikeHardTopicShift(segment->text, context) &&
					  (TextAnalysis::looksLikeSameExchangeContinuation(segment->text) ||
					   TextAnalysis::looksLikeSameTopicContinuation(segment->text, context));
		if (!closeEnough && !continuation)
			break;
		if (TextAnalysis::looksLikeHardTopicShift(segment->text, context))
			break;

		endSec = std::min(maxEndSec, std::max(endSec, segment->endSec));
		context.append(QLatin1Char(' '));
		context.append(segment->text);
	}

	return {range.startSec, std::max(range.endSec, endSec)};
}

ClipDuration CandidateGenerator::trimBeforeNextTopicShift(const TranscriptIndex &index,
							  const CandidateGenerationOptions &options,
							  const ClipDuration &range, int anchorSegmentIndex) const
{
	CheapClipScorer cueScorer;
	const TranscriptSegment *anchor = index.segmentAt(anchorSegmentIndex);
	if (!anchor)
		return range;

	QString context = index.textForRange({range.startSec, std::min(range.endSec, anchor->startSec + 6.0)});
	if (context.trimmed().isEmpty())
		context = index.textForRange(range);

	for (int i = anchorSegmentIndex + 1; i < index.size(); ++i) {
		const TranscriptSegment *segment = index.segmentAt(i);
		if (!segment || !TranscriptIndex::segmentOverlapsRange(*segment, range) ||
		    segment->text.trimmed().isEmpty())
			continue;
		if (segment->startSec <= anchor->startSec + 1.75)
			continue;

		const bool hardTopicShift = TextAnalysis::looksLikeHardTopicShift(segment->text, context);
		if (!hardTopicShift && segment->startSec <= anchor->startSec + NEXT_CUE_MIN_GAP_SEC)
			continue;

		const bool newStrongCue = cueScorer.hasStrongLocalCue(segment->text);
		if (!hardTopicShift && !newStrongCue)
			continue;
		if (!hardTopicShift && (TextAnalysis::looksLikeSameExchangeContinuation(segment->text) ||
					TextAnalysis::looksLikeSameTopicContinuation(segment->text, context))) {
			context.append(QLatin1Char(' '));
			context.append(segment->text);
			continue;
		}

		const double trimmedEndSec = segment->startSec - NEXT_CUE_PADDING_SEC;
		if (trimmedEndSec > range.startSec + options.boundaryMinDurationSec)
			return {range.startSec, std::min(range.endSec, trimmedEndSec)};
	}

	return range;
}

} // namespace Curation::Scoring
