#include "ui/upload-review-dialog.hpp"

#include "ui/review/diagnostics-dialogs.hpp"
#include "ui/review/upload-review-dialog-utils.hpp"
#include "ui/video-marker-editor.hpp"
#include "curation/scoring/cheap-clip-scorer.hpp"
#include "curation/scoring/exchange-arc-boundary-refiner.hpp"
#include "curation/scoring/transcript-index.hpp"
#include "transcription/transcript-store.hpp"

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QJsonValue>

#include <algorithm>
#include <cmath>
#include <limits>

#include <utility>

using namespace ReviewDialogUtils;

void UploadReviewDialog::captureInitialSemanticSuggestion()
{
	if (advancedSettingsOnly || initialSettings.clipDurations.isEmpty())
		return;

	lastSemanticSuggestion.ranges = initialSettings.clipDurations;
	lastSemanticSuggestion.contentId = Curation::Feedback::CurationFeedbackStore::fileContentId(videoPath);
	lastSemanticSuggestion.contentIdAliases.clear();
	lastSemanticSuggestion.source = QStringLiteral("initial_review_suggestion");
	lastSemanticSuggestion.summary = QStringLiteral("initial semantic suggestion ranges");
	diagnosticReviewStates.clear();
	diagnosticEditedRanges.clear();
	diagnosticFeedbackSuggestedIndices.clear();
	diagnosticReviewMode = false;
	explicitReviewDecisions.clear();
	explicitReviewFeedbackDetails.clear();
	boundaryFeedbackSaved = false;
}

void UploadReviewDialog::ensureReviewFeedbackSnapshot(const QString &source)
{
	if (advancedSettingsOnly || !videoEditor)
		return;

	if (Curation::Feedback::CurationFeedbackStore::hasUsefulSuggestion(lastSemanticSuggestion))
		return;

	const QVector<ClipDuration> ranges = videoEditor->clipRanges();
	if (ranges.isEmpty())
		return;
	if (isDefaultNoMarkerPlaceholderRange(videoEditor && videoEditor->hasExplicitClipMarkers(),
								 videoEditor ? videoEditor->durationMilliseconds() : 0, ranges)) {
		blog(LOG_INFO,
		     "Skipping baseline review feedback snapshot without explicit markers. video=%s source=%s",
		     videoPath.toUtf8().constData(), source.toUtf8().constData());
		return;
	}

	const QJsonArray preservedDiagnostics = lastSemanticSuggestion.candidateDiagnostics;
	lastSemanticSuggestion.ranges = ranges;
	lastSemanticSuggestion.contentId = Curation::Feedback::CurationFeedbackStore::fileContentId(videoPath);
	lastSemanticSuggestion.contentIdAliases.clear();
	lastSemanticSuggestion.source = source.trimmed().isEmpty() ? QStringLiteral("existing_review_markers")
								   : source.trimmed();
	lastSemanticSuggestion.summary = QStringLiteral("baseline marker list captured for feedback");
	lastSemanticSuggestion.candidateDiagnostics = preservedDiagnostics;
	explicitReviewDecisions.clear();
	explicitReviewFeedbackDetails.clear();
	boundaryFeedbackSaved = false;
	blog(LOG_INFO, "Captured baseline review markers for feedback. video=%s ranges=%d source=%s",
	     videoPath.toUtf8().constData(), static_cast<int>(ranges.size()),
	     lastSemanticSuggestion.source.toUtf8().constData());
}

int UploadReviewDialog::suggestedIndexForRange(const ClipDuration &range) const
{
	int bestIndex = -1;
	double bestScore = 0.0;
	for (int i = 0; i < lastSemanticSuggestion.ranges.size(); ++i) {
		const ClipDuration &suggested = lastSemanticSuggestion.ranges.at(i);
		if (!std::isfinite(suggested.startSec) || !std::isfinite(suggested.endSec) ||
		    suggested.endSec <= suggested.startSec)
			continue;
		const double score = reviewRangeSimilarity(range, suggested);
		if (score > bestScore) {
			bestScore = score;
			bestIndex = i;
		}
	}
	return bestScore >= 0.18 ? bestIndex : -1;
}

