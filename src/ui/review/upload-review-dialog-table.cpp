#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include "ui/upload-review-dialog.hpp"

#include "ui/review/upload-review-dialog-utils.hpp"
#include "ui/ui-common.hpp"
#include "ui/video-marker-editor.hpp"

#include <QEvent>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QMessageBox>
#include <QModelIndex>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QWidget>

#include <algorithm>
#include <cmath>

using namespace ReviewDialogUtils;

void UploadReviewDialog::refreshClipTable(const QVector<ClipDuration> &ranges)
{
	if (!clipTable || !videoEditor)
		return;

	const int previousRow = clipTable->currentRow();
	updatingClipTable = true;
	clipTable->setRowCount(0);

	Curation::Feedback::FeedbackRangeMemory persistedReviewMemory;
	{
		const CurationSettings settings = curationSettings();
		QString presetId = settings.curationPreset.trimmed();
		if (presetId.isEmpty())
			presetId = QStringLiteral("viewer_message_response");
		QStringList contentIds;
		const QString fileContentId = Curation::Feedback::CurationFeedbackStore::fileContentId(videoPath);
		if (!fileContentId.isEmpty())
			contentIds.append(fileContentId);
		persistedReviewMemory = Curation::Feedback::CurationFeedbackStore::loadRangeMemoryForVideo(
			videoPath, presetId, contentIds);
	}

	for (int i = 0; i < ranges.size(); ++i) {
		const auto &range = ranges[i];
		const double selectedSec = std::max(0.0, range.endSec - range.startSec);
		const int row = clipTable->rowCount();
		clipTable->insertRow(row);
		auto *markerItem = new QTableWidgetItem(QString("#%1  %2 → %3  (%4s)")
								.arg(i + 1)
								.arg(videoEditor->formatTimecode(range.startSec),
								     videoEditor->formatTimecode(range.endSec))
								.arg(selectedSec, 0, 'f', 0));
		markerItem->setFlags(markerItem->flags() & ~Qt::ItemIsEditable);
		markerItem->setData(REVIEW_ROW_KIND_ROLE, REVIEW_ROW_KIND_MARKER);
		clipTable->setItem(row, 0, markerItem);
		auto *startItem = new QTableWidgetItem(formatEditableReviewTime(range.startSec));
		startItem->setToolTip(QStringLiteral(
			"Double-click to type time as seconds, mm:ss, hh:mm:ss, or 1m30. Ctrl+Alt+, / Ctrl+Alt+. nudges the selected cell by 1s."));
		startItem->setData(Qt::UserRole, range.startSec);
		startItem->setFlags(startItem->flags() | Qt::ItemIsEditable);
		auto *endItem = new QTableWidgetItem(formatEditableReviewTime(range.endSec));
		endItem->setToolTip(QStringLiteral(
			"Double-click to type time as seconds, mm:ss, hh:mm:ss, or 1m30. Ctrl+Alt+, / Ctrl+Alt+. nudges the selected cell by 1s."));
		endItem->setData(Qt::UserRole, range.endSec);
		endItem->setFlags(endItem->flags() | Qt::ItemIsEditable);
		clipTable->setItem(row, 1, startItem);
		clipTable->setItem(row, 2, endItem);

		const int suggestedIndex = suggestedIndexForRange(range);
		const QString decision = suggestedIndex >= 0 ? explicitReviewDecisions.value(suggestedIndex)
							     : QString{};
		auto *feedbackWidget = new QWidget(clipTable);
		auto *feedbackLayout = new QHBoxLayout(feedbackWidget);
		feedbackLayout->setContentsMargins(2, 0, 2, 0);
		feedbackLayout->setSpacing(4);
		const QString persistedDecision =
			decision.trimmed().isEmpty() ? persistedFeedbackDecisionForRange(range, persistedReviewMemory)
						     : QString{};
		const bool alreadyReviewed = !decision.trimmed().isEmpty() || !persistedDecision.trimmed().isEmpty();
		if (alreadyReviewed) {
			startItem->setFlags(startItem->flags() & ~Qt::ItemIsEditable);
			endItem->setFlags(endItem->flags() & ~Qt::ItemIsEditable);
			startItem->setToolTip(QStringLiteral(
				"This reviewed marker is locked. Remove it from the list or create a new marker instead of re-editing/re-rating it."));
			endItem->setToolTip(startItem->toolTip());
			const QString displayDecision = !decision.trimmed().isEmpty() ? decision : persistedDecision;
			auto *reviewedLabel = new QLabel(reviewDecisionDisplayLabel(displayDecision), feedbackWidget);
			reviewedLabel->setToolTip(QStringLiteral(
				"This range already has feedback saved or pending. It is locked to avoid re-evaluating and reinforcing the same clip twice."));
			feedbackLayout->addWidget(reviewedLabel);
		} else {
			auto *likeButton = new QPushButton(QStringLiteral("👍"), feedbackWidget);
			auto *dislikeButton = new QPushButton(QStringLiteral("👎"), feedbackWidget);
			auto *adjustedButton = new QPushButton(QStringLiteral("✎"), feedbackWidget);
			auto *approvedAdjustedButton = new QPushButton(QStringLiteral("⭐"), feedbackWidget);
			auto *ignoreButton = new QPushButton(QStringLiteral("∅"), feedbackWidget);
			likeButton->setFocusPolicy(Qt::NoFocus);
			dislikeButton->setFocusPolicy(Qt::NoFocus);
			adjustedButton->setFocusPolicy(Qt::NoFocus);
			approvedAdjustedButton->setFocusPolicy(Qt::NoFocus);
			ignoreButton->setFocusPolicy(Qt::NoFocus);
			likeButton->setToolTip(QStringLiteral(
				"Approve this candidate as-is and describe its structure. This becomes a positive training signal."));
			dislikeButton->setToolTip(QStringLiteral(
				"Reject this candidate, describe why, and remove it from upload ranges. Use only for truly bad patterns."));
			adjustedButton->setToolTip(
				QStringLiteral("Mark this edited candidate as recoverable boundary feedback."));
			approvedAdjustedButton->setToolTip(QStringLiteral(
				"Edit start/end first, then save the corrected marker as a good semantic clip example."));
			ignoreButton->setToolTip(QStringLiteral(
				"Ignore this candidate for dataset/training. It suppresses near-duplicates without becoming a strong negative."));
			connect(likeButton, &QPushButton::clicked, this,
				[this, row]() { setClipReviewDecision(row, QStringLiteral("liked")); });
			connect(dislikeButton, &QPushButton::clicked, this,
				[this, row]() { setClipReviewDecision(row, QStringLiteral("disliked")); });
			connect(adjustedButton, &QPushButton::clicked, this,
				[this, row]() { setClipReviewDecision(row, QStringLiteral("adjusted")); });
			connect(approvedAdjustedButton, &QPushButton::clicked, this,
				[this, row]() { setClipReviewDecision(row, QStringLiteral("approved_adjusted")); });
			connect(ignoreButton, &QPushButton::clicked, this,
				[this, row]() { setClipReviewDecision(row, QStringLiteral("ignored_diagnostic")); });
			feedbackLayout->addWidget(likeButton);
			feedbackLayout->addWidget(dislikeButton);
			feedbackLayout->addWidget(adjustedButton);
			feedbackLayout->addWidget(approvedAdjustedButton);
			feedbackLayout->addWidget(ignoreButton);
		}
		clipTable->setCellWidget(row, 3, feedbackWidget);
	}

	int visibleSupplementalDiagnostics = 0;
	for (int i = 0; i < lastSemanticSuggestion.candidateDiagnostics.size(); ++i) {
		const QJsonValue value = lastSemanticSuggestion.candidateDiagnostics.at(i);
		if (!value.isObject())
			continue;
		const QJsonObject diagnostic = value.toObject();
		if (!isReviewDiagnosticCandidateObject(diagnostic))
			continue;

		ClipDuration originalRange;
		if (!diagnosticRangeFromObject(diagnostic, &originalRange))
			continue;

		const ClipDuration range = diagnosticEditedRanges.contains(i)
						   ? normalizedDiagnosticRange(diagnosticEditedRanges.value(i))
						   : normalizedDiagnosticRange(originalRange);
		const bool rangeEdited = diagnosticRangeMeaningfullyEdited(originalRange, range);

		const int row = clipTable->rowCount();
		clipTable->insertRow(row);
		const double selectedSec = std::max(0.0, range.endSec - range.startSec);
		const QString reason = diagnosticReasonLabel(diagnostic);
		const double finalScore = diagnostic.value(QStringLiteral("final_score")).toDouble(0.0);
		const QString state = diagnosticReviewState(i);
		const QString stateLabel = diagnosticStateLabel(state);

		auto *markerItem =
			new QTableWidgetItem(QStringLiteral("Diagnostic #%1  [%2%3]  %4 → %5  (%6s)  %7  final=%8")
						     .arg(visibleSupplementalDiagnostics + 1)
						     .arg(stateLabel)
						     .arg(rangeEdited ? QStringLiteral(" / range edited") : QString{})
						     .arg(videoEditor->formatTimecode(range.startSec),
							  videoEditor->formatTimecode(range.endSec))
						     .arg(selectedSec, 0, 'f', 0)
						     .arg(reason.left(42))
						     .arg(finalScore, 0, 'f', 2));
		markerItem->setToolTip(QStringLiteral(
			"Supplemental rejected/diagnostic candidate kept for feedback. Double-click this description to open the full diagnostic."));
		markerItem->setFlags(markerItem->flags() & ~Qt::ItemIsEditable);
		markerItem->setData(REVIEW_ROW_KIND_ROLE, REVIEW_ROW_KIND_DIAGNOSTIC);
		markerItem->setData(REVIEW_DIAGNOSTIC_INDEX_ROLE, i);
		clipTable->setItem(row, 0, markerItem);

		auto *startItem = new QTableWidgetItem(formatEditableReviewTime(range.startSec));
		startItem->setToolTip(QStringLiteral(
			"Double-click to edit this diagnostic candidate start time. This does not change the preserved scores/evidence."));
		startItem->setFlags(startItem->flags() | Qt::ItemIsEditable);
		startItem->setData(REVIEW_ROW_KIND_ROLE, REVIEW_ROW_KIND_DIAGNOSTIC);
		startItem->setData(REVIEW_DIAGNOSTIC_INDEX_ROLE, i);
		clipTable->setItem(row, 1, startItem);

		auto *endItem = new QTableWidgetItem(formatEditableReviewTime(range.endSec));
		endItem->setToolTip(QStringLiteral(
			"Double-click to edit this diagnostic candidate end time. This does not change the preserved scores/evidence."));
		endItem->setFlags(endItem->flags() | Qt::ItemIsEditable);
		endItem->setData(REVIEW_ROW_KIND_ROLE, REVIEW_ROW_KIND_DIAGNOSTIC);
		endItem->setData(REVIEW_DIAGNOSTIC_INDEX_ROLE, i);
		clipTable->setItem(row, 2, endItem);

		auto *feedbackWidget = new QWidget(clipTable);
		auto *feedbackLayout = new QHBoxLayout(feedbackWidget);
		feedbackLayout->setContentsMargins(2, 0, 2, 0);
		feedbackLayout->setSpacing(4);
		const QString persistedDecision =
			state.trimmed().isEmpty() ? persistedFeedbackDecisionForRange(range, persistedReviewMemory)
						  : QString{};
		const bool alreadyReviewed = !state.trimmed().isEmpty() || !persistedDecision.trimmed().isEmpty();
		if (alreadyReviewed) {
			startItem->setFlags(startItem->flags() & ~Qt::ItemIsEditable);
			endItem->setFlags(endItem->flags() & ~Qt::ItemIsEditable);
			startItem->setToolTip(QStringLiteral(
				"This reviewed diagnostic/probe is locked. It cannot be edited or rated again in this session."));
			endItem->setToolTip(startItem->toolTip());
			const QString displayDecision = !state.trimmed().isEmpty() ? state : persistedDecision;
			auto *reviewedLabel = new QLabel(reviewDecisionDisplayLabel(displayDecision), feedbackWidget);
			reviewedLabel->setToolTip(QStringLiteral(
				"This diagnostic/probe already has feedback saved or pending. It is locked to avoid asking for feedback twice."));
			feedbackLayout->addWidget(reviewedLabel);
		} else {
			auto *likeButton = new QPushButton(QStringLiteral("👍"), feedbackWidget);
			auto *dislikeButton = new QPushButton(QStringLiteral("👎"), feedbackWidget);
			auto *adjustedButton = new QPushButton(QStringLiteral("✎"), feedbackWidget);
			auto *approvedAdjustedButton = new QPushButton(QStringLiteral("⭐"), feedbackWidget);
			auto *ignoreButton = new QPushButton(QStringLiteral("∅"), feedbackWidget);
			likeButton->setFocusPolicy(Qt::NoFocus);
			dislikeButton->setFocusPolicy(Qt::NoFocus);
			adjustedButton->setFocusPolicy(Qt::NoFocus);
			approvedAdjustedButton->setFocusPolicy(Qt::NoFocus);
			ignoreButton->setFocusPolicy(Qt::NoFocus);
			likeButton->setToolTip(
				QStringLiteral("Approve this diagnostic candidate as-is and describe its structure."));
			dislikeButton->setToolTip(QStringLiteral("Reject this diagnostic candidate and describe why."));
			adjustedButton->setToolTip(QStringLiteral(
				"Edit start/end first, then mark this diagnostic as adjusted boundary feedback."));
			approvedAdjustedButton->setToolTip(QStringLiteral(
				"Edit start/end first, then save the corrected range as a good semantic clip example."));
			ignoreButton->setToolTip(QStringLiteral(
				"Ignore this exploration diagnostic for training. It only suppresses near-duplicates lightly and does not become a strong negative."));
			connect(likeButton, &QPushButton::clicked, this,
				[this, row]() { setDiagnosticReviewDecision(row, QStringLiteral("liked")); });
			connect(dislikeButton, &QPushButton::clicked, this,
				[this, row]() { setDiagnosticReviewDecision(row, QStringLiteral("disliked")); });
			connect(adjustedButton, &QPushButton::clicked, this,
				[this, row]() { setDiagnosticReviewDecision(row, QStringLiteral("adjusted")); });
			connect(approvedAdjustedButton, &QPushButton::clicked, this, [this, row]() {
				setDiagnosticReviewDecision(row, QStringLiteral("approved_adjusted"));
			});
			connect(ignoreButton, &QPushButton::clicked, this, [this, row]() {
				setDiagnosticReviewDecision(row, QStringLiteral("ignored_diagnostic"));
			});
			feedbackLayout->addWidget(likeButton);
			feedbackLayout->addWidget(dislikeButton);
			feedbackLayout->addWidget(adjustedButton);
			feedbackLayout->addWidget(approvedAdjustedButton);
			feedbackLayout->addWidget(ignoreButton);
		}
		clipTable->setCellWidget(row, 3, feedbackWidget);
		++visibleSupplementalDiagnostics;
	}
	if (visibleSupplementalDiagnostics > 0) {
		blog(LOG_INFO,
		     "Displayed supplemental semantic diagnostic candidate rows alongside applied markers. video=%s diagnostics=%d",
		     videoPath.toUtf8().constData(), visibleSupplementalDiagnostics);
	}

	updatingClipTable = false;
	updateCreditEstimate(ranges);

	if (clipTable->rowCount() <= 0)
		return;

	const int rowToSelect = std::clamp(previousRow, 0, clipTable->rowCount() - 1);
	clipTable->selectRow(rowToSelect);
	if (isDiagnosticTableRow(rowToSelect)) {
		ClipDuration diagnosticRange;
		if (diagnosticRangeForTableRow(rowToSelect, &diagnosticRange))
			videoEditor->seekToSeconds(diagnosticRange.startSec);
	} else {
		videoEditor->selectRange(rowToSelect);
	}
	updateSuggestClipRangesButtonState();
	updateDiagnosticModeControls();
}

