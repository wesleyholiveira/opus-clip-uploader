#include "curation/feedback/curation-feedback-detail.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Curation::Feedback::Detail {

bool validFeedbackRange(const ClipDuration &range)
{
	return std::isfinite(range.startSec) && std::isfinite(range.endSec) && range.endSec > range.startSec;
}

bool feedbackRangeMeaningfullyEdited(const ClipDuration &generated, const ClipDuration &user)
{
	if (!validFeedbackRange(generated) || !validFeedbackRange(user))
		return false;
	const double startDelta = std::fabs(generated.startSec - user.startSec);
	const double endDelta = std::fabs(generated.endSec - user.endSec);
	const double durationDelta = std::fabs((generated.endSec - generated.startSec) - (user.endSec - user.startSec));
	return startDelta > 0.25 || endDelta > 0.25 || durationDelta > 0.25;
}

ClipDuration recordRange(const QJsonObject &record, const QString &startKey, const QString &endKey)
{
	return ClipDuration{record.value(startKey).toDouble(std::numeric_limits<double>::quiet_NaN()),
			    record.value(endKey).toDouble(std::numeric_limits<double>::quiet_NaN())};
}

bool structuredFeedbackBool(const QJsonObject &record, const QString &key, bool fallback)
{
	const QJsonObject structured = record.value(QStringLiteral("explicit_structured_feedback")).toObject();
	if (structured.contains(key))
		return structured.value(key).toBool(fallback);
	const QString flatKey = QStringLiteral("feedback_%1").arg(key);
	if (record.contains(flatKey))
		return record.value(flatKey).toBool(fallback);
	return fallback;
}

QString structuredFeedbackString(const QJsonObject &record, const QString &key)
{
	const QJsonObject structured = record.value(QStringLiteral("explicit_structured_feedback")).toObject();
	if (structured.contains(key))
		return structured.value(key).toString();
	return record.value(key).toString();
}

bool diagnosticReasonIsIncompleteViewerArc(const QJsonObject &record)
{
	return structuredFeedbackString(record, QStringLiteral("diagnostic_rejection_reason"))
		.contains(QStringLiteral("incomplete_viewer_arc"), Qt::CaseInsensitive);
}

bool structuredFeedbackMarksBadTopic(const QJsonObject &record)
{
	return record.value(QStringLiteral("generated_feedback_class")).toString() == QStringLiteral("bad_topic") ||
	       structuredFeedbackBool(record, QStringLiteral("bad_topic"));
}

bool structuredFeedbackMarksRecoverableBoundary(const QJsonObject &record)
{
	if (structuredFeedbackMarksBadTopic(record))
		return false;
	return record.value(QStringLiteral("generated_feedback_class")).toString() ==
		       QStringLiteral("good_topic_bad_boundary") ||
	       structuredFeedbackBool(record, QStringLiteral("boundary_recoverable")) ||
	       structuredFeedbackBool(record, QStringLiteral("good_topic_bad_boundary")) ||
	       structuredFeedbackBool(record, QStringLiteral("incomplete_but_recoverable"));
}

bool structuredFeedbackIgnoresTraining(const QJsonObject &record)
{
	return record.value(QStringLiteral("generated_feedback_class")).toString() ==
		       QStringLiteral("ignored_diagnostic") ||
	       record.value(QStringLiteral("decision")).toString().trimmed().toLower() ==
		       QStringLiteral("ignored_diagnostic") ||
	       record.value(QStringLiteral("explicit_review_decision")).toString().trimmed().toLower() ==
		       QStringLiteral("ignored_diagnostic") ||
	       structuredFeedbackBool(record, QStringLiteral("ignore_for_training"));
}

bool structuredFeedbackMarksWeakNegative(const QJsonObject &record)
{
	return record.value(QStringLiteral("generated_feedback_class")).toString() == QStringLiteral("weak_negative") ||
	       structuredFeedbackBool(record, QStringLiteral("weak_negative"));
}

bool diagnosticReasonIsExploratoryOrLowSignal(const QJsonObject &record)
{
	const QString reason = structuredFeedbackString(record, QStringLiteral("diagnostic_rejection_reason"));
	return reason.contains(QStringLiteral("novelty_exploration_review_required"), Qt::CaseInsensitive) ||
	       reason.contains(QStringLiteral("too_short"), Qt::CaseInsensitive) ||
	       reason.contains(QStringLiteral("incomplete_viewer_arc"), Qt::CaseInsensitive);
}

