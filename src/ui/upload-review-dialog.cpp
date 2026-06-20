#include "ui/upload-review-dialog.hpp"

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

#include "ui/advanced-settings-tree.hpp"
#include "ui/review-scoring-preparation.hpp"
#include "ui/ui-common.hpp"
#include "ui/video-marker-editor.hpp"
#include "curation/curation-preset.hpp"
#include "utils/config.hpp"

#include <QAbstractItemView>
#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

static constexpr const char *CONFIG_REVIEW_SETTINGS_PREFIX = "review.settings";
static constexpr const char *CONFIG_OPUS_SOURCE_LANGUAGE = "opus_source_lang";
static constexpr const char *LANGUAGE_AUTO = "auto";

static QString normalizeLanguageSetting(QString value)
{
	value = value.trimmed().toLower();
	if (value.isEmpty())
		return QString::fromLatin1(LANGUAGE_AUTO);

	if (value == QStringLiteral("pt-br") || value == QStringLiteral("portuguese"))
		return QStringLiteral("pt");

	if (value == QStringLiteral("en-us") || value == QStringLiteral("english"))
		return QStringLiteral("en");

	return value;
}

static void addLanguageOptions(QComboBox *combo)
{
	combo->addItem(QStringLiteral("auto"), QStringLiteral("auto"));
	combo->addItem(QStringLiteral("pt"), QStringLiteral("pt"));
	combo->addItem(QStringLiteral("en"), QStringLiteral("en"));
}

