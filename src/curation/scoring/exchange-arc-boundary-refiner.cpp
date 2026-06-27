#include "curation/scoring/exchange-arc-boundary-refiner.hpp"

#include "curation/scoring/arc-dp-boundary-refiner.hpp"
#include "curation/feedback/boundary-calibration.hpp"

#include "curation/scoring/semantic-prototypes.hpp"

#include <QStringList>

#include <algorithm>
#include <cmath>
#include <initializer_list>

using namespace Curation::Scoring;

namespace {

static constexpr double VIEWER_PRESET_ANALYSIS_LOOKBACK_SEC = 56.0;
static constexpr double VIEWER_PRESET_ANALYSIS_LOOKAHEAD_SEC = 36.0;
static constexpr int VIEWER_PRESET_MAX_CONTEXT_EXTENSIONS = 12;
static constexpr int DEFAULT_MAX_CONTEXT_EXTENSIONS = 3;

// DFS boundary recovery must be window-based, but still needs hard guards so
// broad semantic themes such as "mental health" do not let the search walk
// across multiple viewer messages/topics. These are independent from the
// existing clip duration limits: one limits origin recovery, the other limits
// answer follow-through.
static constexpr int VIEWER_DFS_MAX_LOOKBACK_WINDOWS = 12;
static constexpr double VIEWER_DFS_MAX_LOOKBACK_SEC = 45.0;
static constexpr int VIEWER_DFS_MAX_FORWARD_WINDOWS = 22;
static constexpr double VIEWER_DFS_MAX_FORWARD_SEC = 82.0;

static double boundedScore(double value)
{
	return std::clamp(value, 0.0, 1.0);
}

enum class ArcChunkRole {
	Unknown,
	OpeningCandidate,
	Development,
	LocalResolution,
	PreviousConclusion,
	SocialOrMetaPrelude,
	TailOrNewTurn
};

static QString roleName(ArcChunkRole role)
{
	switch (role) {
	case ArcChunkRole::OpeningCandidate:
		return QStringLiteral("opening");
	case ArcChunkRole::Development:
		return QStringLiteral("development");
	case ArcChunkRole::LocalResolution:
		return QStringLiteral("resolution");
	case ArcChunkRole::PreviousConclusion:
		return QStringLiteral("previous_conclusion");
	case ArcChunkRole::SocialOrMetaPrelude:
		return QStringLiteral("meta_prelude");
	case ArcChunkRole::TailOrNewTurn:
		return QStringLiteral("tail_or_new_turn");
	case ArcChunkRole::Unknown:
		break;
	}
	return QStringLiteral("unknown");
}

struct ArcChunk {
	int firstIndex = -1;
	int lastIndex = -1;
	double startSec = 0.0;
	double endSec = 0.0;
	QString text;
	SemanticEmbedding embedding;
	double pauseBeforeSec = 0.0;
	double pauseAfterSec = 0.0;
	double target = 0.0;
	double value = 0.0;
	double hook = 0.0;
	double resolution = 0.0;
	double meta = 0.0;
	double shift = 0.0;
	double openingScore = 0.0;
	double developmentScore = 0.0;
	double conclusionScore = 0.0;
	double defectScore = 0.0;
	double explicitOpeningCue = 0.0;
	double explicitDevelopmentCue = 0.0;
	double explicitConclusionCue = 0.0;
	double explicitMetaCue = 0.0;
	double explicitShiftCue = 0.0;
	double contextOpeningScore = 0.0;
	double contextDevelopmentScore = 0.0;
	double contextConclusionScore = 0.0;
	double contextPreviousTopicScore = 0.0;
	double contextMetaScore = 0.0;
	double contextNewTopicScore = 0.0;
	double contextCoherenceScore = 0.0;
	QString roleReason;
	ArcChunkRole role = ArcChunkRole::Unknown;
};

struct ArcSpanScore {
	int first = -1;
	int last = -1;
	double score = 0.0;
	double opening = 0.0;
	double development = 0.0;
	double conclusion = 0.0;
	double boundaryCleanliness = 0.0;
	double tailRisk = 0.0;
	double cohesion = 0.0;
};

struct ContextualArcScore {
	bool valid = false;
	bool implicitOpening = false;
	bool semanticOpeningFallback = false;
	bool terminalFollowThrough = false;
	QString reason;
	int openingIndex = -1;
	int firstDevelopmentIndex = -1;
	int conclusionIndex = -1;
	double score = 0.0;
	double opening = 0.0;
	double development = 0.0;
	double conclusion = 0.0;
	double penalty = 0.0;
};

static bool canUseSpan(const QVector<ArcChunk> &chunks, int first, int last,
	const ExchangeArcBoundaryRefinementOptions &options);
static ArcSpanScore scoreSpan(const QVector<ArcChunk> &chunks, int first, int last,
	const ExchangeArcBoundaryRefinementOptions &options);
static bool isSemanticPrelude(const ArcChunk &chunk);
static bool isBlockingArcRole(ArcChunkRole role);
static bool hasForwardContextualArcSupport(const QVector<ArcChunk> &chunks, int first);
static QString windowDfsGraphEvidence(const QVector<ArcChunk> &chunks, int first, int last);


#include "curation/scoring/internal/exchange-arc-boundary-refiner-viewer-cues.inc"
#include "curation/scoring/internal/exchange-arc-boundary-refiner-role-scoring.inc"
#include "curation/scoring/internal/exchange-arc-boundary-refiner-span-rules.inc"
#include "curation/scoring/internal/exchange-arc-boundary-refiner-recovery.inc"
#include "curation/scoring/internal/exchange-arc-boundary-refiner-evidence.inc"

} // namespace