int UploadReviewDialog::ensureFeedbackSuggestionForRange(const ClipDuration &range, const QString &source)
{
	if (!std::isfinite(range.startSec) || !std::isfinite(range.endSec) || range.endSec <= range.startSec)
		return -1;

	ensureReviewFeedbackSnapshot(source.trimmed().isEmpty() ? QStringLiteral("existing_review_markers")
								: source.trimmed());
	if (!Curation::Feedback::CurationFeedbackStore::hasUsefulSuggestion(lastSemanticSuggestion) && videoEditor) {
		const QVector<ClipDuration> currentRanges = videoEditor->clipRanges();
		if (isDefaultNoMarkerPlaceholderRange(videoEditor && videoEditor->hasExplicitClipMarkers(),
								 videoEditor ? videoEditor->durationMilliseconds() : 0, currentRanges) && currentRanges.size() == 1 &&
		    reviewRangeSimilarity(range, currentRanges.first()) >= 0.99) {
			blog(LOG_INFO,
			     "Skipping default no-marker placeholder as explicit feedback candidate. video=%s range=%.2f-%.2f source=%s",
			     videoPath.toUtf8().constData(), range.startSec, range.endSec, source.toUtf8().constData());
			return -1;
		}
	}
	const int existingIndex = suggestedIndexForRange(range);
	if (existingIndex >= 0)
		return existingIndex;

	if (lastSemanticSuggestion.contentId.trimmed().isEmpty())
		lastSemanticSuggestion.contentId = Curation::Feedback::CurationFeedbackStore::fileContentId(videoPath);
	if (lastSemanticSuggestion.source.trimmed().isEmpty())
		lastSemanticSuggestion.source = source.trimmed().isEmpty()
							? QStringLiteral("manual_or_edited_review_marker")
							: source.trimmed();
	if (lastSemanticSuggestion.summary.trimmed().isEmpty())
		lastSemanticSuggestion.summary = QStringLiteral("review markers captured for explicit feedback");

	lastSemanticSuggestion.ranges.append(range);
	const int newIndex = lastSemanticSuggestion.ranges.size() - 1;

	QJsonObject diagnostic;
	diagnostic.insert(QStringLiteral("index"), newIndex);
	diagnostic.insert(QStringLiteral("start_sec"), range.startSec);
	diagnostic.insert(QStringLiteral("end_sec"), range.endSec);
	diagnostic.insert(QStringLiteral("duration_sec"), range.endSec - range.startSec);
	diagnostic.insert(QStringLiteral("source"), source.trimmed().isEmpty()
							    ? QStringLiteral("manual_or_edited_review_marker")
							    : source.trimmed());
	diagnostic.insert(QStringLiteral("final_score"), 0.0);
	diagnostic.insert(QStringLiteral("review_status"), QStringLiteral("explicit_user_marker"));
	QJsonArray evidence;
	evidence.append(QStringLiteral("explicit_user_marker"));
	evidence.append(QStringLiteral("manual_or_heavily_edited_marker_feedback"));
	diagnostic.insert(QStringLiteral("evidence"), evidence);
	lastSemanticSuggestion.candidateDiagnostics.append(diagnostic);

	boundaryFeedbackSaved = false;
	blog(LOG_INFO,
	     "Registered review marker as explicit feedback candidate. video=%s index=%d range=%.2f-%.2f source=%s",
	     videoPath.toUtf8().constData(), newIndex, range.startSec, range.endSec,
	     diagnostic.value(QStringLiteral("source")).toString().toUtf8().constData());
	return newIndex;
}

bool UploadReviewDialog::loadFeedbackTranscriptIfAvailable()
{
	if (feedbackTranscriptLoaded)
		return !feedbackTranscript.isEmpty();

	feedbackTranscriptLoaded = true;
	const CurationSettings settings = curationSettings();
	QString language = settings.transcriptionLanguage.trimmed();
	if (language.isEmpty() || language == QStringLiteral("auto"))
		language = settings.sourceLanguage.trimmed();
	if (language.isEmpty() || language == QStringLiteral("auto"))
		language = QStringLiteral("pt");

	feedbackTranscript = TranscriptStore::loadAlignedForVideoPath(videoPath, language);
	if (feedbackTranscript.isEmpty())
		feedbackTranscript = TranscriptStore::loadForVideoPath(videoPath, language);

	if (!feedbackTranscript.isEmpty()) {
		const QString transcriptId =
			Curation::Feedback::CurationFeedbackStore::transcriptContentId(feedbackTranscript);
		const QString fileId = Curation::Feedback::CurationFeedbackStore::fileContentId(videoPath);
		if (!transcriptId.isEmpty()) {
			lastSemanticSuggestion.contentId = transcriptId;
			if (!fileId.isEmpty() && fileId != transcriptId &&
			    !lastSemanticSuggestion.contentIdAliases.contains(fileId))
				lastSemanticSuggestion.contentIdAliases.append(fileId);
		}
		blog(LOG_INFO,
		     "Loaded transcript cache for explicit marker feedback re-evaluation. video=%s segments=%d wordAligned=%s contentId=%s",
		     videoPath.toUtf8().constData(), static_cast<int>(feedbackTranscript.segments.size()),
		     feedbackTranscript.hasWordTimings() ? "true" : "false",
		     lastSemanticSuggestion.contentId.toUtf8().constData());
	}

	return !feedbackTranscript.isEmpty();
}

void UploadReviewDialog::upsertFeedbackDiagnostic(int suggestedIndex, const QJsonObject &diagnostic)
{
	if (suggestedIndex < 0 || diagnostic.isEmpty())
		return;

	QJsonObject object = diagnostic;
	object.insert(QStringLiteral("index"), suggestedIndex);

	for (int i = 0; i < lastSemanticSuggestion.candidateDiagnostics.size(); ++i) {
		const QJsonValue value = lastSemanticSuggestion.candidateDiagnostics.at(i);
		if (!value.isObject())
			continue;
		if (value.toObject().value(QStringLiteral("index")).toInt(-1) == suggestedIndex) {
			lastSemanticSuggestion.candidateDiagnostics.replace(i, object);
			return;
		}
	}

	lastSemanticSuggestion.candidateDiagnostics.append(object);
}