static QString safeReviewFileKey(const QString &videoPath)
{
	QString fileName = QFileInfo(videoPath).fileName().trimmed();
	if (fileName.isEmpty())
		fileName = videoPath.trimmed();

	if (fileName.isEmpty())
		return QStringLiteral("unknown");

	return QString::fromLatin1(
		fileName.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

static QString formatReviewTimeToken(double seconds)
{
	const qint64 milliseconds = static_cast<qint64>(std::max(0.0, std::round(seconds * 1000.0)));
	return QString::number(milliseconds);
}

static QString reviewRangeKeyFromMarkers(QVector<double> markers)
{
	std::sort(markers.begin(), markers.end());

	if (markers.isEmpty())
		return QStringLiteral("no_markers");

	QStringList values;
	values.reserve(markers.size());
	for (double marker : markers) {
		if (std::isfinite(marker))
			values.append(formatReviewTimeToken(marker));
	}

	return values.isEmpty() ? QStringLiteral("no_markers") : values.join(QStringLiteral("_"));
}


static QVector<double> markerPositionsFromRanges(const QVector<ClipDuration> &ranges)
{
	QVector<double> markers;
	markers.reserve(ranges.size() * 2);
	for (const ClipDuration &range : ranges) {
		if (!std::isfinite(range.startSec) || !std::isfinite(range.endSec) || range.endSec <= range.startSec)
			continue;
		markers.append(std::max(0.0, range.startSec));
		markers.append(std::max(0.0, range.endSec));
	}
	std::sort(markers.begin(), markers.end());
	return markers;
}

static QString reviewRangeKeyFromRanges(const QVector<ClipDuration> &ranges)
{
	if (ranges.isEmpty())
		return QStringLiteral("no_markers");

	QStringList values;
	values.reserve(ranges.size());
	for (const ClipDuration &range : ranges) {
		if (!std::isfinite(range.startSec) || !std::isfinite(range.endSec))
			continue;

		values.append(QStringLiteral("%1-%2").arg(formatReviewTimeToken(range.startSec),
							  formatReviewTimeToken(range.endSec)));
	}

	return values.isEmpty() ? QStringLiteral("no_markers") : values.join(QStringLiteral("_"));
}

static QString reviewSettingsConfigKey(const QString &videoPath, const QString &rangeKey)
{
	return QStringLiteral("%1.%2.%3")
		.arg(QString::fromLatin1(CONFIG_REVIEW_SETTINGS_PREFIX), safeReviewFileKey(videoPath), rangeKey);
}

static void setComboCurrentTextIfExists(QComboBox *combo, const QString &value)
{
	if (!combo || value.trimmed().isEmpty())
		return;

	const int index = combo->findText(value, Qt::MatchFixedString);
	if (index >= 0)
		combo->setCurrentIndex(index);
}

static void setComboCurrentDataIfExists(QComboBox *combo, const QString &value)
{
	if (!combo || value.trimmed().isEmpty())
		return;

	const int index = combo->findData(value.trimmed(), Qt::UserRole, Qt::MatchFixedString);
	if (index >= 0) {
		combo->setCurrentIndex(index);
		return;
	}

	setComboCurrentTextIfExists(combo, value);
}

static void setOpusModelOrDefault(QComboBox *combo, const QString &value = {})
{
	if (!combo)
		return;

	const QString requested = value.trimmed().isEmpty() ? QStringLiteral("ClipAnything") : value.trimmed();
	setComboCurrentTextIfExists(combo, requested);

	if (combo->currentText().trimmed().isEmpty() || combo->currentText() == QStringLiteral("ClipBasic"))
		setComboCurrentTextIfExists(combo, QStringLiteral("ClipAnything"));
}

static double totalRangeDurationSeconds(const QVector<ClipDuration> &ranges)
{
	double totalSeconds = 0.0;
	for (const ClipDuration &range : ranges)
		totalSeconds += std::max(0.0, range.endSec - range.startSec);

	return totalSeconds;
}

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
	setWindowTitle(QString("Clip Cropper - %1")
			       .arg(advancedSettingsOnly ? obsText("Dialog.ReviewGeneratedPromptTitle")
							 : obsText("Dialog.ReviewVideoTitle")));
	resize(advancedSettingsOnly ? 780 : 920, advancedSettingsOnly ? 560 : 720);

	auto *mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(14, 14, 14, 12);
	mainLayout->setSpacing(8);

	if (!advancedSettingsOnly) {
		videoEditor = new VideoMarkerEditor(videoPath, this);

		clipTable = new QTableWidget(this);
		clipTable->setColumnCount(3);
		clipTable->setHorizontalHeaderLabels(
			{obsText("Table.Marker"), obsText("Table.StartSec"), obsText("Table.EndSec")});
		clipTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
		clipTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
		clipTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
		clipTable->setMinimumHeight(145);
		clipTable->setSelectionBehavior(QAbstractItemView::SelectRows);
		clipTable->setSelectionMode(QAbstractItemView::SingleSelection);
		clipTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
		clipTable->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
		clipTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

		creditEstimateLabel = new QLabel(this);
		creditEstimateLabel->setWordWrap(true);
		creditEstimateLabel->setToolTip(obsText("Tooltip.OpusCreditsSelectedEstimate"));
		updateCreditEstimate(videoEditor->clipRanges());

		connect(videoEditor, &VideoMarkerEditor::rangesChanged, this, &UploadReviewDialog::refreshClipTable);
		connect(clipTable, &QTableWidget::currentCellChanged, this,
			[this](int currentRow, int, int, int) { videoEditor->selectRange(currentRow); });
		connect(clipTable, &QTableWidget::cellClicked, this, [this](int row, int) {
			videoEditor->selectRange(row);
			videoEditor->goToSelectedRange();
		});
		connect(clipTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
			videoEditor->selectRange(row);
			videoEditor->goToSelectedRange();
		});
	}

	topicKeywordsInput = new QLineEdit(this);
	topicKeywordsInput->setPlaceholderText(obsText("Placeholder.TopicKeywords"));

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

	customPromptInput = new QPlainTextEdit(this);
	customPromptInput->setMinimumHeight(advancedSettingsOnly ? 280 : 110);
	customPromptInput->setPlaceholderText(obsText("Placeholder.CustomPrompt"));

	if (!advancedSettingsOnly) {
		suggestClipRangesButton = new QPushButton(obsText("Button.SuggestBestClips"), this);
		connect(suggestClipRangesButton, &QPushButton::clicked, this,
			&UploadReviewDialog::requestSemanticClipSuggestions);
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
	advancedTree->addField(obsText("Settings.Genre"), genreInput);
	advancedTree->addField(obsText("Settings.CurationPreset"), curationPresetInput);
	advancedTree->addField(obsText("Settings.SkipCurate"), skipCurateInput);
	advancedTree->addField(obsText("Settings.CustomPrompt"), customPromptInput);

	if (advancedSettingsOnly) {
		applyCurationSettings(initialSettings);
		if (advancedTree->rootItem())
			advancedTree->rootItem()->setExpanded(true);
		advancedTree->setMinimumHeight(360);
	} else {
		loadSavedCurationOptions();
		if (videoEditor && !initialSettings.clipDurations.isEmpty()) {
			videoEditor->setMarkerPositions(markerPositionsFromRanges(initialSettings.clipDurations));
			refreshClipTable(videoEditor->clipRanges());
			blog(LOG_INFO, "Applied semantic review suggestion ranges in review dialog. video=%s ranges=%d",
			     videoPath.toUtf8().constData(), static_cast<int>(initialSettings.clipDurations.size()));
		}
	}

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	if (advancedSettingsOnly) {
		if (auto *okButton = buttons->button(QDialogButtonBox::Ok))
			okButton->setText(obsText("Button.ConfirmAndUpload"));
	}
	connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
		saveCurationOptions();
		accept();
	});
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	mainLayout->addWidget(new QLabel(QFileInfo(videoPath).fileName(), this));
	if (languageRow)
		mainLayout->addLayout(languageRow);

	if (advancedSettingsOnly) {
		auto *promptReviewLabel = new QLabel(obsText("Message.ReviewGeneratedPromptBeforeUpload"), this);
		promptReviewLabel->setWordWrap(true);
		mainLayout->addWidget(promptReviewLabel);
		mainLayout->addWidget(advancedTree, 1);
	} else {
		mainLayout->addWidget(videoEditor, 1);
		mainLayout->addWidget(new QLabel(obsText("Label.Markers"), this));
		mainLayout->addWidget(clipTable);
		mainLayout->addWidget(creditEstimateLabel);
		mainLayout->addWidget(advancedTree);
	}

	mainLayout->addWidget(buttons);
}

