#include "ui/upload-review-dialog.hpp"

#include "ui/review-scoring-preparation.hpp"
#include "ui/review/upload-review-dialog-utils.hpp"
#include "ui/ui-common.hpp"
#include "curation/curation-preset.hpp"
#include "curation/curation-preset-profile.hpp"
#include "ui/video-marker-editor.hpp"
#include "utils/config.hpp"

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QMetaObject>
#include <QObject>
#include <QPointer>

#include <algorithm>

using namespace ReviewDialogUtils;

void UploadReviewDialog::applyCurationSettings(const CurationSettings &settings)
{
	if (topicKeywordsInput)
		topicKeywordsInput->setText(settings.topicKeywords.join(QStringLiteral(", ")));

	setComboCurrentTextIfExists(genreInput, settings.genre);
	setComboCurrentDataIfExists(curationPresetInput, CurationPreset::normalizeId(settings.curationPreset));
	setOpusModelOrDefault(modelInput, settings.model);
	setComboCurrentDataIfExists(clipLengthInput, settings.clipLengthPreset);
	setComboCurrentDataIfExists(sourceLanguageInput, normalizeLanguageSetting(settings.sourceLanguage));
	setComboCurrentDataIfExists(transcriptionLanguageInput,
				    normalizeLanguageSetting(settings.transcriptionLanguage));

	if (skipCurateInput)
		skipCurateInput->setChecked(settings.skipCurate);

	if (opusPromptInput)
		opusPromptInput->setPlainText(settings.aiPrompt.trimmed());
}

void UploadReviewDialog::showSemanticSuggestionProgressDialog()
{
	closeSemanticSuggestionProgressDialog();

	auto *progress = new QProgressDialog(obsText("Status.SuggestClipsCheckingSettings"), QString(), 0, 100, this);
	semanticSuggestionProgressDialog = progress;
	progress->setWindowTitle(obsText("Dialog.SuggestingBestClipsTitle"));
	progress->setWindowModality(Qt::WindowModal);
	progress->setMinimumDuration(0);
	progress->setAutoClose(false);
	progress->setAutoReset(false);
	progress->setValue(0);
	progress->setAttribute(Qt::WA_DeleteOnClose, false);
	progress->setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::CustomizeWindowHint |
				 Qt::MSWindowsFixedSizeDialogHint | Qt::WindowStaysOnTopHint);

	connect(progress, &QObject::destroyed, this, [this, progress]() {
		if (semanticSuggestionProgressDialog == progress)
			semanticSuggestionProgressDialog = nullptr;
	});

	progress->show();
	progress->raise();
	progress->activateWindow();
	QTimer::singleShot(0, progress, [safeProgress = QPointer<QProgressDialog>(progress)]() {
		if (!safeProgress)
			return;
		safeProgress->show();
		safeProgress->raise();
		safeProgress->activateWindow();
	});
}

void UploadReviewDialog::updateSemanticSuggestionProgress(const ReviewScoringProgressUpdate &progressUpdate)
{
	if (QThread::currentThread() != thread()) {
		QPointer<UploadReviewDialog> safeThis(this);
		QMetaObject::invokeMethod(
			this,
			[safeThis, progressUpdate]() {
				if (!safeThis)
					return;
				safeThis->updateSemanticSuggestionProgress(progressUpdate);
			},
			Qt::QueuedConnection);
		return;
	}

	QPointer<QProgressDialog> progress = semanticSuggestionProgressDialog;
	if (!progress)
		return;

	const int maximum = std::max(0, progressUpdate.maximum);
	const int value = std::clamp(progressUpdate.value, 0, maximum);
	progress->setRange(0, maximum);
	if (!progressUpdate.message.isEmpty())
		progress->setLabelText(progressUpdate.message);
	progress->setValue(value);
	if (!progress->isVisible())
		progress->show();
}

void UploadReviewDialog::closeSemanticSuggestionProgressDialog()
{
	if (!semanticSuggestionProgressDialog)
		return;

	QProgressDialog *progress = semanticSuggestionProgressDialog;
	semanticSuggestionProgressDialog = nullptr;
	progress->close();
	progress->deleteLater();
}