QJsonObject UploadReviewDialog::reevaluateFeedbackRange(const ClipDuration &range, int suggestedIndex,
							const QString &source)
{
	QJsonObject diagnostic;
	diagnostic.insert(QStringLiteral("index"), suggestedIndex);
	diagnostic.insert(QStringLiteral("start_sec"), range.startSec);
	diagnostic.insert(QStringLiteral("end_sec"), range.endSec);
	diagnostic.insert(QStringLiteral("duration_sec"), range.endSec - range.startSec);
	diagnostic.insert(QStringLiteral("source"), source.trimmed().isEmpty()
							    ? QStringLiteral("manual_or_edited_review_marker")
							    : source.trimmed());
	diagnostic.insert(QStringLiteral("review_status"), QStringLiteral("explicit_user_marker_re_evaluated"));

	QJsonArray evidence;
	evidence.append(QStringLiteral("explicit_user_marker"));
	evidence.append(QStringLiteral("feedback_range_re_evaluated_on_review_decision"));

	if (!loadFeedbackTranscriptIfAvailable()) {
		evidence.append(QStringLiteral("feedback_re_evaluation_transcript_unavailable"));
		diagnostic.insert(QStringLiteral("evidence"), evidence);
		diagnostic.insert(QStringLiteral("final_score"), 0.0);
		return diagnostic;
	}

	Curation::Scoring::TranscriptIndex index(feedbackTranscript);
	Curation::Scoring::ClipCandidate candidate;
	candidate.range = range;
	candidate.source = source.trimmed().isEmpty() ? QStringLiteral("manual_or_edited_review_marker")
						      : source.trimmed();
	candidate.evidence = QStringList{QStringLiteral("explicit_user_marker"),
					 QStringLiteral("feedback_range_re_evaluated_on_review_decision")};

	const CurationSettings settings = curationSettings();
	Curation::Scoring::CheapScoringContext context;
	context.presetId = settings.curationPreset.trimmed().isEmpty() ? QStringLiteral("viewer_message_response")
								       : settings.curationPreset.trimmed();
	context.mainTarget = settings.topicKeywords.join(QStringLiteral(", ")).trimmed();
	context.transcriptionLanguage = settings.transcriptionLanguage;
	context.sourceLanguage = settings.sourceLanguage;
	context.reliableMainTarget = !context.mainTarget.isEmpty();

	Curation::Scoring::CheapClipScorer cheapScorer;
	Curation::Scoring::ClipCandidate scored = cheapScorer.score(index, candidate, context);

	Curation::Scoring::ExchangeArcBoundaryRefinementOptions refineOptions;
	refineOptions.scoring = context;
	refineOptions.generation.minDurationSec = 8.0;
	refineOptions.generation.maxDurationSec = 180.0;
	refineOptions.generation.boundaryMinDurationSec = 8.0;
	Curation::Scoring::ExchangeArcBoundaryRefiner refiner;
	Curation::Scoring::ClipCandidate refined = refiner.refine(index, scored, refineOptions);
	refined.range = range;
	if (refined.text.trimmed().isEmpty())
		refined.text = scored.text;
	if (refined.timedText.trimmed().isEmpty())
		refined.timedText = scored.timedText;

	evidence = reviewStringListToJson(refined.evidence);
	diagnostic.insert(QStringLiteral("final_score"), refined.scores.final);
	diagnostic.insert(QStringLiteral("scores"), reviewScoresToJson(refined.scores));
	QJsonObject labels;
	labels.insert(QStringLiteral("boundary_state"),
		      reviewEvidenceValue(refined.evidence, QStringLiteral("boundary_state:")));
	labels.insert(QStringLiteral("boundary_reasons"),
		      reviewEvidenceValue(refined.evidence, QStringLiteral("boundary_reasons:")));
	labels.insert(QStringLiteral("arc_dp"),
		      reviewEvidenceValue(refined.evidence, QStringLiteral("arc_dp_boundary_refined")));
	diagnostic.insert(QStringLiteral("classifier_labels"), labels);
	diagnostic.insert(QStringLiteral("evidence"), evidence);
	diagnostic.insert(QStringLiteral("text_preview"), refined.text.left(1200));
	if (!refined.timedText.trimmed().isEmpty())
		diagnostic.insert(QStringLiteral("timed_text_preview"), refined.timedText.left(1600));
	diagnostic.insert(QStringLiteral("review_status"), QStringLiteral("explicit_user_marker_re_evaluated"));
	diagnostic.insert(QStringLiteral("re_evaluation_backend"), QStringLiteral("cheap_scorer_exchange_arc"));
	return diagnostic;
}

static double diagnosticRangeSimilarityForReview(const ClipDuration &a, const ClipDuration &b)
{
	const double overlap = std::max(0.0, std::min(a.endSec, b.endSec) - std::max(a.startSec, b.startSec));
	const double uni = std::max(a.endSec, b.endSec) - std::min(a.startSec, b.startSec);
	if (uni <= 0.0)
		return 0.0;
	const double iou = overlap / uni;
	const double boundaryDistance = std::fabs(a.startSec - b.startSec) + std::fabs(a.endSec - b.endSec);
	const double boundaryScore = std::max(0.0, 1.0 - (boundaryDistance / 30.0));
	return (iou * 0.78) + (boundaryScore * 0.22);
}

