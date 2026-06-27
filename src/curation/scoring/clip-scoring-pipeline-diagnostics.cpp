#include "curation/scoring/clip-scoring-pipeline-detail.hpp"
#include "curation/feedback/curation-feedback-store.hpp"
#include "curation/scoring/candidate-source-builder.hpp"
#include "curation/scoring/feedback-similarity-scorer.hpp"
#include <QStringList>

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

#include <algorithm>
#include <cmath>

namespace Curation::Scoring::ClipScoringPipelineDetail {

bool isViewerMessagePreset(const ClipScoringPipelineOptions &options);
CandidateSourceBuilderOptions candidateSourceOptionsFromOptions(const ClipScoringPipelineOptions &options);

bool isSelectionEvidence(const QString &evidence)
{
	return evidence == QStringLiteral("mmr_diversity_selected") ||
	       evidence.startsWith(QStringLiteral("selected_rank:")) || evidence.startsWith(QStringLiteral("mmr:")) ||
	       evidence.startsWith(QStringLiteral("mmr_similarity:"));
}

void clearSelectionMetadata(QVector<ClipCandidate> &candidates)
{
	for (ClipCandidate &candidate : candidates) {
		candidate.selectedRank = 0;
		candidate.selectedMmrScore = 0.0;
		candidate.selectedMmrSimilarity = 0.0;
		candidate.evidence.erase(std::remove_if(candidate.evidence.begin(), candidate.evidence.end(),
							isSelectionEvidence),
					 candidate.evidence.end());
	}
}

int usableCandidateCount(const QVector<ClipCandidate> &candidates)
{
	return static_cast<int>(
		std::count_if(candidates.constBegin(), candidates.constEnd(), [](const ClipCandidate &candidate) {
			return candidate.range.endSec > candidate.range.startSec &&
			       !candidate.text.trimmed().isEmpty() && !candidate.rejectedByQualityGate &&
			       !candidate.rejectedAsNoise;
		}));
}

int rejectedCandidateCount(const QVector<ClipCandidate> &candidates)
{
	return static_cast<int>(
		std::count_if(candidates.constBegin(), candidates.constEnd(), [](const ClipCandidate &candidate) {
			return candidate.rejectedByQualityGate || candidate.rejectedAsNoise;
		}));
}

int positiveFeedbackCandidateCount(const QVector<ClipCandidate> &candidates)
{
	return static_cast<int>(
		std::count_if(candidates.constBegin(), candidates.constEnd(), [](const ClipCandidate &candidate) {
			return candidate.source.contains(QStringLiteral("feedback_positive")) ||
			       candidate.evidence.contains(QStringLiteral("feedback_positive_exact_seed_preserved")) ||
			       candidate.evidence.contains(
				       QStringLiteral("complete_viewer_arc_gate_passed_by_user_feedback")) ||
			       candidate.evidence.contains(
				       QStringLiteral("complete_viewer_arc_gate_passed_by_feedback_trained_ranker")) ||
			       candidate.evidence.contains(QStringLiteral("feedback_trained_ranker_strong_accept")) ||
			       candidate.evidence.contains(QStringLiteral("feedback_trained_ranker_accept"));
		}));
}

int learnedFeedbackAcceptedCandidateCount(const QVector<ClipCandidate> &candidates)
{
	return static_cast<int>(
		std::count_if(candidates.constBegin(), candidates.constEnd(), [](const ClipCandidate &candidate) {
			return candidate.evidence.contains(
				       QStringLiteral("complete_viewer_arc_gate_passed_by_feedback_trained_ranker")) ||
			       candidate.evidence.contains(QStringLiteral("feedback_trained_ranker_strong_accept")) ||
			       candidate.evidence.contains(QStringLiteral("feedback_trained_ranker_accept"));
		}));
}

bool isRejectedCandidate(const ClipCandidate &candidate)
{
	return candidate.rejectedByQualityGate || candidate.rejectedAsNoise;
}

bool candidateHasEvidence(const ClipCandidate &candidate, const QString &needle)
{
	for (const QString &evidence : candidate.evidence) {
		if (evidence == needle || evidence.contains(needle))
			return true;
	}
	return false;
}

bool isExactPositiveFeedbackSeed(const ClipCandidate &candidate)
{
	return candidate.source == QStringLiteral("feedback_positive_exact_seed") ||
	       candidate.source == QStringLiteral("feedback_positive_seed") ||
	       candidateHasEvidence(candidate, QStringLiteral("feedback_positive_exact_seed")) ||
	       candidateHasEvidence(candidate, QStringLiteral("feedback_positive_seed"));
}

bool isUnsafeArcBypass(const ClipCandidate &candidate)
{
	const bool directPositiveBypass = candidateHasEvidence(
		candidate, QStringLiteral("complete_viewer_arc_gate_passed_by_direct_positive_feedback"));
	const bool selfContainedAdviceBypass = candidateHasEvidence(
		candidate, QStringLiteral("complete_viewer_arc_gate_passed_by_self_contained_advice_feedback"));
	if (!directPositiveBypass && !selfContainedAdviceBypass)
		return false;
	if (isExactPositiveFeedbackSeed(candidate))
		return false;

	const bool clearedPriorQualityRejection =
		candidateHasEvidence(candidate, QStringLiteral("arc_gate_cleared_prior_quality_rejection"));
	const bool missingArcOrOrigin =
		candidateHasEvidence(candidate, QStringLiteral("quality_rejected:missing_contextual_arc")) ||
		candidateHasEvidence(
			candidate,
			QStringLiteral("arc_gate_cleared_prior_quality_rejection_from:missing_contextual_arc")) ||
		candidateHasEvidence(candidate, QStringLiteral("exchange_arc_window_dfs_no_valid_origin_or_answer")) ||
		candidateHasEvidence(candidate, QStringLiteral("exchange_arc_no_valid_subspan")) ||
		candidateHasEvidence(candidate, QStringLiteral("reason:missing_viewer_message_cue")) ||
		candidateHasEvidence(candidate, QStringLiteral("reason:missing_explicit_opening"));
	const bool noArcShape = candidate.scores.arcCompleteness <= 0.08 && candidate.scores.arcOpening <= 0.08 &&
				candidate.scores.arcDevelopment <= 0.08 && candidate.scores.arcConclusion <= 0.08;
	return missingArcOrOrigin && (clearedPriorQualityRejection || noArcShape);
}

ClipCandidate demoteUnsafeArcBypass(ClipCandidate candidate)
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

bool hasSimilarDiagnosticRange(const QVector<ClipCandidate> &diagnostics, const ClipDuration &range);

double candidateEvidenceScoreValue(const ClipCandidate &candidate, const QString &prefix)
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

bool hasHardArcBlockerEvidence(const ClipCandidate &candidate)
{
	return candidateHasEvidence(candidate, QStringLiteral("arc_gate_hard_context_blocker")) ||
	       candidateHasEvidence(candidate, QStringLiteral("multiple_viewer_messages_inside_arc")) ||
	       candidateHasEvidence(candidate, QStringLiteral("topic_shift_before_resolution"));
}

bool isRecoverableIncompleteArcDiagnostic(const ClipCandidate &candidate)
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
				       candidate.rejectionReason ==
					       QStringLiteral("novelty_exploration_review_required");
	if (!recoverableReason)
		return false;