int UploadReviewDialog::diagnosticIndexForTableRow(int row) const
{
	if (!clipTable || row < 0 || row >= clipTable->rowCount())
		return -1;
	const QTableWidgetItem *item = clipTable->item(row, 0);
	if (!item ||
	    tableItemIntDataOr(item, REVIEW_ROW_KIND_ROLE, REVIEW_ROW_KIND_MARKER) != REVIEW_ROW_KIND_DIAGNOSTIC)
		return -1;
	return tableItemIntDataOr(item, REVIEW_DIAGNOSTIC_INDEX_ROLE, -1);
}

QString UploadReviewDialog::diagnosticReviewState(int diagnosticIndex) const
{
	return diagnosticReviewStates.value(diagnosticIndex).trimmed().toLower();
}

bool UploadReviewDialog::hasReviewDiagnosticCandidates() const
{
	for (const QJsonValue &value : lastSemanticSuggestion.candidateDiagnostics) {
		if (value.isObject() && isReviewDiagnosticCandidateObject(value.toObject()))
			return true;
	}
	return false;
}

QVector<int> UploadReviewDialog::selectedDiagnosticTableRows() const
{
	QVector<int> rows;
	if (!clipTable || !clipTable->selectionModel())
		return rows;
	const QModelIndexList selectedRows = clipTable->selectionModel()->selectedRows();
	rows.reserve(selectedRows.size());
	for (const QModelIndex &index : selectedRows) {
		if (!index.isValid())
			continue;
		const int row = index.row();
		if (isDiagnosticTableRow(row))
			rows.append(row);
	}
	std::sort(rows.begin(), rows.end());
	rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
	return rows;
}