void UploadReviewDialog::requestSemanticClipSuggestions()
{
	if (advancedSettingsOnly || !videoEditor || semanticSuggestionInProgress)
		return;

	if (!boundaryFeedbackSaved && !explicitReviewDecisions.isEmpty())
		saveBoundaryFeedback(QStringLiteral("before_new_semantic_suggestions"));

	const CurationSettings reviewSettings = curationSettings();
	semanticSuggestionInProgress = true;
	const int suggestionGeneration = ++semanticSuggestionProgressGeneration;
	showSemanticSuggestionProgressDialog();
	if (suggestClipRangesButton) {
		suggestClipRangesButton->setEnabled(false);
		suggestClipRangesButton->setText(obsText("Status.SuggestingBestClips"));
	}

	blog(LOG_INFO,
	     "Starting semantic review suggestions from review dialog. video=%s sourceLanguage=%s transcriptionLanguage=%s",
	     videoPath.toUtf8().constData(), reviewSettings.sourceLanguage.toUtf8().constData(),
	     reviewSettings.transcriptionLanguage.toUtf8().constData());

	QPointer<UploadReviewDialog> safeThis(this);
	prepare_review_scoring_async(
		this, videoPath, reviewSettings,
		[safeThis, suggestionGeneration](ReviewScoringPreparationResult result) mutable {
			if (!safeThis || safeThis->semanticSuggestionProgressGeneration != suggestionGeneration)
				return;

			safeThis->semanticSuggestionInProgress = false;
			++safeThis->semanticSuggestionProgressGeneration;
			safeThis->closeSemanticSuggestionProgressDialog();
			safeThis->updateSuggestClipRangesButtonState();

			safeThis->applySemanticClipSuggestionResult(result);
		},
		[safeThis, suggestionGeneration](ReviewScoringProgressUpdate progress) mutable {
			if (!safeThis || !safeThis->semanticSuggestionInProgress ||
			    safeThis->semanticSuggestionProgressGeneration != suggestionGeneration)
				return;
			safeThis->updateSemanticSuggestionProgress(progress);
		});
}

void UploadReviewDialog::applySemanticClipSuggestionResult(const ReviewScoringPreparationResult &result)
{
	if (result.attempted) {
		blog(LOG_INFO,
		     "Semantic review suggestions returned to review dialog. video=%s applied=%s canceled=%s summary=%s",
		     videoPath.toUtf8().constData(), result.applied ? "true" : "false",
		     result.canceled ? "true" : "false", result.summary.toUtf8().constData());
	}

	if (!videoEditor)
		return;

	if (!result.applied) {
		restoreReviewedPositiveMarkersFromFeedbackIfEmpty(
			QStringLiteral("diagnostics_only_result_preserve_reviewed_markers"));
		lastSemanticSuggestion.ranges.clear();
		lastSemanticSuggestion.contentId = result.contentId;
		lastSemanticSuggestion.contentIdAliases = result.contentIdAliases;
		lastSemanticSuggestion.summary = result.summary;
		lastSemanticSuggestion.source = QStringLiteral("semantic_review_diagnostics_only");
		lastSemanticSuggestion.candidateDiagnostics = result.candidateDiagnostics;
		lastSemanticSuggestionTrainingProfile = result.resolvedProfileId;
		diagnosticReviewStates.clear();
		diagnosticEditedRanges.clear();
		diagnosticFeedbackSuggestedIndices.clear();
		diagnosticReviewMode = false;
		explicitReviewDecisions.clear();
		explicitReviewFeedbackDetails.clear();
		refreshClipTable(videoEditor->clipRanges());
		blog(LOG_INFO,
		     "Preserved semantic review diagnostics without applying markers. video=%s diagnostics=%d",
		     videoPath.toUtf8().constData(), static_cast<int>(result.candidateDiagnostics.size()));
		return;
	}

	QVector<ClipDuration> currentRanges = videoEditor->clipRanges();
	if (isDefaultNoMarkerPlaceholderRange(videoEditor && videoEditor->hasExplicitClipMarkers(),
								 videoEditor ? videoEditor->durationMilliseconds() : 0, currentRanges))
		currentRanges.clear();
	int addedSuggestionCount = 0;
	const QVector<ClipDuration> mergedRanges = mergeReviewRangesPreservingExisting(
		currentRanges, result.settings.clipDurations, &addedSuggestionCount);

	lastSemanticSuggestion.ranges = mergedRanges;
	lastSemanticSuggestion.contentId = result.contentId;
	lastSemanticSuggestion.contentIdAliases = result.contentIdAliases;
	lastSemanticSuggestion.summary = result.summary;
	lastSemanticSuggestion.source = QStringLiteral("semantic_review_button");
	lastSemanticSuggestion.candidateDiagnostics = result.candidateDiagnostics;
	lastSemanticSuggestionTrainingProfile = result.resolvedProfileId;
	diagnosticReviewStates.clear();
	diagnosticEditedRanges.clear();
	diagnosticFeedbackSuggestedIndices.clear();
	diagnosticReviewMode = false;
	explicitReviewDecisions.clear();
	explicitReviewFeedbackDetails.clear();
	boundaryFeedbackSaved = false;
	if (finishReviewButton) {
		finishReviewButton->setEnabled(true);
		finishReviewButton->setText(QStringLiteral("Finish review (save feedback)"));
	}
	videoEditor->setMarkerPositions(markerPositionsFromRanges(mergedRanges));
	refreshClipTable(videoEditor->clipRanges());
	blog(LOG_INFO,
	     "Merged semantic review suggestion ranges without deleting existing markers. video=%s existing=%d suggested=%d added=%d total=%d",
	     videoPath.toUtf8().constData(), static_cast<int>(currentRanges.size()),
	     static_cast<int>(result.settings.clipDurations.size()), addedSuggestionCount,
	     static_cast<int>(mergedRanges.size()));
}

