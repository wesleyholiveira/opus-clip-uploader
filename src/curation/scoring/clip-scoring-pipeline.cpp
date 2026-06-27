#include "curation/scoring/clip-scoring-pipeline.hpp"

#include "curation/feedback/curation-feedback-store.hpp"
#include "curation/scoring/candidate-stage-limiter.hpp"
#include "curation/scoring/candidate-beam-expander.hpp"
#include "curation/scoring/candidate-builder.hpp"
#include "curation/scoring/cheap-clip-scorer.hpp"
#include "curation/scoring/candidate-source-builder.hpp"
#include "curation/scoring/boundary-refinement-stage.hpp"
#include "curation/scoring/feedback-aware-interval-selector.hpp"
#include "curation/scoring/feedback-consistency-gate.hpp"
#include "curation/scoring/feedback-guided-candidate-stage.hpp"
#include "curation/scoring/feedback-similarity-scorer.hpp"
#include "curation/scoring/feedback-trained-ranker.hpp"
#include "curation/scoring/viewer-arc-gate.hpp"
#include "curation/scoring/clip-scoring-pipeline-summary.hpp"
#include "curation/scoring/pipeline-progress.hpp"

#include <QStringList>
#include <QSet>
#include <QElapsedTimer>

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

#include <algorithm>
#include <cmath>
#include <utility>
#include <future>
#include <thread>
#include <vector>
#include <initializer_list>

using namespace Curation::Scoring;