void UploadReviewDialog::updateDiagnosticModeControls() {}

void UploadReviewDialog::updateSuggestClipRangesButtonState()
{
	if (!suggestClipRangesButton)
		return;
	if (semanticSuggestionInProgress) {
		suggestClipRangesButton->setEnabled(false);
		suggestClipRangesButton->setText(obsText("Status.SuggestingBestClips"));
		suggestClipRangesButton->setToolTip(QString{});
		updateDiagnosticModeControls();
		return;
	}
	suggestClipRangesButton->setEnabled(true);
	suggestClipRangesButton->setText(obsText("Button.SuggestBestClips"));
	suggestClipRangesButton->setToolTip(QString{});
	updateDiagnosticModeControls();
}

void UploadReviewDialog::showDiagnosticCandidateTable()
{
	if (!hasReviewDiagnosticCandidates())
		return;
	diagnosticReviewMode = false;
	if (videoEditor)
		refreshClipTable(videoEditor->clipRanges());
}

void UploadReviewDialog::showReviewMarkerTable()
{
	diagnosticReviewMode = false;
	if (videoEditor)
		refreshClipTable(videoEditor->clipRanges());
	updateSuggestClipRangesButtonState();
}

void UploadReviewDialog::refreshDiagnosticCandidateTable()
{
	if (videoEditor) {
		diagnosticReviewMode = false;
		refreshClipTable(videoEditor->clipRanges());
	}
	return;
}