QJsonObject UploadReviewDialog::diagnosticForRange(const ClipDuration &range, int suggestedIndex) const
{
	if (suggestedIndex >= 0) {
		for (const QJsonValue &value : lastSemanticSuggestion.candidateDiagnostics) {
			if (!value.isObject())
				continue;
			const QJsonObject object = value.toObject();
			if (object.value(QStringLiteral("index")).toInt(-1) != suggestedIndex)
				continue;
			ClipDuration candidateRange;
			if (diagnosticRangeFromObject(object, &candidateRange) &&
			    diagnosticRangeSimilarityForReview(range, candidateRange) >= 0.50)
				return object;
		}
	}

	QJsonObject best;
	double bestScore = 0.0;
	for (const QJsonValue &value : lastSemanticSuggestion.candidateDiagnostics) {
		if (!value.isObject())
			continue;
		const QJsonObject object = value.toObject();
		const ClipDuration candidateRange{object.value(QStringLiteral("start_sec")).toDouble(),
						  object.value(QStringLiteral("end_sec")).toDouble()};
		if (!std::isfinite(candidateRange.startSec) || !std::isfinite(candidateRange.endSec) ||
		    candidateRange.endSec <= candidateRange.startSec)
			continue;
		const double score = diagnosticRangeSimilarityForReview(range, candidateRange);
		if (score > bestScore) {
			bestScore = score;
			best = object;
		}
	}
	return bestScore >= 0.50 ? best : QJsonObject{};
}

void UploadReviewDialog::showMarkerDiagnostics(int row)
{
	if (!videoEditor || row < 0)
		return;

	if (isDiagnosticTableRow(row)) {
		ClipDuration range;
		QJsonObject diagnostic;
		if (!diagnosticRangeForTableRow(row, &range, &diagnostic))
			return;
		ClipCropper::Review::showMarkerDiagnosticsDialog(range, diagnostic,
								 videoEditor->formatTimecode(range.startSec),
								 videoEditor->formatTimecode(range.endSec), this);
		return;
	}

	const QVector<ClipDuration> ranges = videoEditor->clipRanges();
	if (row >= ranges.size())
		return;

	const ClipDuration range = ranges.at(row);
	const int suggestedIndex = suggestedIndexForRange(range);
	const QJsonObject diagnostic = diagnosticForRange(range, suggestedIndex);
	ClipCropper::Review::showMarkerDiagnosticsDialog(range, diagnostic, videoEditor->formatTimecode(range.startSec),
							 videoEditor->formatTimecode(range.endSec), this);
}

int UploadReviewDialog::ensureDiagnosticFeedbackSuggestion(int diagnosticIndex, const ClipDuration &range,
							   const QJsonObject &diagnostic)
{
	if (diagnosticIndex < 0 || !std::isfinite(range.startSec) || !std::isfinite(range.endSec) ||
	    range.endSec <= range.startSec)
		return -1;

	ClipDuration originalRange;
	const bool hasOriginalRange = diagnosticRangeFromObject(diagnostic, &originalRange);
	const ClipDuration reviewedRange = normalizedDiagnosticRange(range);

	if (lastSemanticSuggestion.contentId.trimmed().isEmpty())
		lastSemanticSuggestion.contentId = Curation::Feedback::CurationFeedbackStore::fileContentId(videoPath);
	if (lastSemanticSuggestion.source.trimmed().isEmpty())
		lastSemanticSuggestion.source = QStringLiteral("semantic_review_diagnostics_only");
	if (lastSemanticSuggestion.summary.trimmed().isEmpty())
		lastSemanticSuggestion.summary = QStringLiteral("diagnostic candidates captured for explicit feedback");

	const auto updateTaggedDiagnostic = [&](int suggestedIndex) {
		if (suggestedIndex < 0 || suggestedIndex >= lastSemanticSuggestion.ranges.size())
			return;
		lastSemanticSuggestion.ranges[suggestedIndex] = reviewedRange;

		QJsonObject taggedDiagnostic = diagnostic;
		taggedDiagnostic.insert(QStringLiteral("index"), suggestedIndex);
		taggedDiagnostic.insert(QStringLiteral("diagnostic_feedback_source_index"), diagnosticIndex);
		taggedDiagnostic.insert(QStringLiteral("start_sec"), reviewedRange.startSec);
		taggedDiagnostic.insert(QStringLiteral("end_sec"), reviewedRange.endSec);
		taggedDiagnostic.insert(QStringLiteral("duration_sec"), reviewedRange.endSec - reviewedRange.startSec);
		if (hasOriginalRange) {
			taggedDiagnostic.insert(QStringLiteral("diagnostic_original_start_sec"),
						originalRange.startSec);
			taggedDiagnostic.insert(QStringLiteral("diagnostic_original_end_sec"), originalRange.endSec);
			const bool edited = diagnosticRangeMeaningfullyEdited(originalRange, reviewedRange);
			taggedDiagnostic.insert(QStringLiteral("diagnostic_range_edited"), edited);
		}

		for (int i = 0; i < lastSemanticSuggestion.candidateDiagnostics.size(); ++i) {
			const QJsonValue value = lastSemanticSuggestion.candidateDiagnostics.at(i);
			if (!value.isObject())
				continue;
			const QJsonObject object = value.toObject();
			if (object.value(QStringLiteral("diagnostic_feedback_source_index")).toInt(-1) ==
			    diagnosticIndex) {
				lastSemanticSuggestion.candidateDiagnostics.replace(i, taggedDiagnostic);
				return;
			}
		}
		lastSemanticSuggestion.candidateDiagnostics.append(taggedDiagnostic);
	};

	const int rememberedIndex = diagnosticFeedbackSuggestedIndices.value(diagnosticIndex, -1);
	if (rememberedIndex >= 0 && rememberedIndex < lastSemanticSuggestion.ranges.size()) {
		updateTaggedDiagnostic(rememberedIndex);
		return rememberedIndex;
	}

	for (int i = 0; i < lastSemanticSuggestion.candidateDiagnostics.size(); ++i) {
		const QJsonValue value = lastSemanticSuggestion.candidateDiagnostics.at(i);
		if (!value.isObject())
			continue;
		const QJsonObject object = value.toObject();
		if (object.value(QStringLiteral("diagnostic_feedback_source_index")).toInt(-1) != diagnosticIndex)
			continue;
		const int existingIndex = object.value(QStringLiteral("index")).toInt(-1);
		if (existingIndex >= 0 && existingIndex < lastSemanticSuggestion.ranges.size()) {
			diagnosticFeedbackSuggestedIndices.insert(diagnosticIndex, existingIndex);
			updateTaggedDiagnostic(existingIndex);
			return existingIndex;
		}
	}

	lastSemanticSuggestion.ranges.append(reviewedRange);
	const int suggestedIndex = lastSemanticSuggestion.ranges.size() - 1;
	diagnosticFeedbackSuggestedIndices.insert(diagnosticIndex, suggestedIndex);
	updateTaggedDiagnostic(suggestedIndex);
	return suggestedIndex;
}

