#include "curation/scoring/candidate-arc-planner.hpp"

#include "curation/scoring/cheap-clip-scorer.hpp"
#include "curation/scoring/text-analysis.hpp"

#include "curation/curation-signals.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Curation::Scoring {

namespace {

static constexpr double NEG_INF = -1.0e9;

static double boundedScore(double value)
{
	return std::clamp(value, 0.0, 1.0);
}

enum class ArcDpState {
	Context = 0,
	Hook = 1,
	Development = 2,
	Resolution = 3,
	Done = 4,
};

struct SegmentArcFeatures {
	int index = -1;
	ClipDuration range;
	double context = 0.0;
	double hook = 0.0;
	double development = 0.0;
	double resolution = 0.0;
	double target = 0.0;
	double bad = 0.0;
	bool viewerCue = false;
};

struct DpCell {
	double score = NEG_INF;
	int previousIndex = -1;
	ArcDpState previousState = ArcDpState::Context;
	int startIndex = -1;
};

struct CompletedArc {
	int firstFeature = -1;
	int lastFeature = -1;
	double score = 0.0;
	bool startsNearViewerCue = false;
	QStringList evidence;
};

static int stateIndex(ArcDpState state)
{
	return static_cast<int>(state);
}

static bool finiteScore(double value)
{
	return value > NEG_INF * 0.5;
}

static double emissionScore(const SegmentArcFeatures &features, ArcDpState state)
{
	switch (state) {
	case ArcDpState::Context:
		return (features.context * 0.70) + (features.target * 0.16) - (features.bad * 0.50);
	case ArcDpState::Hook:
		return (features.hook * 0.72) + (features.context * 0.22) + (features.target * 0.18) - (features.bad * 0.46);
	case ArcDpState::Development:
		return (features.development * 0.58) + (features.target * 0.24) + (features.hook * 0.10) - (features.bad * 0.34);
	case ArcDpState::Resolution:
		return (features.resolution * 0.70) + (features.development * 0.12) + (features.target * 0.12) - (features.bad * 0.42);
	case ArcDpState::Done:
		break;
	}
	return NEG_INF;
}

static double transitionScore(ArcDpState from, ArcDpState to)
{
	if (from == to) {
		if (from == ArcDpState::Development)
			return 0.12;
		if (from == ArcDpState::Context)
			return -0.06;
		if (from == ArcDpState::Resolution)
			return -0.08;
		return 0.02;
	}
	if (from == ArcDpState::Context && to == ArcDpState::Hook)
		return 0.22;
	if (from == ArcDpState::Context && to == ArcDpState::Development)
		return -0.02;
	if (from == ArcDpState::Hook && to == ArcDpState::Development)
		return 0.24;
	if (from == ArcDpState::Hook && to == ArcDpState::Resolution)
		return -0.10;
	if (from == ArcDpState::Development && to == ArcDpState::Resolution)
		return 0.34;
	return -0.48;
}

static QVector<SegmentArcFeatures> buildFeatures(const TranscriptIndex &index,
	const CandidateGenerationOptions &options)
{
	QVector<SegmentArcFeatures> features;
	CheapClipScorer scorer;
	QString previousUsefulText;
	for (int i = 0; i < index.size(); ++i) {
		const TranscriptSegment *segment = index.segmentAt(i);
		if (!segment || segment->text.trimmed().isEmpty() ||
		    !TranscriptIndex::segmentOverlapsRange(*segment, options.searchRange))
			continue;

		const QString text = segment->text.trimmed();
		SegmentArcFeatures feature;
		feature.index = i;
		feature.range = {segment->startSec, segment->endSec};
		feature.viewerCue = scorer.looksLikeQuestionOrViewerMessage(text) || TextAnalysis::hasConcreteViewerQuestion(text);
		feature.target = options.reliableMainTarget ? scorer.targetKeywordScore(text, options.mainTarget) : 0.0;
		const double emotional = Curation::emotionalScoreForText(text);
		const double advice = Curation::adviceScoreForText(text);
		const bool mentalHealth = TextAnalysis::looksLikeMentalHealthContext(text);
		const bool sameExchange = TextAnalysis::looksLikeSameExchangeContinuation(text);
		const bool hardShift = !previousUsefulText.isEmpty() && TextAnalysis::looksLikeHardTopicShift(text, previousUsefulText);
		const bool meta = TextAnalysis::isSocialOrStreamMetaText(text) || TextAnalysis::isBacklogOrGreetingText(text) ||
			TextAnalysis::hasNoiseOnlySemanticTopic(text);

		feature.context = boundedScore((feature.viewerCue ? 0.72 : 0.0) + (mentalHealth ? 0.16 : 0.0) +
			(feature.target * 0.18));
		feature.hook = boundedScore((scorer.hasStrongLocalCue(text) ? 0.66 : 0.0) + (feature.viewerCue ? 0.22 : 0.0) +
			(feature.target * 0.30) + (emotional * 0.10));
		feature.development = boundedScore((sameExchange ? 0.38 : 0.0) + (mentalHealth ? 0.24 : 0.0) +
			(advice * 0.25) + (feature.target * 0.32));
		feature.resolution = boundedScore((TextAnalysis::looksLikeSameExchangeContinuation(text) ? 0.18 : 0.0) +
			(advice * 0.28) + (feature.target * 0.16) +
			(text.contains(QLatin1Char('.')) || text.contains(QLatin1Char('!')) ? 0.08 : 0.0));
		feature.bad = boundedScore((hardShift ? 0.72 : 0.0) + (meta ? 0.62 : 0.0) +
			(TextAnalysis::looksLikeNewViewerTurnAfterPause(text) ? 0.40 : 0.0));

		features.append(feature);
		previousUsefulText = text;
	}
	return features;
}

static bool durationAllowed(const ClipDuration &range, const CandidateGenerationOptions &options)
{
	const double duration = range.endSec - range.startSec;
	return duration >= options.minDurationSec && duration <= options.maxDurationSec;
}

static QVector<CompletedArc> recoverArcs(const QVector<SegmentArcFeatures> &features,
	const CandidateGenerationOptions &options)
{
	QVector<CompletedArc> arcs;
	if (features.isEmpty())
		return arcs;

	constexpr int StateCount = 4;
	QVector<std::array<DpCell, StateCount>> dp(features.size());
	for (int i = 0; i < static_cast<int>(features.size()); ++i) {
		const SegmentArcFeatures &feature = features.at(i);
		for (int s = 0; s < StateCount; ++s) {
			const ArcDpState state = static_cast<ArcDpState>(s);
			const double emission = emissionScore(feature, state);
			if (!finiteScore(emission))
				continue;
			DpCell best;
			best.score = emission;
			best.startIndex = i;

			if (i > 0) {
				for (int prev = 0; prev < StateCount; ++prev) {
					const DpCell &previous = dp.at(i - 1).at(prev);
					if (!finiteScore(previous.score))
						continue;
					const double gapSec = std::max(0.0, feature.range.startSec - features.at(i - 1).range.endSec);
					const double gapPenalty = gapSec > 3.5 ? std::min(0.65, gapSec * 0.055) : 0.0;
					const double candidateScore = previous.score + emission +
						transitionScore(static_cast<ArcDpState>(prev), state) - gapPenalty;
					if (candidateScore > best.score + 0.0001) {
						best.score = candidateScore;
						best.previousIndex = i - 1;
						best.previousState = static_cast<ArcDpState>(prev);
						best.startIndex = previous.startIndex;
					}
				}
			}
			dp[i][s] = best;
		}

		const DpCell &resolution = dp.at(i).at(stateIndex(ArcDpState::Resolution));
		if (!finiteScore(resolution.score) || resolution.startIndex < 0)
			continue;
		const ClipDuration range{features.at(resolution.startIndex).range.startSec, feature.range.endSec};
		if (!durationAllowed(range, options))
			continue;
		const double normalized = resolution.score / std::max(1, i - resolution.startIndex + 1);
		if (normalized < 0.28)
			continue;

		CompletedArc arc;
		arc.firstFeature = resolution.startIndex;
		arc.lastFeature = i;
		arc.score = boundedScore(normalized);
		for (int j = arc.firstFeature; j <= arc.lastFeature; ++j) {
			if (features.at(j).viewerCue) {
				arc.startsNearViewerCue = true;
				break;
			}
		}
		arc.evidence.append(QStringLiteral("candidate_arc_dp score:%1 startFeature:%2 endFeature:%3")
			.arg(QString::number(arc.score, 'f', 2))
			.arg(arc.firstFeature)
			.arg(arc.lastFeature));
		arcs.append(arc);
	}
	return arcs;
}

static bool rangesConflict(const PlannedArcCandidateRange &left, const PlannedArcCandidateRange &right)
{
	const double overlap = std::min(left.range.endSec, right.range.endSec) -
		std::max(left.range.startSec, right.range.startSec);
	return overlap > 0.0;
}

} // namespace