namespace {

static bool isViewerMessagePreset(const ClipScoringPipelineOptions &options);
static CandidateSourceBuilderOptions candidateSourceOptionsFromOptions(const ClipScoringPipelineOptions &options);

static bool isSelectionEvidence(const QString &evidence)
{
	return evidence == QStringLiteral("mmr_diversity_selected") || evidence.startsWith(QStringLiteral("selected_rank:")) ||
	       evidence.startsWith(QStringLiteral("mmr:")) || evidence.startsWith(QStringLiteral("mmr_similarity:"));
}

static void clearSelectionMetadata(QVector<ClipCandidate> &candidates)
{
	for (ClipCandidate &candidate : candidates) {
		candidate.selectedRank = 0;
		candidate.selectedMmrScore = 0.0;
		candidate.selectedMmrSimilarity = 0.0;
		candidate.evidence.erase(std::remove_if(candidate.evidence.begin(), candidate.evidence.end(), isSelectionEvidence),
					     candidate.evidence.end());
	}
}

static int usableCandidateCount(const QVector<ClipCandidate> &candidates)
{
	return static_cast<int>(std::count_if(candidates.constBegin(), candidates.constEnd(), [](const ClipCandidate &candidate) {
		return candidate.range.endSec > candidate.range.startSec && !candidate.text.trimmed().isEmpty() &&
		       !candidate.rejectedByQualityGate && !candidate.rejectedAsNoise;
	}));
}

static int rejectedCandidateCount(const QVector<ClipCandidate> &candidates)
{
	return static_cast<int>(std::count_if(candidates.constBegin(), candidates.constEnd(), [](const ClipCandidate &candidate) {
		return candidate.rejectedByQualityGate || candidate.rejectedAsNoise;
	}));
}

static int positiveFeedbackCandidateCount(const QVector<ClipCandidate> &candidates)
{
	return static_cast<int>(std::count_if(candidates.constBegin(), candidates.constEnd(), [](const ClipCandidate &candidate) {
		return candidate.source.contains(QStringLiteral("feedback_positive")) ||
		       candidate.evidence.contains(QStringLiteral("feedback_positive_exact_seed_preserved")) ||
		       candidate.evidence.contains(QStringLiteral("complete_viewer_arc_gate_passed_by_user_feedback")) ||
		       candidate.evidence.contains(QStringLiteral("complete_viewer_arc_gate_passed_by_feedback_trained_ranker")) ||
		       candidate.evidence.contains(QStringLiteral("feedback_trained_ranker_strong_accept")) ||
		       candidate.evidence.contains(QStringLiteral("feedback_trained_ranker_accept"));
	}));
}

static int learnedFeedbackAcceptedCandidateCount(const QVector<ClipCandidate> &candidates)
{
	return static_cast<int>(std::count_if(candidates.constBegin(), candidates.constEnd(), [](const ClipCandidate &candidate) {
		return candidate.evidence.contains(QStringLiteral("complete_viewer_arc_gate_passed_by_feedback_trained_ranker")) ||
		       candidate.evidence.contains(QStringLiteral("feedback_trained_ranker_strong_accept")) ||
		       candidate.evidence.contains(QStringLiteral("feedback_trained_ranker_accept"));
	}));
}


static bool isRejectedCandidate(const ClipCandidate &candidate)
{
	return candidate.rejectedByQualityGate || candidate.rejectedAsNoise;
}

static bool candidateHasEvidence(const ClipCandidate &candidate, const QString &needle)
{
	for (const QString &evidence : candidate.evidence) {
		if (evidence == needle || evidence.contains(needle))
			return true;
	}
	return false;
}

static bool isExactPositiveFeedbackSeed(const ClipCandidate &candidate)
{
	return candidate.source == QStringLiteral("feedback_positive_exact_seed") ||
	       candidate.source == QStringLiteral("feedback_positive_seed") ||
	       candidateHasEvidence(candidate, QStringLiteral("feedback_positive_exact_seed")) ||
	       candidateHasEvidence(candidate, QStringLiteral("feedback_positive_seed"));
}

static bool isUnsafeArcBypass(const ClipCandidate &candidate)
{
	const bool directPositiveBypass =
		candidateHasEvidence(candidate, QStringLiteral("complete_viewer_arc_gate_passed_by_direct_positive_feedback"));
	const bool selfContainedAdviceBypass =
		candidateHasEvidence(candidate, QStringLiteral("complete_viewer_arc_gate_passed_by_self_contained_advice_feedback"));
	if (!directPositiveBypass && !selfContainedAdviceBypass)
		return false;
	if (isExactPositiveFeedbackSeed(candidate))
		return false;

	const bool clearedPriorQualityRejection =
		candidateHasEvidence(candidate, QStringLiteral("arc_gate_cleared_prior_quality_rejection"));
	const bool missingArcOrOrigin =
		candidateHasEvidence(candidate, QStringLiteral("quality_rejected:missing_contextual_arc")) ||
		candidateHasEvidence(candidate, QStringLiteral("arc_gate_cleared_prior_quality_rejection_from:missing_contextual_arc")) ||
		candidateHasEvidence(candidate, QStringLiteral("exchange_arc_window_dfs_no_valid_origin_or_answer")) ||
		candidateHasEvidence(candidate, QStringLiteral("exchange_arc_no_valid_subspan")) ||
		candidateHasEvidence(candidate, QStringLiteral("reason:missing_viewer_message_cue")) ||
		candidateHasEvidence(candidate, QStringLiteral("reason:missing_explicit_opening"));
	const bool noArcShape = candidate.scores.arcCompleteness <= 0.08 &&
		candidate.scores.arcOpening <= 0.08 && candidate.scores.arcDevelopment <= 0.08 &&
		candidate.scores.arcConclusion <= 0.08;
	return missingArcOrOrigin && (clearedPriorQualityRejection || noArcShape);
}

static ClipCandidate demoteUnsafeArcBypass(ClipCandidate candidate)
{
	candidate.rejectedByQualityGate = true;
	candidate.qualityGateChecked = true;
	candidate.rejectionReason = QStringLiteral("incomplete_viewer_arc");
	candidate.scores.qualityGate = std::min(candidate.scores.qualityGate, 0.05);
	candidate.scores.final = std::min(candidate.scores.final, 0.08);
	candidate.evidence.append(QStringLiteral("arc_bypass_demoted_to_diagnostic"));
	candidate.evidence.append(QStringLiteral("review_marker_blocked_missing_contextual_arc"));
	candidate.evidence.removeDuplicates();
	return candidate;
}

static bool hasSimilarDiagnosticRange(const QVector<ClipCandidate> &diagnostics, const ClipDuration &range);

static double candidateEvidenceScoreValue(const ClipCandidate &candidate, const QString &prefix)
{
	for (const QString &evidence : candidate.evidence) {
		if (!evidence.startsWith(prefix))
			continue;
		bool ok = false;
		const double value = evidence.mid(prefix.size()).toDouble(&ok);
		if (ok)
			return value;
	}
	return 0.0;
}

static bool hasHardArcBlockerEvidence(const ClipCandidate &candidate)
{
	return candidateHasEvidence(candidate, QStringLiteral("arc_gate_hard_context_blocker")) ||
	       candidateHasEvidence(candidate, QStringLiteral("multiple_viewer_messages_inside_arc")) ||
	       candidateHasEvidence(candidate, QStringLiteral("topic_shift_before_resolution"));
}

static bool isRecoverableIncompleteArcDiagnostic(const ClipCandidate &candidate)
{
	const double duration = candidate.range.endSec - candidate.range.startSec;
	if (duration < 16.0 || duration > 180.0 || candidate.text.trimmed().size() < 80)
		return false;
	if (candidate.rejectionReason == QStringLiteral("too_short") || candidate.rejectedAsNoise ||
	    candidateHasEvidence(candidate, QStringLiteral("feedback_negative_suppressed")) ||
	    candidateHasEvidence(candidate, QStringLiteral("feedback_consistency_rejected")) ||
	    hasHardArcBlockerEvidence(candidate))
		return false;

	const bool recoverableReason = candidate.rejectionReason == QStringLiteral("incomplete_viewer_arc") ||
		candidate.rejectionReason == QStringLiteral("novelty_exploration_review_required");
	if (!recoverableReason)
		return false;

	const double positiveRange = candidateEvidenceScoreValue(candidate, QStringLiteral("feedback_positive_range:"));
	const double positiveText = candidateEvidenceScoreValue(candidate, QStringLiteral("feedback_positive_text:"));
	const double negativeRange = candidateEvidenceScoreValue(candidate, QStringLiteral("feedback_negative_range:"));
	const double negativeText = candidateEvidenceScoreValue(candidate, QStringLiteral("feedback_negative_text:"));
	const double feedbackMargin = candidateEvidenceScoreValue(candidate, QStringLiteral("feedback_margin:"));
	const bool positiveBoundaryVariant = candidate.source.contains(QStringLiteral("feedback_positive_boundary_variant")) ||
		candidateHasEvidence(candidate, QStringLiteral("feedback_positive_boundary_variant"));
	const bool noveltyPositiveProbe = candidateHasEvidence(candidate, QStringLiteral("feedback_novelty_relaxed_negative_review_probe")) &&
		(positiveRange >= 0.32 || positiveText >= 0.42);
	const bool positiveGuided = positiveBoundaryVariant || noveltyPositiveProbe ||
		positiveRange >= 0.34 || positiveText >= 0.46 || feedbackMargin >= 0.16;
	const bool semanticBody = candidate.scores.semanticClipValue >= 0.62 || candidate.scores.semanticHook >= 0.62 ||
		candidate.scores.semanticOpeningHook >= 0.62 || candidate.scores.semanticResolution >= 0.62 ||
		candidate.scores.semanticEndingResolution >= 0.62 || candidate.scores.rerankerRaw >= 0.28;
	const bool notDominatedByNegative = negativeRange < 0.72 && negativeText < 0.74 &&
		(positiveRange + positiveText + 0.10) >= (negativeRange + negativeText);
	return positiveGuided && semanticBody && notDominatedByNegative;
}

static QVector<ClipCandidate> tentativeRecoverableReviewMarkersFromDiagnostics(const QVector<ClipCandidate> &diagnostics,
	int limit)
{
	QVector<ClipCandidate> markers;
	const int maxItems = std::clamp(limit, 1, 5);
	markers.reserve(maxItems);
	for (ClipCandidate candidate : diagnostics) {
		if (markers.size() >= maxItems)
			break;
		if (!isRecoverableIncompleteArcDiagnostic(candidate))
			continue;
		if (hasSimilarDiagnosticRange(markers, candidate.range))
			continue;
		candidate.rejectedByQualityGate = false;
		candidate.rejectedAsNoise = false;
		candidate.rejectionReason.clear();
		candidate.qualityGateChecked = true;
		candidate.scores.qualityGate = std::max(candidate.scores.qualityGate, 0.42);
		candidate.scores.final = std::max(candidate.scores.final, 0.42 + std::min(0.18, candidate.scores.semanticClipValue * 0.12));
		candidate.source = QStringLiteral("tentative_recoverable_review_marker:%1").arg(candidate.source.left(120));
		candidate.evidence.append(QStringLiteral("tentative_recoverable_marker_from_incomplete_arc_diagnostic"));
		candidate.evidence.append(QStringLiteral("review_marker_requires_human_boundary_decision"));
		candidate.evidence.append(QStringLiteral("not_training_positive_until_user_approval"));
		candidate.evidence.removeDuplicates();
		markers.append(candidate);
	}
	return markers;
}

static bool isFeedbackSuppressedRejectedCandidate(const ClipCandidate &candidate)
{
	return candidateHasEvidence(candidate, QStringLiteral("feedback_negative_suppressed")) ||
		candidateHasEvidence(candidate, QStringLiteral("feedback_consistency_rejected"));
}

static QString diagnosticEvidenceSummary(const ClipCandidate &candidate)
{
	QStringList evidence;
	for (const QString &item : candidate.evidence) {
		const bool useful = item.startsWith(QStringLiteral("arc_gate_")) ||
			item.startsWith(QStringLiteral("exchange_arc_")) ||
			item.startsWith(QStringLiteral("feedback_")) ||
			item.startsWith(QStringLiteral("semantic_quality_gate_")) ||
			item.startsWith(QStringLiteral("quality_gate_")) ||
			item.startsWith(QStringLiteral("reranker_"));
		if (!useful)
			continue;
		evidence.append(item.left(180));
		if (evidence.size() >= 8)
			break;
	}
	return evidence.isEmpty() ? QStringLiteral("none") : evidence.join(QLatin1Char('|'));
}

static double diagnosticRangeCenter(const ClipDuration &range)
{
	return range.startSec + ((range.endSec - range.startSec) * 0.5);
}

static bool hasSimilarDiagnosticRange(const QVector<ClipCandidate> &diagnostics, const ClipDuration &range)
{
	for (const ClipCandidate &candidate : diagnostics) {
		if (FeedbackSimilarityScorer::rangeSimilarity(candidate.range, range) >= 0.64)
			return true;
		const double centerDistance = std::fabs(diagnosticRangeCenter(candidate.range) - diagnosticRangeCenter(range));
		const double boundaryDistance = std::fabs(candidate.range.startSec - range.startSec) +
			std::fabs(candidate.range.endSec - range.endSec);
		if (centerDistance <= 28.0 && boundaryDistance <= 52.0)
			return true;
	}
	return false;
}

static bool hasNearbyDiagnosticReviewCluster(const QVector<ClipCandidate> &diagnostics, const ClipDuration &range)
{
	for (const ClipCandidate &candidate : diagnostics) {
		const double centerDistance = std::fabs(diagnosticRangeCenter(candidate.range) - diagnosticRangeCenter(range));
		if (centerDistance <= 95.0)
			return true;
		const double startDistance = std::fabs(candidate.range.startSec - range.startSec);
		const double overlap = std::max(0.0, std::min(candidate.range.endSec, range.endSec) -
			std::max(candidate.range.startSec, range.startSec));
		if (startDistance <= 70.0 && overlap > 0.0)
			return true;
	}
	return false;
}

static void appendUniqueDiagnosticCandidates(QVector<ClipCandidate> &target,
	const QVector<ClipCandidate> &candidates, int limit)
{
	for (const ClipCandidate &candidate : candidates) {
		if (limit > 0 && target.size() >= limit)
			break;
		if (hasSimilarDiagnosticRange(target, candidate.range))
			continue;
		if (hasNearbyDiagnosticReviewCluster(target, candidate.range))
			continue;
		target.append(candidate);
	}
}

static bool validMemoryRange(const ClipDuration &range)
{
	return std::isfinite(range.startSec) && std::isfinite(range.endSec) && range.endSec > range.startSec;
}

static double rangeOverlapSec(const ClipDuration &left, const ClipDuration &right)
{
	return std::max(0.0, std::min(left.endSec, right.endSec) - std::max(left.startSec, right.startSec));
}

static bool rangeMatchesReviewedSignal(const ClipDuration &range,
	const Curation::Feedback::FeedbackRangeSignal &signal)
{
	if (!validMemoryRange(range) || !validMemoryRange(signal.range))
		return false;
	const double similarity = FeedbackSimilarityScorer::rangeSimilarity(signal.range, range);
	if (similarity >= 0.78)
		return true;
	const double boundaryDistance = std::fabs(signal.range.startSec - range.startSec) +
		std::fabs(signal.range.endSec - range.endSec);
	if (boundaryDistance <= 18.0)
		return true;
	const double centerDistance = std::fabs(diagnosticRangeCenter(signal.range) - diagnosticRangeCenter(range));
	const double minDuration = std::min(signal.range.endSec - signal.range.startSec, range.endSec - range.startSec);
	return centerDistance <= 24.0 && minDuration > 0.0 && rangeOverlapSec(signal.range, range) >= minDuration * 0.70;
}

static bool rangeWasAlreadyReviewedByFeedback(const ClipDuration &range,
	const Curation::Feedback::FeedbackRangeMemory &memory)
{
	if (!memory.loaded)
		return false;
	for (const Curation::Feedback::FeedbackRangeSignal &signal : memory.positiveRanges) {
		if (rangeMatchesReviewedSignal(range, signal))
			return true;
	}
	for (const Curation::Feedback::FeedbackRangeSignal &signal : memory.negativeRanges) {
		if (rangeMatchesReviewedSignal(range, signal))
			return true;
	}
	return false;
}


static bool rangeMatchesExactReviewedSignal(const ClipDuration &range,
	const Curation::Feedback::FeedbackRangeSignal &signal)
{
	if (!validMemoryRange(range) || !validMemoryRange(signal.range))
		return false;
	if (FeedbackSimilarityScorer::rangeSimilarity(signal.range, range) >= 0.94)
		return true;
	const double boundaryDistance = std::fabs(signal.range.startSec - range.startSec) +
		std::fabs(signal.range.endSec - range.endSec);
	if (boundaryDistance <= 6.0)
		return true;
	const double centerDistance = std::fabs(diagnosticRangeCenter(signal.range) - diagnosticRangeCenter(range));
	const double minDuration = std::min(signal.range.endSec - signal.range.startSec,
		range.endSec - range.startSec);
	return centerDistance <= 8.0 && minDuration > 0.0 &&
		rangeOverlapSec(signal.range, range) >= minDuration * 0.92;
}

static bool rangeWasExactlyReviewedByFeedback(const ClipDuration &range,
	const Curation::Feedback::FeedbackRangeMemory &memory)
{
	if (!memory.loaded)
		return false;
	for (const Curation::Feedback::FeedbackRangeSignal &signal : memory.positiveRanges) {
		if (rangeMatchesExactReviewedSignal(range, signal))
			return true;
	}
	for (const Curation::Feedback::FeedbackRangeSignal &signal : memory.negativeRanges) {
		if (rangeMatchesExactReviewedSignal(range, signal))
			return true;
	}
	return false;
}

static bool isIgnoredDiagnosticFeedbackSignal(const Curation::Feedback::FeedbackRangeSignal &signal)
{
	return signal.ignoreForTraining || signal.decision == QStringLiteral("ignored_diagnostic") ||
	       signal.reason.contains(QStringLiteral("ignored_diagnostic"), Qt::CaseInsensitive) ||
	       signal.reason.contains(QStringLiteral("not_training_signal"), Qt::CaseInsensitive);
}

static bool rangeMatchesIgnoredDiagnosticNeighborhood(const ClipDuration &range,
	const Curation::Feedback::FeedbackRangeSignal &signal)
{
	if (!isIgnoredDiagnosticFeedbackSignal(signal) || !validMemoryRange(range) || !validMemoryRange(signal.range))
		return false;

	// "Ignore for dataset" means: do not use this row as ML/ranking signal, but also
	// do not ask the user to review the same local failure again.  Use a wider
	// neighborhood than normal novelty exploration so near-boundary variants such as
	// 95-113, 110-181, 47-95, etc. disappear from subsequent diagnostic passes.
	const double similarity = FeedbackSimilarityScorer::rangeSimilarity(signal.range, range);
	if (similarity >= 0.52)
		return true;
	const double boundaryDistance = std::fabs(signal.range.startSec - range.startSec) +
		std::fabs(signal.range.endSec - range.endSec);
	if (boundaryDistance <= 70.0)
		return true;
	const double centerDistance = std::fabs(diagnosticRangeCenter(signal.range) - diagnosticRangeCenter(range));
	const double minDuration = std::min(signal.range.endSec - signal.range.startSec, range.endSec - range.startSec);
	if (centerDistance <= 65.0 && minDuration > 0.0 &&
	    rangeOverlapSec(signal.range, range) >= std::max(8.0, minDuration * 0.35))
		return true;
	return centerDistance <= 38.0;
}

static bool rangeMatchesReviewedSignalForExploration(const ClipDuration &range,
	const Curation::Feedback::FeedbackRangeSignal &signal)
{
	if (!validMemoryRange(range) || !validMemoryRange(signal.range))
		return false;
	if (rangeMatchesIgnoredDiagnosticNeighborhood(range, signal))
		return true;

	// The normal feedback matcher intentionally suppresses a fairly wide neighborhood
	// around reviewed ranges so exact positives/rejections do not keep returning as
	// final markers. Novelty exploration is different: it must keep surfacing nearby
	// alternatives for review when the current dataset is mostly boundary mistakes.
	// Therefore only suppress ranges that are essentially the same review item.
	const double similarity = FeedbackSimilarityScorer::rangeSimilarity(signal.range, range);
	if (similarity >= 0.90)
		return true;
	const double boundaryDistance = std::fabs(signal.range.startSec - range.startSec) +
		std::fabs(signal.range.endSec - range.endSec);
	if (boundaryDistance <= 7.0)
		return true;
	const double centerDistance = std::fabs(diagnosticRangeCenter(signal.range) - diagnosticRangeCenter(range));
	const double minDuration = std::min(signal.range.endSec - signal.range.startSec, range.endSec - range.startSec);
	return centerDistance <= 10.0 && minDuration > 0.0 && rangeOverlapSec(signal.range, range) >= minDuration * 0.88;
}

static bool rangeWasAlreadyReviewedByFeedbackForExploration(const ClipDuration &range,
	const Curation::Feedback::FeedbackRangeMemory &memory)
{
	if (!memory.loaded)
		return false;
	for (const Curation::Feedback::FeedbackRangeSignal &signal : memory.positiveRanges) {
		if (rangeMatchesReviewedSignalForExploration(range, signal))
			return true;
	}
	for (const Curation::Feedback::FeedbackRangeSignal &signal : memory.negativeRanges) {
		if (rangeMatchesReviewedSignalForExploration(range, signal))
			return true;
	}
	return false;
}


enum class DiagnosticReviewedRangeMode {
	Strict,
	RelaxedExploration,
};

static bool diagnosticRangeWasAlreadyReviewed(const ClipDuration &range,
	const Curation::Feedback::FeedbackRangeMemory *feedbackMemory,
	DiagnosticReviewedRangeMode reviewedRangeMode)
{
	if (!feedbackMemory)
		return false;
	return reviewedRangeMode == DiagnosticReviewedRangeMode::RelaxedExploration
		? rangeWasAlreadyReviewedByFeedbackForExploration(range, *feedbackMemory)
		: rangeWasAlreadyReviewedByFeedback(range, *feedbackMemory);
}

static QVector<ClipCandidate> topRejectedCandidatesForDiagnostics(const QVector<ClipCandidate> &candidates,
	int limit = 12,
	const Curation::Feedback::FeedbackRangeMemory *feedbackMemory = nullptr,
	DiagnosticReviewedRangeMode reviewedRangeMode = DiagnosticReviewedRangeMode::Strict)
{
	QVector<ClipCandidate> rejected;
	for (ClipCandidate candidate : candidates) {
		if (candidate.range.endSec <= candidate.range.startSec || !isRejectedCandidate(candidate))
			continue;
		if (isFeedbackSuppressedRejectedCandidate(candidate))
			continue;
		if (diagnosticRangeWasAlreadyReviewed(candidate.range, feedbackMemory, reviewedRangeMode))
			continue;
		if (reviewedRangeMode == DiagnosticReviewedRangeMode::RelaxedExploration)
			candidate.evidence.append(QStringLiteral("diagnostic_relaxed_reviewed_range_filter"));
		candidate.evidence.removeDuplicates();
		rejected.append(candidate);
	}
	std::sort(rejected.begin(), rejected.end(), [](const ClipCandidate &left, const ClipCandidate &right) {
		if (std::fabs(left.scores.final - right.scores.final) > 0.0001)
			return left.scores.final > right.scores.final;
		if (std::fabs(left.scores.semanticClipValue - right.scores.semanticClipValue) > 0.0001)
			return left.scores.semanticClipValue > right.scores.semanticClipValue;
		if (std::fabs(left.scores.rerankerRaw - right.scores.rerankerRaw) > 0.0001)
			return left.scores.rerankerRaw > right.scores.rerankerRaw;
		return left.range.startSec < right.range.startSec;
	});

	QVector<ClipCandidate> diverse;
	diverse.reserve(limit > 0 ? std::min(limit, static_cast<int>(rejected.size())) : rejected.size());
	for (const ClipCandidate &candidate : rejected) {
		if (limit > 0 && diverse.size() >= limit)
			break;
		if (hasSimilarDiagnosticRange(diverse, candidate.range))
			continue;
		if (reviewedRangeMode == DiagnosticReviewedRangeMode::RelaxedExploration &&
		    hasNearbyDiagnosticReviewCluster(diverse, candidate.range))
			continue;
		diverse.append(candidate);
	}
	return diverse;
}


static void logRejectedCandidateDiagnostics(const QString &videoPath, const QString &stage,
	const QVector<ClipCandidate> &diagnostics)
{
	if (diagnostics.isEmpty())
		return;
	blog(LOG_INFO,
	     "[clip-cropper] Top rejected candidate diagnostics preserved. video=%s stage=%s count=%d",
	     videoPath.toUtf8().constData(), stage.toUtf8().constData(), static_cast<int>(diagnostics.size()));
	for (int i = 0; i < diagnostics.size(); ++i) {
		const ClipCandidate &candidate = diagnostics.at(i);
		const QString reason = candidate.rejectionReason.trimmed().isEmpty()
			? QStringLiteral("unknown")
			: candidate.rejectionReason.trimmed().left(80);
		blog(LOG_INFO,
		     "[clip-cropper] Rejected candidate diagnostic #%d. video=%s stage=%s range=%.2f-%.2f duration=%.2f reason=%s final=%.3f value=%.3f hook=%.3f openingHook=%.3f resolution=%.3f endingResolution=%.3f arc=%.3f arcOpen=%.3f arcDev=%.3f arcEnd=%.3f topicContinuity=%.3f rerankerRaw=%.3f margin=%.3f source=%s evidence=%s",
		     i + 1, videoPath.toUtf8().constData(), stage.toUtf8().constData(), candidate.range.startSec,
		     candidate.range.endSec, candidate.range.endSec - candidate.range.startSec,
		     reason.toUtf8().constData(), candidate.scores.final, candidate.scores.semanticClipValue,
		     candidate.scores.semanticHook, candidate.scores.semanticOpeningHook,
		     candidate.scores.semanticResolution, candidate.scores.semanticEndingResolution,
		     candidate.scores.arcCompleteness, candidate.scores.arcOpening, candidate.scores.arcDevelopment,
		     candidate.scores.arcConclusion, candidate.scores.topicContinuity, candidate.scores.rerankerRaw,
		     candidate.scores.rerankerClipQualityMargin, candidate.source.left(160).toUtf8().constData(),
		     diagnosticEvidenceSummary(candidate).toUtf8().constData());
	}
}

static QString feedbackGateRejectionDiagnostics(const QVector<ClipCandidate> &candidates)
{
	int incompleteArc = 0;
	int hardBlocker = 0;
	int missingOrigin = 0;
	int learnedRejected = 0;
	int notPositiveBacked = 0;
	int trainedBelow = 0;
	int trainedNegative = 0;
	int trainedHard = 0;
	int feedbackSuppressed = 0;
	for (const ClipCandidate &candidate : candidates) {
		if (!candidate.rejectedByQualityGate && !candidate.rejectedAsNoise)
			continue;
		const auto has = [&candidate](const QString &evidence) { return candidateHasEvidence(candidate, evidence); };
		if (candidate.rejectionReason == QStringLiteral("incomplete_viewer_arc"))
			++incompleteArc;
		if (has(QStringLiteral("arc_gate_hard_context_blocker")))
			++hardBlocker;
		if (has(QStringLiteral("arc_gate_missing_viewer_origin")))
			++missingOrigin;
		if (has(QStringLiteral("arc_gate_learned_accept_rejected")))
			++learnedRejected;
		if (has(QStringLiteral("feedback_trained_ranker_not_positive_backed")))
			++notPositiveBacked;
		if (has(QStringLiteral("feedback_trained_ranker_rejected_below_threshold")))
			++trainedBelow;
		if (has(QStringLiteral("feedback_trained_ranker_rejected_negative_contamination")))
			++trainedNegative;
		if (has(QStringLiteral("feedback_trained_ranker_rejected_hard_context_blocker")))
			++trainedHard;
		if (has(QStringLiteral("feedback_negative_suppressed")) || has(QStringLiteral("feedback_consistency_rejected")))
			++feedbackSuppressed;
	}
	return QStringLiteral("incomplete=%1 hard=%2 missingOrigin=%3 learnedRejected=%4 notPositive=%5 below=%6 negative=%7 rankerHard=%8 feedbackSuppressed=%9")
		.arg(incompleteArc)
		.arg(hardBlocker)
		.arg(missingOrigin)
		.arg(learnedRejected)
		.arg(notPositiveBacked)
		.arg(trainedBelow)
		.arg(trainedNegative)
		.arg(trainedHard)
		.arg(feedbackSuppressed);
}

static int feedbackSuppressedCandidateCount(const QVector<ClipCandidate> &candidates)
{
	return static_cast<int>(std::count_if(candidates.constBegin(), candidates.constEnd(), [](const ClipCandidate &candidate) {
		return isFeedbackSuppressedRejectedCandidate(candidate);
	}));
}

static int semanticPrototypePositiveRangeCount(const Curation::Feedback::FeedbackRangeMemory &memory)
{
	return static_cast<int>(std::count_if(memory.positiveRanges.constBegin(), memory.positiveRanges.constEnd(),
		[](const Curation::Feedback::FeedbackRangeSignal &signal) {
			return signal.semanticPrototypeEligible;
		}));
}

static bool positiveSignalCanReplayAsTimelineMarker(const Curation::Feedback::FeedbackRangeSignal &signal)
{
	return signal.semanticPrototypeEligible ||
		signal.decision == QStringLiteral("accepted") ||
		signal.decision == QStringLiteral("approved_adjusted") ||
		signal.decision == QStringLiteral("added_by_user");
}

static bool hasSimilarTimelineCandidateRange(const QVector<ClipCandidate> &candidates, const ClipDuration &range)
{
	for (const ClipCandidate &candidate : candidates) {
		if (FeedbackSimilarityScorer::rangeSimilarity(candidate.range, range) >= 0.72)
			return true;
		const double boundaryDistance = std::fabs(candidate.range.startSec - range.startSec) +
			std::fabs(candidate.range.endSec - range.endSec);
		if (boundaryDistance <= 8.0)
			return true;
	}
	return false;
}

static bool rangeMatchesExistingReviewRange(const ClipDuration &range,
	const QVector<ClipDuration> &existingRanges)
{
	if (!validMemoryRange(range) || existingRanges.isEmpty())
		return false;
	for (const ClipDuration &existing : existingRanges) {
		if (!validMemoryRange(existing))
			continue;
		if (FeedbackSimilarityScorer::rangeSimilarity(existing, range) >= 0.76)
			return true;
		const double boundaryDistance = std::fabs(existing.startSec - range.startSec) +
			std::fabs(existing.endSec - range.endSec);
		if (boundaryDistance <= 10.0)
			return true;
		const double overlap = rangeOverlapSec(existing, range);
		const double minDuration = std::min(existing.endSec - existing.startSec, range.endSec - range.startSec);
		if (minDuration > 0.0 && overlap >= minDuration * 0.86)
			return true;
	}
	return false;
}

static bool isStrictExactPositiveFeedbackSeedCandidate(const ClipCandidate &candidate)
{
	return candidate.source.contains(QStringLiteral("feedback_positive_exact_seed")) ||
		candidateHasEvidence(candidate, QStringLiteral("feedback_positive_exact_seed_preserved"));
}

static bool isExactUserFeedbackSeedCandidate(const ClipCandidate &candidate)
{
	return isStrictExactPositiveFeedbackSeedCandidate(candidate) ||
		candidateHasEvidence(candidate, QStringLiteral("complete_viewer_arc_gate_passed_by_user_feedback"));
}

static bool exactSeedMatchesPositiveMemory(const ClipCandidate &candidate,
	const Curation::Feedback::FeedbackRangeMemory &memory)
{
	if (!memory.loaded || !isStrictExactPositiveFeedbackSeedCandidate(candidate))
		return false;
	for (const Curation::Feedback::FeedbackRangeSignal &signal : memory.positiveRanges) {
		if (rangeMatchesReviewedSignal(candidate.range, signal))
			return true;
	}
	return false;
}

static int suppressAlreadyReviewedExactPositiveSeeds(QVector<ClipCandidate> &candidates,
	const Curation::Feedback::FeedbackRangeMemory &memory)
{
	if (!memory.loaded || candidates.isEmpty())
		return 0;
	int suppressed = 0;
	for (ClipCandidate &candidate : candidates) {
		if (candidate.rejectedByQualityGate || candidate.rejectedAsNoise)
			continue;
		if (!exactSeedMatchesPositiveMemory(candidate, memory))
			continue;
		candidate.rejectedByQualityGate = true;
		candidate.rejectionReason = QStringLiteral("already_reviewed_positive_feedback_seed");
		candidate.evidence.append(QStringLiteral("feedback_positive_exact_seed_suppressed_already_reviewed"));
		candidate.evidence.append(QStringLiteral("feedback_positive_seed_kept_for_guidance_not_auto_replay"));
		candidate.evidence.removeDuplicates();
		++suppressed;
	}
	return suppressed;
}

static int suppressAlreadyReviewedFeedbackCandidates(QVector<ClipCandidate> &candidates,
	const Curation::Feedback::FeedbackRangeMemory &memory)
{
	if (!memory.loaded || candidates.isEmpty())
		return 0;
	int suppressed = 0;
	for (ClipCandidate &candidate : candidates) {
		if (candidate.rejectedByQualityGate || candidate.rejectedAsNoise)
			continue;
		if (!rangeWasAlreadyReviewedByFeedback(candidate.range, memory))
			continue;
		candidate.rejectedByQualityGate = true;
		candidate.rejectionReason = QStringLiteral("already_reviewed_feedback_candidate");
		candidate.evidence.append(QStringLiteral("feedback_candidate_suppressed_already_reviewed"));
		candidate.evidence.append(QStringLiteral("reviewed_feedback_not_asked_again"));
		candidate.evidence.removeDuplicates();
		++suppressed;
	}
	return suppressed;
}


static int suppressExistingReviewRangeExactSeeds(QVector<ClipCandidate> &candidates,
	const QVector<ClipDuration> &existingRanges)
{
	if (existingRanges.isEmpty() || candidates.isEmpty())
		return 0;
	int suppressed = 0;
	for (ClipCandidate &candidate : candidates) {
		if (candidate.rejectedByQualityGate || candidate.rejectedAsNoise)
			continue;
		if (!isExactUserFeedbackSeedCandidate(candidate))
			continue;
		if (!rangeMatchesExistingReviewRange(candidate.range, existingRanges))
			continue;
		candidate.rejectedByQualityGate = true;
		candidate.rejectionReason = QStringLiteral("already_existing_review_marker");
		candidate.evidence.append(QStringLiteral("feedback_positive_exact_seed_suppressed_existing_marker"));
		candidate.evidence.removeDuplicates();
		++suppressed;
	}
	return suppressed;
}

static QVector<ClipCandidate> replayPositiveFeedbackTimelineCandidates(const TranscriptIndex &index,
	const ClipScoringPipelineOptions &options,
	const Curation::Feedback::FeedbackRangeMemory &memory,
	int limit)
{
	QVector<ClipCandidate> replay;
	if (!memory.loaded || memory.positiveRanges.isEmpty() || limit <= 0)
		return replay;

	QVector<Curation::Feedback::FeedbackRangeSignal> positives = memory.positiveRanges;
	std::sort(positives.begin(), positives.end(), [](const auto &left, const auto &right) {
		if (left.semanticPrototypeEligible != right.semanticPrototypeEligible)
			return left.semanticPrototypeEligible;
		if (left.sequence != right.sequence)
			return left.sequence > right.sequence;
		return left.weight > right.weight;
	});

	SemanticCoarseRegion replayRegion;
	replayRegion.score = 0.86;
	replayRegion.evidence.append(QStringLiteral("feedback_positive_replay_region"));

	FeedbackSimilarityScorer scorer;
	for (const Curation::Feedback::FeedbackRangeSignal &signal : positives) {
		if (static_cast<int>(replay.size()) >= limit)
			break;
		if (!positiveSignalCanReplayAsTimelineMarker(signal) || !validMemoryRange(signal.range))
			continue;

		ClipDuration range = index.clampRange(signal.range, options.generation.searchRange);
		if (!validMemoryRange(range))
			continue;
		if (rangeMatchesExistingReviewRange(range, options.existingReviewRanges))
			continue;
		const double durationSec = range.endSec - range.startSec;
		if (durationSec < options.generation.minDurationSec || durationSec > options.generation.maxDurationSec + 1.0)
			continue;
		if (hasSimilarTimelineCandidateRange(replay, range))
			continue;

		const QString text = index.textForRange(range).simplified();
		if (text.trimmed().isEmpty())
			continue;
		const FeedbackSimilarityFeatures features = scorer.scoreRange(range, text, index, memory);
		const bool contradictedByNewerNegative = features.negativeRangeContamination &&
			!features.explainedByPositiveRange && features.negativeScore >= features.positiveScore + 0.16;
		const bool ambiguousConflict = features.negativeRangeContamination && features.negativeScore >= 0.72 &&
			features.margin <= 0.08 && features.positiveScore < 0.88;
		if (contradictedByNewerNegative || ambiguousConflict)
			continue;

		ClipCandidate candidate = CandidateBuilder::buildForRange(index, options.generation, options.scoring,
			options.qualityGate, range, replayRegion);
		if (candidate.text.trimmed().isEmpty() ||
		    !CandidateBuilder::isStructurallyViable(candidate, options.generation, options.qualityGate))
			continue;
		candidate.source = QStringLiteral("feedback_positive_exact_seed");
		candidate.startsNearViewerCue = true;
		candidate.rejectedAsNoise = false;
		candidate.rejectedByQualityGate = false;
		candidate.rejectionReason.clear();
		candidate.qualityGateChecked = true;
		candidate.scores.final = std::max(candidate.scores.final, 0.88);
		candidate.scores.qualityGate = std::max(candidate.scores.qualityGate, 0.92);
		candidate.scores.semanticTarget = std::max(candidate.scores.semanticTarget, 0.70);
		candidate.scores.semanticClipValue = std::max(candidate.scores.semanticClipValue, 0.72);
		candidate.scores.semanticViewerMessage = std::max(candidate.scores.semanticViewerMessage, 0.66);
		candidate.scores.semanticDirectAnswer = std::max(candidate.scores.semanticDirectAnswer, 0.64);
		candidate.scores.semanticOpeningHook = std::max(candidate.scores.semanticOpeningHook, 0.62);
		candidate.scores.semanticResolution = std::max(candidate.scores.semanticResolution, 0.62);
		candidate.scores.semanticEndingResolution = std::max(candidate.scores.semanticEndingResolution, 0.62);
		candidate.scores.arcOpening = std::max(candidate.scores.arcOpening, 0.48);
		candidate.scores.arcDevelopment = std::max(candidate.scores.arcDevelopment, 0.52);
		candidate.scores.arcConclusion = std::max(candidate.scores.arcConclusion, 0.52);
		candidate.scores.arcBoundaryCleanliness = std::max(candidate.scores.arcBoundaryCleanliness, 0.46);
		candidate.scores.arcCompleteness = std::max(candidate.scores.arcCompleteness, 0.52);
		candidate.evidence.append(QStringLiteral("feedback_positive_exact_seed"));
		candidate.evidence.append(QStringLiteral("feedback_positive_exact_seed_preserved"));
		candidate.evidence.append(QStringLiteral("feedback_positive_replayed_to_timeline"));
		candidate.evidence.append(QStringLiteral("complete_viewer_arc_gate_passed_by_user_feedback"));
		candidate.evidence.append(QStringLiteral("feedback_positive_decision:%1").arg(signal.decision.left(32)));
		if (!signal.reason.trimmed().isEmpty())
			candidate.evidence.append(QStringLiteral("feedback_positive_reason:%1").arg(signal.reason.left(96)));
		candidate.evidence.append(features.evidence);
		candidate.evidence.removeDuplicates();
		replay.append(candidate);
	}
	return replay;
}


struct NoveltyExplorationBuildStats {
	int rawCandidates = 0;
	int localAccepted = 0;
	int selectedForExpensiveScoring = 0;
	int workerCount = 1;
	long long generationMs = 0;
	long long localScoreMs = 0;
	long long diversityMs = 0;
	int reviewedSuppressed = 0;
};

static double localExplorationScore(const ClipCandidate &candidate, const FeedbackSimilarityFeatures &feedback)
{
	const double viewer = std::max({candidate.scores.viewerResponse, candidate.scores.semanticViewerMessage,
		candidate.scores.semanticDirectAnswer});
	const double structure = std::max({candidate.scores.arcCompleteness, candidate.scores.arcOpening,
		candidate.scores.arcDevelopment, candidate.scores.arcConclusion});
	const double hook = std::max(candidate.scores.hook, candidate.scores.semanticHook);
	const double resolution = std::max(candidate.scores.semanticResolution, candidate.scores.semanticEndingResolution);
	const double positive = std::max(0.0, feedback.positiveScore) * 0.16;
	const double noveltyPenalty = std::max(0.0, feedback.negativeScore) * 0.22;
	return (viewer * 0.30) + (structure * 0.22) + (hook * 0.18) + (resolution * 0.18) + positive - noveltyPenalty;
}

static bool hasSimilarExplorationRange(const QVector<ClipCandidate> &candidates, const ClipDuration &range)
{
	for (const ClipCandidate &candidate : candidates) {
		if (FeedbackSimilarityScorer::rangeSimilarity(candidate.range, range) >= 0.70)
			return true;
		const double boundaryDistance = std::fabs(candidate.range.startSec - range.startSec) +
			std::fabs(candidate.range.endSec - range.endSec);
		if (boundaryDistance <= 8.0)
			return true;
	}
	return false;
}

static QString noveltyRangeKey(const ClipDuration &range)
{
	return QStringLiteral("%1:%2")
		.arg(static_cast<int>(std::round(range.startSec * 2.0)))
		.arg(static_cast<int>(std::round(range.endSec * 2.0)));
}

static QVector<double> fastNoveltyDurations(const CandidateGenerationOptions &generation)
{
	QVector<double> durations{
		std::clamp(24.0, generation.minDurationSec, generation.maxDurationSec),
		std::clamp(38.0, generation.minDurationSec, generation.maxDurationSec),
		std::clamp(58.0, generation.minDurationSec, generation.maxDurationSec),
		std::clamp(82.0, generation.minDurationSec, generation.maxDurationSec),
	};
	durations.erase(std::unique(durations.begin(), durations.end(), [](double left, double right) {
		return std::fabs(left - right) < 0.25;
	}), durations.end());
	return durations;
}

static QVector<ClipCandidate> buildFastNoveltyRawCandidates(const TranscriptIndex &index,
	const ClipScoringPipelineOptions &options,
	int maxRawCandidates)
{
	QVector<ClipCandidate> candidates;
	if (index.isEmpty() || maxRawCandidates <= 0)
		return candidates;

	CandidateSourceBuilderOptions sourceOptions = candidateSourceOptionsFromOptions(options);
	sourceOptions.generation.maxRawCandidates = maxRawCandidates;
	CheapClipScorer cueScorer;

	struct Anchor {
		int segmentIndex = -1;
		double score = 0.0;
		bool targetHit = false;
		bool viewerCue = false;
	};

	QVector<Anchor> anchors;
	anchors.reserve(96);
	const ClipDuration searchRange = sourceOptions.generation.searchRange;
	const int segmentStride = index.size() >= 2200 ? 2 : 1;
	for (int i = 0; i < index.size(); i += segmentStride) {
		const TranscriptSegment *segment = index.segmentAt(i);
		if (!segment || !TranscriptIndex::segmentOverlapsRange(*segment, searchRange) || segment->text.trimmed().isEmpty())
			continue;
		const bool targetHit = sourceOptions.generation.reliableMainTarget &&
			cueScorer.targetKeywordScore(segment->text, sourceOptions.generation.mainTarget) >= 0.30;
		const bool strongCue = cueScorer.hasStrongLocalCue(segment->text);
		if (!targetHit && !strongCue)
			continue;
		const bool viewerCue = cueScorer.looksLikeQuestionOrViewerMessage(segment->text);
		double score = 0.0;
		if (targetHit)
			score += 0.44;
		if (viewerCue)
			score += 0.24;
		if (strongCue)
			score += 0.18;
		score += std::min(0.18, static_cast<double>(segment->text.trimmed().size()) / 700.0);
		anchors.append(Anchor{i, score, targetHit, viewerCue});
	}

	std::sort(anchors.begin(), anchors.end(), [](const Anchor &left, const Anchor &right) {
		if (std::fabs(left.score - right.score) > 0.0001)
			return left.score > right.score;
		return left.segmentIndex < right.segmentIndex;
	});
	const int maxAnchors = std::min(static_cast<int>(anchors.size()), std::max(16, maxRawCandidates / 3));
	QVector<Anchor> spacedAnchors;
	spacedAnchors.reserve(maxAnchors);
	const auto anchorStartSec = [&index](const Anchor &anchor) {
		const TranscriptSegment *segment = index.segmentAt(anchor.segmentIndex);
		return segment ? segment->startSec : 0.0;
	};
	const auto alreadySelectedNear = [&spacedAnchors, &anchorStartSec](const Anchor &anchor, double minSpacingSec) {
		const double start = anchorStartSec(anchor);
		for (const Anchor &selected : spacedAnchors) {
			if (std::fabs(anchorStartSec(selected) - start) <= minSpacingSec)
				return true;
		}
		return false;
	};
	for (const double minSpacingSec : QVector<double>{300.0, 180.0, 90.0, 0.0}) {
		for (const Anchor &anchor : anchors) {
			if (spacedAnchors.size() >= maxAnchors)
				break;
			bool alreadySelected = false;
			for (const Anchor &selected : spacedAnchors) {
				if (selected.segmentIndex == anchor.segmentIndex) {
					alreadySelected = true;
					break;
				}
			}
			if (alreadySelected)
				continue;
			if (minSpacingSec > 0.0 && alreadySelectedNear(anchor, minSpacingSec))
				continue;
			spacedAnchors.append(anchor);
		}
		if (spacedAnchors.size() >= maxAnchors)
			break;
	}
	const QVector<double> durations = fastNoveltyDurations(sourceOptions.generation);
	const QVector<double> startOffsets = options.scoring.presetId == QStringLiteral("viewer_message_response")
		? QVector<double>{-44.0, -32.0, -18.0, -8.0}
		: QVector<double>{-18.0, -8.0, 0.0};
	QSet<QString> seen;
	candidates.reserve(maxRawCandidates);

	SemanticCoarseRegion syntheticRegion;
	syntheticRegion.score = 0.58;
	syntheticRegion.evidence.append(QStringLiteral("feedback_novelty_fast_generation"));

	for (int anchorPosition = 0; anchorPosition < spacedAnchors.size() && static_cast<int>(candidates.size()) < maxRawCandidates;
	     ++anchorPosition) {
		const Anchor &anchor = spacedAnchors.at(anchorPosition);
		const TranscriptSegment *segment = index.segmentAt(anchor.segmentIndex);
		if (!segment)
			continue;
		for (const double duration : durations) {
			for (const double offset : startOffsets) {
				ClipDuration range{segment->startSec + offset, segment->startSec + offset + duration};
				range = index.clampRange(range, searchRange);
				if (range.endSec - range.startSec < sourceOptions.generation.minDurationSec)
					continue;
				const QString key = noveltyRangeKey(range);
				if (seen.contains(key))
					continue;
				seen.insert(key);
				ClipCandidate candidate = CandidateBuilder::buildForRange(index, sourceOptions.generation,
					sourceOptions.scoring, sourceOptions.qualityGate, range, syntheticRegion);
				candidate.source = anchor.targetHit ? QStringLiteral("target_anchor") : QStringLiteral("cue_anchor");
				candidate.startsNearViewerCue = anchor.viewerCue;
				candidate.anchorText = segment->text.trimmed().left(220);
				candidate.evidence.append(QStringLiteral("feedback_novelty_fast_anchor_generation"));
				candidate.evidence.removeDuplicates();
				if (!CandidateBuilder::isStructurallyViable(candidate, sourceOptions.generation, sourceOptions.qualityGate))
					continue;
				candidates.append(candidate);
				if (static_cast<int>(candidates.size()) >= maxRawCandidates)
					break;
			}
			if (static_cast<int>(candidates.size()) >= maxRawCandidates)
				break;
		}
	}
	return candidates;
}

static QVector<ClipCandidate> buildNoveltyExplorationCandidates(const TranscriptIndex &index,
	const ClipScoringPipelineOptions &options,
	const Curation::Feedback::FeedbackRangeMemory &memory,
	NoveltyExplorationBuildStats *stats = nullptr)
{
	NoveltyExplorationBuildStats localStats;
	QElapsedTimer stageTimer;
	stageTimer.start();

	CandidateSourceBuilderOptions sourceOptions = candidateSourceOptionsFromOptions(options);
	const int semanticPositiveCount = semanticPrototypePositiveRangeCount(memory);
	const int explorationRawLimit = semanticPositiveCount > 0
		? std::min(220, std::max(120, options.budget.maxCandidatesBeforeEmbedding))
		: std::min(260, std::max(160, options.budget.maxCandidatesBeforeEmbedding * 2));
	sourceOptions.maxRawCandidates = explorationRawLimit;
	sourceOptions.generation.maxRawCandidates = explorationRawLimit;
	sourceOptions.generation.slidingWindowStepSec = std::max(30.0, options.generation.slidingWindowStepSec);

	QVector<ClipCandidate> raw = buildFastNoveltyRawCandidates(index, options, explorationRawLimit);
	localStats.generationMs = stageTimer.elapsed();
	localStats.rawCandidates = static_cast<int>(raw.size());
	if (memory.loaded && !raw.isEmpty()) {
		QVector<ClipCandidate> unreviewedRaw;
		unreviewedRaw.reserve(raw.size());
		for (const ClipCandidate &candidate : raw) {
			if (rangeWasAlreadyReviewedByFeedbackForExploration(candidate.range, memory)) {
				++localStats.reviewedSuppressed;
				continue;
			}
			unreviewedRaw.append(candidate);
		}
		raw = std::move(unreviewedRaw);
	}

	if (raw.isEmpty()) {
		if (stats)
			*stats = localStats;
		return {};
	}

	stageTimer.restart();
	const unsigned hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
	const int maxWorkers = std::min(6, std::max(1, static_cast<int>(hardwareThreads) - 1));
	const int workerCount = std::clamp(static_cast<int>(raw.size() / 24) + 1, 1, maxWorkers);
	localStats.workerCount = workerCount;

	struct LocalScoredCandidate {
		ClipCandidate candidate;
		double score = 0.0;
	};
	const auto scoreRange = [&index, &memory, &sourceOptions](QVector<ClipCandidate> slice) {
		FeedbackSimilarityScorer feedbackScorer;
		QVector<LocalScoredCandidate> accepted;
		accepted.reserve(slice.size());
		for (ClipCandidate candidate : slice) {
			if (rangeWasAlreadyReviewedByFeedbackForExploration(candidate.range, memory))
				continue;
			if (candidate.range.endSec <= candidate.range.startSec || candidate.text.trimmed().size() < 80)
				continue;
			if (!CandidateBuilder::isStructurallyViable(candidate, sourceOptions.generation, sourceOptions.qualityGate))
				continue;

			const FeedbackSimilarityFeatures feedback = feedbackScorer.score(candidate, index, memory);
			const double duration = candidate.range.endSec - candidate.range.startSec;
			// Exact/near-exact reviewed ranges are already filtered above. For novelty
			// review we should not discard every nearby candidate just because it is
			// close to a previous bad boundary. Treat moderately contaminated ranges as
			// diagnostics instead of hard-dropping them, otherwise the review table dries
			// up after a few negative/adjusted decisions.
			if (feedback.negativeRangeSimilarity >= 0.86 ||
			    feedback.negativeOverlapSec >= std::max(20.0, duration * 0.55))
				continue;
			const bool diagnosticOnly = feedback.negativeRangeContamination ||
				feedback.negativeRangeSimilarity >= 0.58 || feedback.negativeOverlapSec >= 10.0 ||
				(feedback.negativeScore >= 0.68 && feedback.margin <= 0.02);

			candidate.source = QStringLiteral("feedback_novelty_exploration:%1").arg(candidate.source.left(80));
			candidate.evidence.append(QStringLiteral("feedback_novelty_exploration"));
			candidate.evidence.append(QStringLiteral("feedback_novelty_outside_suppressed_ranges"));
			if (diagnosticOnly)
				candidate.evidence.append(QStringLiteral("feedback_novelty_relaxed_negative_review_probe"));
			candidate.evidence.append(feedback.evidence);
			candidate.evidence.append(QStringLiteral("feedback_novelty_parallel_local_score"));
			candidate.evidence.removeDuplicates();
			const double localScore = localExplorationScore(candidate, feedback);
			candidate.scores.final = std::max(candidate.scores.final, 0.34 + std::max(0.0, localScore) * 0.10);
			if (diagnosticOnly) {
				candidate.rejectedByQualityGate = true;
				candidate.rejectionReason = QStringLiteral("novelty_exploration_review_required");
			}
			accepted.append(LocalScoredCandidate{candidate, localScore});
		}
		return accepted;
	};

	std::vector<std::future<QVector<LocalScoredCandidate>>> futures;
	futures.reserve(workerCount);
	const int chunkSize = std::max(1, static_cast<int>(std::ceil(static_cast<double>(raw.size()) / workerCount)));
	for (int offset = 0; offset < raw.size(); offset += chunkSize) {
		QVector<ClipCandidate> slice;
		const int end = std::min(static_cast<int>(raw.size()), offset + chunkSize);
		slice.reserve(end - offset);
		for (int i = offset; i < end; ++i)
			slice.append(raw.at(i));
		futures.push_back(std::async(std::launch::async, scoreRange, std::move(slice)));
	}

	QVector<LocalScoredCandidate> scored;
	for (std::future<QVector<LocalScoredCandidate>> &future : futures) {
		const QVector<LocalScoredCandidate> partial = future.get();
		for (const LocalScoredCandidate &candidate : partial)
			scored.append(candidate);
	}
	localStats.localScoreMs = stageTimer.elapsed();
	localStats.localAccepted = static_cast<int>(scored.size());

	stageTimer.restart();
	std::sort(scored.begin(), scored.end(), [](const LocalScoredCandidate &left, const LocalScoredCandidate &right) {
		if (std::fabs(left.score - right.score) > 0.0001)
			return left.score > right.score;
		if (std::fabs(left.candidate.scores.viewerResponse - right.candidate.scores.viewerResponse) > 0.0001)
			return left.candidate.scores.viewerResponse > right.candidate.scores.viewerResponse;
		if (std::fabs(left.candidate.scores.hook - right.candidate.scores.hook) > 0.0001)
			return left.candidate.scores.hook > right.candidate.scores.hook;
		return left.candidate.range.startSec < right.candidate.range.startSec;
	});

	QVector<ClipCandidate> exploration;
	// This fallback only needs a small diagnostic pool. Keeping it tight avoids
	// spending tens of seconds reranking and boundary-refining candidates that are
	// likely to be shown as review diagnostics instead of applied markers.
	const int expensiveLimit = semanticPositiveCount > 0 ? 12 : 14;
	exploration.reserve(expensiveLimit);
	for (const LocalScoredCandidate &candidate : scored) {
		if (exploration.size() >= expensiveLimit)
			break;
		if (hasSimilarExplorationRange(exploration, candidate.candidate.range))
			continue;
		exploration.append(candidate.candidate);
	}
	localStats.diversityMs = stageTimer.elapsed();
	localStats.selectedForExpensiveScoring = static_cast<int>(exploration.size());
	if (stats)
		*stats = localStats;
	return exploration;
}


static QVector<ClipCandidate> buildExhaustionCoverageDiagnostics(const TranscriptIndex &index,
	const ClipScoringPipelineOptions &options,
	const Curation::Feedback::FeedbackRangeMemory &memory,
	int limit = 6)
{
	QVector<ClipCandidate> diagnostics;
	if (index.isEmpty() || limit <= 0)
		return diagnostics;

	CandidateSourceBuilderOptions sourceOptions = candidateSourceOptionsFromOptions(options);
	const ClipDuration searchRange = sourceOptions.generation.searchRange;
	const double searchStart = std::max(0.0, searchRange.startSec);
	const double searchEnd = searchRange.endSec > searchStart ? searchRange.endSec : searchStart;
	const double span = searchEnd - searchStart;
	if (span <= 1.0)
		return diagnostics;

	CheapClipScorer cueScorer;
	const QVector<double> durations = fastNoveltyDurations(sourceOptions.generation);
	const QVector<double> startOffsets = options.scoring.presetId == QStringLiteral("viewer_message_response")
		? QVector<double>{-58.0, -44.0, -28.0, -12.0, 0.0}
		: QVector<double>{-24.0, -12.0, 0.0};
	const int bucketCount = std::clamp(limit * 6, 12, 48);
	const double bucketSize = span / static_cast<double>(bucketCount);

	struct BucketAnchor {
		int segmentIndex = -1;
		double score = -1.0;
		double bucketCenter = 0.0;
	};

	QVector<BucketAnchor> anchors;
	anchors.reserve(bucketCount);
	for (int bucket = 0; bucket < bucketCount; ++bucket) {
		const double bucketStart = searchStart + bucketSize * static_cast<double>(bucket);
		const double bucketEnd = bucket == bucketCount - 1 ? searchEnd : bucketStart + bucketSize;
		const double bucketCenter = (bucketStart + bucketEnd) * 0.5;
		BucketAnchor best;
		best.bucketCenter = bucketCenter;
		for (int i = 0; i < index.size(); ++i) {
			const TranscriptSegment *segment = index.segmentAt(i);
			if (!segment || segment->text.trimmed().isEmpty())
				continue;
			if (segment->endSec < bucketStart || segment->startSec > bucketEnd)
				continue;
			const QString text = segment->text.trimmed();
			const bool targetHit = sourceOptions.generation.reliableMainTarget &&
				cueScorer.targetKeywordScore(text, sourceOptions.generation.mainTarget) >= 0.22;
			const bool strongCue = cueScorer.hasStrongLocalCue(text);
			const bool viewerCue = cueScorer.looksLikeQuestionOrViewerMessage(text);
			const double segmentCenter = (segment->startSec + segment->endSec) * 0.5;
			const double distancePenalty = std::min(0.20, std::fabs(segmentCenter - bucketCenter) /
				std::max(1.0, bucketSize) * 0.20);
			double score = 0.08 + std::min(0.16, static_cast<double>(text.size()) / 900.0) - distancePenalty;
			if (targetHit)
				score += 0.34;
			if (viewerCue)
				score += 0.24;
			if (strongCue)
				score += 0.18;
			if (score > best.score)
				best = BucketAnchor{i, score, bucketCenter};
		}
		if (best.segmentIndex >= 0)
			anchors.append(best);
	}

	std::sort(anchors.begin(), anchors.end(), [](const BucketAnchor &left, const BucketAnchor &right) {
		if (std::fabs(left.score - right.score) > 0.0001)
			return left.score > right.score;
		return left.bucketCenter < right.bucketCenter;
	});

	SemanticCoarseRegion syntheticRegion;
	syntheticRegion.score = 0.42;
	syntheticRegion.evidence.append(QStringLiteral("feedback_exhaustion_coverage_fallback"));
	QSet<QString> seen;
	const int desiredMinimum = std::min(limit, 3);
	for (int pass = 0; pass < 2 && diagnostics.size() < desiredMinimum; ++pass) {
		const bool forceBeyondIgnoredNeighborhoods = pass > 0;
		for (const BucketAnchor &anchor : anchors) {
			if (diagnostics.size() >= limit)
				break;
			const TranscriptSegment *segment = index.segmentAt(anchor.segmentIndex);
			if (!segment)
				continue;
			for (const double duration : durations) {
				if (diagnostics.size() >= limit)
					break;
				for (const double offset : startOffsets) {
					ClipDuration range{segment->startSec + offset, segment->startSec + offset + duration};
					range = index.clampRange(range, searchRange);
					if (range.endSec - range.startSec < sourceOptions.generation.minDurationSec)
						continue;
					if (rangeWasExactlyReviewedByFeedback(range, memory))
						continue;
					if (!forceBeyondIgnoredNeighborhoods && rangeWasAlreadyReviewedByFeedbackForExploration(range, memory))
						continue;
					if (hasSimilarDiagnosticRange(diagnostics, range) || hasNearbyDiagnosticReviewCluster(diagnostics, range))
						continue;
					const QString key = QStringLiteral("%1:%2").arg(pass).arg(noveltyRangeKey(range));
					if (seen.contains(key))
						continue;
					seen.insert(key);
					ClipCandidate candidate = CandidateBuilder::buildForRange(index, sourceOptions.generation,
						sourceOptions.scoring, sourceOptions.qualityGate, range, syntheticRegion);
					if (candidate.range.endSec <= candidate.range.startSec || candidate.text.trimmed().size() < 60)
						continue;
					candidate.source = forceBeyondIgnoredNeighborhoods
						? QStringLiteral("feedback_exhaustion_forced_coverage_probe")
						: QStringLiteral("feedback_exhaustion_coverage_probe");
					candidate.rejectedByQualityGate = true;
					candidate.qualityGateChecked = true;
					candidate.rejectionReason = QStringLiteral("novelty_exploration_review_required");
					candidate.scores.final = std::max(candidate.scores.final, 0.18);
					candidate.evidence.append(QStringLiteral("feedback_exhaustion_coverage_probe"));
					candidate.evidence.append(QStringLiteral("feedback_exhaustion_after_reviewed_neighborhoods"));
					candidate.evidence.append(QStringLiteral("not_training_signal_until_user_decision"));
					if (forceBeyondIgnoredNeighborhoods)
						candidate.evidence.append(QStringLiteral("feedback_exhaustion_forced_after_all_unreviewed_regions_exhausted"));
					candidate.evidence.append(QStringLiteral("coverage_bucket_center:%1").arg(anchor.bucketCenter, 0, 'f', 1));
					candidate.evidence.removeDuplicates();
					diagnostics.append(candidate);
					break;
				}
			}
		}
	}
	std::sort(diagnostics.begin(), diagnostics.end(), [](const ClipCandidate &left, const ClipCandidate &right) {
		return left.range.startSec < right.range.startSec;
	});
	return diagnostics;
}

static QVector<ClipCandidate> explorationDiagnosticsFromCandidates(const QVector<ClipCandidate> &candidates,
	const Curation::Feedback::FeedbackRangeMemory &memory,
	int limit = 12)
{
	QVector<ClipCandidate> diagnostics = topRejectedCandidatesForDiagnostics(candidates, limit, &memory);
	if (diagnostics.size() >= limit)
		return diagnostics;

	for (ClipCandidate candidate : candidates) {
		if (diagnostics.size() >= limit)
			break;
		if (candidate.range.endSec <= candidate.range.startSec || isFeedbackSuppressedRejectedCandidate(candidate))
			continue;
		if (rangeWasAlreadyReviewedByFeedbackForExploration(candidate.range, memory))
			continue;
		if (isRejectedCandidate(candidate))
			continue;
		if (hasSimilarExplorationRange(diagnostics, candidate.range))
			continue;

		candidate.rejectedByQualityGate = true;
		candidate.rejectionReason = QStringLiteral("novelty_exploration_review_required");
		candidate.evidence.append(QStringLiteral("feedback_novelty_exploration_review_required"));
		candidate.evidence.removeDuplicates();
		diagnostics.append(candidate);
	}
	return diagnostics;
}

static QVector<ClipCandidate> noveltyTimelineCandidatesFromCandidates(const QVector<ClipCandidate> &candidates,
	const Curation::Feedback::FeedbackRangeMemory &memory,
	int limit = 4)
{
	QVector<ClipCandidate> promotable;
	const int maxItems = std::max(1, limit);
	for (ClipCandidate candidate : candidates) {
		if (promotable.size() >= maxItems)
			break;
		if (candidate.range.endSec <= candidate.range.startSec || candidate.text.trimmed().isEmpty())
			continue;
		if (isRejectedCandidate(candidate) || isFeedbackSuppressedRejectedCandidate(candidate))
			continue;
		if (rangeWasAlreadyReviewedByFeedback(candidate.range, memory) &&
		    !candidateHasEvidence(candidate, QStringLiteral("complete_viewer_arc_gate_passed_by_user_feedback")) &&
		    !candidateHasEvidence(candidate, QStringLiteral("complete_viewer_arc_gate_passed_by_direct_positive_feedback")))
			continue;
		if (hasSimilarExplorationRange(promotable, candidate.range))
			continue;
		candidate.evidence.append(QStringLiteral("feedback_novelty_exploration_promoted_to_timeline"));
		candidate.evidence.removeDuplicates();
		promotable.append(candidate);
	}
	return promotable;
}


static ViewerArcGateOptions viewerArcGateOptionsFromOptions(const ClipScoringPipelineOptions &options)
{
	ViewerArcGateOptions gateOptions;
	gateOptions.enabled = isViewerMessagePreset(options);
	gateOptions.presetId = options.scoring.presetId;
	gateOptions.searchRange = options.generation.searchRange;
	gateOptions.minDurationSec = options.generation.minDurationSec;
	gateOptions.maxDurationSec = options.generation.maxDurationSec;
	gateOptions.mainTarget = options.scoring.mainTarget;
	gateOptions.reliableMainTarget = options.scoring.reliableMainTarget;
	return gateOptions;
}

static CandidateSourceBuilderOptions candidateSourceOptionsFromOptions(const ClipScoringPipelineOptions &options)
{
	CandidateSourceBuilderOptions sourceOptions;
	sourceOptions.generation = options.generation;
	sourceOptions.scoring = options.scoring;
	sourceOptions.qualityGate = options.qualityGate;
	sourceOptions.maxRawCandidates = options.generation.maxRawCandidates;
	sourceOptions.viewerMessagePreset = isViewerMessagePreset(options);
	return sourceOptions;
}

static CandidateBeamExpansionOptions beamExpansionOptionsFromOptions(const ClipScoringPipelineOptions &options)
{
	CandidateBeamExpansionOptions beamOptions;
	beamOptions.generation = options.generation;
	beamOptions.scoring = options.scoring;
	beamOptions.qualityGate = options.qualityGate;
	beamOptions.finalBudget = CandidateSourceBuilder::semanticCandidateBudget(candidateSourceOptionsFromOptions(options),
		options.budget.maxCandidatesBeforeEmbedding);
	beamOptions.viewerMessagePreset = isViewerMessagePreset(options);
	return beamOptions;
}

static BoundaryRefinementStageOptions boundaryRefinementOptionsFromOptions(const ClipScoringPipelineOptions &options)
{
	BoundaryRefinementStageOptions refinementOptions;
	refinementOptions.generation = options.generation;
	refinementOptions.scoring = options.scoring;
	refinementOptions.qualityGate = options.qualityGate;
	refinementOptions.embeddingProvider = options.embeddingProvider;
	refinementOptions.viewerMessagePreset = isViewerMessagePreset(options);
	return refinementOptions;
}

static FeedbackGuidedCandidateStageOptions feedbackGuidedStageOptionsFromOptions(const ClipScoringPipelineOptions &options)
{
	FeedbackGuidedCandidateStageOptions stageOptions;
	stageOptions.generation = options.generation;
	stageOptions.scoring = options.scoring;
	stageOptions.qualityGate = options.qualityGate;
	stageOptions.ranking = options.ranking;
	stageOptions.videoPath = options.videoPath;
	return stageOptions;
}

static bool isViewerMessagePreset(const ClipScoringPipelineOptions &options)
{
	return options.scoring.presetId == QStringLiteral("viewer_message_response");
}

} // namespace