	const double positiveRange = candidateEvidenceScoreValue(candidate, QStringLiteral("feedback_positive_range:"));
	const double positiveText = candidateEvidenceScoreValue(candidate, QStringLiteral("feedback_positive_text:"));
	const double negativeRange = candidateEvidenceScoreValue(candidate, QStringLiteral("feedback_negative_range:"));
	const double negativeText = candidateEvidenceScoreValue(candidate, QStringLiteral("feedback_negative_text:"));
	const double feedbackMargin = candidateEvidenceScoreValue(candidate, QStringLiteral("feedback_margin:"));
	const bool positiveBoundaryVariant =
		candidate.source.contains(QStringLiteral("feedback_positive_boundary_variant")) ||
		candidateHasEvidence(candidate, QStringLiteral("feedback_positive_boundary_variant"));
	const bool noveltyPositiveProbe =
		candidateHasEvidence(candidate, QStringLiteral("feedback_novelty_relaxed_negative_review_probe")) &&
		(positiveRange >= 0.32 || positiveText >= 0.42);
	const bool positiveGuided = positiveBoundaryVariant || noveltyPositiveProbe || positiveRange >= 0.34 ||
				    positiveText >= 0.46 || feedbackMargin >= 0.16;
	const bool semanticBody =
		candidate.scores.semanticClipValue >= 0.62 || candidate.scores.semanticHook >= 0.62 ||
		candidate.scores.semanticOpeningHook >= 0.62 || candidate.scores.semanticResolution >= 0.62 ||
		candidate.scores.semanticEndingResolution >= 0.62 || candidate.scores.rerankerRaw >= 0.28;
	const bool notDominatedByNegative = negativeRange < 0.72 && negativeText < 0.74 &&
					    (positiveRange + positiveText + 0.10) >= (negativeRange + negativeText);
	return positiveGuided && semanticBody && notDominatedByNegative;
}