bool UploadReviewDialog::isDiagnosticTableRow(int row) const
{
	if (!clipTable || row < 0 || row >= clipTable->rowCount())
		return false;
	const QTableWidgetItem *item = clipTable->item(row, 0);
	return item &&
	       tableItemIntDataOr(item, REVIEW_ROW_KIND_ROLE, REVIEW_ROW_KIND_MARKER) == REVIEW_ROW_KIND_DIAGNOSTIC;
}

bool UploadReviewDialog::diagnosticRangeForTableRow(int row, ClipDuration *outRange, QJsonObject *outDiagnostic) const
{
	if (!outRange || !clipTable || row < 0 || row >= clipTable->rowCount())
		return false;
	const QTableWidgetItem *item = clipTable->item(row, 0);
	if (!item ||
	    tableItemIntDataOr(item, REVIEW_ROW_KIND_ROLE, REVIEW_ROW_KIND_MARKER) != REVIEW_ROW_KIND_DIAGNOSTIC)
		return false;
	const int diagnosticIndex = tableItemIntDataOr(item, REVIEW_DIAGNOSTIC_INDEX_ROLE, -1);
	if (diagnosticIndex < 0 || diagnosticIndex >= lastSemanticSuggestion.candidateDiagnostics.size())
		return false;
	const QJsonValue value = lastSemanticSuggestion.candidateDiagnostics.at(diagnosticIndex);
	if (!value.isObject())
		return false;
	const QJsonObject diagnostic = value.toObject();
	ClipDuration range;
	if (!diagnosticRangeFromObject(diagnostic, &range))
		return false;
	if (diagnosticEditedRanges.contains(diagnosticIndex))
		range = diagnosticEditedRanges.value(diagnosticIndex);
	*outRange = normalizedDiagnosticRange(range);
	if (outDiagnostic)
		*outDiagnostic = diagnostic;
	return true;
}

