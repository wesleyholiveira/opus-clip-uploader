#include "curation/scoring/arc-dp-boundary-refiner.hpp"

#include "curation/scoring/cheap-clip-scorer.hpp"
#include "curation/scoring/text-analysis.hpp"

#include "curation/curation-signals.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace Curation::Scoring {

namespace {

static constexpr double NEG_INF = -1.0e9;

static double boundedScore(double value)
{
	return std::clamp(value, 0.0, 1.0);
}

enum class State {
	Context = 0,
	Hook = 1,
	Development = 2,
	Resolution = 3,
};

struct NodeFeatures {
	int segmentIndex = -1;
	ClipDuration range;
	double context = 0.0;
	double hook = 0.0;
	double development = 0.0;
	double resolution = 0.0;
	double target = 0.0;
	double defect = 0.0;
	bool viewerCue = false;
};

struct Cell {
	double score = NEG_INF;
	int previousNode = -1;
	State previousState = State::Context;
	int firstNode = -1;
	bool sawContext = false;
	bool sawHook = false;
	bool sawDevelopment = false;
};

struct BestPath {
	int firstNode = -1;
	int lastNode = -1;
	double score = 0.0;
	bool sawContext = false;
	bool sawHook = false;
	bool sawDevelopment = false;
};

static bool finiteScore(double value)
{
	return value > NEG_INF * 0.5;
}

static int stateIndex(State state)
{
	return static_cast<int>(state);
}

static double emissionScore(const NodeFeatures &features, State state, const ArcDpBoundaryRefinerOptions &options)
{
	const double target = features.target * std::clamp(options.targetWeight, 0.25, 3.0);
	const double defect = features.defect * std::clamp(options.defectPenalty, 0.25, 4.0);
	switch (state) {
	case State::Context:
		return ((features.context * options.contextWeight) * 0.76) + (target * 0.12) - (defect * 0.52);
	case State::Hook:
		return ((features.hook * options.hookWeight) * 0.74) +
		       ((features.context * options.contextWeight) * 0.18) + (target * 0.18) - (defect * 0.48);
	case State::Development:
		return ((features.development * options.developmentWeight) * 0.60) + (target * 0.24) +
		       ((features.hook * options.hookWeight) * 0.08) - (defect * 0.34);
	case State::Resolution:
		return ((features.resolution * options.resolutionWeight) * 0.72) +
		       ((features.development * options.developmentWeight) * 0.14) + (target * 0.10) - (defect * 0.40);
	}
	return NEG_INF;
}

static double transitionScore(State from, State to)
{
	if (from == to) {
		if (from == State::Development)
			return 0.12;
		if (from == State::Resolution)
			return -0.10;
		return 0.02;
	}
	if (from == State::Context && to == State::Hook)
		return 0.22;
	if (from == State::Context && to == State::Development)
		return -0.04;
	if (from == State::Hook && to == State::Development)
		return 0.24;
	if (from == State::Development && to == State::Resolution)
		return 0.35;
	return -0.55;
}

static ClipDuration clampRange(const ClipDuration &range, const ClipDuration &bounds)
{
	ClipDuration clamped;
	clamped.startSec = std::clamp(range.startSec, bounds.startSec, bounds.endSec);
	clamped.endSec = std::clamp(range.endSec, bounds.startSec, bounds.endSec);
	if (clamped.endSec < clamped.startSec)
		std::swap(clamped.startSec, clamped.endSec);
	return clamped;
}

static QVector<NodeFeatures> buildFeatures(const TranscriptIndex &index, const ClipDuration &analysisRange,
					   const ArcDpBoundaryRefinerOptions &options)
{
	QVector<NodeFeatures> features;
	CheapClipScorer scorer;
	QString previousText;
	for (int i = 0; i < index.size(); ++i) {
		const TranscriptSegment *segment = index.segmentAt(i);
		if (!segment || segment->text.trimmed().isEmpty() ||
		    !TranscriptIndex::segmentOverlapsRange(*segment, analysisRange))
			continue;

		const QString text = segment->text.trimmed();
		NodeFeatures feature;
		feature.segmentIndex = i;
		feature.range = {segment->startSec, segment->endSec};
		feature.viewerCue = scorer.looksLikeQuestionOrViewerMessage(text) ||
				    TextAnalysis::hasConcreteViewerQuestion(text);
		feature.target = options.reliableMainTarget ? scorer.targetKeywordScore(text, options.mainTarget) : 0.0;
		const double emotional = Curation::emotionalScoreForText(text);
		const double advice = Curation::adviceScoreForText(text);
		const bool mental = TextAnalysis::looksLikeMentalHealthContext(text);
		const bool continuation =
			TextAnalysis::looksLikeSameExchangeContinuation(text) ||
			(!previousText.isEmpty() && TextAnalysis::looksLikeSameTopicContinuation(text, previousText));
		const bool shift = !previousText.isEmpty() && TextAnalysis::looksLikeHardTopicShift(text, previousText);
		const bool meta = TextAnalysis::isSocialOrStreamMetaText(text) ||
				  TextAnalysis::isBacklogOrGreetingText(text) ||
				  TextAnalysis::hasNoiseOnlySemanticTopic(text);

		feature.context = boundedScore((feature.viewerCue ? 0.74 : 0.0) + (mental ? 0.12 : 0.0) +
					       (feature.target * 0.16));
		feature.hook =
			boundedScore((scorer.hasStrongLocalCue(text) ? 0.64 : 0.0) + (feature.viewerCue ? 0.20 : 0.0) +
				     (feature.target * 0.26) + (emotional * 0.12));
		feature.development = boundedScore((continuation ? 0.40 : 0.0) + (mental ? 0.22 : 0.0) +
						   (advice * 0.22) + (feature.target * 0.30));
		feature.resolution =
			boundedScore((advice * 0.34) + (continuation ? 0.14 : 0.0) + (feature.target * 0.14) +
				     (segment->endSec >= analysisRange.startSec ? 0.04 : 0.0));
		feature.defect = boundedScore((shift ? 0.74 : 0.0) + (meta ? 0.62 : 0.0) +
					      (TextAnalysis::looksLikeNewViewerTurnAfterPause(text) ? 0.42 : 0.0));
		features.append(feature);
		previousText = text;
	}
	return features;
}

static BestPath bestArcPath(const QVector<NodeFeatures> &features, const ArcDpBoundaryRefinerOptions &options)
{
	BestPath best;
	if (features.isEmpty())
		return best;

	constexpr int StateCount = 4;
	QVector<std::array<Cell, StateCount>> dp(features.size());
	for (int i = 0; i < static_cast<int>(features.size()); ++i) {
		for (int s = 0; s < StateCount; ++s) {
			const State state = static_cast<State>(s);
			const NodeFeatures &feature = features.at(i);
			const double emission = emissionScore(feature, state, options);
			Cell cell;
			cell.score = emission;
			cell.firstNode = i;
			cell.sawContext = state == State::Context && feature.context >= 0.42;
			cell.sawHook = state == State::Hook && feature.hook >= 0.42;
			cell.sawDevelopment = state == State::Development && feature.development >= 0.34;

			if (i > 0) {
				for (int prev = 0; prev < StateCount; ++prev) {
					const Cell &previous = dp.at(i - 1).at(prev);
					if (!finiteScore(previous.score))
						continue;
					const double gapSec =
						std::max(0.0, feature.range.startSec - features.at(i - 1).range.endSec);
					const double gapPenalty = gapSec > 4.0 ? std::min(0.75, gapSec * 0.06) : 0.0;
					const double score = previous.score + emission +
							     transitionScore(static_cast<State>(prev), state) -
							     gapPenalty;
					if (score > cell.score + 0.0001) {
						cell = previous;
						cell.score = score;
						cell.previousNode = i - 1;
						cell.previousState = static_cast<State>(prev);
						cell.sawContext = cell.sawContext ||
								  (state == State::Context && feature.context >= 0.42);
						cell.sawHook = cell.sawHook ||
							       (state == State::Hook && feature.hook >= 0.42);
						cell.sawDevelopment =
							cell.sawDevelopment ||
							(state == State::Development && feature.development >= 0.34);
					}
				}
			}
			dp[i][s] = cell;
		}

		const Cell &resolution = dp.at(i).at(stateIndex(State::Resolution));
		if (!finiteScore(resolution.score) || resolution.firstNode < 0)
			continue;
		const ClipDuration range{features.at(resolution.firstNode).range.startSec, features.at(i).range.endSec};
		const double duration = range.endSec - range.startSec;
		if (duration < options.minDurationSec || duration > options.maxDurationSec)
			continue;
		const int nodeCount = std::max(1, i - resolution.firstNode + 1);
		const double normalized = boundedScore(resolution.score / static_cast<double>(nodeCount));
		const bool hasRequiredArc = (resolution.sawHook || resolution.sawContext) && resolution.sawDevelopment;
		if (!hasRequiredArc || normalized < std::clamp(options.minArcConfidence, 0.10, 0.85))
			continue;
		if (best.firstNode < 0 || normalized > best.score + 0.025 ||
		    (std::fabs(normalized - best.score) <= 0.025 &&
		     range.startSec < features.at(best.firstNode).range.startSec)) {
			best.firstNode = resolution.firstNode;
			best.lastNode = i;
			best.score = normalized;
			best.sawContext = resolution.sawContext;
			best.sawHook = resolution.sawHook;
			best.sawDevelopment = resolution.sawDevelopment;
		}
	}
	return best;
}

} // namespace