QVector<ClipCandidate> tentativeRecoverableReviewMarkersFromDiagnostics(const QVector<ClipCandidate> &diagnostics,
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
		candidate.scores.final = std::max(candidate.scores.final,
						  0.42 + std::min(0.18, candidate.scores.semanticClipValue * 0.12));
		candidate.source =
			QStringLiteral("tentative_recoverable_review_marker:%1").arg(candidate.source.left(120));
		candidate.evidence.append(
			QStringLiteral("tentative_recoverable_marker_from_incomplete_arc_diagnostic"));
		candidate.evidence.append(QStringLiteral("review_marker_requires_human_boundary_decision"));
		candidate.evidence.append(QStringLiteral("not_training_positive_until_user_approval"));
		candidate.evidence.removeDuplicates();
		markers.append(candidate);
	}
	return markers;
}

bool isFeedbackSuppressedRejectedCandidate(const ClipCandidate &candidate)
{
	return candidateHasEvidence(candidate, QStringLiteral("feedback_negative_suppressed")) ||
	       candidateHasEvidence(candidate, QStringLiteral("feedback_consistency_rejected"));
}

QString diagnosticEvidenceSummary(const ClipCandidate &candidate)
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

double diagnosticRangeCenter(const ClipDuration &range)
{
	return range.startSec + ((range.endSec - range.startSec) * 0.5);
}

bool hasSimilarDiagnosticRange(const QVector<ClipCandidate> &diagnostics, const ClipDuration &range)
{
	for (const ClipCandidate &candidate : diagnostics) {
		if (FeedbackSimilarityScorer::rangeSimilarity(candidate.range, range) >= 0.64)
			return true;
		const double centerDistance =
			std::fabs(diagnosticRangeCenter(candidate.range) - diagnosticRangeCenter(range));
		const double boundaryDistance = std::fabs(candidate.range.startSec - range.startSec) +
						std::fabs(candidate.range.endSec - range.endSec);
		if (centerDistance <= 28.0 && boundaryDistance <= 52.0)
			return true;
	}
	return false;
}