void UploadReviewDialog::seekReviewTableRowStart(int row)
{
	if (!videoEditor || row < 0)
		return;
	if (isDiagnosticTableRow(row)) {
		ClipDuration range;
		if (diagnosticRangeForTableRow(row, &range))
			videoEditor->seekToSeconds(range.startSec);
		return;
	}
	const QVector<ClipDuration> ranges = videoEditor->clipRanges();
	if (row >= 0 && row < ranges.size()) {
		videoEditor->selectRange(row);
		videoEditor->seekToSeconds(ranges.at(row).startSec);
	}
}

void UploadReviewDialog::seekReviewTableRowEnd(int row)
{
	if (!videoEditor || row < 0)
		return;
	if (isDiagnosticTableRow(row)) {
		ClipDuration range;
		if (diagnosticRangeForTableRow(row, &range))
			videoEditor->seekToSeconds(range.endSec);
		return;
	}
	const QVector<ClipDuration> ranges = videoEditor->clipRanges();
	if (row >= 0 && row < ranges.size()) {
		videoEditor->selectRange(row);
		videoEditor->seekToSeconds(ranges.at(row).endSec);
	}
}

bool UploadReviewDialog::eventFilter(QObject *watched, QEvent *event)
{
	return QDialog::eventFilter(watched, event);
}

