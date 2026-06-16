#include "ui/ui.hpp"

#include "ui/ui-common.hpp"

#include "gpt/gpt-prompt-client.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <utils/config.hpp>

#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSize>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

static const QString &title = clipCropperTitle();
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

	auto *sourceLangItem = new QTreeWidgetItem(advancedItem);
	sourceLangItem->setText(0, obsText("Settings.SourceLanguage"));

	QComboBox *sourceLangInput = new QComboBox(treeWidget);
	sourceLangInput->addItems(QStringList{"auto", "pt", "en"});

	const QString savedSourceLang = PluginConfig::getValue("opus_source_lang", "auto");
	const int savedSourceLangIndex = sourceLangInput->findText(savedSourceLang);
	sourceLangInput->setCurrentIndex(savedSourceLangIndex >= 0 ? savedSourceLangIndex : 0);

	treeWidget->setItemWidget(sourceLangItem, 1, sourceLangInput);

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
	gptDefaultPromptInput->setPlainText(GptPromptClient::configuredInputTextTemplate());
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
		[&dialog, apiKeyInput, openAiApiKeyInput, whisperModelInput, brandTemplateIdInput, sourceLangInput,
		 openAiModelInput, gptDefaultPromptInput]() {
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
				obs_log(LOG_WARNING,
					"[clip-cropper] Selected Whisper model was not found. Saving OpenAI model as disabled.");
			}

			PluginConfig::setValue("openai_model", openAiModel);
			PluginConfig::setValue(GptPromptClient::inputTemplateConfigKey(),
					       gptDefaultPromptInput->toPlainText().trimmed());

			const QString sourceLang = sourceLangInput->currentText().trimmed();
			PluginConfig::setValue("opus_source_lang", sourceLang.isEmpty() ? "auto" : sourceLang);

			obs_log(LOG_INFO, "Clip Cropper settings saved. Opus Clip settings updated.");

			dialog.accept();
		});

	dialog.exec();
}

void ensure_opus_api_key_impl(QWidget *parent)
{
	const QString apiKey = get_opus_api_key();

	if (!apiKey.isEmpty()) {
		obs_log(LOG_INFO, "Opus Clip API key already configured");
		return;
	}

	QMessageBox::information(parent, title, obsText("Message.ConfigureApiKeyBeforeCuts"));

	open_settings(nullptr);
}