bool hasNearbyDiagnosticReviewCluster(const QVector<ClipCandidate> &diagnostics, const ClipDuration &range)
{
	for (const ClipCandidate &candidate : diagnostics) {
		const double centerDistance =
			std::fabs(diagnosticRangeCenter(candidate.range) - diagnosticRangeCenter(range));
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

void appendUniqueDiagnosticCandidates(QVector<ClipCandidate> &target, const QVector<ClipCandidate> &candidates,
				      int limit)
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

bool validMemoryRange(const ClipDuration &range)
{
	return std::isfinite(range.startSec) && std::isfinite(range.endSec) && range.endSec > range.startSec;
}

double rangeOverlapSec(const ClipDuration &left, const ClipDuration &right)
{
	return std::max(0.0, std::min(left.endSec, right.endSec) - std::max(left.startSec, right.startSec));
}

bool rangeMatchesReviewedSignal(const ClipDuration &range, const Curation::Feedback::FeedbackRangeSignal &signal)
{
	if (!validMemoryRange(range) || !validMemoryRange(signal.range))
		return false;
	const double similarity = FeedbackSimilarityScorer::rangeSimilarity(signal.range, range);
	if (similarity >= 0.78)
		return true;
	const double boundaryDistance =
		std::fabs(signal.range.startSec - range.startSec) + std::fabs(signal.range.endSec - range.endSec);
	if (boundaryDistance <= 18.0)
		return true;
	const double centerDistance = std::fabs(diagnosticRangeCenter(signal.range) - diagnosticRangeCenter(range));
	const double minDuration = std::min(signal.range.endSec - signal.range.startSec, range.endSec - range.startSec);
	return centerDistance <= 24.0 && minDuration > 0.0 &&
	       rangeOverlapSec(signal.range, range) >= minDuration * 0.70;
}

bool rangeWasAlreadyReviewedByFeedback(const ClipDuration &range, const Curation::Feedback::FeedbackRangeMemory &memory)
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

bool rangeMatchesExactReviewedSignal(const ClipDuration &range, const Curation::Feedback::FeedbackRangeSignal &signal)
{
	if (!validMemoryRange(range) || !validMemoryRange(signal.range))
		return false;
	if (FeedbackSimilarityScorer::rangeSimilarity(signal.range, range) >= 0.94)
		return true;
	const double boundaryDistance =
		std::fabs(signal.range.startSec - range.startSec) + std::fabs(signal.range.endSec - range.endSec);
	if (boundaryDistance <= 6.0)
		return true;
	const double centerDistance = std::fabs(diagnosticRangeCenter(signal.range) - diagnosticRangeCenter(range));
	const double minDuration = std::min(signal.range.endSec - signal.range.startSec, range.endSec - range.startSec);
	return centerDistance <= 8.0 && minDuration > 0.0 && rangeOverlapSec(signal.range, range) >= minDuration * 0.92;
}

bool rangeWasExactlyReviewedByFeedback(const ClipDuration &range, const Curation::Feedback::FeedbackRangeMemory &memory)
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

bool isIgnoredDiagnosticFeedbackSignal(const Curation::Feedback::FeedbackRangeSignal &signal)
{
	return signal.ignoreForTraining || signal.decision == QStringLiteral("ignored_diagnostic") ||
	       signal.reason.contains(QStringLiteral("ignored_diagnostic"), Qt::CaseInsensitive) ||
	       signal.reason.contains(QStringLiteral("not_training_signal"), Qt::CaseInsensitive);
}

bool rangeMatchesIgnoredDiagnosticNeighborhood(const ClipDuration &range,
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
	const double boundaryDistance =
		std::fabs(signal.range.startSec - range.startSec) + std::fabs(signal.range.endSec - range.endSec);
	if (boundaryDistance <= 70.0)
		return true;
	const double centerDistance = std::fabs(diagnosticRangeCenter(signal.range) - diagnosticRangeCenter(range));
	const double minDuration = std::min(signal.range.endSec - signal.range.startSec, range.endSec - range.startSec);
	if (centerDistance <= 65.0 && minDuration > 0.0 &&
	    rangeOverlapSec(signal.range, range) >= std::max(8.0, minDuration * 0.35))
		return true;
	return centerDistance <= 38.0;
}

bool rangeMatchesReviewedSignalForExploration(const ClipDuration &range,
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
	const double boundaryDistance =
		std::fabs(signal.range.startSec - range.startSec) + std::fabs(signal.range.endSec - range.endSec);
	if (boundaryDistance <= 7.0)
		return true;
	const double centerDistance = std::fabs(diagnosticRangeCenter(signal.range) - diagnosticRangeCenter(range));
	const double minDuration = std::min(signal.range.endSec - signal.range.startSec, range.endSec - range.startSec);
	return centerDistance <= 10.0 && minDuration > 0.0 &&
	       rangeOverlapSec(signal.range, range) >= minDuration * 0.88;
}

bool rangeWasAlreadyReviewedByFeedbackForExploration(const ClipDuration &range,
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

bool diagnosticRangeWasAlreadyReviewed(const ClipDuration &range,
				       const Curation::Feedback::FeedbackRangeMemory *feedbackMemory,
				       DiagnosticReviewedRangeMode reviewedRangeMode)
{
	if (!feedbackMemory)
		return false;
	return reviewedRangeMode == DiagnosticReviewedRangeMode::RelaxedExploration
		       ? rangeWasAlreadyReviewedByFeedbackForExploration(range, *feedbackMemory)
		       : rangeWasAlreadyReviewedByFeedback(range, *feedbackMemory);
}

QVector<ClipCandidate>
topRejectedCandidatesForDiagnostics(const QVector<ClipCandidate> &candidates, int limit,
				    const Curation::Feedback::FeedbackRangeMemory *feedbackMemory,
				    DiagnosticReviewedRangeMode reviewedRangeMode)
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

void logRejectedCandidateDiagnostics(const QString &videoPath, const QString &stage,
				     const QVector<ClipCandidate> &diagnostics)
{
	if (diagnostics.isEmpty())
		return;
	blog(LOG_INFO, "[clip-cropper] Top rejected candidate diagnostics preserved. video=%s stage=%s count=%d",
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

QString feedbackGateRejectionDiagnostics(const QVector<ClipCandidate> &candidates)
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
		const auto has = [&candidate](const QString &evidence) {
			return candidateHasEvidence(candidate, evidence);
		};
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
		if (has(QStringLiteral("feedback_negative_suppressed")) ||
		    has(QStringLiteral("feedback_consistency_rejected")))
			++feedbackSuppressed;
	}
	return QStringLiteral(
		       "incomplete=%1 hard=%2 missingOrigin=%3 learnedRejected=%4 notPositive=%5 below=%6 negative=%7 rankerHard=%8 feedbackSuppressed=%9")
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

int feedbackSuppressedCandidateCount(const QVector<ClipCandidate> &candidates)
{
	return static_cast<int>(
		std::count_if(candidates.constBegin(), candidates.constEnd(), [](const ClipCandidate &candidate) {
			return isFeedbackSuppressedRejectedCandidate(candidate);
		}));
}

int semanticPrototypePositiveRangeCount(const Curation::Feedback::FeedbackRangeMemory &memory)
{
	return static_cast<int>(std::count_if(memory.positiveRanges.constBegin(), memory.positiveRanges.constEnd(),
					      [](const Curation::Feedback::FeedbackRangeSignal &signal) {
						      return signal.semanticPrototypeEligible;
					      }));
}
} // namespace Curation::Scoring::ClipScoringPipelineDetail