void UploadReviewDialog::handleClipTableItemChanged(QTableWidgetItem *item)
{
	if (updatingClipTable || !item || !clipTable)
		return;

	const int row = item->row();
	const int column = item->column();
	if (column != 1 && column != 2)
		return;

	const QString value = item->text();
	const bool diagnosticRow = isDiagnosticTableRow(row);
	QTimer::singleShot(0, this, [guard = QPointer<UploadReviewDialog>(this), row, column, value, diagnosticRow]() {
		if (!guard || !guard->clipTable || guard->updatingClipTable || row < 0 ||
		    row >= guard->clipTable->rowCount())
			return;
		if (diagnosticRow)
			guard->applyEditedDiagnosticRangeTime(row, column, value);
		else
			guard->applyEditedRangeTime(row, column, value);
	});
}

ClipDuration UploadReviewDialog::normalizedDiagnosticRange(const ClipDuration &range) const
{
	ClipDuration normalized = range;
	const double durationSec = videoEditor && videoEditor->durationMilliseconds() > 0
					   ? videoEditor->durationMilliseconds() / 1000.0
					   : 0.0;
	normalized.startSec = std::max(0.0, normalized.startSec);
	normalized.endSec = std::max(0.0, normalized.endSec);
	if (durationSec > 0.0) {
		normalized.startSec = std::min(normalized.startSec, durationSec);
		normalized.endSec = std::min(normalized.endSec, durationSec);
	}
	static constexpr double MinRangeDurationSec = 0.25;
	if (normalized.endSec <= normalized.startSec) {
		if (durationSec > 0.0 && normalized.startSec + MinRangeDurationSec > durationSec)
			normalized.startSec = std::max(0.0, durationSec - MinRangeDurationSec);
		normalized.endSec =
			std::min(durationSec > 0.0 ? durationSec : normalized.startSec + MinRangeDurationSec,
				 normalized.startSec + MinRangeDurationSec);
	}
	return normalized;
}