bool structuredFeedbackDescribesCompleteClip(const QJsonObject &record)
{
	return structuredFeedbackBool(record, QStringLiteral("has_beginning")) &&
	       structuredFeedbackBool(record, QStringLiteral("has_development")) &&
	       structuredFeedbackBool(record, QStringLiteral("has_conclusion")) &&
	       structuredFeedbackBool(record, QStringLiteral("has_single_topic")) &&
	       structuredFeedbackBool(record, QStringLiteral("has_smooth_ending")) &&
	       structuredFeedbackBool(record, QStringLiteral("has_good_hook")) &&
	       structuredFeedbackBool(record, QStringLiteral("has_viewer_cue")) &&
	       !structuredFeedbackBool(record, QStringLiteral("has_topic_shift")) &&
	       !structuredFeedbackBool(record, QStringLiteral("has_meta_noise")) &&
	       !structuredFeedbackBool(record, QStringLiteral("starts_too_late")) &&
	       !structuredFeedbackBool(record, QStringLiteral("starts_too_early")) &&
	       !structuredFeedbackBool(record, QStringLiteral("ends_too_early")) &&
	       !structuredFeedbackBool(record, QStringLiteral("overextended_after_resolution"));
}

bool structuredFeedbackForcesSemanticPositive(const QJsonObject &record)
{
	return structuredFeedbackBool(record, QStringLiteral("semantic_positive_example")) ||
	       structuredFeedbackBool(record, QStringLiteral("approved_corrected_range"));
}

void countPositiveSemanticEligibility(FeedbackRangeMemory &memory, bool semanticPrototypeEligible)
{
	if (semanticPrototypeEligible)
		++memory.semanticPrototypePositiveSignals;
	else
		++memory.boundaryOnlyPositiveSignals;
}

bool isDefaultNoMarkerPlaceholderFeedback(const QJsonObject &record)
{
	const QString decision = record.value(QStringLiteral("decision")).toString().trimmed().toLower();
	if (decision != QStringLiteral("added_by_user"))
		return false;

	const QString reviewSettingsKey = record.value(QStringLiteral("review_settings_key")).toString();
	if (!reviewSettingsKey.endsWith(QStringLiteral(".no_markers")) &&
	    !reviewSettingsKey.contains(QStringLiteral(".no_markers")))
		return false;

	const ClipDuration user = recordRange(record, QStringLiteral("user_start_sec"), QStringLiteral("user_end_sec"));
	return validFeedbackRange(user) && std::fabs(user.startSec) <= 0.05 && std::fabs(user.endSec - 90.0) <= 0.25;
}

bool presetMatchesFeedback(const QString &recordPreset, const QString &requestedPreset)
{
	const QString rowPreset = recordPreset.trimmed().isEmpty() ? QStringLiteral("auto") : recordPreset.trimmed();
	const QString preset = requestedPreset.trimmed().isEmpty() ? QStringLiteral("auto") : requestedPreset.trimmed();
	return rowPreset == preset || rowPreset == QStringLiteral("auto") || preset == QStringLiteral("auto");
}

bool rangeLooksLikeColdStartPrelude(const ClipDuration &range)
{
	if (!validFeedbackRange(range))
		return false;
	// Cross-video temporal memory is intentionally narrow. It is safe for repeated live
	// preludes/setup/music at the start of long recordings, but it would be too risky
	// to apply arbitrary rejected timestamps globally across unrelated videos.
	return range.startSec >= 0.0 && range.startSec <= 210.0 && range.endSec <= 390.0;
}

bool feedbackDecisionCanBeUsedAcrossVideos(const QString &decision, const QString &explicitDecision)
{
	return decision == QStringLiteral("rejected") || explicitDecision == QStringLiteral("disliked") ||
	       decision == QStringLiteral("adjusted") || decision == QStringLiteral("accepted") ||
	       explicitDecision == QStringLiteral("liked") || decision == QStringLiteral("added_by_user");
}

QString crossVideoReason(const QString &reason)
{
	const QString clean = reason.trimmed();
	return clean.isEmpty() ? QStringLiteral("cross_video_cold_start_feedback")
			       : QStringLiteral("cross_video_cold_start_feedback:%1").arg(clean.left(96));
}