ClipCandidate ExchangeArcBoundaryRefiner::refine(const TranscriptIndex &index, const ClipCandidate &candidate,
	const ExchangeArcBoundaryRefinementOptions &options) const
{
	const double originalDurationSec = candidate.range.endSec - candidate.range.startSec;
	const bool viewerPreset = options.scoring.presetId == QStringLiteral("viewer_message_response");
	auto skipped = [&candidate](const QString &reason) {
		ClipCandidate candidateWithArcEvidence = candidate;
		candidateWithArcEvidence.evidence.append(QStringLiteral("exchange_arc_role_classifier:skipped"));
		candidateWithArcEvidence.evidence.append(QStringLiteral("exchange_arc_refiner_skipped:%1").arg(reason));
		candidateWithArcEvidence.evidence.removeDuplicates();
		return candidateWithArcEvidence;
	};
	if (!viewerPreset && originalDurationSec <= std::max(options.generation.minDurationSec + 4.0, 20.0))
		return skipped(QStringLiteral("short_non_viewer_candidate"));
	if (!options.embeddingProvider)
		return skipped(QStringLiteral("missing_embedding_provider"));
	if (!options.embeddingProvider->isAvailable())
		return skipped(QStringLiteral("embedding_provider_unavailable"));

	ClipCandidate analysisCandidate = candidate;
	if (viewerPreset) {
		const ClipDuration expandedRange = index.clampRange(
			{std::max(options.generation.searchRange.startSec,
				 candidate.range.startSec - VIEWER_PRESET_ANALYSIS_LOOKBACK_SEC),
			 std::min(options.generation.searchRange.endSec,
				 candidate.range.endSec + VIEWER_PRESET_ANALYSIS_LOOKAHEAD_SEC)},
			options.generation.searchRange);
		if (expandedRange.endSec > expandedRange.startSec &&
		    (expandedRange.startSec < candidate.range.startSec - 0.5 ||
		     expandedRange.endSec > candidate.range.endSec + 0.5)) {
			analysisCandidate.range = expandedRange;
			analysisCandidate.firstSegmentIndex = index.firstSegmentIndexOverlapping(expandedRange);
			analysisCandidate.lastSegmentIndex = index.lastSegmentIndexOverlapping(expandedRange);
			analysisCandidate.text = index.textForRange(expandedRange).simplified();
		}
	}

	QVector<ArcChunk> chunks = chunksForCandidate(index, analysisCandidate, options);
	if (chunks.size() < 2)
		return skipped(QStringLiteral("not_enough_contextual_chunks"));
	scoreChunks(chunks, options);
	classifyContextualRoles(chunks, options);

	ArcSpanScore best;
	const double minDurationSec = std::max(8.0, options.generation.boundaryMinDurationSec);
	const double maxDurationSec = std::max(minDurationSec, options.generation.maxDurationSec);
	for (int first = 0; first < static_cast<int>(chunks.size()); ++first) {
		for (int last = first; last < static_cast<int>(chunks.size()); ++last) {
			const double durationSec = chunks.at(last).endSec - chunks.at(first).startSec;
			if (durationSec < minDurationSec)
				continue;
			if (durationSec > maxDurationSec)
				break;
			const QString spanText = index.textForSegmentWindow(chunks.at(first).firstIndex, chunks.at(last).lastIndex);
			if (spanText.trimmed().size() < std::max(36, options.qualityGate.minTextChars / 2))
				continue;
			const ArcSpanScore candidateScore = scoreSpan(chunks, first, last, options);
			const bool betterScore = candidateScore.score > best.score + 0.012;
			const bool bestHasBadViewerStart = viewerPreset && best.first >= 0 &&
				(isSemanticPrelude(chunks.at(best.first)) ||
				 isStaleViewerLeadingResolution(chunks, best.first) ||
				 isViewerStartBoundaryContaminated(chunks, best.first));
			const bool canPreferLaterOpening = !viewerPreset || bestHasBadViewerStart;
			const bool similarCleanerOpening = canPreferLaterOpening &&
				std::fabs(candidateScore.score - best.score) <= (bestHasBadViewerStart ? 0.055 : 0.018) &&
				best.first >= 0 && first > best.first &&
				(candidateScore.opening >= best.opening + (bestHasBadViewerStart ? 0.010 : 0.030) ||
				 candidateScore.boundaryCleanliness >= best.boundaryCleanliness + 0.020) &&
				candidateScore.development >= best.development - 0.040 &&
				candidateScore.conclusion >= best.conclusion - 0.050 &&
				candidateScore.tailRisk <= best.tailRisk + 0.050;
			const bool similarShorterCleanerEnding = std::fabs(candidateScore.score - best.score) <= 0.020 &&
				best.first >= 0 && first == best.first && last < best.last &&
				candidateScore.conclusion >= best.conclusion - 0.010 && candidateScore.tailRisk <= best.tailRisk + 0.015;
			const bool addsCleanViewerContext = viewerPreset && best.first >= 0 && first < best.first &&
				!isStaleViewerLeadingResolution(chunks, first) &&
				!isViewerStartBoundaryContaminated(chunks, first) &&
				addsUsefulViewerOpeningContext(chunks, first, best.first);
			const bool similarEarlierViewerContext = viewerPreset && best.first >= 0 && first < best.first &&
				last >= best.last && candidateScore.score + 0.032 >= best.score &&
				candidateScore.development >= best.development - 0.035 &&
				candidateScore.conclusion >= best.conclusion - 0.045 &&
				candidateScore.tailRisk <= best.tailRisk + 0.050 && addsCleanViewerContext;
			if (betterScore || similarCleanerOpening || similarShorterCleanerEnding || similarEarlierViewerContext)
				best = candidateScore;
		}
	}

	RecoveredSubspan recoveredSubspan;
	const bool bestHasValidContextualArc = !viewerPreset ||
		(best.first >= 0 && best.last >= best.first && contextualStateMachineScore(chunks, best.first, best.last, options).valid);
	if (best.first < 0 || best.last < best.first || best.score < 0.34 || !bestHasValidContextualArc) {
		recoveredSubspan = recoverViewerMessageArcByWindowDfs(chunks, options);
		if (!recoveredSubspan.valid)
			recoveredSubspan = recoverSemanticHookSubspan(chunks, options);
		if (recoveredSubspan.valid) {
			best = recoveredSubspan.span;
		} else {
			ArcSpanScore bestRejectedArcSpan;
			for (int first = 0; first < static_cast<int>(chunks.size()); ++first) {
				for (int last = first; last < static_cast<int>(chunks.size()); ++last) {
					if (!canUseSpan(chunks, first, last, options))
						continue;
					const ArcSpanScore probe = scoreSpan(chunks, first, last, options);
					if (bestRejectedArcSpan.first < 0 || probe.score > bestRejectedArcSpan.score)
						bestRejectedArcSpan = probe;
				}
			}
			ClipCandidate candidateWithArcEvidence = candidate;
			if (bestRejectedArcSpan.first >= 0 && bestRejectedArcSpan.last >= bestRejectedArcSpan.first) {
				candidateWithArcEvidence.evidence.append(QStringLiteral("exchange_arc_role_classifier:v32_window_dfs"));
				candidateWithArcEvidence.evidence.append(QStringLiteral("exchange_arc_window_dfs_limits:back12_45s_forward22_82s_split_origin_graph_intra"));
				candidateWithArcEvidence.evidence.append(QStringLiteral("exchange_arc_role_classifier:v23_context_window_dp"));
				candidateWithArcEvidence.evidence.append(roleEvidence(chunks, bestRejectedArcSpan.first, bestRejectedArcSpan.last));
				candidateWithArcEvidence.evidence.append(roleReasonEvidence(chunks, bestRejectedArcSpan.first, bestRejectedArcSpan.last));
				candidateWithArcEvidence.evidence.append(contextualRoleScoreEvidence(chunks, bestRejectedArcSpan.first, bestRejectedArcSpan.last));
				candidateWithArcEvidence.evidence.append(windowDfsGraphEvidence(chunks, bestRejectedArcSpan.first, bestRejectedArcSpan.last));
				candidateWithArcEvidence.evidence.append(stateMachineEvidence(chunks, bestRejectedArcSpan.first, bestRejectedArcSpan.last, options));
				candidateWithArcEvidence.evidence.append(QStringLiteral("exchange_arc_window_dfs_no_valid_origin_or_answer"));
				candidateWithArcEvidence.evidence.append(QStringLiteral("exchange_arc_no_valid_subspan"));
				candidateWithArcEvidence.evidence.append(arcEvidence(bestRejectedArcSpan));
				candidateWithArcEvidence.evidence.append(boundaryEvidence(chunks, bestRejectedArcSpan.first, bestRejectedArcSpan.last));
				candidateWithArcEvidence.evidence.removeDuplicates();
			} else {
				candidateWithArcEvidence.evidence.append(QStringLiteral("exchange_arc_role_classifier:v32_window_dfs"));
				candidateWithArcEvidence.evidence.append(QStringLiteral("exchange_arc_window_dfs_limits:back12_45s_forward22_82s_split_origin_graph_intra"));
				candidateWithArcEvidence.evidence.append(QStringLiteral("exchange_arc_role_classifier:v23_context_window_dp"));
				candidateWithArcEvidence.evidence.append(QStringLiteral("exchange_arc_no_scoreable_span"));
				candidateWithArcEvidence.evidence.append(QStringLiteral("exchange_arc_state_machine:invalid penalty:0.99 reason:no_scoreable_span"));
				candidateWithArcEvidence.evidence.removeDuplicates();
			}
			return candidateWithArcEvidence;
		}
	}

	int adjustedFirst = best.first;
	int adjustedLast = best.last;
	bool trimmedOpeningMeta = false;
	bool trimmedWeakOpeningPrelude = false;
	bool trimmedStaleOpeningResolution = false;
	bool extendedTailContinuation = false;
	bool trimmedHardNoisyEnding = false;
	bool trimmedLongPauseTail = false;
	bool blockedTailByLongPause = false;
	bool extendedStartContext = false;
	bool trimmedFirstResolutionTail = false;
	bool extendedViewerConclusionFollowThrough = false;
	const bool usedRecoveredSubspan = recoveredSubspan.valid;

	while (adjustedFirst < adjustedLast && isSemanticPrelude(chunks.at(adjustedFirst)) &&
	       canUseSpan(chunks, adjustedFirst + 1, adjustedLast, options)) {
		++adjustedFirst;
		trimmedOpeningMeta = true;
	}
	while (!viewerPreset && adjustedFirst < adjustedLast && isWeakOpeningPrelude(chunks, adjustedFirst) &&
	       canUseSpan(chunks, adjustedFirst + 1, adjustedLast, options)) {
		++adjustedFirst;
		trimmedWeakOpeningPrelude = true;
	}
	while (viewerPreset && adjustedFirst < adjustedLast &&
	       (isStaleViewerLeadingResolution(chunks, adjustedFirst) ||
		isViewerStartBoundaryContaminated(chunks, adjustedFirst)) &&
	       canUseSpan(chunks, adjustedFirst + 1, adjustedLast, options)) {
		++adjustedFirst;
		trimmedStaleOpeningResolution = true;
	}

	bool trimmedToViewerMessageBoundary = false;
	bool prependedViewerMessageCue = false;
	bool trimmedAtNextViewerMessage = false;
	if (viewerPreset) {
		const int targetedCue = bestTargetedViewerCueInsideSpan(chunks, adjustedFirst, adjustedLast, options);
		if (targetedCue > adjustedFirst && canUseSpan(chunks, targetedCue, adjustedLast, options)) {
			adjustedFirst = targetedCue;
			trimmedToViewerMessageBoundary = true;
		}
		if (!hasReliableViewerCueNearStart(chunks, adjustedFirst, adjustedLast)) {
			const int previousCue = nearestTargetedViewerCueBefore(chunks, adjustedFirst, adjustedLast, options);
			if (previousCue >= 0) {
				adjustedFirst = previousCue;
				prependedViewerMessageCue = true;
			}
		}
		const int nextCue = nextViewerCueInsideArc(chunks, adjustedFirst, adjustedLast);
		if (nextCue > adjustedFirst && canUseSpan(chunks, adjustedFirst, nextCue - 1, options)) {
			adjustedLast = nextCue - 1;
			trimmedAtNextViewerMessage = true;
		}
	}

	int contextExtensions = 0;
	const int maxContextExtensions = viewerPreset ? VIEWER_PRESET_MAX_CONTEXT_EXTENSIONS : DEFAULT_MAX_CONTEXT_EXTENSIONS;
	while (contextExtensions < maxContextExtensions && adjustedFirst > 0 &&
	       (shouldPrependOpeningContext(chunks, adjustedFirst) ||
		(viewerPreset && addsUsefulViewerOpeningContext(chunks, adjustedFirst - 1, adjustedFirst))) &&
	       isEssentialOpeningContext(chunks.at(adjustedFirst - 1), chunks.at(adjustedFirst)) &&
	       (!viewerPreset || (!isStaleViewerLeadingResolution(chunks, adjustedFirst - 1) &&
				  !isViewerStartBoundaryContaminated(chunks, adjustedFirst - 1))) &&
	       canUseSpan(chunks, adjustedFirst - 1, adjustedLast, options)) {
		--adjustedFirst;
		++contextExtensions;
		extendedStartContext = true;
	}

	int tailExtensions = 0;
	double viewerTailExtensionSec = 0.0;
	while (tailExtensions < (viewerPreset ? 10 : 6) && adjustedLast + 1 < static_cast<int>(chunks.size())) {
		if (viewerPreset && looksLikeAnyViewerMessageTurn(chunks.at(adjustedLast + 1)))
			break;
		if (viewerPreset && isHardWindowTopicDrift(chunks.at(adjustedLast), chunks.at(adjustedLast + 1)) &&
		    !isModeratePauseSameViewerAnswer(chunks.at(adjustedLast), chunks.at(adjustedLast + 1)))
			break;
		const bool normalContinuation = isSemanticContinuation(chunks, adjustedLast, adjustedLast + 1);
		const bool viewerContinuation = viewerPreset && isViewerAnswerContinuation(chunks, adjustedLast, adjustedLast + 1) &&
			viewerTailExtensionSec + std::max(0.0, chunks.at(adjustedLast + 1).endSec - chunks.at(adjustedLast + 1).startSec) <= 22.0;
		if ((!normalContinuation && !viewerContinuation) ||
		    !canUseSpan(chunks, adjustedFirst, adjustedLast + 1, options))
			break;
		viewerTailExtensionSec += std::max(0.0, chunks.at(adjustedLast + 1).endSec - chunks.at(adjustedLast + 1).startSec);
		++adjustedLast;
		++tailExtensions;
		extendedTailContinuation = true;
	}

	while (adjustedLast > adjustedFirst && isPauseSeparatedSemanticTurn(chunks.at(adjustedLast - 1), chunks.at(adjustedLast)) &&
	       canUseSpan(chunks, adjustedFirst, adjustedLast - 1, options)) {
		--adjustedLast;
		trimmedLongPauseTail = true;
	}
	while (adjustedLast > adjustedFirst && isTailRisk(chunks, adjustedLast) &&
	       canUseSpan(chunks, adjustedFirst, adjustedLast - 1, options)) {
		--adjustedLast;
		trimmedHardNoisyEnding = true;
	}
	const int earlyConclusionEnd = firstLocalResolutionEnd(chunks, adjustedFirst, adjustedLast, options);
	if (earlyConclusionEnd >= adjustedFirst && earlyConclusionEnd < adjustedLast) {
		adjustedLast = earlyConclusionEnd;
		trimmedFirstResolutionTail = true;
		while (viewerPreset && adjustedLast + 1 < static_cast<int>(chunks.size()) &&
		       !looksLikeAnyViewerMessageTurn(chunks.at(adjustedLast + 1)) &&
		       (isViewerConclusionFollowThrough(chunks, adjustedLast, adjustedLast + 1) ||
			shouldExtendViewerConclusionFollowThroughSoft(chunks, adjustedLast, adjustedLast + 1) ||
			isViewerAnswerContinuation(chunks, adjustedLast, adjustedLast + 1)) &&
		       canUseSpan(chunks, adjustedFirst, adjustedLast + 1, options) &&
		       chunks.at(adjustedLast + 1).endSec - chunks.at(earlyConclusionEnd).endSec <= 22.0) {
			++adjustedLast;
			extendedViewerConclusionFollowThrough = true;
		}
	}
	if (adjustedLast + 1 < static_cast<int>(chunks.size()) &&
	    isPauseSeparatedSemanticTurn(chunks.at(adjustedLast), chunks.at(adjustedLast + 1)))
		blockedTailByLongPause = true;

	ArcSpanScore adjustedScore = scoreSpan(chunks, adjustedFirst, adjustedLast, options);
	if (adjustedScore.first < 0 || adjustedScore.last < adjustedScore.first)
		adjustedScore = best;
	if (usedRecoveredSubspan) {
		// The regular span scorer calls the strict contextual state machine again. A DP-recovered
		// subspan is already a validated contextual arc, so do not let the strict scorer overwrite
		// its arc metrics with zero and make the quality gate reject it as missing_contextual_arc.
		// Keep any improved boundary/tail numbers from the regular scorer, but preserve the
		// recovered opening/development/conclusion/completeness contract expected by the gate.
		adjustedScore.first = adjustedFirst;
		adjustedScore.last = adjustedLast;
		adjustedScore.opening = std::max(adjustedScore.opening, recoveredSubspan.opening);
		adjustedScore.development = std::max(adjustedScore.development, recoveredSubspan.development);
		adjustedScore.conclusion = std::max(adjustedScore.conclusion, recoveredSubspan.conclusion);
		adjustedScore.cohesion = std::max(adjustedScore.cohesion, recoveredSubspan.span.cohesion);
		adjustedScore.boundaryCleanliness = std::max(adjustedScore.boundaryCleanliness,
			recoveredSubspan.span.boundaryCleanliness);
		adjustedScore.tailRisk = adjustedScore.tailRisk > 0.0
			? std::min(adjustedScore.tailRisk, recoveredSubspan.span.tailRisk)
			: recoveredSubspan.span.tailRisk;
		adjustedScore.score = std::max(adjustedScore.score, recoveredSubspan.stateMachineScore);
	}

	ClipDuration refinedRange{chunks.at(adjustedFirst).startSec, chunks.at(adjustedLast).endSec};
	refinedRange = index.clampRange(refinedRange, options.generation.searchRange);
	bool intraWindowViewerOriginTrimmed = false;
	bool transcriptWindowTailExtended = false;
	if (viewerPreset) {
		const ClipDuration originSearchRange = index.clampRange(
			{std::max(options.generation.searchRange.startSec, refinedRange.startSec - 1.5),
			 std::min(options.generation.searchRange.endSec, refinedRange.endSec + 2.0)},
			options.generation.searchRange);
		const double intraOriginStart = findIntraWindowViewerOriginStartSec(index, originSearchRange);
		if (intraOriginStart >= refinedRange.startSec + 1.0 &&
		    refinedRange.endSec - intraOriginStart >= std::max(6.0, minDurationSec - 4.0)) {
			refinedRange.startSec = intraOriginStart;
			intraWindowViewerOriginTrimmed = true;
		}
		if (intraWindowViewerOriginTrimmed || usedRecoveredSubspan) {
			const double extendedEnd = extendViewerRangeEndByTranscriptWindows(index, refinedRange, options);
			if (extendedEnd > refinedRange.endSec + 1.0) {
				refinedRange.endSec = extendedEnd;
				transcriptWindowTailExtended = true;
			}
		}
	}
	bool snappedToWordBoundary = false;
	if (index.hasWordTimings()) {
		const ClipDuration wordSnapped = index.snapRangeToWordBoundaries(refinedRange, options.generation.searchRange);
		snappedToWordBoundary = std::fabs(wordSnapped.startSec - refinedRange.startSec) > 0.08 ||
			std::fabs(wordSnapped.endSec - refinedRange.endSec) > 0.08;
		refinedRange = wordSnapped;
	}
	const double refinedDurationSec = refinedRange.endSec - refinedRange.startSec;
	if (refinedDurationSec < minDurationSec || refinedDurationSec > maxDurationSec + 0.1) {
		ClipCandidate candidateWithArcEvidence = candidate;
		candidateWithArcEvidence.evidence.append(QStringLiteral("exchange_arc_role_classifier:v23_context_window_dp"));
		candidateWithArcEvidence.evidence.append(QStringLiteral("exchange_arc_refiner_skipped:refined_duration_out_of_bounds"));
		candidateWithArcEvidence.evidence.append(arcEvidence(adjustedScore));
		candidateWithArcEvidence.evidence.append(roleEvidence(chunks, adjustedFirst, adjustedLast));
		candidateWithArcEvidence.evidence.append(roleReasonEvidence(chunks, adjustedFirst, adjustedLast));
		candidateWithArcEvidence.evidence.append(contextualRoleScoreEvidence(chunks, adjustedFirst, adjustedLast));
		candidateWithArcEvidence.evidence.append(windowDfsGraphEvidence(chunks, adjustedFirst, adjustedLast));
		candidateWithArcEvidence.evidence.append(stateMachineEvidence(chunks, adjustedFirst, adjustedLast, options));
		candidateWithArcEvidence.evidence.removeDuplicates();
		return candidateWithArcEvidence;
	}

	ClipCandidate refined = candidate;
	writeArcScores(refined, adjustedScore);
	const bool materiallyChanged = std::fabs(refinedRange.startSec - candidate.range.startSec) > 1.0 ||
		std::fabs(refinedRange.endSec - candidate.range.endSec) > 1.0;
	if (materiallyChanged) {
		refined.range = refinedRange;
		refined.firstSegmentIndex = index.firstSegmentIndexOverlapping(refined.range);
		refined.lastSegmentIndex = index.lastSegmentIndexOverlapping(refined.range);
		refined.text = index.textForRange(refined.range).simplified();
		refined.timedText = index.timedTextForRange(refined.range);
		refined.anchorText = refined.text.left(220);
		refined.evidence.append(QStringLiteral("exchange_arc_refined"));
		if (std::fabs(refinedRange.startSec - candidate.range.startSec) > 1.0)
			refined.evidence.append(QStringLiteral("exchange_arc_start_refined"));
		if (std::fabs(refinedRange.endSec - candidate.range.endSec) > 1.0)
			refined.evidence.append(QStringLiteral("exchange_arc_end_refined"));
		if (refinedRange.startSec < candidate.range.startSec - 1.0)
			refined.evidence.append(QStringLiteral("exchange_arc_context_start_extended"));
		if (extendedStartContext && refinedRange.startSec < candidate.range.startSec - 0.5)
			refined.evidence.append(QStringLiteral("exchange_arc_essential_context_prepended"));
		if (refinedRange.endSec > candidate.range.endSec + 1.0)
			refined.evidence.append(QStringLiteral("exchange_arc_conclusion_extended"));
		if (snappedToWordBoundary)
			refined.evidence.append(QStringLiteral("exchange_arc_word_boundary_snapped"));
	} else {
		refined.evidence.append(QStringLiteral("exchange_arc_kept"));
		if (snappedToWordBoundary)
			refined.evidence.append(QStringLiteral("exchange_arc_word_boundary_snapped"));
	}

	if (trimmedOpeningMeta)
		refined.evidence.append(QStringLiteral("exchange_arc_opening_meta_trimmed"));
	if (trimmedWeakOpeningPrelude)
		refined.evidence.append(QStringLiteral("exchange_arc_weak_opening_prelude_trimmed"));
	if (trimmedStaleOpeningResolution)
		refined.evidence.append(QStringLiteral("exchange_arc_stale_opening_resolution_trimmed"));
	if (trimmedToViewerMessageBoundary)
		refined.evidence.append(QStringLiteral("exchange_arc_viewer_message_boundary_trimmed"));
	if (prependedViewerMessageCue)
		refined.evidence.append(QStringLiteral("exchange_arc_viewer_message_cue_prepended"));
	if (trimmedAtNextViewerMessage)
		refined.evidence.append(QStringLiteral("exchange_arc_next_viewer_message_trimmed"));
	if (extendedTailContinuation)
		refined.evidence.append(QStringLiteral("exchange_arc_tail_continuation"));
	if (trimmedHardNoisyEnding)
		refined.evidence.append(QStringLiteral("exchange_arc_hard_topic_break_trimmed"));
	if (trimmedLongPauseTail)
		refined.evidence.append(QStringLiteral("exchange_arc_pause_tail_trimmed"));
	if (trimmedFirstResolutionTail)
		refined.evidence.append(QStringLiteral("exchange_arc_first_resolution_tail_trimmed"));
	if (extendedViewerConclusionFollowThrough)
		refined.evidence.append(QStringLiteral("exchange_arc_viewer_conclusion_followthrough_extended"));
	if (blockedTailByLongPause)
		refined.evidence.append(QStringLiteral("exchange_arc_pause_tail_blocked"));
	if (intraWindowViewerOriginTrimmed)
		refined.evidence.append(QStringLiteral("exchange_arc_intra_window_viewer_origin_trimmed"));
	if (transcriptWindowTailExtended)
		refined.evidence.append(QStringLiteral("exchange_arc_transcript_window_tail_extended"));
	refined.evidence.append(QStringLiteral("exchange_arc_refined_candidate_range:%1-%2")
		.arg(QString::number(refinedRange.startSec, 'f', 2), QString::number(refinedRange.endSec, 'f', 2)));
	refined.evidence.append(QStringLiteral("exchange_arc_role_classifier:v32_window_dfs"));
	refined.evidence.append(QStringLiteral("exchange_arc_window_dfs_limits:back12_45s_forward22_82s_split_origin_graph_intra"));
	refined.evidence.append(QStringLiteral("exchange_arc_role_classifier:v23_context_window_dp"));
	if (usedRecoveredSubspan) {
		refined.evidence.append(QStringLiteral("exchange_arc_semantic_hook_subspan_recovered"));
		if (recoveredSubspan.reason == QStringLiteral("contextual_role_dp_subspan"))
			refined.evidence.append(QStringLiteral("exchange_arc_contextual_dp_subspan_recovered"));
		if (recoveredSubspan.reason == QStringLiteral("viewer_message_window_dfs_arc"))
			refined.evidence.append(QStringLiteral("exchange_arc_viewer_message_window_dfs_recovered"));
		if (!recoveredSubspan.graphEvidence.isEmpty())
			refined.evidence.append(recoveredSubspan.graphEvidence);
		QStringList recoveryFlags;
		if (recoveredSubspan.semanticOpening)
			recoveryFlags.append(QStringLiteral("semantic_opening"));
		recoveryFlags.append(recoveredSubspan.reason.isEmpty() ? QStringLiteral("subspan_recovered") : recoveredSubspan.reason);
		recoveryFlags.append(QStringLiteral("subspan_recovered"));
		refined.evidence.append(QStringLiteral("exchange_arc_state_machine:valid score:%1 opening:%2 development:%3 conclusion:%4 flags:%5")
			.arg(QString::number(recoveredSubspan.stateMachineScore, 'f', 2),
			     QString::number(recoveredSubspan.opening, 'f', 2),
			     QString::number(recoveredSubspan.development, 'f', 2),
			     QString::number(recoveredSubspan.conclusion, 'f', 2),
			     recoveryFlags.join(QLatin1Char(','))));
	} else {
		refined.evidence.append(stateMachineEvidence(chunks, adjustedFirst, adjustedLast, options));
	}
	refined.evidence.append(arcEvidence(adjustedScore));
	refined.evidence.append(roleEvidence(chunks, adjustedFirst, adjustedLast));
	refined.evidence.append(roleReasonEvidence(chunks, adjustedFirst, adjustedLast));
	refined.evidence.append(contextualRoleScoreEvidence(chunks, adjustedFirst, adjustedLast));
	if (viewerPreset)
		refined.evidence.append(windowDfsGraphEvidence(chunks, adjustedFirst, adjustedLast));
	refined.evidence.append(boundaryEvidence(chunks, adjustedFirst, adjustedLast));
	if (viewerPreset && hasReliableViewerCueNearStart(chunks, adjustedFirst, adjustedLast))
		refined.evidence.append(QStringLiteral("exchange_arc_viewer_message_cue_confirmed"));

	ArcDpBoundaryRefiner dpRefiner;
	ArcDpBoundaryRefinerOptions dpOptions;
	dpOptions.searchRange = options.generation.searchRange;
	dpOptions.presetId = options.generation.presetId;
	dpOptions.mainTarget = options.generation.mainTarget;
	dpOptions.reliableMainTarget = options.generation.reliableMainTarget;
	dpOptions.minDurationSec = std::max(8.0, options.generation.boundaryMinDurationSec);
	dpOptions.maxDurationSec = std::max(options.generation.boundaryMinDurationSec, options.generation.maxDurationSec);
	dpOptions.lookbackSec = viewerPreset ? VIEWER_PRESET_ANALYSIS_LOOKBACK_SEC : 32.0;
	dpOptions.lookaheadSec = viewerPreset ? VIEWER_PRESET_ANALYSIS_LOOKAHEAD_SEC : 24.0;
	const Curation::Feedback::BoundaryCalibrationProfile calibration =
		Curation::Feedback::BoundaryCalibration::profileForPreset(dpOptions.presetId);
	if (calibration.loaded) {
		if (calibration.maxQuestionLookbackSec > 0.0)
			dpOptions.lookbackSec = std::max(dpOptions.lookbackSec, calibration.maxQuestionLookbackSec);
		if (calibration.lookaheadSec > 0.0)
			dpOptions.lookaheadSec = std::max(dpOptions.lookaheadSec, calibration.lookaheadSec);
		dpOptions.contextWeight = calibration.contextWeight;
		dpOptions.hookWeight = calibration.hookWeight;
		dpOptions.developmentWeight = calibration.developmentWeight;
		dpOptions.resolutionWeight = calibration.resolutionWeight;
		dpOptions.targetWeight = calibration.targetWeight;
		dpOptions.defectPenalty = calibration.defectPenalty;
		if (calibration.minArcConfidence >= 0.0)
			dpOptions.minArcConfidence = calibration.minArcConfidence;
		refined.evidence.append(QStringLiteral("boundary_calibration_applied preset:%1 lookback:%2 lookahead:%3")
			.arg(dpOptions.presetId, QString::number(dpOptions.lookbackSec, 'f', 0),
			     QString::number(dpOptions.lookaheadSec, 'f', 0)));
	}
	refined = dpRefiner.refine(index, refined, dpOptions);
	refined.evidence.removeDuplicates();
	return refined;
}