void UploadReviewDialog::refreshClipTable(const QVector<ClipDuration> &ranges)
{
	if (!clipTable || !videoEditor)
		return;

	const int previousRow = clipTable->currentRow();
	clipTable->setRowCount(0);

	for (int i = 0; i < ranges.size(); ++i) {
		const auto &range = ranges[i];
		const double selectedSec = std::max(0.0, range.endSec - range.startSec);
		const int row = clipTable->rowCount();
		clipTable->insertRow(row);
		clipTable->setItem(row, 0,
				   new QTableWidgetItem(QString("#%1  %2 → %3  (%4s)")
								.arg(i + 1)
								.arg(videoEditor->formatTimecode(range.startSec),
								     videoEditor->formatTimecode(range.endSec))
								.arg(selectedSec, 0, 'f', 0)));
		clipTable->setItem(row, 1, new QTableWidgetItem(QString::number(range.startSec, 'f', 2)));
		clipTable->setItem(row, 2, new QTableWidgetItem(QString::number(range.endSec, 'f', 2)));
	}

	updateCreditEstimate(ranges);

	if (clipTable->rowCount() <= 0)
		return;

	const int rowToSelect = std::clamp(previousRow, 0, clipTable->rowCount() - 1);
	clipTable->selectRow(rowToSelect);
	videoEditor->selectRange(rowToSelect);
}

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

	if (customPromptInput)
		customPromptInput->setPlainText(settings.aiPrompt.trimmed());
}

