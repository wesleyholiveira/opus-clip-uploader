#include "ui/ui.hpp"

#include "ui/ui-common.hpp"

#include "curation/scoring/semantic-embedding-settings.hpp"
#include "transcription/whisperx-settings.hpp"

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
#include <QSize>
#include <QSpinBox>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

static const QString &title = clipCropperTitle();
static constexpr const char *CONFIG_UPLOAD_RESAMPLE_THRESHOLD_PERCENT = "upload_resample_threshold_percent";

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
		QStringLiteral("video_transcript_v3."),
		QStringLiteral("video_transcript_range."),
		QStringLiteral("video_transcript_range_v3."),
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

	auto *whisperXBackendItem = new QTreeWidgetItem(advancedItem);
	whisperXBackendItem->setText(0, obsText("Settings.WhisperXBackend"));

	QComboBox *whisperXBackendInput = new QComboBox(treeWidget);
	whisperXBackendInput->addItem(obsText("Combobox.Disabled"),
					 QString::fromLatin1(Transcription::WHISPERX_BACKEND_DISABLED));
	whisperXBackendInput->addItem(obsText("Combobox.WhisperXAlignmentOnly"),
					 QString::fromLatin1(Transcription::WHISPERX_BACKEND_ALIGNMENT));
	whisperXBackendInput->addItem(obsText("Combobox.WhisperXPrimary"),
					 QString::fromLatin1(Transcription::WHISPERX_BACKEND_PRIMARY));
	set_combo_current_data(whisperXBackendInput,
		PluginConfig::getValue(QString::fromLatin1(Transcription::CONFIG_WHISPERX_BACKEND),
			QString::fromLatin1(Transcription::WHISPERX_BACKEND_DISABLED)), 0);
	treeWidget->setItemWidget(whisperXBackendItem, 1, whisperXBackendInput);

	auto *whisperXPythonPathItem = new QTreeWidgetItem(advancedItem);
	whisperXPythonPathItem->setText(0, obsText("Settings.WhisperXPythonPath"));

	QLineEdit *whisperXPythonPathInput = new QLineEdit(treeWidget);
	whisperXPythonPathInput->setPlaceholderText(QStringLiteral("C:/path/to/.venv-whisperx/Scripts/python.exe"));
	whisperXPythonPathInput->setText(PluginConfig::getValue(
		QString::fromLatin1(Transcription::CONFIG_WHISPERX_PYTHON_PATH)));
	treeWidget->setItemWidget(whisperXPythonPathItem, 1, whisperXPythonPathInput);

	auto *whisperXWorkerPathItem = new QTreeWidgetItem(advancedItem);
	whisperXWorkerPathItem->setText(0, obsText("Settings.WhisperXWorkerPath"));

	QLineEdit *whisperXWorkerPathInput = new QLineEdit(treeWidget);
	whisperXWorkerPathInput->setPlaceholderText(Transcription::defaultWhisperXWorkerPath());
	whisperXWorkerPathInput->setText(PluginConfig::getValue(
		QString::fromLatin1(Transcription::CONFIG_WHISPERX_WORKER_PATH),
		Transcription::defaultWhisperXWorkerPath()));
	treeWidget->setItemWidget(whisperXWorkerPathItem, 1, whisperXWorkerPathInput);

	auto *whisperXDeviceItem = new QTreeWidgetItem(advancedItem);
	whisperXDeviceItem->setText(0, obsText("Settings.WhisperXDevice"));

	QComboBox *whisperXDeviceInput = new QComboBox(treeWidget);
	whisperXDeviceInput->addItem(QStringLiteral("CUDA/GPU"), QStringLiteral("cuda"));
	whisperXDeviceInput->addItem(QStringLiteral("CPU"), QStringLiteral("cpu"));
	set_combo_current_data(whisperXDeviceInput, PluginConfig::getValue(
		QString::fromLatin1(Transcription::CONFIG_WHISPERX_DEVICE), QStringLiteral("cuda")), 0);
	treeWidget->setItemWidget(whisperXDeviceItem, 1, whisperXDeviceInput);

	auto *whisperXFfmpegPathItem = new QTreeWidgetItem(advancedItem);
	whisperXFfmpegPathItem->setText(0, obsText("Settings.WhisperXFfmpegPath"));

	QLineEdit *whisperXFfmpegPathInput = new QLineEdit(treeWidget);
	whisperXFfmpegPathInput->setPlaceholderText(QStringLiteral("Optional ffmpeg.exe or ffmpeg bin directory"));
	whisperXFfmpegPathInput->setText(PluginConfig::getValue(
		QString::fromLatin1(Transcription::CONFIG_WHISPERX_FFMPEG_PATH)));
	treeWidget->setItemWidget(whisperXFfmpegPathItem, 1, whisperXFfmpegPathInput);

	auto *whisperXModelItem = new QTreeWidgetItem(advancedItem);
	whisperXModelItem->setText(0, obsText("Settings.WhisperXModel"));

	QLineEdit *whisperXModelInput = new QLineEdit(treeWidget);
	whisperXModelInput->setPlaceholderText(QStringLiteral("large-v3"));
	whisperXModelInput->setText(PluginConfig::getValue(
		QString::fromLatin1(Transcription::CONFIG_WHISPERX_MODEL), QStringLiteral("large-v3")));
	treeWidget->setItemWidget(whisperXModelItem, 1, whisperXModelInput);

	auto *whisperXComputeTypeItem = new QTreeWidgetItem(advancedItem);
	whisperXComputeTypeItem->setText(0, obsText("Settings.WhisperXComputeType"));

	QComboBox *whisperXComputeTypeInput = new QComboBox(treeWidget);
	whisperXComputeTypeInput->addItem(QStringLiteral("float16"), QStringLiteral("float16"));
	whisperXComputeTypeInput->addItem(QStringLiteral("int8_float16"), QStringLiteral("int8_float16"));
	whisperXComputeTypeInput->addItem(QStringLiteral("int8"), QStringLiteral("int8"));
	whisperXComputeTypeInput->addItem(QStringLiteral("float32"), QStringLiteral("float32"));
	set_combo_current_data(whisperXComputeTypeInput, PluginConfig::getValue(
		QString::fromLatin1(Transcription::CONFIG_WHISPERX_COMPUTE_TYPE), QStringLiteral("float16")), 0);
	treeWidget->setItemWidget(whisperXComputeTypeItem, 1, whisperXComputeTypeInput);

	auto *whisperXBatchSizeItem = new QTreeWidgetItem(advancedItem);
	whisperXBatchSizeItem->setText(0, obsText("Settings.WhisperXBatchSize"));

	QSpinBox *whisperXBatchSizeInput = new QSpinBox(treeWidget);
	whisperXBatchSizeInput->setRange(1, 128);
	whisperXBatchSizeInput->setValue(PluginConfig::getValue(
		QString::fromLatin1(Transcription::CONFIG_WHISPERX_BATCH_SIZE), QStringLiteral("8")).toInt());
	treeWidget->setItemWidget(whisperXBatchSizeItem, 1, whisperXBatchSizeInput);

	auto *localEmbeddingBackendItem = new QTreeWidgetItem(advancedItem);
	localEmbeddingBackendItem->setText(0, obsText("Settings.LocalEmbeddingBackend"));

	QComboBox *localEmbeddingBackendInput = new QComboBox(treeWidget);
	localEmbeddingBackendInput->addItem(QStringLiteral("llama.cpp native (in-process)"),
						 QString::fromLatin1(Curation::Scoring::LOCAL_EMBEDDING_BACKEND_LLAMA_CPP));
	localEmbeddingBackendInput->addItem(QStringLiteral("llama-server HTTP"),
						 QString::fromLatin1(Curation::Scoring::LOCAL_EMBEDDING_BACKEND_LLAMA_SERVER));
	set_combo_current_data(localEmbeddingBackendInput, Curation::Scoring::localEmbeddingBackendFromConfig(), 0);
	treeWidget->setItemWidget(localEmbeddingBackendItem, 1, localEmbeddingBackendInput);

	auto *localEmbeddingEndpointItem = new QTreeWidgetItem(advancedItem);
	localEmbeddingEndpointItem->setText(0, obsText("Settings.LocalEmbeddingEndpoint"));

	QLineEdit *localEmbeddingEndpointInput = new QLineEdit(treeWidget);
	localEmbeddingEndpointInput->setPlaceholderText(QStringLiteral("http://127.0.0.1:8080/v1/embeddings"));
	localEmbeddingEndpointInput->setText(PluginConfig::getValue(
		QString::fromLatin1(Curation::Scoring::CONFIG_LOCAL_EMBEDDING_ENDPOINT),
		QStringLiteral("http://127.0.0.1:8080/v1/embeddings")));
	treeWidget->setItemWidget(localEmbeddingEndpointItem, 1, localEmbeddingEndpointInput);

	auto *localEmbeddingModelItem = new QTreeWidgetItem(advancedItem);
	localEmbeddingModelItem->setText(0, obsText("Settings.LocalEmbeddingModel"));

	QLineEdit *localEmbeddingModelInput = new QLineEdit(treeWidget);
	localEmbeddingModelInput->setPlaceholderText(QStringLiteral("qwen3-embedding-0.6b-q8_0"));
	localEmbeddingModelInput->setText(PluginConfig::getValue(
		QString::fromLatin1(Curation::Scoring::CONFIG_LOCAL_EMBEDDING_MODEL_ID),
		QStringLiteral("qwen3-embedding-0.6b-q8_0")));
	treeWidget->setItemWidget(localEmbeddingModelItem, 1, localEmbeddingModelInput);

	auto *localRerankerBackendItem = new QTreeWidgetItem(advancedItem);
	localRerankerBackendItem->setText(0, obsText("Settings.LocalRerankerBackend"));

	QComboBox *localRerankerBackendInput = new QComboBox(treeWidget);
	localRerankerBackendInput->addItem(QStringLiteral("llama.cpp native (in-process)"),
						 QString::fromLatin1(Curation::Scoring::LOCAL_RERANKER_BACKEND_LLAMA_CPP));
	localRerankerBackendInput->addItem(QStringLiteral("llama-server HTTP /v1/rerank"),
						 QString::fromLatin1(Curation::Scoring::LOCAL_RERANKER_BACKEND_LLAMA_SERVER));
	set_combo_current_data(localRerankerBackendInput, Curation::Scoring::localRerankerBackendFromConfig(), 0);
	treeWidget->setItemWidget(localRerankerBackendItem, 1, localRerankerBackendInput);

	auto *localRerankerEndpointItem = new QTreeWidgetItem(advancedItem);
	localRerankerEndpointItem->setText(0, obsText("Settings.LocalRerankerEndpoint"));

	QLineEdit *localRerankerEndpointInput = new QLineEdit(treeWidget);
	localRerankerEndpointInput->setPlaceholderText(QStringLiteral("http://127.0.0.1:8081/v1/rerank"));
	localRerankerEndpointInput->setText(PluginConfig::getValue(
		QString::fromLatin1(Curation::Scoring::CONFIG_LOCAL_RERANKER_ENDPOINT),
		QStringLiteral("http://127.0.0.1:8081/v1/rerank")));
	treeWidget->setItemWidget(localRerankerEndpointItem, 1, localRerankerEndpointInput);

	auto *localRerankerModelItem = new QTreeWidgetItem(advancedItem);
	localRerankerModelItem->setText(0, obsText("Settings.LocalRerankerModel"));

	QLineEdit *localRerankerModelInput = new QLineEdit(treeWidget);
	localRerankerModelInput->setPlaceholderText(QStringLiteral("qwen3-reranker-0.6b-q8_0"));
	localRerankerModelInput->setText(PluginConfig::getValue(
		QString::fromLatin1(Curation::Scoring::CONFIG_LOCAL_RERANKER_MODEL_ID),
		QStringLiteral("qwen3-reranker-0.6b-q8_0")));
	treeWidget->setItemWidget(localRerankerModelItem, 1, localRerankerModelInput);

	auto updateWhisperXFields = [whisperXBackendInput, whisperXPythonPathInput, whisperXWorkerPathInput,
					   whisperXDeviceInput, whisperXFfmpegPathInput, whisperXModelInput, whisperXComputeTypeInput,
					   whisperXBatchSizeInput]() {
		const bool enabled = whisperXBackendInput->currentData().toString() !=
			QString::fromLatin1(Transcription::WHISPERX_BACKEND_DISABLED);
		whisperXPythonPathInput->setEnabled(enabled);
		whisperXWorkerPathInput->setEnabled(enabled);
		whisperXDeviceInput->setEnabled(enabled);
		whisperXFfmpegPathInput->setEnabled(enabled);
		whisperXModelInput->setEnabled(enabled);
		whisperXComputeTypeInput->setEnabled(enabled);
		whisperXBatchSizeInput->setEnabled(enabled);
	};

	auto updateLocalEmbeddingFields = [localEmbeddingBackendInput, localEmbeddingEndpointInput, localEmbeddingModelInput]() {
		const QString backend = localEmbeddingBackendInput->currentData().toString();
		const bool serverEnabled = backend ==
			QString::fromLatin1(Curation::Scoring::LOCAL_EMBEDDING_BACKEND_LLAMA_SERVER);
		const bool modelEnabled = backend !=
			QString::fromLatin1(Curation::Scoring::LOCAL_EMBEDDING_BACKEND_DISABLED);
		localEmbeddingEndpointInput->setEnabled(serverEnabled);
		localEmbeddingModelInput->setEnabled(modelEnabled);
		localEmbeddingModelInput->setPlaceholderText(serverEnabled ? QStringLiteral("qwen3-embedding-0.6b-q8_0")
								     : QStringLiteral("qwen3-embedding-0.6b-q8_0.gguf or full GGUF path"));
	};

	auto updateLocalRerankerFields = [localRerankerBackendInput, localRerankerEndpointInput, localRerankerModelInput]() {
		const QString backend = localRerankerBackendInput->currentData().toString();
		const bool serverEnabled = backend ==
			QString::fromLatin1(Curation::Scoring::LOCAL_RERANKER_BACKEND_LLAMA_SERVER);
		const bool modelEnabled = backend !=
			QString::fromLatin1(Curation::Scoring::LOCAL_RERANKER_BACKEND_DISABLED);
		localRerankerEndpointInput->setEnabled(serverEnabled);
		localRerankerModelInput->setEnabled(modelEnabled);
		localRerankerModelInput->setPlaceholderText(serverEnabled ? QStringLiteral("qwen3-reranker-0.6b-q8_0")
								    : QStringLiteral("qwen3-reranker-0.6b-q8_0.gguf or full GGUF path"));
	};

	QObject::connect(localEmbeddingBackendInput, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
			 [&updateLocalEmbeddingFields](int) { updateLocalEmbeddingFields(); });
	QObject::connect(localRerankerBackendInput, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
			 [&updateLocalRerankerFields](int) { updateLocalRerankerFields(); });
	QObject::connect(whisperXBackendInput, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
			 [&updateWhisperXFields](int) { updateWhisperXFields(); });
	updateWhisperXFields();
	updateLocalEmbeddingFields();
	updateLocalRerankerFields();

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
		[&dialog, apiKeyInput, whisperModelInput, brandTemplateIdInput, resampleThresholdInput,
		 whisperXBackendInput, whisperXPythonPathInput, whisperXWorkerPathInput, whisperXDeviceInput,
		 whisperXFfmpegPathInput, whisperXModelInput, whisperXComputeTypeInput, whisperXBatchSizeInput,
		 localEmbeddingBackendInput, localEmbeddingEndpointInput, localEmbeddingModelInput,
		 localRerankerBackendInput, localRerankerEndpointInput, localRerankerModelInput]() {
			PluginConfig::setValue("opus_api_key", apiKeyInput->text().trimmed());
			PluginConfig::setValue("opus_brand_template_id", brandTemplateIdInput->text().trimmed());
			const QString selectedWhisperModel = whisperModelInput->currentData().toString().trimmed();
			PluginConfig::setValue("whisper_model_file", selectedWhisperModel);

			PluginConfig::setValue(QString::fromLatin1(CONFIG_UPLOAD_RESAMPLE_THRESHOLD_PERCENT),
					       QString::number(resampleThresholdInput->value(), 'f', 0));
			PluginConfig::setValue(QString::fromLatin1(Transcription::CONFIG_WHISPERX_BACKEND),
					       whisperXBackendInput->currentData().toString().trimmed());
			PluginConfig::setValue(QString::fromLatin1(Transcription::CONFIG_WHISPERX_PYTHON_PATH),
					       whisperXPythonPathInput->text().trimmed());
			PluginConfig::setValue(QString::fromLatin1(Transcription::CONFIG_WHISPERX_WORKER_PATH),
					       whisperXWorkerPathInput->text().trimmed());
			PluginConfig::setValue(QString::fromLatin1(Transcription::CONFIG_WHISPERX_DEVICE),
					       whisperXDeviceInput->currentData().toString().trimmed());
			PluginConfig::setValue(QString::fromLatin1(Transcription::CONFIG_WHISPERX_FFMPEG_PATH),
					       whisperXFfmpegPathInput->text().trimmed());
			PluginConfig::setValue(QString::fromLatin1(Transcription::CONFIG_WHISPERX_MODEL),
					       whisperXModelInput->text().trimmed());
			PluginConfig::setValue(QString::fromLatin1(Transcription::CONFIG_WHISPERX_COMPUTE_TYPE),
					       whisperXComputeTypeInput->currentData().toString().trimmed());
			PluginConfig::setValue(QString::fromLatin1(Transcription::CONFIG_WHISPERX_BATCH_SIZE),
					       QString::number(whisperXBatchSizeInput->value()));
			PluginConfig::setValue(QString::fromLatin1(Curation::Scoring::CONFIG_LOCAL_EMBEDDING_BACKEND),
					       localEmbeddingBackendInput->currentData().toString().trimmed());
			PluginConfig::setValue(QString::fromLatin1(Curation::Scoring::CONFIG_LOCAL_EMBEDDING_ENDPOINT),
					       localEmbeddingEndpointInput->text().trimmed());
			PluginConfig::setValue(QString::fromLatin1(Curation::Scoring::CONFIG_LOCAL_EMBEDDING_MODEL_ID),
					       localEmbeddingModelInput->text().trimmed());
			PluginConfig::setValue(QString::fromLatin1(Curation::Scoring::CONFIG_LOCAL_RERANKER_BACKEND),
					       localRerankerBackendInput->currentData().toString().trimmed());
			PluginConfig::setValue(QString::fromLatin1(Curation::Scoring::CONFIG_LOCAL_RERANKER_ENDPOINT),
					       localRerankerEndpointInput->text().trimmed());
			PluginConfig::setValue(QString::fromLatin1(Curation::Scoring::CONFIG_LOCAL_RERANKER_MODEL_ID),
					       localRerankerModelInput->text().trimmed());

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