ClipCandidate ArcDpBoundaryRefiner::refine(const TranscriptIndex &index, const ClipCandidate &candidate,
					   const ArcDpBoundaryRefinerOptions &options) const
{
	if (candidate.range.endSec <= candidate.range.startSec)
		return candidate;
	const ClipDuration globalBounds =
		options.searchRange.endSec > options.searchRange.startSec
			? options.searchRange
			: ClipDuration{0.0, std::max(index.durationSec(), candidate.range.endSec)};
	const ClipDuration analysisRange = clampRange({candidate.range.startSec - std::max(0.0, options.lookbackSec),
						       candidate.range.endSec + std::max(0.0, options.lookaheadSec)},
						      globalBounds);
	const QVector<NodeFeatures> features = buildFeatures(index, analysisRange, options);
	const BestPath best = bestArcPath(features, options);
	if (best.firstNode < 0 || best.lastNode < best.firstNode)
		return candidate;

	ClipDuration refinedRange{features.at(best.firstNode).range.startSec, features.at(best.lastNode).range.endSec};
	refinedRange = clampRange(refinedRange, globalBounds);
	if (index.hasWordTimings())
		refinedRange = index.snapRangeToWordBoundaries(refinedRange, globalBounds);
	const double duration = refinedRange.endSec - refinedRange.startSec;
	if (duration < options.minDurationSec || duration > options.maxDurationSec + 0.1)
		return candidate;

	const bool materiallyChanged = std::fabs(refinedRange.startSec - candidate.range.startSec) > 0.80 ||
				       std::fabs(refinedRange.endSec - candidate.range.endSec) > 0.80;
	if (!materiallyChanged)
		return candidate;

	ClipCandidate refined = candidate;
	refined.range = refinedRange;
	refined.firstSegmentIndex = index.firstSegmentIndexOverlapping(refined.range);
	refined.lastSegmentIndex = index.lastSegmentIndexOverlapping(refined.range);
	refined.text = index.textForRange(refined.range).simplified();
	refined.timedText = index.timedTextForRange(refined.range);
	refined.anchorText = refined.text.left(220);
	refined.evidence.append(QStringLiteral("arc_dp_boundary_refined score:%1 startNode:%2 endNode:%3")
					.arg(QString::number(best.score, 'f', 2))
					.arg(best.firstNode)
					.arg(best.lastNode));
	if (refinedRange.startSec < candidate.range.startSec - 0.80)
		refined.evidence.append(QStringLiteral("arc_dp_context_start_extended"));
	if (refinedRange.endSec > candidate.range.endSec + 0.80)
		refined.evidence.append(QStringLiteral("arc_dp_resolution_end_extended"));
	if (refinedRange.endSec < candidate.range.endSec - 0.80)
		refined.evidence.append(QStringLiteral("arc_dp_tail_trimmed"));
	refined.evidence.removeDuplicates();
	return refined;
}

} // namespace Curation::Scoring
