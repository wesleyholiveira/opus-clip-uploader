#include "ui/ui.hpp"

#include "ui/ui-common.hpp"

#include "gpt/gpt-prompt-client.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <utils/config.hpp>

#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSize>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

static const QString &title = clipCropperTitle();
static constexpr const char *CONFIG_UPLOAD_RESAMPLE_THRESHOLD_PERCENT = "upload_resample_threshold_percent";
static constexpr const char *CONFIG_GPT_TRANSCRIPT_CONTEXT_PADDING_SEC = "gpt_transcript_context_padding_sec";

static QString transcription_cache_directory_path()
{
	QString baseDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	if (baseDir.trimmed().isEmpty())
		baseDir = QDir::tempPath() + QStringLiteral("/clip-cropper");

	return QDir(baseDir).filePath(QStringLiteral("transcription-cache"));
}

static bool purge_transcription_cache_directory()
{
	QDir dir(transcription_cache_directory_path());
	if (!dir.exists())
		return true;

	const bool removed = dir.removeRecursively();
	QDir().mkpath(dir.absolutePath());
	return removed;
}

static int purge_plugin_cache_config_keys()
{
	return PluginConfig::removeValuesWithPrefixes(QStringList{
		QStringLiteral("video_markers."),
		QStringLiteral("review.settings."),
		QStringLiteral("video_transcript."),
		QStringLiteral("video_transcript_range."),
		QStringLiteral("gpt_prompt."),
		QStringLiteral("gpt_prompt_input."),
		QStringLiteral("gpt_prompt_pending."),
	});
}
static void set_combo_current_data(QComboBox *combo, const QString &value, int fallbackIndex = 0)
{
	const int index = combo->findData(value);
	combo->setCurrentIndex(index >= 0 ? index : fallbackIndex);
}

