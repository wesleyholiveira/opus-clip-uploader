#include "ui/upload-review-dialog.hpp"

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

#include "ui/advanced-settings-tree.hpp"
#include "ui/video-marker-editor.hpp"
#include "utils/config.hpp"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>

static constexpr const char *CONFIG_REVIEW_TOPIC_KEYWORDS = "review.topic_keywords";
static constexpr const char *CONFIG_REVIEW_GENRE = "review.genre";
static constexpr const char *CONFIG_REVIEW_MODEL = "review.model";
static constexpr const char *CONFIG_REVIEW_SKIP_CURATE = "review.skip_curate";

static QString obsText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

static void setComboCurrentTextIfExists(QComboBox *combo, const QString &value)
{
	if (!combo || value.trimmed().isEmpty())
		return;

	const int index = combo->findText(value, Qt::MatchFixedString);
	if (index >= 0)
		combo->setCurrentIndex(index);
}

UploadReviewDialog::UploadReviewDialog(const QString &videoPath, QWidget *parent) : QDialog(parent), videoPath(videoPath)
{
	setWindowTitle(QString("Clip Cropper - %1").arg(obsText("Dialog.ReviewVideoTitle")));
	resize(920, 720);

	auto *mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(14, 14, 14, 12);
	mainLayout->setSpacing(8);

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

	connect(videoEditor, &VideoMarkerEditor::rangesChanged, this, &UploadReviewDialog::refreshClipTable);
	connect(clipTable, &QTableWidget::currentCellChanged, this, [this](int currentRow, int, int, int) {
		videoEditor->selectRange(currentRow);
	});
	connect(clipTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
		videoEditor->selectRange(row);
		videoEditor->goToSelectedRange();
	});

	topicKeywordsInput = new QLineEdit(this);
	topicKeywordsInput->setPlaceholderText(obsText("Placeholder.TopicKeywords"));

	modelInput = new QComboBox(this);
	modelInput->addItems(QStringList{"ClipBasic", "ClipAnything"});

	genreInput = new QComboBox(this);
	genreInput->addItems(QStringList{"Auto", "Q&A", "Commentary", "Marketing", "Webinar", "Motivational speech",
					 "Podcast", "Academic", "Listicle", "Product reviews", "How-to", "Comedy",
					 "Sports commentary", "Church", "News", "Vlog", "Gaming", "Others"});

	skipCurateInput = new QCheckBox(obsText("Label.SkipCurate"), this);

	auto *advancedTree = new AdvancedSettingsTree(this);
	advancedTree->addField(obsText("Settings.Model"), modelInput);
	advancedTree->addField(obsText("Settings.TopicKeywords"), topicKeywordsInput);
	advancedTree->addField(obsText("Settings.Genre"), genreInput);
	advancedTree->addField(obsText("Settings.SkipCurate"), skipCurateInput);

	loadSavedCurationOptions();

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
		saveCurationOptions();
		accept();
	});
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	mainLayout->addWidget(new QLabel(QFileInfo(videoPath).fileName(), this));
	mainLayout->addWidget(videoEditor, 1);
	mainLayout->addWidget(new QLabel(obsText("Label.Markers"), this));
	mainLayout->addWidget(clipTable);
	mainLayout->addWidget(advancedTree);
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
		clipTable->setItem(
			row, 0,
			new QTableWidgetItem(QString("#%1  %2 → %3  (%4s)")
						     .arg(i + 1)
						     .arg(videoEditor->formatTimecode(range.startSec),
							  videoEditor->formatTimecode(range.endSec))
						     .arg(selectedSec, 0, 'f', 0)));
		clipTable->setItem(row, 1, new QTableWidgetItem(QString::number(range.startSec, 'f', 2)));
		clipTable->setItem(row, 2, new QTableWidgetItem(QString::number(range.endSec, 'f', 2)));
	}

	if (clipTable->rowCount() <= 0)
		return;

	const int rowToSelect = std::clamp(previousRow, 0, clipTable->rowCount() - 1);
	clipTable->selectRow(rowToSelect);
	videoEditor->selectRange(rowToSelect);
}

void UploadReviewDialog::loadSavedCurationOptions()
{
	const QString savedTopicKeywords = PluginConfig::getValue(CONFIG_REVIEW_TOPIC_KEYWORDS).trimmed();
	if (!savedTopicKeywords.isEmpty())
		topicKeywordsInput->setText(savedTopicKeywords);

	setComboCurrentTextIfExists(genreInput, PluginConfig::getValue(CONFIG_REVIEW_GENRE));
	setComboCurrentTextIfExists(modelInput, PluginConfig::getValue(CONFIG_REVIEW_MODEL));

	const QString savedSkipCurate = PluginConfig::getValue(CONFIG_REVIEW_SKIP_CURATE).trimmed().toLower();
	if (!savedSkipCurate.isEmpty())
		skipCurateInput->setChecked(savedSkipCurate == "true" || savedSkipCurate == "1" ||
					    savedSkipCurate == "yes");
}

void UploadReviewDialog::saveCurationOptions() const
{
	PluginConfig::setValue(CONFIG_REVIEW_TOPIC_KEYWORDS, topicKeywordsInput->text().trimmed());
	PluginConfig::setValue(CONFIG_REVIEW_GENRE, genreInput->currentText());
	PluginConfig::setValue(CONFIG_REVIEW_MODEL, modelInput->currentText());
	PluginConfig::setValue(CONFIG_REVIEW_SKIP_CURATE, skipCurateInput->isChecked() ? "true" : "false");
}

CurationSettings UploadReviewDialog::curationSettings() const
{
	CurationSettings settings;

	const QVector<ClipDuration> ranges = videoEditor ? videoEditor->clipRanges() : QVector<ClipDuration>{};

	settings.rangeStartSec = ranges.isEmpty() ? 0 : ranges.first().startSec;
	settings.rangeEndSec = ranges.isEmpty() ? 0 : ranges.last().endSec;
	settings.genre = genreInput->currentText();
	settings.skipCurate = skipCurateInput->isChecked();
	settings.model = modelInput->currentText();

	const QStringList keywords = topicKeywordsInput->text().split(",", Qt::SkipEmptyParts);
	for (QString keyword : keywords)
		settings.topicKeywords.append(keyword.trimmed());

	for (const auto &range : ranges)
		settings.clipDurations.append(range);

	return settings;
}