void UploadReviewDialog::applyEditedDiagnosticRangeTime(int row, int column, const QString &rawValue)
{
	if (!videoEditor || row < 0 || (column != 1 && column != 2))
		return;

	const int diagnosticIndex = diagnosticIndexForTableRow(row);
	ClipDuration range;
	QJsonObject diagnostic;
	if (diagnosticIndex < 0 || !diagnosticRangeForTableRow(row, &range, &diagnostic))
		return;
	if (!diagnosticReviewState(diagnosticIndex).trimmed().isEmpty()) {
		blog(LOG_INFO,
		     "Ignored edit for already-reviewed diagnostic candidate in current session. video=%s row=%d diagnosticIndex=%d",
		     videoPath.toUtf8().constData(), row, diagnosticIndex);
		refreshDiagnosticCandidateTable();
		return;
	}

	double seconds = 0.0;
	if (!parseEditableReviewTime(rawValue, &seconds)) {
		refreshDiagnosticCandidateTable();
		return;
	}

	if (column == 1)
		range.startSec = seconds;
	else
		range.endSec = seconds;
	range = normalizedDiagnosticRange(range);
	diagnosticEditedRanges.insert(diagnosticIndex, range);
	ClipDuration originalRange;
	const bool edited = diagnosticRangeFromObject(diagnostic, &originalRange) &&
			    diagnosticRangeMeaningfullyEdited(originalRange, range);
	const int previousSuggestedIndex = diagnosticFeedbackSuggestedIndices.value(diagnosticIndex, -1);
	if (previousSuggestedIndex >= 0 && previousSuggestedIndex < lastSemanticSuggestion.ranges.size()) {
		ensureDiagnosticFeedbackSuggestion(diagnosticIndex, range, diagnostic);
		QJsonObject details = explicitReviewFeedbackDetails.value(previousSuggestedIndex);
		if (!details.isEmpty()) {
			details.insert(QStringLiteral("range_start_sec"), range.startSec);
			details.insert(QStringLiteral("range_end_sec"), range.endSec);
			details.insert(QStringLiteral("diagnostic_range_edited"), edited);
			explicitReviewFeedbackDetails.insert(previousSuggestedIndex, details);
		}
	}

	if (column == 2)
		videoEditor->seekToSeconds(range.endSec);
	else
		videoEditor->seekToSeconds(range.startSec);

	refreshDiagnosticCandidateTable();
	if (clipTable && row >= 0 && row < clipTable->rowCount()) {
		clipTable->selectRow(row);
		clipTable->setCurrentCell(row, column);
	}

	blog(LOG_INFO, "Edited diagnostic candidate review range. video=%s row=%d diagnosticIndex=%d range=%.2f-%.2f",
	     videoPath.toUtf8().constData(), row, diagnosticIndex, range.startSec, range.endSec);
}

void UploadReviewDialog::applyEditedRangeTime(int row, int column, const QString &rawValue)
{
	if (!videoEditor || row < 0 || (column != 1 && column != 2))
		return;

	const QVector<ClipDuration> ranges = videoEditor->clipRanges();
	if (row >= ranges.size())
		return;
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
		const QString persistedDecision = persistedFeedbackDecisionForRange(ranges.at(row), memory);
		if (!persistedDecision.trimmed().isEmpty()) {
			blog(LOG_INFO,
			     "Ignored edit for already-reviewed marker range. video=%s row=%d previous=%s range=%.2f-%.2f",
			     videoPath.toUtf8().constData(), row, persistedDecision.toUtf8().constData(),
			     ranges.at(row).startSec, ranges.at(row).endSec);
			refreshClipTable(ranges);
			return;
		}
	}

	double seconds = 0.0;
	if (!parseEditableReviewTime(rawValue, &seconds)) {
		refreshClipTable(ranges);
		return;
	}

	ClipDuration edited = ranges.at(row);
	if (column == 1)
		edited.startSec = seconds;
	else
		edited.endSec = seconds;

	setClipRangeAtIndex(row, edited.startSec, edited.endSec, true);
}

void UploadReviewDialog::setClipRangeAtIndex(int row, double startSec, double endSec, bool seekToChangedBoundary)
{
	if (!videoEditor || row < 0)
		return;

	QVector<ClipDuration> ranges = videoEditor->clipRanges();
	if (row >= ranges.size())
		return;

	const double durationSec =
		videoEditor->durationMilliseconds() > 0 ? videoEditor->durationMilliseconds() / 1000.0 : 0.0;
	startSec = std::max(0.0, startSec);
	endSec = std::max(0.0, endSec);
	if (durationSec > 0.0) {
		startSec = std::min(startSec, durationSec);
		endSec = std::min(endSec, durationSec);
	}

	static constexpr double MinRangeDurationSec = 0.25;
	if (endSec <= startSec) {
		if (durationSec > 0.0 && startSec + MinRangeDurationSec > durationSec)
			startSec = std::max(0.0, durationSec - MinRangeDurationSec);
		endSec = std::min(durationSec > 0.0 ? durationSec : startSec + MinRangeDurationSec,
				  startSec + MinRangeDurationSec);
	}

	ranges[row].startSec = startSec;
	ranges[row].endSec = endSec;

	QVector<double> markers;
	markers.reserve(ranges.size() * 2);
	for (const ClipDuration &range : ranges) {
		if (!std::isfinite(range.startSec) || !std::isfinite(range.endSec) || range.endSec <= range.startSec)
			continue;
		markers.append(range.startSec);
		markers.append(range.endSec);
	}

	boundaryFeedbackSaved = false;
	if (finishReviewButton) {
		finishReviewButton->setEnabled(true);
		finishReviewButton->setText(QStringLiteral("Finish review (save feedback)"));
	}

	videoEditor->setMarkerPositions(markers);
	videoEditor->selectRange(row);
	if (seekToChangedBoundary)
		videoEditor->seekToSeconds(clipTable && clipTable->currentColumn() == 2 ? endSec : startSec);
	if (clipTable && row < clipTable->rowCount())
		clipTable->selectRow(row);
}