void open_settings_impl(void *private_data)
{
	UNUSED_PARAMETER(private_data);

	QWidget *parent = reinterpret_cast<QWidget *>(obs_frontend_get_main_window());

	QDialog dialog(parent);
	dialog.setWindowTitle(title);
	dialog.resize(820, 620);

	QVBoxLayout *mainLayout = new QVBoxLayout(&dialog);
	mainLayout->setContentsMargins(20, 20, 20, 12);
	mainLayout->setSpacing(12);

	QFormLayout *formLayout = new QFormLayout();
	formLayout->setLabelAlignment(Qt::AlignLeft);

	QLineEdit *apiKeyInput = new QLineEdit(&dialog);
	apiKeyInput->setEchoMode(QLineEdit::Password);
	apiKeyInput->setPlaceholderText("Opus Clip API Key");
	apiKeyInput->setText(PluginConfig::getValue("opus_api_key"));

	formLayout->addRow(obsText("Settings.OpusApiKey"), apiKeyInput);

	QLineEdit *openAiApiKeyInput = new QLineEdit(&dialog);
	openAiApiKeyInput->setEchoMode(QLineEdit::Password);
	openAiApiKeyInput->setPlaceholderText("OpenAI API Key");
	openAiApiKeyInput->setText(PluginConfig::getValue("openai_api_key"));
	formLayout->addRow(obsText("Settings.OpenAiApiKey"), openAiApiKeyInput);

	QComboBox *whisperModelInput = new QComboBox(&dialog);
	whisperModelInput->addItem(obsText("WhisperModel.Tiny"), QStringLiteral("ggml-tiny.bin"));
	whisperModelInput->addItem(obsText("WhisperModel.Base"), QStringLiteral("ggml-base.bin"));
	whisperModelInput->addItem(obsText("WhisperModel.Small"), QStringLiteral("ggml-small.bin"));
	whisperModelInput->addItem(obsText("WhisperModel.Medium"), QStringLiteral("ggml-medium.bin"));
	whisperModelInput->addItem(obsText("WhisperModel.LargeV3"), QStringLiteral("ggml-large-v3.bin"));
	set_combo_current_data(whisperModelInput, PluginConfig::getValue("whisper_model_file", "ggml-base.bin"), 1);
	formLayout->addRow(obsText("Settings.WhisperModel"), whisperModelInput);

	QTreeWidget *treeWidget = new QTreeWidget(&dialog);
	treeWidget->setColumnCount(2);
	treeWidget->setHeaderHidden(true);
	treeWidget->setRootIsDecorated(true);
	treeWidget->setItemsExpandable(true);
	treeWidget->setAnimated(true);
	treeWidget->setMinimumHeight(330);
	treeWidget->setFrameShape(QFrame::NoFrame);
	treeWidget->setAutoFillBackground(false);
	treeWidget->setAttribute(Qt::WA_TranslucentBackground);
	treeWidget->viewport()->setAutoFillBackground(false);
	treeWidget->viewport()->setAttribute(Qt::WA_TranslucentBackground);

	treeWidget->setStyleSheet(R"(
	QTreeWidget {
		background: transparent;
		border: none;
	}
	QTreeWidget::viewport {
		background: transparent;
	}
	QTreeWidget::item {
		background: transparent;
	}
)");

	auto *advancedItem = new QTreeWidgetItem();
	advancedItem->setText(0, obsText("AdvancedSettings"));
	advancedItem->setExpanded(false);
	treeWidget->addTopLevelItem(advancedItem);

	auto *brandItem = new QTreeWidgetItem(advancedItem);
	brandItem->setText(0, obsText("Settings.BrandTemplateId"));

	QLineEdit *brandTemplateIdInput = new QLineEdit(treeWidget);
	brandTemplateIdInput->setPlaceholderText("Brand Template ID");
	brandTemplateIdInput->setText(PluginConfig::getValue("opus_brand_template_id"));
	treeWidget->setItemWidget(brandItem, 1, brandTemplateIdInput);

	auto *resampleThresholdItem = new QTreeWidgetItem(advancedItem);
	resampleThresholdItem->setText(0, obsText("Settings.UploadResampleThresholdPercent"));

	QDoubleSpinBox *resampleThresholdInput = new QDoubleSpinBox(treeWidget);
	resampleThresholdInput->setRange(0.0, 100.0);
	resampleThresholdInput->setDecimals(0);
	resampleThresholdInput->setSingleStep(5.0);
	resampleThresholdInput->setSuffix(QStringLiteral(" %"));
	bool resampleThresholdOk = false;
	const double savedResampleThreshold =
		PluginConfig::getValue(QString::fromLatin1(CONFIG_UPLOAD_RESAMPLE_THRESHOLD_PERCENT),
				       QStringLiteral("60"))
			.toDouble(&resampleThresholdOk);
	resampleThresholdInput->setValue(resampleThresholdOk ? savedResampleThreshold : 60.0);
	resampleThresholdInput->setToolTip(obsText("Tooltip.UploadResampleThresholdPercent"));
	treeWidget->setItemWidget(resampleThresholdItem, 1, resampleThresholdInput);

	auto *gptContextPaddingItem = new QTreeWidgetItem(advancedItem);
	gptContextPaddingItem->setText(0, obsText("Settings.GptTranscriptContextPaddingSec"));

	QDoubleSpinBox *gptContextPaddingInput = new QDoubleSpinBox(treeWidget);
	gptContextPaddingInput->setRange(0.0, 600.0);
	gptContextPaddingInput->setDecimals(0);
	gptContextPaddingInput->setSingleStep(15.0);
	gptContextPaddingInput->setSuffix(QStringLiteral(" s"));
	bool gptContextPaddingOk = false;
	const double savedGptContextPadding =
		PluginConfig::getValue(QString::fromLatin1(CONFIG_GPT_TRANSCRIPT_CONTEXT_PADDING_SEC),
				       QStringLiteral("60"))
			.toDouble(&gptContextPaddingOk);
	gptContextPaddingInput->setValue(gptContextPaddingOk ? savedGptContextPadding : 60.0);
	gptContextPaddingInput->setToolTip(obsText("Tooltip.GptTranscriptContextPaddingSec"));
	treeWidget->setItemWidget(gptContextPaddingItem, 1, gptContextPaddingInput);

	auto *purgeCachesItem = new QTreeWidgetItem(advancedItem);
	purgeCachesItem->setText(0, obsText("Settings.PurgePluginCaches"));

	QPushButton *purgeCachesButton = new QPushButton(obsText("Button.PurgePluginCaches"), treeWidget);
	purgeCachesButton->setToolTip(obsText("Tooltip.PurgePluginCaches"));
	treeWidget->setItemWidget(purgeCachesItem, 1, purgeCachesButton);

	QObject::connect(purgeCachesButton, &QPushButton::clicked, &dialog, [&dialog]() {
		const QMessageBox::StandardButton answer =
			QMessageBox::question(&dialog, title, obsText("Message.PurgePluginCachesConfirm"),
					      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

		if (answer != QMessageBox::Yes)
			return;

		const int removedKeys = purge_plugin_cache_config_keys();
		const bool removedTempFiles = purge_transcription_cache_directory();

		if (!removedTempFiles) {
			QMessageBox::warning(&dialog, title,
					     obsText("Message.PurgePluginCachesPartialFailure").arg(removedKeys));
			return;
		}

		QMessageBox::information(&dialog, title, obsText("Message.PurgePluginCachesDone").arg(removedKeys));
	});

	auto *openAiModelItem = new QTreeWidgetItem(advancedItem);
	openAiModelItem->setText(0, obsText("Settings.OpenAiModel"));

	QComboBox *openAiModelInput = new QComboBox(treeWidget);
	openAiModelInput->addItem(QStringLiteral("GPT-5.4 mini"), QStringLiteral("gpt-5.4-mini"));
	openAiModelInput->addItem(QStringLiteral("GPT-5.4 nano"), QStringLiteral("gpt-5.4-nano"));
	openAiModelInput->addItem(QStringLiteral("GPT-5.5"), QStringLiteral("gpt-5.5"));
	openAiModelInput->addItem(QStringLiteral("GPT-5.4"), QStringLiteral("gpt-5.4"));
	openAiModelInput->addItem(QStringLiteral("GPT-5.2"), QStringLiteral("gpt-5.2"));
	openAiModelInput->addItem(QStringLiteral("GPT-5 mini"), QStringLiteral("gpt-5-mini"));
	openAiModelInput->addItem(QStringLiteral("GPT-5"), QStringLiteral("gpt-5"));
	openAiModelInput->addItem(QStringLiteral("GPT-4.1 mini"), QStringLiteral("gpt-4.1-mini"));
	openAiModelInput->addItem(obsText("Combobox.Disabled"), OPENAI_MODEL_DISABLED);

	set_combo_current_data(openAiModelInput, PluginConfig::getValue("openai_model", OPENAI_MODEL_DISABLED), 0);
	treeWidget->setItemWidget(openAiModelItem, 1, openAiModelInput);

	auto *openAiModelStatusItem = new QTreeWidgetItem(advancedItem);
	openAiModelStatusItem->setText(0, obsText("Settings.OpenAiModelStatus"));

	QLabel *openAiModelStatusLabel = new QLabel(treeWidget);
	openAiModelStatusLabel->setWordWrap(true);
	treeWidget->setItemWidget(openAiModelStatusItem, 1, openAiModelStatusLabel);

	auto updateOpenAiModelAvailability = [whisperModelInput, openAiModelInput, openAiModelStatusLabel]() {
		const QString selectedModelFile = whisperModelInput->currentData().toString().trimmed();
		const bool modelExists = whisper_model_exists(selectedModelFile);

		if (!modelExists) {
			const QSignalBlocker blocker(openAiModelInput);
			set_combo_current_data(openAiModelInput, OPENAI_MODEL_DISABLED, openAiModelInput->count() - 1);
			openAiModelInput->setEnabled(false);
			openAiModelStatusLabel->setText(
				obsText("Status.OpenAiModelDisabledMissingWhisper").arg(selectedModelFile));
			return;
		}

		openAiModelInput->setEnabled(true);
		openAiModelStatusLabel->setText(obsText("Status.OpenAiModelEnabledWhisperFound"));
	};

	QObject::connect(whisperModelInput, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
			 [&updateOpenAiModelAvailability](int) { updateOpenAiModelAvailability(); });
	updateOpenAiModelAvailability();

	auto *gptDefaultPromptItem = new QTreeWidgetItem(advancedItem);
	gptDefaultPromptItem->setText(0, obsText("Settings.GptInputTemplate"));
	gptDefaultPromptItem->setSizeHint(0, QSize(220, 180));
	gptDefaultPromptItem->setSizeHint(1, QSize(520, 180));

	QPlainTextEdit *gptDefaultPromptInput = new QPlainTextEdit(treeWidget);
	gptDefaultPromptInput->setMinimumHeight(170);
	gptDefaultPromptInput->setPlaceholderText(obsText("Placeholder.GptInputTemplate"));
	gptDefaultPromptInput->setPlainText(GptPromptClient::configuredInputTextTemplate(QStringLiteral("auto")));
	treeWidget->setItemWidget(gptDefaultPromptItem, 1, gptDefaultPromptInput);

	treeWidget->resizeColumnToContents(0);

	QPushButton *btn = new QPushButton(obsText("Button.Save"), &dialog);

	mainLayout->addLayout(formLayout);
	mainLayout->addWidget(treeWidget);
	mainLayout->addWidget(btn);
	mainLayout->addStretch();

	QObject::connect(treeWidget, &QTreeWidget::itemClicked, [advancedItem](QTreeWidgetItem *item, int column) {
		Q_UNUSED(column);

		if (item == advancedItem)
			advancedItem->setExpanded(!advancedItem->isExpanded());
	});

	QObject::connect(
		btn, &QPushButton::clicked,
		[&dialog, apiKeyInput, openAiApiKeyInput, whisperModelInput, brandTemplateIdInput,
		 resampleThresholdInput, gptContextPaddingInput, openAiModelInput, gptDefaultPromptInput]() {
			PluginConfig::setValue("opus_api_key", apiKeyInput->text().trimmed());
			PluginConfig::setValue("opus_brand_template_id", brandTemplateIdInput->text().trimmed());
			PluginConfig::setValue("openai_api_key", openAiApiKeyInput->text().trimmed());
			const QString selectedWhisperModel = whisperModelInput->currentData().toString().trimmed();
			PluginConfig::setValue("whisper_model_file", selectedWhisperModel);

			const bool selectedWhisperModelExists = whisper_model_exists(selectedWhisperModel);
			QString openAiModel = openAiModelInput->currentData().toString().trimmed();
			if (openAiModel.isEmpty())
				openAiModel = QStringLiteral("disabled");

			if (!selectedWhisperModelExists) {
				openAiModel = QStringLiteral("disabled");
				blog(LOG_WARNING,
				     "[clip-cropper] Selected Whisper model was not found. Saving OpenAI model as disabled.");
			}

			PluginConfig::setValue("openai_model", openAiModel);
			PluginConfig::setValue(GptPromptClient::inputTemplateConfigKey(QStringLiteral("auto")),
					       gptDefaultPromptInput->toPlainText().trimmed());
			PluginConfig::setValue(QString::fromLatin1(CONFIG_UPLOAD_RESAMPLE_THRESHOLD_PERCENT),
					       QString::number(resampleThresholdInput->value(), 'f', 0));
			PluginConfig::setValue(QString::fromLatin1(CONFIG_GPT_TRANSCRIPT_CONTEXT_PADDING_SEC),
					       QString::number(gptContextPaddingInput->value(), 'f', 0));

			blog(LOG_INFO, "Clip Cropper settings saved. Opus Clip settings updated.");

			dialog.accept();
		});

	dialog.exec();
}

void ensure_opus_api_key_impl(QWidget *parent)
{
	const QString apiKey = get_opus_api_key();

	if (!apiKey.isEmpty()) {
		blog(LOG_INFO, "Opus Clip API key already configured");
		return;
	}

	QMessageBox::information(parent, title, obsText("Message.ConfigureApiKeyBeforeCuts"));

	open_settings(nullptr);
}