bool UploadReviewDialog::restoreReviewedPositiveMarkersFromFeedbackIfEmpty(const QString &reason)
{
	if (advancedSettingsOnly || !videoEditor)
		return false;
	if (videoEditor->hasExplicitClipMarkers())
		return false;

	const CurationSettings settings = curationSettings();
	QString presetId = Curation::resolvePresetProfileId(settings, settings.aiPrompt);
	if (presetId.isEmpty())
		presetId = Curation::autoPresetProfileId();
	QStringList contentIds;
	const QString fileContentId = Curation::Feedback::CurationFeedbackStore::fileContentId(videoPath);
	if (!fileContentId.isEmpty())
		contentIds.append(fileContentId);
	const Curation::Feedback::FeedbackRangeMemory memory =
		Curation::Feedback::CurationFeedbackStore::loadRangeMemoryForVideo(videoPath, presetId, contentIds);
	const QVector<ClipDuration> restored = restoredReviewedPositiveRangesFromMemory(memory, 32);
	if (restored.isEmpty())
		return false;

	videoEditor->setMarkerPositions(markerPositionsFromRanges(restored));
	lastSemanticSuggestion.ranges = restored;
	lastSemanticSuggestion.contentId = fileContentId;
	lastSemanticSuggestion.contentIdAliases.clear();
	lastSemanticSuggestion.source = QStringLiteral("restored_reviewed_positive_markers");
	lastSemanticSuggestion.summary = QStringLiteral("reviewed positive markers restored from feedback memory");
	lastSemanticSuggestion.candidateDiagnostics = QJsonArray{};
	lastSemanticSuggestionTrainingProfile = presetId;
	explicitReviewDecisions.clear();
	explicitReviewFeedbackDetails.clear();
	diagnosticReviewStates.clear();
	diagnosticEditedRanges.clear();
	diagnosticFeedbackSuggestedIndices.clear();
	diagnosticReviewMode = false;
	boundaryFeedbackSaved = false;
	restoredReviewedPositiveMarkersFromMemory = true;
	blog(LOG_INFO,
	     "Restored reviewed positive markers from feedback memory. video=%s reason=%s restored=%d positives=%d records=%d",
	     videoPath.toUtf8().constData(), reason.toUtf8().constData(), static_cast<int>(restored.size()),
	     static_cast<int>(memory.positiveRanges.size()), memory.recordsRead);
	return true;
}