bool UploadReviewDialog::collectStructuredFeedback(int row, const QString &decision, QJsonObject *outFeedback)
{
	if (!videoEditor || row < 0 || !outFeedback)
		return false;
	const QVector<ClipDuration> ranges = videoEditor->clipRanges();
	if (row >= ranges.size())
		return false;

	const ClipDuration range = ranges.at(row);
	const int suggestedIndex = suggestedIndexForRange(range);
	const QJsonObject diagnostic = diagnosticForRange(range, suggestedIndex);
	auto feedback = ClipCropper::Review::collectStructuredFeedbackDialog(
		decision, range, diagnostic, videoEditor->formatTimecode(range.startSec),
		videoEditor->formatTimecode(range.endSec), this);
	if (!feedback)
		return false;

	*outFeedback = std::move(*feedback);
	outFeedback->insert(QStringLiteral("range_start_sec"), range.startSec);
	outFeedback->insert(QStringLiteral("range_end_sec"), range.endSec);
	if (suggestedIndex >= 0)
		outFeedback->insert(QStringLiteral("suggested_index"), suggestedIndex);
	const QString diagnosticKind = diagnostic.value(QStringLiteral("diagnostic_kind")).toString().trimmed();
	if (!diagnosticKind.isEmpty())
		outFeedback->insert(QStringLiteral("diagnostic_kind"), diagnosticKind);
	const QString rejectionReason = diagnostic.value(QStringLiteral("rejection_reason")).toString().trimmed();
	if (!rejectionReason.isEmpty())
		outFeedback->insert(QStringLiteral("diagnostic_rejection_reason"), rejectionReason);
	return true;
}