bool appendRangeSignal(QVector<FeedbackRangeSignal> &results, const ClipDuration &range, const QString &decision,
		       const QString &source, const QString &reason, double weight, int sequence,
		       bool semanticPrototypeEligible, bool weakNegative, bool ignoreForTraining)
{
	if (!validFeedbackRange(range))
		return false;
	FeedbackRangeSignal signal;
	signal.range = range;
	signal.decision = decision;
	signal.source = source;
	signal.reason = reason;
	signal.weight = std::clamp(weight, 0.05, 4.0);
	signal.sequence = sequence;
	signal.semanticPrototypeEligible = semanticPrototypeEligible;
	signal.weakNegative = weakNegative;
	signal.ignoreForTraining = ignoreForTraining;
	results.append(signal);
	return true;
}

double feedbackRangeDuration(const ClipDuration &range)
{
	return std::max(0.0, range.endSec - range.startSec);
}

double feedbackRangeCenter(const ClipDuration &range)
{
	return range.startSec + ((range.endSec - range.startSec) * 0.5);
}

double feedbackRangeOverlap(const ClipDuration &left, const ClipDuration &right)
{
	return std::max(0.0, std::min(left.endSec, right.endSec) - std::max(left.startSec, right.startSec));
}

bool feedbackSignalsConflict(const FeedbackRangeSignal &left, const FeedbackRangeSignal &right)
{
	const double leftDuration = feedbackRangeDuration(left.range);
	const double rightDuration = feedbackRangeDuration(right.range);
	if (leftDuration <= 0.0 || rightDuration <= 0.0)
		return false;
	const double overlap = feedbackRangeOverlap(left.range, right.range);
	if (overlap <= 0.0)
		return false;
	const double shorterCoverage = overlap / std::min(leftDuration, rightDuration);
	const double longerCoverage = overlap / std::max(leftDuration, rightDuration);
	const double centerDistance = std::fabs(feedbackRangeCenter(left.range) - feedbackRangeCenter(right.range));
	const bool closeBoundaries = std::fabs(left.range.startSec - right.range.startSec) <= 5.0 &&
				     std::fabs(left.range.endSec - right.range.endSec) <= 7.0;
	return closeBoundaries || (shorterCoverage >= 0.72 && centerDistance <= 12.0) ||
	       (shorterCoverage >= 0.88 && longerCoverage >= 0.42);
}

bool feedbackSignalReasonContains(const FeedbackRangeSignal &signal, const QString &needle)
{
	return signal.reason.contains(needle, Qt::CaseInsensitive);
}

bool adjustedGeneratedNegativeSignal(const FeedbackRangeSignal &signal)
{
	return signal.decision == QStringLiteral("adjusted") &&
	       (feedbackSignalReasonContains(signal, QStringLiteral("user_adjusted_generated_boundaries")) ||
		feedbackSignalReasonContains(signal, QStringLiteral("user_adjusted_recoverable_boundary_original")));
}

bool adjustedCorrectedPositiveSignal(const FeedbackRangeSignal &signal)
{
	return signal.decision == QStringLiteral("adjusted") &&
	       feedbackSignalReasonContains(signal, QStringLiteral("user_corrected_range"));
}

bool approvedAdjustedOriginalNegativeSignal(const FeedbackRangeSignal &signal)
{
	return signal.decision == QStringLiteral("approved_adjusted") &&
	       (feedbackSignalReasonContains(signal, QStringLiteral("user_approved_corrected_original_boundaries")) ||
		feedbackSignalReasonContains(signal, QStringLiteral("user_approved_recoverable_boundary_original")));
}

bool approvedAdjustedCorrectedPositiveSignal(const FeedbackRangeSignal &signal)
{
	return signal.decision == QStringLiteral("approved_adjusted") &&
	       feedbackSignalReasonContains(signal, QStringLiteral("user_approved_corrected_range_complete_clip"));
}

bool sameCorrectedFeedbackPair(const FeedbackRangeSignal &negative, const FeedbackRangeSignal &positive)
{
	if (negative.sequence != positive.sequence)
		return false;
	return (adjustedGeneratedNegativeSignal(negative) && adjustedCorrectedPositiveSignal(positive)) ||
	       (approvedAdjustedOriginalNegativeSignal(negative) && approvedAdjustedCorrectedPositiveSignal(positive));
}
} // namespace Curation::Feedback::Detail