QVector<PlannedArcCandidateRange> CandidateArcPlanner::plan(const TranscriptIndex &index,
	const CandidateGenerationOptions &options) const
{
	QVector<PlannedArcCandidateRange> planned;
	const QVector<SegmentArcFeatures> features = buildFeatures(index, options);
	QVector<CompletedArc> arcs = recoverArcs(features, options);
	std::sort(arcs.begin(), arcs.end(), [](const CompletedArc &left, const CompletedArc &right) {
		if (std::fabs(left.score - right.score) > 0.0001)
			return left.score > right.score;
		return left.firstFeature < right.firstFeature;
	});

	for (const CompletedArc &arc : arcs) {
		if (arc.firstFeature < 0 || arc.lastFeature < arc.firstFeature || arc.lastFeature >= static_cast<int>(features.size()))
			continue;
		PlannedArcCandidateRange range;
		range.range = {features.at(arc.firstFeature).range.startSec, features.at(arc.lastFeature).range.endSec};
		range.range = index.clampRange(range.range, options.searchRange);
		range.source = QStringLiteral("arc_dp_candidate");
		range.startsNearViewerCue = arc.startsNearViewerCue;
		range.evidence = arc.evidence;
		bool conflict = false;
		for (const PlannedArcCandidateRange &existing : planned) {
			if (rangesConflict(existing, range)) {
				conflict = true;
				break;
			}
		}
		if (conflict)
			continue;
		planned.append(range);
		if (static_cast<int>(planned.size()) >= std::max(4, options.maxRawCandidates / 4))
			break;
	}
	return planned;
}

} // namespace Curation::Scoring