void UploadReviewDialog::setDiagnosticReviewDecision(int row, const QString &decision)
{
	if (!videoEditor || row < 0 || !isDiagnosticTableRow(row))
		return;

	const int diagnosticIndex = diagnosticIndexForTableRow(row);
	ClipDuration range;
	QJsonObject diagnostic;
	if (diagnosticIndex < 0 || !diagnosticRangeForTableRow(row, &range, &diagnostic))
		return;

	const QString normalizedDecision = decision.trimmed().toLower();
	const QString existingState = diagnosticReviewState(diagnosticIndex).trimmed();
	if (!existingState.isEmpty()) {
		blog(LOG_INFO,
		     "Ignored duplicate diagnostic feedback decision in current review session. video=%s row=%d diagnosticIndex=%d existing=%s requested=%s range=%.2f-%.2f",
		     videoPath.toUtf8().constData(), row, diagnosticIndex, existingState.toUtf8().constData(),
		     normalizedDecision.toUtf8().constData(), range.startSec, range.endSec);
		refreshDiagnosticCandidateTable();
		return;
	}
	{
		const CurationSettings settings = curationSettings();
		QString presetId = settings.curationPreset.trimmed();
		if (presetId.isEmpty())
			presetId = QStringLiteral("viewer_message_response");
		QStringList contentIds;
		const QString fileContentId = Curation::Feedback::CurationFeedbackStore::fileContentId(videoPath);
		if (!fileContentId.isEmpty())
			contentIds.append(fileContentId);
		const Curation::Feedback::FeedbackRangeMemory memory =
			Curation::Feedback::CurationFeedbackStore::loadRangeMemoryForVideo(videoPath, presetId,
											   contentIds);
		const QString persistedDecision = persistedFeedbackDecisionForRange(range, memory);
		if (!persistedDecision.trimmed().isEmpty()) {
			blog(LOG_INFO,
			     "Ignored duplicate diagnostic feedback decision for already-reviewed range. video=%s row=%d diagnosticIndex=%d decision=%s previous=%s range=%.2f-%.2f",
			     videoPath.toUtf8().constData(), row, diagnosticIndex,
			     normalizedDecision.toUtf8().constData(), persistedDecision.toUtf8().constData(),
			     range.startSec, range.endSec);
			refreshDiagnosticCandidateTable();
			return;
		}
	}
	ClipDuration originalRange;
	const bool hasOriginalRange = diagnosticRangeFromObject(diagnostic, &originalRange);
	const bool rangeEdited = hasOriginalRange && diagnosticRangeMeaningfullyEdited(originalRange, range);
	if (normalizedDecision == QStringLiteral("ignored_diagnostic")) {
		const int suggestedIndex = ensureDiagnosticFeedbackSuggestion(diagnosticIndex, range, diagnostic);
		if (suggestedIndex < 0)
			return;

		QJsonObject structuredFeedback;
		structuredFeedback.insert(QStringLiteral("schema_version"), 1);
		structuredFeedback.insert(QStringLiteral("decision"), normalizedDecision);
		structuredFeedback.insert(QStringLiteral("ignore_for_training"), true);
		structuredFeedback.insert(QStringLiteral("weak_negative"), true);
		structuredFeedback.insert(QStringLiteral("bad_topic"), false);
		structuredFeedback.insert(QStringLiteral("boundary_recoverable"), false);
		structuredFeedback.insert(QStringLiteral("range_start_sec"), range.startSec);
		structuredFeedback.insert(QStringLiteral("range_end_sec"), range.endSec);
		structuredFeedback.insert(QStringLiteral("suggested_index"), suggestedIndex);
		structuredFeedback.insert(QStringLiteral("diagnostic_source_index"), diagnosticIndex);
		if (hasOriginalRange) {
			structuredFeedback.insert(QStringLiteral("diagnostic_original_start_sec"),
						  originalRange.startSec);
			structuredFeedback.insert(QStringLiteral("diagnostic_original_end_sec"), originalRange.endSec);
			structuredFeedback.insert(QStringLiteral("diagnostic_range_edited"), rangeEdited);
		}
		const QString diagnosticKind = diagnostic.value(QStringLiteral("diagnostic_kind")).toString().trimmed();
		if (!diagnosticKind.isEmpty())
			structuredFeedback.insert(QStringLiteral("diagnostic_kind"), diagnosticKind);
		const QString rejectionReason =
			diagnostic.value(QStringLiteral("rejection_reason")).toString().trimmed();
		if (!rejectionReason.isEmpty())
			structuredFeedback.insert(QStringLiteral("diagnostic_rejection_reason"), rejectionReason);

		explicitReviewDecisions.insert(suggestedIndex, normalizedDecision);
		explicitReviewFeedbackDetails.insert(suggestedIndex, structuredFeedback);
		diagnosticReviewStates.insert(diagnosticIndex, normalizedDecision);

		boundaryFeedbackSaved = false;
		if (finishReviewButton) {
			finishReviewButton->setEnabled(true);
			finishReviewButton->setText(QStringLiteral("Finish review (save feedback)"));
		}
		refreshDiagnosticCandidateTable();
		if (clipTable && row >= 0 && row < clipTable->rowCount())
			clipTable->selectRow(row);
		blog(LOG_INFO,
		     "Explicit diagnostic candidate ignored for training. video=%s row=%d diagnosticIndex=%d suggestionIndex=%d range=%.2f-%.2f",
		     videoPath.toUtf8().constData(), row, diagnosticIndex, suggestedIndex, range.startSec,
		     range.endSec);
		return;
	}
	if ((normalizedDecision == QStringLiteral("adjusted") ||
	     normalizedDecision == QStringLiteral("approved_adjusted")) &&
	    !rangeEdited) {
		QMessageBox::information(
			this, QStringLiteral("Adjust range first"),
			normalizedDecision == QStringLiteral("approved_adjusted")
				? QStringLiteral(
					  "Para salvar como bom clip corrigido, edite o início ou o fim do range antes. Se o range original já está bom, use aprovar.")
				: QStringLiteral(
					  "Para marcar um diagnóstico como ajustado, edite o início ou o fim do range antes. Se o range já está bom, use aprovar; se continua ruim, use rejeitar."));
		refreshDiagnosticCandidateTable();
		return;
	}

	auto feedback = ClipCropper::Review::collectStructuredFeedbackDialog(
		normalizedDecision, range, diagnostic, videoEditor->formatTimecode(range.startSec),
		videoEditor->formatTimecode(range.endSec), this);
	if (!feedback) {
		refreshDiagnosticCandidateTable();
		return;
	}

	const int suggestedIndex = ensureDiagnosticFeedbackSuggestion(diagnosticIndex, range, diagnostic);
	if (suggestedIndex < 0)
		return;

	QJsonObject structuredFeedback = std::move(*feedback);
	structuredFeedback.insert(QStringLiteral("range_start_sec"), range.startSec);
	structuredFeedback.insert(QStringLiteral("range_end_sec"), range.endSec);
	structuredFeedback.insert(QStringLiteral("suggested_index"), suggestedIndex);
	structuredFeedback.insert(QStringLiteral("diagnostic_source_index"), diagnosticIndex);
	if (hasOriginalRange) {
		structuredFeedback.insert(QStringLiteral("diagnostic_original_start_sec"), originalRange.startSec);
		structuredFeedback.insert(QStringLiteral("diagnostic_original_end_sec"), originalRange.endSec);
		structuredFeedback.insert(QStringLiteral("diagnostic_range_edited"), rangeEdited);
	}
	if (normalizedDecision == QStringLiteral("approved_adjusted")) {
		structuredFeedback.insert(QStringLiteral("approved_corrected_range"), true);
		structuredFeedback.insert(QStringLiteral("semantic_positive_example"), true);
	}
	const QString diagnosticKind = diagnostic.value(QStringLiteral("diagnostic_kind")).toString().trimmed();
	if (!diagnosticKind.isEmpty())
		structuredFeedback.insert(QStringLiteral("diagnostic_kind"), diagnosticKind);
	const QString rejectionReason = diagnostic.value(QStringLiteral("rejection_reason")).toString().trimmed();
	if (!rejectionReason.isEmpty())
		structuredFeedback.insert(QStringLiteral("diagnostic_rejection_reason"), rejectionReason);

	explicitReviewDecisions.insert(suggestedIndex, normalizedDecision);
	explicitReviewFeedbackDetails.insert(suggestedIndex, structuredFeedback);
	diagnosticReviewStates.insert(diagnosticIndex, normalizedDecision);

	boundaryFeedbackSaved = false;
	if (finishReviewButton) {
		finishReviewButton->setEnabled(true);
		finishReviewButton->setText(QStringLiteral("Finish review (save feedback)"));
	}
	refreshDiagnosticCandidateTable();
	if (clipTable && row >= 0 && row < clipTable->rowCount())
		clipTable->selectRow(row);

	blog(LOG_INFO,
	     "Explicit diagnostic candidate decision captured. video=%s row=%d diagnosticIndex=%d suggestionIndex=%d decision=%s range=%.2f-%.2f",
	     videoPath.toUtf8().constData(), row, diagnosticIndex, suggestedIndex,
	     normalizedDecision.toUtf8().constData(), range.startSec, range.endSec);
}