ClipScoringResult ClipScoringPipeline::score(const RecordingTranscript &transcript,
	const ClipScoringPipelineOptions &options) const
{
	TranscriptIndex index(transcript);
	ClipScoringResult result;
	if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_before_scoring")))
		return result;
	PipelineProgress::report(options, QStringLiteral("Indexing transcript for clip suggestions..."), 1);
	if (index.isEmpty()) {
		result.summary = QStringLiteral("no_candidates: empty_transcript");
		return result;
	}

	const bool viewerPreset = isViewerMessagePreset(options);
	Curation::Feedback::FeedbackRangeMemory feedbackMemory;
	if (viewerPreset && !options.videoPath.trimmed().isEmpty()) {
		feedbackMemory = Curation::Feedback::CurationFeedbackStore::loadRangeMemoryForVideo(
			options.videoPath, options.scoring.presetId, options.contentIds);
	}

	QVector<ClipCandidate> candidates;
	const bool semanticBackendAvailable = options.embeddingProvider && options.embeddingProvider->isAvailable() &&
		options.coarseRetrieval.enabled;
	if (semanticBackendAvailable) {
		if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_before_coarse_retrieval")))
			return result;
		PipelineProgress::report(options, QStringLiteral("Embedding coarse transcript windows..."), 5);
		SemanticCoarseRetriever retriever;
		QElapsedTimer coarseRetrievalTimer;
		coarseRetrievalTimer.start();
		const QVector<SemanticCoarseRegion> regions = retriever.retrieve(index, coarseContextFromOptions(options),
			options.coarseRetrieval, options.embeddingProvider);
		blog(LOG_INFO, "[clip-cropper] Semantic coarse retrieval stage finished. video=%s regions=%d elapsedMs=%lld",
		     options.videoPath.toUtf8().constData(), static_cast<int>(regions.size()),
		     static_cast<long long>(coarseRetrievalTimer.elapsed()));
		if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_during_coarse_retrieval")))
			return result;
		PipelineProgress::report(options, QStringLiteral("Coarse semantic regions selected: %1").arg(static_cast<int>(regions.size())), 18);
		if (!regions.isEmpty()) {
			CandidateSourceBuilder sourceBuilder;
			candidates = sourceBuilder.fromSemanticCoarseRegions(index, regions, candidateSourceOptionsFromOptions(options));
		}
		PipelineProgress::report(options, QStringLiteral("Generated candidate marker windows: %1").arg(static_cast<int>(candidates.size())), 28);
	} else {
		CandidateSourceBuilder sourceBuilder;
		candidates = sourceBuilder.fromLocalHeuristics(index, candidateSourceOptionsFromOptions(options));
		PipelineProgress::report(options, QStringLiteral("Generated local heuristic candidate marker windows: %1").arg(static_cast<int>(candidates.size())), 28);
	}

	if (viewerPreset && feedbackMemory.loaded) {
		FeedbackGuidedCandidateStage feedbackStage;
		candidates = feedbackStage.appendCandidates(index, std::move(candidates), feedbackMemory,
			feedbackGuidedStageOptionsFromOptions(options));
	}

if (candidates.isEmpty()) {
		result.summary = QStringLiteral("no_candidates: candidate_generation_found_no_viable_ranges");
		return result;
	}

	ClipRanker ranker;
	PipelineProgress::report(options, QStringLiteral("Pre-ranking candidate marker windows..."), 32);
	candidates = ranker.rank(std::move(candidates), preSemanticRankingOptionsFromOptions(options));
	clearSelectionMetadata(candidates);
	if (semanticBackendAvailable) {
		CandidateBeamExpander beamExpander;
		candidates = beamExpander.expand(index, std::move(candidates), beamExpansionOptionsFromOptions(options));
		PipelineProgress::report(options, QStringLiteral("Expanded candidate beam variants: %1").arg(static_cast<int>(candidates.size())), 37);
	}
	if (candidates.isEmpty()) {
		result.summary = semanticBackendAvailable
			? QStringLiteral("no_candidates: semantic_coarse_structural_ranking_found_no_viable_ranges")
			: QStringLiteral("no_candidates: pre_semantic_ranking_found_no_viable_ranges");
		return result;
	}

	if (semanticBackendAvailable) {
		const int embeddingLimit = std::max(1, options.budget.maxCandidatesBeforeEmbedding);
		candidates = CandidateStageLimiter::limit(std::move(candidates), embeddingLimit,
			QStringLiteral("embedding_top_candidate_stage_limit:%1").arg(embeddingLimit));
	}

	const int semanticTotal = static_cast<int>(candidates.size());
	if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_before_semantic_scoring")))
		return result;
	PipelineProgress::report(options, QStringLiteral("Embedding/scoring %1 marker candidates...").arg(semanticTotal), 40);
	QElapsedTimer semanticStageTimer;
	semanticStageTimer.start();
	candidates = semanticScoreCandidates(index, options, std::move(candidates));
	blog(LOG_INFO, "[clip-cropper] Semantic embedding/scoring stage finished. video=%s candidates=%d elapsedMs=%lld",
	     options.videoPath.toUtf8().constData(), semanticTotal, static_cast<long long>(semanticStageTimer.elapsed()));
	if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_during_semantic_scoring")))
		return result;
	PipelineProgress::report(options, QStringLiteral("Semantic scoring finished for %1 marker candidates").arg(semanticTotal), 66);

	if (feedbackMemory.loaded) {
		FeedbackConsistencyGate feedbackGate;
		FeedbackConsistencyGateOptions feedbackGateOptions;
		feedbackGateOptions.viewerMessagePreset = viewerPreset;
		candidates = feedbackGate.apply(std::move(candidates), index, feedbackMemory, feedbackGateOptions);
		FeedbackTrainedRanker trainedRanker;
		candidates = trainedRanker.apply(std::move(candidates), index, feedbackMemory, options.scoring.presetId, options.videoPath);
	}

	candidates = CandidateBuilder::enforceSemanticAvailability(std::move(candidates),
		options.budget.requireSemanticScoringWhenEmbeddingProviderEnabled, options.embeddingProvider != nullptr);
	candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [](const ClipCandidate &candidate) {
		return candidate.rejectedByQualityGate;
	}), candidates.end());
	if (candidates.isEmpty()) {
		result.summary = QStringLiteral("no_candidates: semantic_embedding_required_but_unavailable_or_failed");
		return result;
	}

	if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_before_reranking")))
		return result;

	const bool rerankerWasAvailable = options.rerankerOptions.enabled && options.reranker && options.reranker->isAvailable();
	if (rerankerWasAvailable) {
		const int rerankerLimit = CandidateStageLimiter::rerankerLimit(options);
		candidates = CandidateStageLimiter::limit(std::move(candidates), rerankerLimit,
			QStringLiteral("reranker_top_candidate_stage_limit:%1").arg(rerankerLimit));
	}

	SemanticRerankerStage rerankerStage;
	PipelineProgress::report(options, QStringLiteral("Reranking top %1 marker candidates...").arg(static_cast<int>(candidates.size())), 72);
	QElapsedTimer rerankerStageTimer;
	rerankerStageTimer.start();
	const int rerankerCandidateCount = static_cast<int>(candidates.size());
	candidates = rerankerStage.apply(std::move(candidates), rerankerContextFromOptions(options), options.rerankerOptions,
		options.reranker);
	blog(LOG_INFO, "[clip-cropper] Reranker stage finished. video=%s candidates=%d elapsedMs=%lld",
	     options.videoPath.toUtf8().constData(), rerankerCandidateCount, static_cast<long long>(rerankerStageTimer.elapsed()));
	if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_during_reranking")))
		return result;
	PipelineProgress::report(options, QStringLiteral("Reranking completed for %1 marker candidates").arg(static_cast<int>(candidates.size())), 80);

	if (semanticBackendAvailable && options.embeddingProvider && options.embeddingProvider->isAvailable()) {
		const int boundaryLimit = CandidateStageLimiter::boundaryDpLimit(options);
		candidates = CandidateStageLimiter::limit(std::move(candidates), boundaryLimit,
			QStringLiteral("boundary_dp_top_candidate_stage_limit:%1").arg(boundaryLimit));
		if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_before_boundary_dp")))
			return result;
		PipelineProgress::report(options, QStringLiteral("Refining top %1 marker candidates with boundary DP...").arg(static_cast<int>(candidates.size())), 82);
		BoundaryRefinementStage refinementStage;
		QElapsedTimer boundaryStageTimer;
		boundaryStageTimer.start();
		const int boundaryCandidateCount = static_cast<int>(candidates.size());
		candidates = refinementStage.apply(index, std::move(candidates), boundaryRefinementOptionsFromOptions(options));
		blog(LOG_INFO, "[clip-cropper] Boundary DP stage finished. video=%s candidates=%d elapsedMs=%lld",
		     options.videoPath.toUtf8().constData(), boundaryCandidateCount, static_cast<long long>(boundaryStageTimer.elapsed()));
		PipelineProgress::report(options, QStringLiteral("Boundary DP refinement finished for %1 marker candidates").arg(static_cast<int>(candidates.size())), 85);
	}

	if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_before_quality_gate")))
		return result;

	ViewerArcGate viewerArcGate;
	const ViewerArcGateOptions arcGateOptions = viewerArcGateOptionsFromOptions(options);
	viewerArcGate.recoverMissingOpenings(index, candidates, arcGateOptions);

	CandidateQualityGate qualityGate;
	PipelineProgress::report(options, QStringLiteral("Applying semantic quality gate..."), 86);
	candidates = qualityGate.apply(std::move(candidates), qualityGateOptionsFromOptions(options, rerankerWasAvailable));

	if (feedbackMemory.loaded) {
		FeedbackConsistencyGate feedbackGate;
		FeedbackConsistencyGateOptions feedbackGateOptions;
		feedbackGateOptions.viewerMessagePreset = viewerPreset;
		candidates = feedbackGate.apply(std::move(candidates), index, feedbackMemory, feedbackGateOptions);
		FeedbackTrainedRanker trainedRanker;
		candidates = trainedRanker.apply(std::move(candidates), index, feedbackMemory, options.scoring.presetId, options.videoPath);
	}
	candidates = viewerArcGate.apply(std::move(candidates), arcGateOptions);
	if (viewerPreset) {
		blog(LOG_INFO,
		     "[clip-cropper] Feedback-guided candidate gate summary. video=%s total=%d usable=%d rejected=%d positive=%d learned=%d",
		     options.videoPath.toUtf8().constData(), static_cast<int>(candidates.size()), usableCandidateCount(candidates),
		     rejectedCandidateCount(candidates), positiveFeedbackCandidateCount(candidates),
		     learnedFeedbackAcceptedCandidateCount(candidates));
		blog(LOG_INFO, "[clip-cropper] Feedback-guided candidate rejection diagnostics. video=%s %s",
		     options.videoPath.toUtf8().constData(), feedbackGateRejectionDiagnostics(candidates).toUtf8().constData());
	}
	const QVector<ClipCandidate> candidatesBeforeRejectedErase = candidates;
	const QVector<ClipCandidate> rejectedAfterArcGate = topRejectedCandidatesForDiagnostics(candidates, 12, feedbackMemory.loaded ? &feedbackMemory : nullptr);
	const QVector<ClipCandidate> relaxedRejectedAfterArcGate = viewerPreset && feedbackMemory.loaded
		? topRejectedCandidatesForDiagnostics(candidates, 12, &feedbackMemory, DiagnosticReviewedRangeMode::RelaxedExploration)
		: rejectedAfterArcGate;
	candidates.erase(std::remove_if(candidates.begin(), candidates.end(), isRejectedCandidate), candidates.end());
	const int suppressedExistingExactSeeds = suppressExistingReviewRangeExactSeeds(candidates, options.existingReviewRanges);
	if (suppressedExistingExactSeeds > 0) {
		blog(LOG_INFO,
		     "[clip-cropper] Suppressed already-existing exact feedback seed markers before final selection. video=%s suppressed=%d",
		     options.videoPath.toUtf8().constData(), suppressedExistingExactSeeds);
		candidates.erase(std::remove_if(candidates.begin(), candidates.end(), isRejectedCandidate), candidates.end());
	}
	int suppressedReviewedExactSeeds = 0;
	int suppressedReviewedFeedbackCandidates = 0;
	if (viewerPreset && feedbackMemory.loaded) {
		suppressedReviewedExactSeeds = suppressAlreadyReviewedExactPositiveSeeds(candidates, feedbackMemory);
		if (suppressedReviewedExactSeeds > 0) {
			blog(LOG_INFO,
			     "[clip-cropper] Suppressed already-reviewed exact positive feedback seeds before final selection. video=%s suppressed=%d",
			     options.videoPath.toUtf8().constData(), suppressedReviewedExactSeeds);
			candidates.erase(std::remove_if(candidates.begin(), candidates.end(), isRejectedCandidate), candidates.end());
		}
		suppressedReviewedFeedbackCandidates = suppressAlreadyReviewedFeedbackCandidates(candidates, feedbackMemory);
		if (suppressedReviewedFeedbackCandidates > 0) {
			blog(LOG_INFO,
			     "[clip-cropper] Suppressed already-reviewed feedback candidates before final selection. video=%s suppressed=%d",
			     options.videoPath.toUtf8().constData(), suppressedReviewedFeedbackCandidates);
			candidates.erase(std::remove_if(candidates.begin(), candidates.end(), isRejectedCandidate), candidates.end());
		}
	}

	if (PipelineProgress::stopIfCanceled(options, result, QStringLiteral("canceled_before_final_ranking")))
		return result;
	PipelineProgress::report(options, QStringLiteral("Selecting final marker suggestions..."), 92);

	if (candidates.isEmpty() && viewerPreset && feedbackMemory.loaded &&
	    (suppressedExistingExactSeeds > 0 || suppressedReviewedExactSeeds > 0 || suppressedReviewedFeedbackCandidates > 0)) {
		blog(LOG_INFO,
		     "[clip-cropper] Positive feedback replay skipped after suppressing reviewed/existing seeds. video=%s existingSuppressed=%d reviewedSuppressed=%d reviewedFeedbackSuppressed=%d",
		     options.videoPath.toUtf8().constData(), suppressedExistingExactSeeds, suppressedReviewedExactSeeds,
		     suppressedReviewedFeedbackCandidates);
	}

	if (candidates.isEmpty() && viewerPreset && feedbackMemory.loaded && rejectedAfterArcGate.isEmpty() &&
	    suppressedExistingExactSeeds <= 0 && suppressedReviewedExactSeeds <= 0 && suppressedReviewedFeedbackCandidates <= 0) {
		// Positive feedback ranges are ground-truth examples, not fresh suggestions. Replaying
		// them into the timeline after an empty gate result made "suggest new clips" overwrite
		// good markers with already-reviewed ranges and let the user re-rate the same clips.
		// Keep them only as memory/calibration signals; surface new rejected/diagnostic probes
		// below instead of restoring exact positives as upload markers.
		blog(LOG_INFO,
		     "[clip-cropper] Positive feedback replay disabled after empty gate result. video=%s positives=%d",
		     options.videoPath.toUtf8().constData(), static_cast<int>(feedbackMemory.positiveRanges.size()));
	}

	if (candidates.isEmpty()) {
		QVector<ClipCandidate> diagnostics = rejectedAfterArcGate;
		appendUniqueDiagnosticCandidates(diagnostics, relaxedRejectedAfterArcGate, 12);
		if (diagnostics.isEmpty() && rejectedCandidateCount(candidatesBeforeRejectedErase) > 0) {
			diagnostics = topRejectedCandidatesForDiagnostics(candidatesBeforeRejectedErase, 12,
				feedbackMemory.loaded ? &feedbackMemory : nullptr,
				DiagnosticReviewedRangeMode::RelaxedExploration);
			if (!diagnostics.isEmpty())
				blog(LOG_INFO,
				     "[clip-cropper] Recovered unreviewed diagnostics from rejected gate pool. video=%s rejected=%d diagnostics=%d",
				     options.videoPath.toUtf8().constData(), rejectedCandidateCount(candidatesBeforeRejectedErase),
				     static_cast<int>(diagnostics.size()));
			else
				blog(LOG_INFO,
				     "[clip-cropper] Rejected gate pool contained only reviewed/ignored diagnostic neighborhoods. video=%s rejected=%d",
				     options.videoPath.toUtf8().constData(), rejectedCandidateCount(candidatesBeforeRejectedErase));
		}
		const int feedbackSuppressedAfterArcGate = feedbackSuppressedCandidateCount(candidatesBeforeRejectedErase);
		const int rejectedAfterArcGateCount = rejectedCandidateCount(candidatesBeforeRejectedErase);
		if (viewerPreset && feedbackMemory.loaded && diagnostics.size() < 4 &&
		    (feedbackSuppressedAfterArcGate > 0 || rejectedAfterArcGateCount > 0 ||
		     suppressedExistingExactSeeds > 0 || suppressedReviewedExactSeeds > 0)) {
			PipelineProgress::report(options, QStringLiteral("Exploring new diagnostic candidates outside rejected feedback ranges..."), 93);
			QElapsedTimer noveltyTimer;
			noveltyTimer.start();
			NoveltyExplorationBuildStats noveltyStats;
			QVector<ClipCandidate> exploration = buildNoveltyExplorationCandidates(index, options, feedbackMemory, &noveltyStats);
			blog(LOG_INFO,
			     "[clip-cropper] Starting novelty exploration fallback. video=%s suppressed=%d raw=%d reviewedSuppressed=%d localAccepted=%d filtered=%d workers=%d generationMs=%lld localScoreMs=%lld diversityMs=%lld negative=%d positive=%d semanticPositive=%d",
			     options.videoPath.toUtf8().constData(), feedbackSuppressedAfterArcGate, noveltyStats.rawCandidates,
			     noveltyStats.reviewedSuppressed, noveltyStats.localAccepted, static_cast<int>(exploration.size()),
			     noveltyStats.workerCount, noveltyStats.generationMs, noveltyStats.localScoreMs, noveltyStats.diversityMs,
			     static_cast<int>(feedbackMemory.negativeRanges.size()),
			     static_cast<int>(feedbackMemory.positiveRanges.size()), semanticPrototypePositiveRangeCount(feedbackMemory));

			long long semanticMs = 0;
			long long rerankMs = 0;
			long long boundaryMs = 0;
			long long gateMs = 0;
			QVector<ClipCandidate> diagnosticFallbackPool = exploration;
			QVector<ClipCandidate> diagnosticPreGatePool;
			if (!exploration.isEmpty()) {
				QElapsedTimer stageTimer;
				if (semanticBackendAvailable && options.embeddingProvider && options.embeddingProvider->isAvailable()) {
					stageTimer.start();
					exploration = semanticScoreCandidates(index, options, std::move(exploration));
					semanticMs = stageTimer.elapsed();
				}
				exploration = CandidateBuilder::enforceSemanticAvailability(std::move(exploration),
					options.budget.requireSemanticScoringWhenEmbeddingProviderEnabled, options.embeddingProvider != nullptr);
				exploration.erase(std::remove_if(exploration.begin(), exploration.end(), [](const ClipCandidate &candidate) {
					return candidate.rejectedByQualityGate && candidate.rejectionReason == QStringLiteral("semantic_embedding_unavailable");
				}), exploration.end());
				SemanticRerankerStage explorationReranker;
				stageTimer.restart();
				exploration = explorationReranker.apply(std::move(exploration), rerankerContextFromOptions(options), options.rerankerOptions,
					options.reranker);
				rerankMs = stageTimer.elapsed();
				if (semanticBackendAvailable && options.embeddingProvider && options.embeddingProvider->isAvailable()) {
					BoundaryRefinementStage explorationRefinement;
					stageTimer.restart();
					exploration = explorationRefinement.apply(index, std::move(exploration), boundaryRefinementOptionsFromOptions(options));
					boundaryMs = stageTimer.elapsed();
				}
				diagnosticPreGatePool = exploration;
				stageTimer.restart();
				CandidateQualityGate explorationQualityGate;
				exploration = explorationQualityGate.apply(std::move(exploration), qualityGateOptionsFromOptions(options, rerankerWasAvailable));
				FeedbackConsistencyGate explorationFeedbackGate;
				FeedbackConsistencyGateOptions explorationFeedbackOptions;
				explorationFeedbackOptions.viewerMessagePreset = true;
				exploration = explorationFeedbackGate.apply(std::move(exploration), index, feedbackMemory, explorationFeedbackOptions);
				FeedbackTrainedRanker explorationTrainedRanker;
				exploration = explorationTrainedRanker.apply(std::move(exploration), index, feedbackMemory, options.scoring.presetId, options.videoPath);
				ViewerArcGate explorationArcGate;
				explorationArcGate.recoverMissingOpenings(index, exploration, arcGateOptions);
				exploration = explorationArcGate.apply(std::move(exploration), arcGateOptions);
				gateMs = stageTimer.elapsed();
				QVector<ClipCandidate> promotedNovelty = noveltyTimelineCandidatesFromCandidates(exploration, feedbackMemory, options.ranking.maxCandidates);
				if (!promotedNovelty.isEmpty()) {
					blog(LOG_INFO,
					     "[clip-cropper] Novelty exploration promoted candidates to timeline. video=%s promoted=%d",
					     options.videoPath.toUtf8().constData(), static_cast<int>(promotedNovelty.size()));
					candidates = std::move(promotedNovelty);
				}
				if (candidates.isEmpty())
					appendUniqueDiagnosticCandidates(diagnostics, explorationDiagnosticsFromCandidates(exploration, feedbackMemory), 12);
				if (candidates.isEmpty() && diagnostics.size() < 4) {
					const int beforeDiagnostics = diagnostics.size();
					appendUniqueDiagnosticCandidates(diagnostics, explorationDiagnosticsFromCandidates(diagnosticPreGatePool, feedbackMemory), 12);
					if (diagnostics.size() > beforeDiagnostics)
						blog(LOG_INFO, "[clip-cropper] Novelty exploration diagnostics recovered from pre-gate pool. video=%s count=%d",
						     options.videoPath.toUtf8().constData(), static_cast<int>(diagnostics.size()));
				}
				if (candidates.isEmpty() && diagnostics.size() < 4) {
					const int beforeDiagnostics = diagnostics.size();
					appendUniqueDiagnosticCandidates(diagnostics, explorationDiagnosticsFromCandidates(diagnosticFallbackPool, feedbackMemory), 12);
					if (diagnostics.size() > beforeDiagnostics)
						blog(LOG_INFO, "[clip-cropper] Novelty exploration diagnostics recovered from local candidate pool. video=%s count=%d",
						     options.videoPath.toUtf8().constData(), static_cast<int>(diagnostics.size()));
				}
			}

			blog(LOG_INFO,
			     "[clip-cropper] Novelty exploration fallback finished. video=%s diagnostics=%d semanticMs=%lld rerankMs=%lld boundaryMs=%lld gateMs=%lld elapsedMs=%lld",
			     options.videoPath.toUtf8().constData(), static_cast<int>(diagnostics.size()),
			     semanticMs, rerankMs, boundaryMs, gateMs, static_cast<long long>(noveltyTimer.elapsed()));
		}

		if (candidates.isEmpty() && diagnostics.size() < 6 && viewerPreset && feedbackMemory.loaded) {
			const int beforeCoverageDiagnostics = diagnostics.size();
			QVector<ClipCandidate> coverageDiagnostics = buildExhaustionCoverageDiagnostics(index, options, feedbackMemory, 6);
			appendUniqueDiagnosticCandidates(diagnostics, coverageDiagnostics, 6);
			if (diagnostics.size() > beforeCoverageDiagnostics)
				blog(LOG_INFO,
				     "[clip-cropper] Coverage exhaustion diagnostics supplemented review pool after reviewed/ignored neighborhoods. video=%s before=%d diagnostics=%d",
				     options.videoPath.toUtf8().constData(), beforeCoverageDiagnostics, static_cast<int>(diagnostics.size()));
		}

		if (candidates.isEmpty()) {
			// Incomplete-arc candidates are useful for human review, but applying them as
			// timeline markers makes the UI look like it found a good clip even when the
			// arc gate explicitly says the viewer message/origin is missing. Preserve them
			// as diagnostics so the user can ignore, reject, or adjust them deliberately.
			result.rejectedCandidateDiagnostics = diagnostics;
			const QString reason = diagnostics.isEmpty()
				? QStringLiteral("feedback_guided_scoring_found_no_consistent_complete_arcs")
				: QStringLiteral("feedback_novelty_exploration_candidates_require_review");
			result.summary = ClipScoringPipelineSummary::noCandidate(reason, diagnostics);
			logRejectedCandidateDiagnostics(options.videoPath, diagnostics.isEmpty() ? QStringLiteral("viewer_arc_gate") : QStringLiteral("novelty_exploration"), diagnostics);
			PipelineProgress::report(options, QStringLiteral("Marker suggestion analysis finished."), 100);
			return result;
		}
	}

	ClipRankerOptions finalRanking = options.ranking;
	bool feedbackDpSelectedFinalPool = false;
	int feedbackDpSelectedCount = 0;
	QVector<ClipCandidate> feedbackDpSelectedCandidates;
	QVector<ClipCandidate> rejectedAfterFinalFeedbackGate;
	if (viewerPreset && feedbackMemory.loaded) {
		FeedbackConsistencyGate feedbackGate;
		FeedbackConsistencyGateOptions feedbackGateOptions;
		feedbackGateOptions.viewerMessagePreset = true;
		candidates = feedbackGate.apply(std::move(candidates), index, feedbackMemory, feedbackGateOptions);
		FeedbackTrainedRanker trainedRanker;
		candidates = trainedRanker.apply(std::move(candidates), index, feedbackMemory, options.scoring.presetId, options.videoPath);
		rejectedAfterFinalFeedbackGate = topRejectedCandidatesForDiagnostics(candidates, 12, &feedbackMemory);
		candidates.erase(std::remove_if(candidates.begin(), candidates.end(), isRejectedCandidate), candidates.end());

		QVector<ClipCandidate> demotedUnsafeDirectPositive;
		auto unsafeEnd = std::remove_if(candidates.begin(), candidates.end(), [&demotedUnsafeDirectPositive](const ClipCandidate &candidate) {
			if (!isUnsafeArcBypass(candidate))
				return false;
			demotedUnsafeDirectPositive.append(demoteUnsafeArcBypass(candidate));
			return true;
		});
		candidates.erase(unsafeEnd, candidates.end());
		if (!demotedUnsafeDirectPositive.isEmpty()) {
			appendUniqueDiagnosticCandidates(rejectedAfterFinalFeedbackGate, demotedUnsafeDirectPositive, 12);
			blog(LOG_INFO,
			     "[clip-cropper] Demoted unsafe direct-positive arc bypass candidates to diagnostics. video=%s demoted=%d",
			     options.videoPath.toUtf8().constData(), static_cast<int>(demotedUnsafeDirectPositive.size()));
		}

		if (candidates.isEmpty()) {
			result.rejectedCandidateDiagnostics = rejectedAfterFinalFeedbackGate;
			result.summary = ClipScoringPipelineSummary::noCandidate(QStringLiteral("feedback_final_selection_found_no_consistent_ranges"), rejectedAfterFinalFeedbackGate);
			logRejectedCandidateDiagnostics(options.videoPath, QStringLiteral("final_feedback_gate"), rejectedAfterFinalFeedbackGate);
			PipelineProgress::report(options, QStringLiteral("Marker suggestion analysis finished."), 100);
			return result;
		}
		FeedbackAwareIntervalSelector feedbackSelector;
		QVector<ClipCandidate> selectedByFeedbackDp = feedbackSelector.select(candidates, index, feedbackMemory, finalRanking);
		if (!selectedByFeedbackDp.isEmpty()) {
			blog(LOG_INFO,
			     "[clip-cropper] Feedback-aware interval DP selected marker pool. video=%s before=%d after=%d",
			     options.videoPath.toUtf8().constData(), static_cast<int>(candidates.size()),
			     static_cast<int>(selectedByFeedbackDp.size()));
			candidates = std::move(selectedByFeedbackDp);
			feedbackDpSelectedFinalPool = true;
			feedbackDpSelectedCount = static_cast<int>(candidates.size());
			feedbackDpSelectedCandidates = candidates;
		}
	}

	if (viewerPreset && feedbackMemory.loaded) {
		QVector<ClipCandidate> diagnostics = rejectedAfterArcGate;
		appendUniqueDiagnosticCandidates(diagnostics, relaxedRejectedAfterArcGate, 12);
		for (const ClipCandidate &candidate : rejectedAfterFinalFeedbackGate) {
			if (!hasSimilarDiagnosticRange(diagnostics, candidate.range))
				diagnostics.append(candidate);
		}
		if (!diagnostics.isEmpty()) {
			logRejectedCandidateDiagnostics(options.videoPath, QStringLiteral("selected_with_rejected_review_pool"), diagnostics);
			result.rejectedCandidateDiagnostics = diagnostics;
		}
	}

	result.candidates = ranker.rank(candidates, finalRanking);
	const int finalRankedCount = static_cast<int>(result.candidates.size());
	if (feedbackDpSelectedFinalPool && feedbackDpSelectedCount > 1 &&
	    result.candidates.size() < std::min(feedbackDpSelectedCount, 3)) {
		QVector<ClipCandidate> restored = feedbackDpSelectedCandidates;
		std::sort(restored.begin(), restored.end(), [](const ClipCandidate &left, const ClipCandidate &right) {
			if (std::fabs(left.scores.final - right.scores.final) > 0.0001)
				return left.scores.final > right.scores.final;
			return left.range.startSec < right.range.startSec;
		});
		const int limit = std::min(finalRanking.maxCandidates, static_cast<int>(restored.size()));
		result.candidates.clear();
		result.candidates.reserve(limit);
		for (int i = 0; i < limit; ++i) {
			ClipCandidate selected = restored.at(i);
			selected.selectedRank = i + 1;
			selected.evidence.append(QStringLiteral("feedback_aware_interval_dp_final_pool_preserved"));
			selected.evidence.append(QStringLiteral("selected_rank:%1").arg(selected.selectedRank));
			selected.evidence.removeDuplicates();
			result.candidates.append(selected);
		}
		blog(LOG_INFO,
		     "[clip-cropper] Preserved feedback-aware DP marker pool after final ranker collapse. video=%s finalRanked=%d restored=%d",
		     options.videoPath.toUtf8().constData(), finalRankedCount,
		     static_cast<int>(result.candidates.size()));
	}
	if (result.candidates.isEmpty())
		result.summary = ClipScoringPipelineSummary::noCandidate(QStringLiteral("final_ranking_found_no_viable_ranges"), candidates);
	else {
		for (int i = 0; i < static_cast<int>(result.candidates.size()); ++i)
			PipelineProgress::report(options,
				PipelineProgress::candidateMessage(QStringLiteral("Selected marker"), result.candidates.at(i), i + 1,
					static_cast<int>(result.candidates.size())),
				94 + ((i * 5) / std::max(1, static_cast<int>(result.candidates.size()))));
		result.summary = ClipScoringPipelineSummary::build(result.candidates);
	}
	PipelineProgress::report(options, QStringLiteral("Marker suggestion analysis finished."), 100);
	return result;
}