void UploadReviewDialog::requestSemanticClipSuggestions()
{
	if (advancedSettingsOnly || !videoEditor || semanticSuggestionInProgress)
		return;

	const CurationSettings reviewSettings = curationSettings();
	semanticSuggestionInProgress = true;
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
		[safeThis](ReviewScoringPreparationResult result) mutable {
			if (!safeThis)
				return;

			safeThis->semanticSuggestionInProgress = false;
			if (safeThis->suggestClipRangesButton) {
				safeThis->suggestClipRangesButton->setEnabled(true);
				safeThis->suggestClipRangesButton->setText(obsText("Button.SuggestBestClips"));
			}

			safeThis->applySemanticClipSuggestionResult(result);
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

	if (!result.applied || !videoEditor)
		return;

	videoEditor->setMarkerPositions(markerPositionsFromRanges(result.settings.clipDurations));
	refreshClipTable(videoEditor->clipRanges());
	blog(LOG_INFO, "Applied semantic review suggestion ranges from review dialog. video=%s ranges=%d",
	     videoPath.toUtf8().constData(), static_cast<int>(result.settings.clipDurations.size()));
}

void UploadReviewDialog::updateCreditEstimate(const QVector<ClipDuration> &ranges)
{
	if (!creditEstimateLabel)
		return;

	const double totalSeconds = totalRangeDurationSeconds(ranges);
	const int estimatedCredits = estimate_opus_credits(totalSeconds);

	if (estimatedCredits <= 0) {
		creditEstimateLabel->setText(obsText("Label.OpusCreditsNoSelection"));
		return;
	}

	creditEstimateLabel->setText(obsText("Label.OpusCreditsSelectedEstimate")
					     .arg(estimatedCredits)
					     .arg(format_duration_seconds(totalSeconds)));
}

void UploadReviewDialog::loadSavedCurationOptions()
{
	const QString key = currentReviewSettingsKey();
	if (key.isEmpty())
		return;

	const QString raw = PluginConfig::getValue(key).trimmed();
	if (raw.isEmpty())
		return;

	const QJsonDocument document = QJsonDocument::fromJson(raw.toUtf8());
	if (!document.isObject())
		return;

	const QJsonObject root = document.object();

	if (topicKeywordsInput && root.contains(QStringLiteral("topicKeywords"))) {
		QStringList keywords;
		const QJsonValue value = root.value(QStringLiteral("topicKeywords"));
		if (value.isArray()) {
			const QJsonArray array = value.toArray();
			for (const QJsonValue &item : array) {
				const QString keyword = item.toString().trimmed();
				if (!keyword.isEmpty())
					keywords.append(keyword);
			}
		} else {
			const QString rawKeywords = value.toString().trimmed();
			if (!rawKeywords.isEmpty())
				keywords = rawKeywords.split(QStringLiteral(","), Qt::SkipEmptyParts);
		}

		for (QString &keyword : keywords)
			keyword = keyword.trimmed();

		topicKeywordsInput->setText(keywords.join(QStringLiteral(", ")));
	}

	setComboCurrentTextIfExists(genreInput, root.value(QStringLiteral("genre")).toString());
	setComboCurrentDataIfExists(
		curationPresetInput,
		CurationPreset::normalizeId(
			root.value(QStringLiteral("curationPreset")).toString(QStringLiteral("auto"))));
	setOpusModelOrDefault(modelInput, root.value(QStringLiteral("model")).toString());
	setComboCurrentDataIfExists(clipLengthInput,
				    root.value(QStringLiteral("clipLengthPreset")).toString(QStringLiteral("Medium")));
	setComboCurrentDataIfExists(
		sourceLanguageInput,
		normalizeLanguageSetting(
			root.value(QStringLiteral("sourceLanguage"))
				.toString(PluginConfig::getValue(QString::fromLatin1(CONFIG_OPUS_SOURCE_LANGUAGE),
								 QString::fromLatin1(LANGUAGE_AUTO)))));
	setComboCurrentDataIfExists(transcriptionLanguageInput,
				    normalizeLanguageSetting(root.value(QStringLiteral("transcriptionLanguage"))
								     .toString(QString::fromLatin1(LANGUAGE_AUTO))));

	if (skipCurateInput && root.contains(QStringLiteral("skipCurate")))
		skipCurateInput->setChecked(root.value(QStringLiteral("skipCurate")).toBool(false));

	if (customPromptInput && root.contains(QStringLiteral("aiPrompt")))
		customPromptInput->setPlainText(root.value(QStringLiteral("aiPrompt")).toString().trimmed());
}

void UploadReviewDialog::saveCurationOptions() const
{
	const QString key = currentReviewSettingsKey();
	if (key.isEmpty())
		return;

	QJsonObject root;
	root.insert(QStringLiteral("videoFileName"), QFileInfo(videoPath).fileName());
	root.insert(QStringLiteral("videoPath"), videoPath);
	root.insert(QStringLiteral("reviewSettingsKey"), key);
	root.insert(QStringLiteral("originalVideoDurationSec"),
		    advancedSettingsOnly ? initialSettings.originalVideoDurationSec
					 : (videoEditor ? videoEditor->durationMilliseconds() / 1000.0 : 0.0));

	QJsonArray ranges;
	const QVector<ClipDuration> currentRanges =
		advancedSettingsOnly ? initialSettings.clipDurations
				     : (videoEditor ? videoEditor->clipRanges() : QVector<ClipDuration>{});
	for (const ClipDuration &range : currentRanges) {
		QJsonObject item;
		item.insert(QStringLiteral("start"), range.startSec);
		item.insert(QStringLiteral("end"), range.endSec);
		ranges.append(item);
	}
	root.insert(QStringLiteral("clipDurations"), ranges);

	QJsonArray topicKeywords;
	if (topicKeywordsInput) {
		const QStringList keywords = topicKeywordsInput->text().split(QStringLiteral(","), Qt::SkipEmptyParts);
		for (QString keyword : keywords) {
			keyword = keyword.trimmed();
			if (!keyword.isEmpty())
				topicKeywords.append(keyword);
		}
	}
	root.insert(QStringLiteral("topicKeywords"), topicKeywords);
	root.insert(QStringLiteral("genre"), genreInput ? genreInput->currentText() : QStringLiteral("Auto"));
	root.insert(QStringLiteral("curationPreset"),
		    curationPresetInput ? curationPresetInput->currentData().toString() : QStringLiteral("auto"));
	root.insert(QStringLiteral("model"), modelInput ? modelInput->currentText() : QStringLiteral("ClipAnything"));
	root.insert(QStringLiteral("clipLengthPreset"),
		    clipLengthInput ? clipLengthInput->currentData().toString() : QStringLiteral("Medium"));
	root.insert(QStringLiteral("skipCurate"), skipCurateInput ? skipCurateInput->isChecked() : false);
	root.insert(QStringLiteral("sourceLanguage"),
		    sourceLanguageInput ? normalizeLanguageSetting(sourceLanguageInput->currentData().toString())
					: QString::fromLatin1(LANGUAGE_AUTO));
	root.insert(QStringLiteral("transcriptionLanguage"),
		    transcriptionLanguageInput
			    ? normalizeLanguageSetting(transcriptionLanguageInput->currentData().toString())
			    : QString::fromLatin1(LANGUAGE_AUTO));
	root.insert(QStringLiteral("aiPrompt"),
		    customPromptInput ? customPromptInput->toPlainText().trimmed() : QString{});

	PluginConfig::setValue(key, QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

QString UploadReviewDialog::currentReviewSettingsKey() const
{
	if (advancedSettingsOnly && !initialSettings.reviewSettingsKey.trimmed().isEmpty())
		return initialSettings.reviewSettingsKey;

	const QString rangeKey =
		advancedSettingsOnly
			? reviewRangeKeyFromRanges(initialSettings.clipDurations)
			: reviewRangeKeyFromMarkers(videoEditor ? videoEditor->markerPositions() : QVector<double>{});
	return reviewSettingsConfigKey(videoPath, rangeKey);
}

CurationSettings UploadReviewDialog::curationSettings() const
{
	CurationSettings settings = advancedSettingsOnly ? initialSettings : CurationSettings{};

	const QVector<ClipDuration> ranges =
		advancedSettingsOnly ? initialSettings.clipDurations
				     : (videoEditor ? videoEditor->clipRanges() : QVector<ClipDuration>{});

	settings.reviewSettingsKey = currentReviewSettingsKey();
	settings.originalVideoDurationSec =
		advancedSettingsOnly ? initialSettings.originalVideoDurationSec
				     : (videoEditor ? videoEditor->durationMilliseconds() / 1000.0 : 0.0);
	settings.rangeStartSec = ranges.isEmpty() ? settings.rangeStartSec : ranges.first().startSec;
	settings.rangeEndSec = ranges.isEmpty() ? settings.rangeEndSec : ranges.last().endSec;
	settings.clipDurations.clear();
	for (const auto &range : ranges)
		settings.clipDurations.append(range);

	settings.genre = genreInput->currentText();
	settings.curationPreset = curationPresetInput ? curationPresetInput->currentData().toString()
						      : QStringLiteral("auto");
	settings.skipCurate = skipCurateInput->isChecked();
	settings.model = modelInput->currentText();
	settings.clipLengthPreset = clipLengthInput ? clipLengthInput->currentData().toString()
						    : QStringLiteral("Medium");
	settings.sourceLanguage = sourceLanguageInput
					  ? normalizeLanguageSetting(sourceLanguageInput->currentData().toString())
					  : QString::fromLatin1(LANGUAGE_AUTO);
	settings.transcriptionLanguage =
		transcriptionLanguageInput
			? normalizeLanguageSetting(transcriptionLanguageInput->currentData().toString())
			: QString::fromLatin1(LANGUAGE_AUTO);
	settings.aiPrompt = customPromptInput ? customPromptInput->toPlainText().trimmed() : QString{};

	settings.topicKeywords.clear();
	const QStringList keywords = topicKeywordsInput->text().split(",", Qt::SkipEmptyParts);
	for (QString keyword : keywords)
		settings.topicKeywords.append(keyword.trimmed());

	return settings;
}