void UploadReviewDialog::setClipReviewDecision(int row, const QString &decision)
{
	if (!videoEditor || row < 0)
		return;
	if (isDiagnosticTableRow(row)) {
		setDiagnosticReviewDecision(row, decision);
		return;
	}
	ensureReviewFeedbackSnapshot(QStringLiteral("existing_review_markers"));
	const QVector<ClipDuration> ranges = videoEditor->clipRanges();
	if (row >= ranges.size())
		return;

	const ClipDuration reviewedRange = ranges.at(row);
	const QString normalizedDecision = decision.trimmed().toLower();
	{
		const CurationSettings settings = curationSettings();
		QString presetId = settings.curationPreset.trimmed();
		if (presetId.isEmpty())
			presetId = QStringLiteral("viewer_message_response");
		QStringList contentIds;
		const QString fileContentId = Curation::Feedback::CurationFeedbackStore::fileContentId(videoPath);
		if (!fileContentId.isEmpty())
			contentIds.append(fileContentId);
		const Curation::Feedback::FeedbackRangeMemory memory =
			Curation::Feedback::CurationFeedbackStore::loadRangeMemoryForVideo(videoPath, presetId,
											   contentIds);
		const QString persistedDecision = persistedFeedbackDecisionForRange(reviewedRange, memory);
		if (!persistedDecision.trimmed().isEmpty()) {
			blog(LOG_INFO,
			     "Ignored duplicate marker feedback decision for already-reviewed range. video=%s row=%d decision=%s previous=%s range=%.2f-%.2f",
			     videoPath.toUtf8().constData(), row, normalizedDecision.toUtf8().constData(),
			     persistedDecision.toUtf8().constData(), reviewedRange.startSec, reviewedRange.endSec);
			refreshClipTable(ranges);
			return;
		}
	}
	const int suggestedIndex =
		ensureFeedbackSuggestionForRange(reviewedRange, QStringLiteral("manual_or_edited_review_marker"));
	if (suggestedIndex < 0) {
		blog(LOG_WARNING, "Could not register marker row for feedback. video=%s row=%d decision=%s",
		     videoPath.toUtf8().constData(), row, normalizedDecision.toUtf8().constData());
		refreshClipTable(ranges);
		return;
	}

	if (explicitReviewDecisions.contains(suggestedIndex)) {
		blog(LOG_INFO,
		     "Ignored duplicate marker feedback decision in current review session. video=%s row=%d suggestionIndex=%d existing=%s requested=%s",
		     videoPath.toUtf8().constData(), row, suggestedIndex,
		     explicitReviewDecisions.value(suggestedIndex).toUtf8().constData(),
		     normalizedDecision.toUtf8().constData());
		refreshClipTable(ranges);
		return;
	}

	QJsonObject structuredFeedback;
	if (normalizedDecision == QStringLiteral("ignored_diagnostic")) {
		structuredFeedback.insert(QStringLiteral("schema_version"), 1);
		structuredFeedback.insert(QStringLiteral("decision"), normalizedDecision);
		structuredFeedback.insert(QStringLiteral("ignore_for_training"), true);
		structuredFeedback.insert(QStringLiteral("weak_negative"), true);
		structuredFeedback.insert(QStringLiteral("bad_topic"), false);
		structuredFeedback.insert(QStringLiteral("boundary_recoverable"), false);
		structuredFeedback.insert(QStringLiteral("range_start_sec"), reviewedRange.startSec);
		structuredFeedback.insert(QStringLiteral("range_end_sec"), reviewedRange.endSec);
		structuredFeedback.insert(QStringLiteral("suggested_index"), suggestedIndex);
		const QJsonObject diagnostic = diagnosticForRange(reviewedRange, suggestedIndex);
		const QString diagnosticKind = diagnostic.value(QStringLiteral("diagnostic_kind")).toString().trimmed();
		if (!diagnosticKind.isEmpty())
			structuredFeedback.insert(QStringLiteral("diagnostic_kind"), diagnosticKind);
		const QString rejectionReason =
			diagnostic.value(QStringLiteral("rejection_reason")).toString().trimmed();
		if (!rejectionReason.isEmpty())
			structuredFeedback.insert(QStringLiteral("diagnostic_rejection_reason"), rejectionReason);
	} else {
		if (!collectStructuredFeedback(row, normalizedDecision, &structuredFeedback)) {
			refreshClipTable(ranges);
			return;
		}
		structuredFeedback.insert(QStringLiteral("suggested_index"), suggestedIndex);
		if (normalizedDecision == QStringLiteral("approved_adjusted")) {
			structuredFeedback.insert(QStringLiteral("approved_corrected_range"), true);
			structuredFeedback.insert(QStringLiteral("semantic_positive_example"), true);
		}
	}

	explicitReviewDecisions.insert(suggestedIndex, normalizedDecision);
	explicitReviewFeedbackDetails.insert(suggestedIndex, structuredFeedback);
	blog(LOG_INFO,
	     "Explicit review marker decision captured. video=%s row=%d suggestionIndex=%d decision=%s range=%.2f-%.2f",
	     videoPath.toUtf8().constData(), row, suggestedIndex, normalizedDecision.toUtf8().constData(),
	     reviewedRange.startSec, reviewedRange.endSec);

	boundaryFeedbackSaved = false;
	if (finishReviewButton) {
		finishReviewButton->setEnabled(true);
		finishReviewButton->setText(QStringLiteral("Finish review (save feedback)"));
	}
	if (normalizedDecision == QStringLiteral("disliked") ||
	    normalizedDecision == QStringLiteral("ignored_diagnostic")) {
		videoEditor->selectRange(row);
		videoEditor->removeSelectedRange();
		return;
	}
	refreshClipTable(videoEditor->clipRanges());
}