void UploadReviewDialog::nudgeSelectedRangeOrBoundary(double deltaSec)
{
	if (!clipTable || !videoEditor)
		return;

	const int row = clipTable->currentRow();
	if (row < 0)
		return;

	const QVector<ClipDuration> ranges = videoEditor->clipRanges();
	if (row >= ranges.size())
		return;

	ClipDuration edited = ranges.at(row);
	const int column = clipTable->currentColumn();
	if (column == 1) {
		edited.startSec += deltaSec;
	} else if (column == 2) {
		edited.endSec += deltaSec;
	} else {
		edited.startSec += deltaSec;
		edited.endSec += deltaSec;
	}

	setClipRangeAtIndex(row, edited.startSec, edited.endSec, true);
}

void UploadReviewDialog::removeSelectedRangeWithoutFeedbackDecision()
{
	if (!clipTable || !videoEditor || clipTable->rowCount() <= 0)
		return;
	if (clipTable->currentRow() >= 0 && isDiagnosticTableRow(clipTable->currentRow()))
		return;

	QVector<int> rowsToRemove;
	if (clipTable->selectionModel()) {
		const QModelIndexList selectedRows = clipTable->selectionModel()->selectedRows();
		rowsToRemove.reserve(selectedRows.size());
		for (const QModelIndex &index : selectedRows) {
			if (index.isValid())
				rowsToRemove.append(index.row());
		}
	}

	if (rowsToRemove.isEmpty() && clipTable->currentRow() >= 0)
		rowsToRemove.append(clipTable->currentRow());

	if (rowsToRemove.isEmpty())
		return;

	std::sort(rowsToRemove.begin(), rowsToRemove.end());
	rowsToRemove.erase(std::unique(rowsToRemove.begin(), rowsToRemove.end()), rowsToRemove.end());

	ensureReviewFeedbackSnapshot(QStringLiteral("existing_review_markers"));
	const QVector<ClipDuration> ranges = videoEditor->clipRanges();
	if (ranges.isEmpty())
		return;

	QVector<ClipDuration> keptRanges;
	keptRanges.reserve(ranges.size());
	int removedCount = 0;
	for (int i = 0; i < ranges.size(); ++i) {
		const bool shouldRemove = std::binary_search(rowsToRemove.constBegin(), rowsToRemove.constEnd(), i);
		if (!shouldRemove) {
			keptRanges.append(ranges.at(i));
			continue;
		}

		const int suggestedIndex = suggestedIndexForRange(ranges.at(i));
		if (suggestedIndex >= 0) {
			explicitReviewDecisions.remove(suggestedIndex);
			explicitReviewFeedbackDetails.remove(suggestedIndex);
		}
		++removedCount;
	}

	if (removedCount <= 0)
		return;

	boundaryFeedbackSaved = false;
	if (finishReviewButton) {
		finishReviewButton->setEnabled(true);
		finishReviewButton->setText(QStringLiteral("Finish review (save feedback)"));
	}

	const int firstRemovedRow = rowsToRemove.first();
	const int nextRow = keptRanges.isEmpty() ? -1
						 : std::min(firstRemovedRow, static_cast<int>(keptRanges.size()) - 1);
	videoEditor->setMarkerPositions(markerPositionsFromRanges(keptRanges));
	if (nextRow >= 0) {
		videoEditor->selectRange(nextRow);
		if (clipTable && nextRow < clipTable->rowCount())
			clipTable->selectRow(nextRow);
	}

	blog(LOG_INFO, "Removed selected review marker rows without rating. video=%s removed=%d remaining=%d",
	     videoPath.toUtf8().constData(), removedCount, static_cast<int>(keptRanges.size()));
}