SemanticCoarseRetrievalContext ClipScoringPipeline::coarseContextFromOptions(
	const ClipScoringPipelineOptions &options) const
{
	SemanticCoarseRetrievalContext context;
	context.presetId = options.scoring.presetId;
	context.mainTarget = options.scoring.mainTarget;
	context.transcriptionLanguage = options.scoring.transcriptionLanguage;
	context.sourceLanguage = options.scoring.sourceLanguage;
	context.reliableMainTarget = options.scoring.reliableMainTarget;
	return context;
}

SemanticScoringContext ClipScoringPipeline::semanticContextFromOptions(const ClipScoringPipelineOptions &options) const
{
	SemanticScoringContext context;
	context.presetId = options.scoring.presetId;
	context.mainTarget = options.scoring.mainTarget;
	context.transcriptionLanguage = options.scoring.transcriptionLanguage;
	context.sourceLanguage = options.scoring.sourceLanguage;
	context.reliableMainTarget = options.scoring.reliableMainTarget;
	return context;
}

SemanticRerankerContext ClipScoringPipeline::rerankerContextFromOptions(const ClipScoringPipelineOptions &options) const
{
	SemanticRerankerContext context;
	context.presetId = options.scoring.presetId;
	context.mainTarget = options.scoring.mainTarget;
	context.transcriptionLanguage = options.scoring.transcriptionLanguage;
	context.sourceLanguage = options.scoring.sourceLanguage;
	context.reliableMainTarget = options.scoring.reliableMainTarget;
	return context;
}