void UploadReviewDialog::finishReviewFeedback()
{
	saveCurationOptions();
	saveBoundaryFeedback(QStringLiteral("review_finished"));
	if (finishReviewButton) {
		finishReviewButton->setEnabled(false);
		finishReviewButton->setText(boundaryFeedbackSaved ? QStringLiteral("Feedback saved")
								  : QStringLiteral("No feedback to save"));
	}
}

void UploadReviewDialog::saveBoundaryFeedback(const QString &eventName)
{
	if (advancedSettingsOnly || boundaryFeedbackSaved || !videoEditor)
		return;
	if (explicitReviewDecisions.isEmpty()) {
		blog(LOG_INFO, "Skipping boundary feedback save without explicit review decisions. video=%s event=%s",
		     videoPath.toUtf8().constData(), eventName.toUtf8().constData());
		return;
	}
	ensureReviewFeedbackSnapshot(QStringLiteral("existing_review_markers"));
	if (!Curation::Feedback::CurationFeedbackStore::hasUsefulSuggestion(lastSemanticSuggestion))
		return;

	if (lastSemanticSuggestion.contentId.trimmed().isEmpty())
		lastSemanticSuggestion.contentId = Curation::Feedback::CurationFeedbackStore::fileContentId(videoPath);
	CurationSettings settings = curationSettings();
	settings.reviewSettingsKey = currentReviewSettingsKey();
	QVector<ClipDuration> userRanges = videoEditor->clipRanges();
	if (isDefaultNoMarkerPlaceholderRange(videoEditor && videoEditor->hasExplicitClipMarkers(),
								 videoEditor ? videoEditor->durationMilliseconds() : 0, userRanges))
		userRanges.clear();
	for (auto it = explicitReviewDecisions.constBegin(); it != explicitReviewDecisions.constEnd(); ++it) {
		const int suggestedIndex = it.key();
		const QString decision = it.value().trimmed().toLower();
		if (decision != QStringLiteral("liked"))
			continue;
		if (suggestedIndex < 0 || suggestedIndex >= lastSemanticSuggestion.ranges.size())
			continue;
		const ClipDuration acceptedRange = lastSemanticSuggestion.ranges.at(suggestedIndex);
		bool alreadyPresent = false;
		for (const ClipDuration &range : std::as_const(userRanges)) {
			if (reviewRangeSimilarity(range, acceptedRange) >= 0.99) {
				alreadyPresent = true;
				break;
			}
		}
		if (!alreadyPresent)
			userRanges.append(acceptedRange);
	}
	if (Curation::Feedback::CurationFeedbackStore::appendReviewFeedback(
		    videoPath, settings, lastSemanticSuggestion, userRanges, eventName, {}, explicitReviewDecisions,
		    explicitReviewFeedbackDetails)) {
		boundaryFeedbackSaved = true;
	}
}

