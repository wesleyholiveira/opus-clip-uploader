#include "ui/upload-review-dialog.hpp"

#include "ui/advanced-settings-tree.hpp"
#include "ui/review/review-clip-table-widget.hpp"
#include "ui/review/upload-review-dialog-utils.hpp"
#include "ui/ui-common.hpp"
#include "ui/video-marker-editor.hpp"
#include "curation/curation-preset.hpp"
#include "utils/config.hpp"

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QTableWidget>
#include <QVBoxLayout>

using namespace ReviewDialogUtils;

UploadReviewDialog::UploadReviewDialog(const QString &videoPath, QWidget *parent)
	: UploadReviewDialog(videoPath, CurationSettings{}, false, parent)
{
}

UploadReviewDialog::UploadReviewDialog(const QString &videoPath, const CurationSettings &initialCurationSettings,
				       bool showAdvancedSettingsOnly, QWidget *parent)
	: QDialog(parent),
	  videoPath(videoPath),
	  initialSettings(initialCurationSettings),
	  advancedSettingsOnly(showAdvancedSettingsOnly)
{
	setWindowTitle(QString("Clip Cropper - %1").arg(obsText("Dialog.ReviewVideoTitle")));
	resize(920, advancedSettingsOnly ? 560 : 720);

	auto *mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(14, 14, 14, 12);
	mainLayout->setSpacing(8);

	if (!advancedSettingsOnly) {
		videoEditor = new VideoMarkerEditor(videoPath, this);

		clipTable = new ReviewClipTableWidget(this);
		clipTable->setColumnCount(4);
		clipTable->setHorizontalHeaderLabels({obsText("Table.Marker"), QStringLiteral("Range Start"),
						      QStringLiteral("Range End"), QStringLiteral("Feedback")});
		clipTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
		clipTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
		clipTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
		clipTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
		clipTable->setMinimumHeight(145);
		clipTable->setSelectionBehavior(QAbstractItemView::SelectRows);
		clipTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
		clipTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
		clipTable->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
		clipTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

		creditEstimateLabel = new QLabel(this);
		creditEstimateLabel->setWordWrap(true);
		creditEstimateLabel->setToolTip(obsText("Tooltip.OpusCreditsSelectedEstimate"));
		updateCreditEstimate(videoEditor->clipRanges());

		connect(videoEditor, &VideoMarkerEditor::rangesChanged, this,
			[this](const QVector<ClipDuration> &ranges) {
				boundaryFeedbackSaved = false;
				if (finishReviewButton) {
					finishReviewButton->setEnabled(true);
					finishReviewButton->setText(QStringLiteral("Finish review (save feedback)"));
				}
				Q_UNUSED(ranges);
				refreshClipTable(videoEditor ? videoEditor->clipRanges() : QVector<ClipDuration>{});
			});
		connect(clipTable, &QTableWidget::currentCellChanged, this, [this](int currentRow, int, int, int) {
			if (!videoEditor || currentRow < 0)
				return;
			if (isDiagnosticTableRow(currentRow)) {
				ClipDuration range;
				if (diagnosticRangeForTableRow(currentRow, &range))
					videoEditor->seekToSeconds(range.startSec);
				return;
			}
			videoEditor->selectRange(currentRow);
		});
		connect(clipTable, &QTableWidget::cellClicked, this, [this](int row, int column) {
			if (!videoEditor || row < 0)
				return;
			if (isDiagnosticTableRow(row)) {
				if (column == 2)
					seekReviewTableRowEnd(row);
				else
					seekReviewTableRowStart(row);
				return;
			}
			videoEditor->selectRange(row);
			videoEditor->goToSelectedRange();
		});
		connect(clipTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int column) {
			if (!videoEditor || row < 0)
				return;
			if (isDiagnosticTableRow(row)) {
				if (column == 0)
					showMarkerDiagnostics(row);
				return;
			}
			videoEditor->selectRange(row);
			if (column != 1 && column != 2)
				videoEditor->goToSelectedRange();
		});
		connect(clipTable, &QTableWidget::itemChanged, this, &UploadReviewDialog::handleClipTableItemChanged);
		connect(clipTable, &QTableWidget::itemSelectionChanged, this,
			[this]() { updateDiagnosticModeControls(); });

		if (auto *reviewTable = dynamic_cast<ReviewClipTableWidget *>(clipTable)) {
			const QPointer<UploadReviewDialog> self(this);
			reviewTable->isDiagnosticRow = [self](int row) {
				return self && self->isDiagnosticTableRow(row);
			};
			reviewTable->removeSelectedRows = [self]() {
				if (self)
					self->removeSelectedRangeWithoutFeedbackDecision();
			};
			reviewTable->nudgeSelectedRange = [self](double deltaSec) {
				if (self)
					self->nudgeSelectedRangeOrBoundary(deltaSec);
			};
			reviewTable->showMarkerDiagnostics = [self](int row) {
				if (self)
					self->showMarkerDiagnostics(row);
			};
			reviewTable->approveMarker = [self](int row) {
				if (self)
					self->setClipReviewDecision(row, QStringLiteral("liked"));
			};
			reviewTable->rejectMarker = [self](int row) {
				if (self)
					self->setClipReviewDecision(row, QStringLiteral("disliked"));
			};
			reviewTable->markMarkerAdjusted = [self](int row) {
				if (self)
					self->setClipReviewDecision(row, QStringLiteral("adjusted"));
			};
			reviewTable->approveEditedMarker = [self](int row) {
				if (self)
					self->setClipReviewDecision(row, QStringLiteral("approved_adjusted"));
			};
			reviewTable->ignoreMarker = [self](int row) {
				if (self)
					self->setClipReviewDecision(row, QStringLiteral("ignored_diagnostic"));
			};
			reviewTable->seekMarkerStart = [self](int row) {
				if (self)
					self->seekReviewTableRowStart(row);
			};
			reviewTable->seekMarkerEnd = [self](int row) {
				if (self)
					self->seekReviewTableRowEnd(row);
			};
		}
	}

	topicKeywordsInput = new QLineEdit(this);
	topicKeywordsInput->setPlaceholderText(obsText("Placeholder.TopicKeywords"));

	opusPromptInput = new QPlainTextEdit(this);
	opusPromptInput->setMinimumHeight(advancedSettingsOnly ? 260 : 96);
	opusPromptInput->setPlaceholderText(obsText("Placeholder.OpusPrompt"));

	modelInput = new QComboBox(this);
	modelInput->addItems(QStringList{"ClipBasic", "ClipAnything"});
	setOpusModelOrDefault(modelInput);

	clipLengthInput = new QComboBox(this);
	clipLengthInput->addItem(obsText("Option.ClipLengthAuto"), QStringLiteral("Auto"));
	clipLengthInput->addItem(obsText("Option.ClipLengthShort"), QStringLiteral("Short"));
	clipLengthInput->addItem(obsText("Option.ClipLengthMedium"), QStringLiteral("Medium"));
	clipLengthInput->addItem(obsText("Option.ClipLengthLong"), QStringLiteral("Long"));
	setComboCurrentDataIfExists(clipLengthInput, QStringLiteral("Medium"));

	sourceLanguageInput = new QComboBox(this);
	addLanguageOptions(sourceLanguageInput);
	setComboCurrentDataIfExists(sourceLanguageInput, normalizeLanguageSetting(PluginConfig::getValue(
								 QString::fromLatin1(CONFIG_OPUS_SOURCE_LANGUAGE),
								 QString::fromLatin1(LANGUAGE_AUTO))));

	transcriptionLanguageInput = new QComboBox(this);
	addLanguageOptions(transcriptionLanguageInput);
	setComboCurrentDataIfExists(transcriptionLanguageInput, QString::fromLatin1(LANGUAGE_AUTO));

	genreInput = new QComboBox(this);
	genreInput->addItems(QStringList{"Auto", "Q&A", "Commentary", "Marketing", "Webinar", "Motivational speech",
					 "Podcast", "Academic", "Listicle", "Product reviews", "How-to", "Comedy",
					 "Sports commentary", "Church", "News", "Vlog", "Gaming", "Others"});

	curationPresetInput = new QComboBox(this);
	for (const auto &option : CurationPreset::options())
		curationPresetInput->addItem(option.second, option.first);
	setComboCurrentDataIfExists(curationPresetInput, QStringLiteral("auto"));

	skipCurateInput = new QCheckBox(obsText("Label.SkipCurate"), this);

	if (!advancedSettingsOnly) {
		suggestClipRangesButton = new QPushButton(obsText("Button.SuggestBestClips"), this);
		connect(suggestClipRangesButton, &QPushButton::clicked, this,
			&UploadReviewDialog::requestSemanticClipSuggestions);

		finishReviewButton = new QPushButton(QStringLiteral("Finish review (save feedback)"), this);
		finishReviewButton->setToolTip(
			QStringLiteral("Persist like/dislike and marker adjustments without starting upload."));
		connect(finishReviewButton, &QPushButton::clicked, this, &UploadReviewDialog::finishReviewFeedback);
	}

	QHBoxLayout *languageRow = nullptr;
	if (!advancedSettingsOnly) {
		languageRow = new QHBoxLayout();
		languageRow->setContentsMargins(0, 0, 0, 0);
		languageRow->setSpacing(8);
		languageRow->addWidget(new QLabel(obsText("Settings.SourceLanguage"), this));
		languageRow->addWidget(sourceLanguageInput);
		languageRow->addSpacing(16);
		languageRow->addWidget(new QLabel(obsText("Settings.TranscriptionLanguage"), this));
		languageRow->addWidget(transcriptionLanguageInput);
		if (suggestClipRangesButton) {
			languageRow->addSpacing(16);
			languageRow->addWidget(suggestClipRangesButton);
		}
		languageRow->addStretch();
	}

	auto *advancedTree = new AdvancedSettingsTree(this);
	if (advancedSettingsOnly) {
		advancedTree->addField(obsText("Settings.SourceLanguage"), sourceLanguageInput);
		advancedTree->addField(obsText("Settings.TranscriptionLanguage"), transcriptionLanguageInput);
	}
	advancedTree->addField(obsText("Settings.Model"), modelInput);
	advancedTree->addField(obsText("Settings.ClipLength"), clipLengthInput);
	advancedTree->addField(obsText("Settings.TopicKeywords"), topicKeywordsInput);
	advancedTree->addField(obsText("Settings.OpusPrompt"), opusPromptInput);
	advancedTree->addField(obsText("Settings.Genre"), genreInput);
	advancedTree->addField(obsText("Settings.CurationPreset"), curationPresetInput);
	advancedTree->addField(obsText("Settings.SkipCurate"), skipCurateInput);

	if (advancedSettingsOnly) {
		applyCurationSettings(initialSettings);
		if (advancedTree->rootItem())
			advancedTree->rootItem()->setExpanded(true);
		advancedTree->setMinimumHeight(360);
	} else {
		/*
		 * The full review dialog keeps the upload action row outside the scrollable
		 * body. Bound the advanced settings tree so expanding it cannot consume the
		 * entire dialog and hide Confirm and Upload.
		 */
		advancedTree->setMinimumHeight(44);
		advancedTree->setMaximumHeight(220);
		advancedTree->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

		loadSavedCurationOptions();
		if (videoEditor && initialSettings.clipDurations.isEmpty())
			restoreReviewedPositiveMarkersFromFeedbackIfEmpty(
				QStringLiteral("dialog_open_no_saved_markers"));
		if (videoEditor && !initialSettings.clipDurations.isEmpty()) {
			videoEditor->setMarkerPositions(markerPositionsFromRanges(initialSettings.clipDurations));
			refreshClipTable(videoEditor->clipRanges());
			captureInitialSemanticSuggestion();
			blog(LOG_INFO, "Applied semantic review suggestion ranges in review dialog. video=%s ranges=%d",
			     videoPath.toUtf8().constData(), static_cast<int>(initialSettings.clipDurations.size()));
		} else if (videoEditor) {
			refreshClipTable(videoEditor->clipRanges());
			ensureReviewFeedbackSnapshot(QStringLiteral("existing_review_markers"));
		}
	}

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	if (auto *okButton = buttons->button(QDialogButtonBox::Ok))
		okButton->setText(obsText("Button.ConfirmAndUpload"));
	connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
		saveCurationOptions();
		saveBoundaryFeedback(QStringLiteral("review_accepted"));
		accept();
	});
	connect(buttons, &QDialogButtonBox::rejected, this, [this]() {
		// Cancel closes the review without teaching the scorer. Explicit marker feedback is
		// saved through Finish review; treating a dialog cancel as rejection pollutes
		// calibration and can make rejected/accepted ranges ambiguous.
		reject();
	});

	auto *contentWidget = new QWidget(this);
	auto *contentLayout = new QVBoxLayout(contentWidget);
	contentLayout->setContentsMargins(0, 0, 0, 0);
	contentLayout->setSpacing(8);
	contentWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

	contentLayout->addWidget(new QLabel(QFileInfo(videoPath).fileName(), contentWidget));
	if (languageRow)
		contentLayout->addLayout(languageRow);

	if (advancedSettingsOnly) {
		contentLayout->addWidget(advancedTree, 1);
		mainLayout->addWidget(contentWidget, 1);
	} else {
		contentLayout->addWidget(videoEditor, 1);
		contentLayout->addWidget(new QLabel(obsText("Label.Markers"), contentWidget));
		contentLayout->addWidget(clipTable);
		auto *reviewNudgeRow = new QHBoxLayout();
		reviewNudgeRow->setContentsMargins(0, 0, 0, 0);
		reviewNudgeRow->setSpacing(6);
		auto *nudgeBackButton = new QPushButton(QStringLiteral("−1s selected"), contentWidget);
		nudgeBackButton->setToolTip(QStringLiteral(
			"Nudge the selected marker row or selected Start/End cell back by 1 second. Shortcut: Ctrl+Alt,"));
		connect(nudgeBackButton, &QPushButton::clicked, this, [this]() { nudgeSelectedRangeOrBoundary(-1.0); });
		auto *nudgeForwardButton = new QPushButton(QStringLiteral("+1s selected"), contentWidget);
		nudgeForwardButton->setToolTip(QStringLiteral(
			"Nudge the selected marker row or selected Start/End cell forward by 1 second. Shortcut: Ctrl+Alt+."));
		connect(nudgeForwardButton, &QPushButton::clicked, this,
			[this]() { nudgeSelectedRangeOrBoundary(1.0); });
		auto *importMarkersButton = new QPushButton(QStringLiteral("Import markers..."), contentWidget);
		importMarkersButton->setToolTip(
			QStringLiteral("Import review markers from JSON, JSON array, or plain text/CSV time ranges."));
		connect(importMarkersButton, &QPushButton::clicked, this, &UploadReviewDialog::importReviewMarkers);
		auto *exportMarkersButton = new QPushButton(QStringLiteral("Export markers..."), contentWidget);
		exportMarkersButton->setToolTip(
			QStringLiteral("Export the current review markers as JSON with ranges and marker positions."));
		connect(exportMarkersButton, &QPushButton::clicked, this, &UploadReviewDialog::exportReviewMarkers);
		reviewNudgeRow->addWidget(nudgeBackButton);
		reviewNudgeRow->addWidget(nudgeForwardButton);
		reviewNudgeRow->addSpacing(12);
		reviewNudgeRow->addWidget(importMarkersButton);
		reviewNudgeRow->addWidget(exportMarkersButton);
		reviewNudgeRow->addStretch(1);
		contentLayout->addLayout(reviewNudgeRow);
		if (finishReviewButton)
			contentLayout->addWidget(finishReviewButton);
		contentLayout->addWidget(creditEstimateLabel);
		contentLayout->addWidget(advancedTree);

		auto *contentScroll = new QScrollArea(this);
		contentScroll->setWidgetResizable(true);
		contentScroll->setFrameShape(QFrame::NoFrame);
		contentScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		contentScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
		contentScroll->setMinimumSize(0, 0);
		contentScroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
		contentScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
		contentScroll->setWidget(contentWidget);
		mainLayout->addWidget(contentScroll, 1);
	}

	buttons->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	mainLayout->addWidget(buttons, 0);
}