ClipRankerOptions ClipScoringPipeline::preSemanticRankingOptionsFromOptions(
	const ClipScoringPipelineOptions &options) const
{
	ClipRankerOptions ranking = options.ranking;
	const int requested = std::max(1, options.budget.maxCandidatesBeforeEmbedding);
	const int rawBudget = std::max(1, options.generation.maxRawCandidates);
	if (isViewerMessagePreset(options)) {
		// Viewer-message boundaries need several nearby variants from the same semantic region.
		// Do not apply the final marker spacing here; final MMR will de-duplicate later.
		ranking.maxCandidates = std::min(rawBudget, std::max(requested * 2, requested + 24));
		ranking.minSpacingSec = 0.0;
	} else {
		ranking.maxCandidates = std::max(1, std::min(24, requested));
		ranking.minSpacingSec = options.budget.preSemanticMinSpacingSec;
	}
	ranking.minFinalScore = options.budget.preSemanticMinFinalScore;
	ranking.useMmr = false;
	return ranking;
}

CandidateQualityGateOptions ClipScoringPipeline::qualityGateOptionsFromOptions(
	const ClipScoringPipelineOptions &options, bool rerankerWasAvailable) const
{
	CandidateQualityGateOptions qualityOptions = options.qualityGate;
	if (qualityOptions.presetId.trimmed().isEmpty())
		qualityOptions.presetId = options.scoring.presetId;
	if (qualityOptions.mainTarget.trimmed().isEmpty())
		qualityOptions.mainTarget = options.scoring.mainTarget;
	qualityOptions.reliableMainTarget = options.scoring.reliableMainTarget;
	qualityOptions.minFinalScore = std::max(qualityOptions.minFinalScore, options.ranking.minFinalScore * 0.72);
	if (rerankerWasAvailable) {
		qualityOptions.requireRerankerWhenAvailable = true;
		qualityOptions.rejectInvalidRerankerWhenRequired = true;
		qualityOptions.minRerankerScoreWhenAvailable =
			std::max(qualityOptions.minRerankerScoreWhenAvailable, 0.55);
		qualityOptions.minRerankerRawScoreWhenAvailable =
			std::max(qualityOptions.minRerankerRawScoreWhenAvailable, 0.50);
		qualityOptions.minStrongRerankerRawScoreWhenAvailable =
			std::max(qualityOptions.minStrongRerankerRawScoreWhenAvailable, 0.78);
		qualityOptions.minConditionalRerankerRawScoreWhenAvailable =
			std::max(qualityOptions.minConditionalRerankerRawScoreWhenAvailable, 0.68);
	}
	if (isViewerMessagePreset(options)) {
		// In the clean feedback-guided architecture, rejected candidates are not recovered
		// by a quality-gate failsafe. Feedback-guided generation and the final consistency
		// gate are the only recovery paths.
		qualityOptions.maxFailsafeRecoveredCandidates = 0;
	}
	return qualityOptions;
}

QVector<ClipCandidate> ClipScoringPipeline::semanticScoreCandidates(const TranscriptIndex &index,
	const ClipScoringPipelineOptions &options, QVector<ClipCandidate> candidates) const
{
	if (candidates.isEmpty())
		return candidates;

	SemanticClipScorer semanticScorer;
	QVector<ClipCandidate> scored = semanticScorer.scoreBatch(index, candidates, semanticContextFromOptions(options),
		options.semantic, options.embeddingProvider);
	for (ClipCandidate &candidate : scored)
		candidate.evidence.append(QStringLiteral("semantic_embedding_batch_stage"));
	return scored;
}
