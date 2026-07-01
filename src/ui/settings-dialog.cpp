#include "ui/ui.hpp"

#include "ui/ui-common.hpp"

#include "curation/scoring/feedback-trained-ranker.hpp"
#include "curation/scoring/semantic-embedding-settings.hpp"
#include "transcription/whisperx-settings.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <utils/config.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSize>
#include <QSpinBox>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardPaths>
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
	dialog.resize(920, 720);

	QVBoxLayout *mainLayout = new QVBoxLayout(&dialog);
	mainLayout->setContentsMargins(20, 20, 20, 12);
	mainLayout->setSpacing(12);

	QFormLayout *formLayout = new QFormLayout();
	formLayout->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	formLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
	formLayout->setRowWrapPolicy(QFormLayout::WrapLongRows);

	QLineEdit *apiKeyInput = new QLineEdit(&dialog);
	apiKeyInput->setEchoMode(QLineEdit::Password);
	apiKeyInput->setPlaceholderText("Opus Clip API Key");
	apiKeyInput->setText(PluginConfig::getValue("opus_api_key"));

	formLayout->addRow(obsText("Settings.OpusApiKey"), apiKeyInput);

	QCheckBox *opusUploadEnabledInput = new QCheckBox(obsText("Settings.OpusUploadEnabled"), &dialog);
	opusUploadEnabledInput->setChecked(opus_upload_enabled());
	opusUploadEnabledInput->setToolTip(obsText("Tooltip.OpusUploadEnabled"));
	formLayout->addRow(QString(), opusUploadEnabledInput);

	QWidget *localExportWidget = new QWidget(&dialog);
	auto *localExportLayout = new QHBoxLayout(localExportWidget);
	localExportLayout->setContentsMargins(0, 0, 0, 0);
	localExportLayout->setSpacing(6);
	QLineEdit *localExportDirectoryInput = new QLineEdit(localExportWidget);
	localExportDirectoryInput->setText(local_export_directory());
	localExportDirectoryInput->setPlaceholderText(default_local_export_directory());
	localExportDirectoryInput->setToolTip(obsText("Tooltip.LocalExportDirectory"));
	QPushButton *localExportBrowseButton = new QPushButton(obsText("Button.Browse"), localExportWidget);
	localExportLayout->addWidget(localExportDirectoryInput, 1);
	localExportLayout->addWidget(localExportBrowseButton);
	formLayout->addRow(obsText("Settings.LocalExportDirectory"), localExportWidget);

	QObject::connect(localExportBrowseButton, &QPushButton::clicked, &dialog, [&dialog, localExportDirectoryInput]() {
		const QString startDir = localExportDirectoryInput->text().trimmed().isEmpty()
			? default_local_export_directory()
			: localExportDirectoryInput->text().trimmed();
		const QString selected = QFileDialog::getExistingDirectory(&dialog, obsText("Dialog.SelectLocalExportDirectory"), startDir);
		if (!selected.trimmed().isEmpty())
			localExportDirectoryInput->setText(QDir::fromNativeSeparators(selected));
	});

	QComboBox *videoQualityInput = new QComboBox(&dialog);
	videoQualityInput->addItem(obsText("Option.VideoQualityHigh"), QStringLiteral("high"));
	videoQualityInput->addItem(obsText("Option.VideoQualityMedium"), QStringLiteral("medium"));
	videoQualityInput->addItem(obsText("Option.VideoQualityLow"), QStringLiteral("low"));
	set_combo_current_data(videoQualityInput, video_quality_preset(), 1);
	videoQualityInput->setToolTip(obsText("Tooltip.VideoQuality"));
	formLayout->addRow(obsText("Settings.VideoQuality"), videoQualityInput);

	QComboBox *whisperModelInput = new QComboBox(&dialog);
	whisperModelInput->addItem(obsText("WhisperModel.Tiny"), QStringLiteral("ggml-tiny.bin"));
	whisperModelInput->addItem(obsText("WhisperModel.Base"), QStringLiteral("ggml-base.bin"));
	whisperModelInput->addItem(obsText("WhisperModel.Small"), QStringLiteral("ggml-small.bin"));
	whisperModelInput->addItem(obsText("WhisperModel.Medium"), QStringLiteral("ggml-medium.bin"));
	whisperModelInput->addItem(obsText("WhisperModel.LargeV3"), QStringLiteral("ggml-large-v3.bin"));
	set_combo_current_data(whisperModelInput, PluginConfig::getValue("whisper_model_file", "ggml-base.bin"), 1);
	formLayout->addRow(obsText("Settings.WhisperModel"), whisperModelInput);

	QScrollArea *advancedScrollArea = new QScrollArea(&dialog);
	advancedScrollArea->setWidgetResizable(true);
	advancedScrollArea->setFrameShape(QFrame::NoFrame);
	advancedScrollArea->setMinimumHeight(300);

	QWidget *advancedContainer = new QWidget(advancedScrollArea);
	QVBoxLayout *advancedLayout = new QVBoxLayout(advancedContainer);
	advancedLayout->setContentsMargins(0, 0, 0, 0);
	advancedLayout->setSpacing(10);

	auto createSection = [advancedContainer, advancedLayout](const QString &sectionTitle) -> QFormLayout * {
		auto *group = new QGroupBox(sectionTitle, advancedContainer);
		auto *layout = new QFormLayout(group);
		layout->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
		layout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
		layout->setRowWrapPolicy(QFormLayout::WrapLongRows);
		layout->setHorizontalSpacing(12);
		layout->setVerticalSpacing(8);
		advancedLayout->addWidget(group);
		return layout;
	};

	QFormLayout *advancedForm = createSection(obsText("AdvancedSettings"));

	QLineEdit *brandTemplateIdInput = new QLineEdit(advancedContainer);
	brandTemplateIdInput->setPlaceholderText("Brand Template ID");
	brandTemplateIdInput->setText(PluginConfig::getValue("opus_brand_template_id"));
	advancedForm->addRow(obsText("Settings.BrandTemplateId"), brandTemplateIdInput);

	QDoubleSpinBox *resampleThresholdInput = new QDoubleSpinBox(advancedContainer);
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
	advancedForm->addRow(obsText("Settings.UploadResampleThresholdPercent"), resampleThresholdInput);

	QFormLayout *whisperXForm = createSection(obsText("Settings.SectionWhisperX"));

	QComboBox *whisperXBackendInput = new QComboBox(advancedContainer);
	whisperXBackendInput->addItem(obsText("Combobox.Disabled"),
					 QString::fromLatin1(Transcription::WHISPERX_BACKEND_DISABLED));
	whisperXBackendInput->addItem(obsText("Combobox.WhisperXAlignmentOnly"),
					 QString::fromLatin1(Transcription::WHISPERX_BACKEND_ALIGNMENT));
	whisperXBackendInput->addItem(obsText("Combobox.WhisperXPrimary"),
					 QString::fromLatin1(Transcription::WHISPERX_BACKEND_PRIMARY));
	set_combo_current_data(whisperXBackendInput,
		PluginConfig::getValue(QString::fromLatin1(Transcription::CONFIG_WHISPERX_BACKEND),
			QString::fromLatin1(Transcription::WHISPERX_BACKEND_DISABLED)), 0);
	whisperXForm->addRow(obsText("Settings.WhisperXBackend"), whisperXBackendInput);

	QLineEdit *whisperXPythonPathInput = new QLineEdit(advancedContainer);
	whisperXPythonPathInput->setPlaceholderText(QStringLiteral("C:/path/to/.venv-whisperx/Scripts/python.exe"));
	whisperXPythonPathInput->setText(PluginConfig::getValue(
		QString::fromLatin1(Transcription::CONFIG_WHISPERX_PYTHON_PATH)));
	whisperXForm->addRow(obsText("Settings.WhisperXPythonPath"), whisperXPythonPathInput);

	QLineEdit *whisperXWorkerPathInput = new QLineEdit(advancedContainer);
	whisperXWorkerPathInput->setPlaceholderText(Transcription::defaultWhisperXWorkerPath());
	whisperXWorkerPathInput->setText(PluginConfig::getValue(
		QString::fromLatin1(Transcription::CONFIG_WHISPERX_WORKER_PATH),
		Transcription::defaultWhisperXWorkerPath()));
	whisperXForm->addRow(obsText("Settings.WhisperXWorkerPath"), whisperXWorkerPathInput);

	QComboBox *whisperXDeviceInput = new QComboBox(advancedContainer);
	whisperXDeviceInput->addItem(QStringLiteral("CUDA/GPU"), QStringLiteral("cuda"));
	whisperXDeviceInput->addItem(QStringLiteral("CPU"), QStringLiteral("cpu"));
	set_combo_current_data(whisperXDeviceInput, PluginConfig::getValue(
		QString::fromLatin1(Transcription::CONFIG_WHISPERX_DEVICE), QStringLiteral("cuda")), 0);
	whisperXForm->addRow(obsText("Settings.WhisperXDevice"), whisperXDeviceInput);

	QLineEdit *whisperXFfmpegPathInput = new QLineEdit(advancedContainer);
	whisperXFfmpegPathInput->setPlaceholderText(QStringLiteral("Optional ffmpeg.exe or ffmpeg bin directory"));
	whisperXFfmpegPathInput->setText(PluginConfig::getValue(
		QString::fromLatin1(Transcription::CONFIG_WHISPERX_FFMPEG_PATH)));
	whisperXForm->addRow(obsText("Settings.WhisperXFfmpegPath"), whisperXFfmpegPathInput);

	QLineEdit *whisperXModelInput = new QLineEdit(advancedContainer);
	whisperXModelInput->setPlaceholderText(QStringLiteral("large-v3"));
	whisperXModelInput->setText(PluginConfig::getValue(
		QString::fromLatin1(Transcription::CONFIG_WHISPERX_MODEL), QStringLiteral("large-v3")));
	whisperXForm->addRow(obsText("Settings.WhisperXModel"), whisperXModelInput);

	QComboBox *whisperXComputeTypeInput = new QComboBox(advancedContainer);
	whisperXComputeTypeInput->addItem(QStringLiteral("float16"), QStringLiteral("float16"));
	whisperXComputeTypeInput->addItem(QStringLiteral("int8_float16"), QStringLiteral("int8_float16"));
	whisperXComputeTypeInput->addItem(QStringLiteral("int8"), QStringLiteral("int8"));
	whisperXComputeTypeInput->addItem(QStringLiteral("float32"), QStringLiteral("float32"));
	set_combo_current_data(whisperXComputeTypeInput, PluginConfig::getValue(
		QString::fromLatin1(Transcription::CONFIG_WHISPERX_COMPUTE_TYPE), QStringLiteral("float16")), 0);
	whisperXForm->addRow(obsText("Settings.WhisperXComputeType"), whisperXComputeTypeInput);

	QSpinBox *whisperXBatchSizeInput = new QSpinBox(advancedContainer);
	whisperXBatchSizeInput->setRange(1, 128);
	whisperXBatchSizeInput->setValue(PluginConfig::getValue(
		QString::fromLatin1(Transcription::CONFIG_WHISPERX_BATCH_SIZE), QStringLiteral("8")).toInt());
	whisperXForm->addRow(obsText("Settings.WhisperXBatchSize"), whisperXBatchSizeInput);

	QFormLayout *localModelsForm = createSection(obsText("Settings.SectionLocalSemanticModels"));

	QComboBox *localEmbeddingBackendInput = new QComboBox(advancedContainer);
	localEmbeddingBackendInput->addItem(QStringLiteral("llama.cpp native (in-process)"),
						 QString::fromLatin1(Curation::Scoring::LOCAL_EMBEDDING_BACKEND_LLAMA_CPP));
	localEmbeddingBackendInput->addItem(QStringLiteral("llama-server HTTP"),
						 QString::fromLatin1(Curation::Scoring::LOCAL_EMBEDDING_BACKEND_LLAMA_SERVER));
	set_combo_current_data(localEmbeddingBackendInput, Curation::Scoring::localEmbeddingBackendFromConfig(), 0);
	localModelsForm->addRow(obsText("Settings.LocalEmbeddingBackend"), localEmbeddingBackendInput);

	QLineEdit *localEmbeddingEndpointInput = new QLineEdit(advancedContainer);
	localEmbeddingEndpointInput->setPlaceholderText(QStringLiteral("http://127.0.0.1:8080/v1/embeddings"));
	localEmbeddingEndpointInput->setText(PluginConfig::getValue(
		QString::fromLatin1(Curation::Scoring::CONFIG_LOCAL_EMBEDDING_ENDPOINT),
		QStringLiteral("http://127.0.0.1:8080/v1/embeddings")));
	localModelsForm->addRow(obsText("Settings.LocalEmbeddingEndpoint"), localEmbeddingEndpointInput);

	QLineEdit *localEmbeddingModelInput = new QLineEdit(advancedContainer);
	localEmbeddingModelInput->setPlaceholderText(QStringLiteral("qwen3-embedding-0.6b-q8_0"));
	localEmbeddingModelInput->setText(PluginConfig::getValue(
		QString::fromLatin1(Curation::Scoring::CONFIG_LOCAL_EMBEDDING_MODEL_ID),
		QStringLiteral("qwen3-embedding-0.6b-q8_0")));
	localModelsForm->addRow(obsText("Settings.LocalEmbeddingModel"), localEmbeddingModelInput);

	QComboBox *localRerankerBackendInput = new QComboBox(advancedContainer);
	localRerankerBackendInput->addItem(QStringLiteral("llama.cpp native (in-process)"),
						 QString::fromLatin1(Curation::Scoring::LOCAL_RERANKER_BACKEND_LLAMA_CPP));
	localRerankerBackendInput->addItem(QStringLiteral("llama-server HTTP /v1/rerank"),
						 QString::fromLatin1(Curation::Scoring::LOCAL_RERANKER_BACKEND_LLAMA_SERVER));
	set_combo_current_data(localRerankerBackendInput, Curation::Scoring::localRerankerBackendFromConfig(), 0);
	localModelsForm->addRow(obsText("Settings.LocalRerankerBackend"), localRerankerBackendInput);

	QLineEdit *localRerankerEndpointInput = new QLineEdit(advancedContainer);
	localRerankerEndpointInput->setPlaceholderText(QStringLiteral("http://127.0.0.1:8081/v1/rerank"));
	localRerankerEndpointInput->setText(PluginConfig::getValue(
		QString::fromLatin1(Curation::Scoring::CONFIG_LOCAL_RERANKER_ENDPOINT),
		QStringLiteral("http://127.0.0.1:8081/v1/rerank")));
	localModelsForm->addRow(obsText("Settings.LocalRerankerEndpoint"), localRerankerEndpointInput);

	QLineEdit *localRerankerModelInput = new QLineEdit(advancedContainer);
	localRerankerModelInput->setPlaceholderText(QStringLiteral("qwen3-reranker-0.6b-q8_0"));
	localRerankerModelInput->setText(PluginConfig::getValue(
		QString::fromLatin1(Curation::Scoring::CONFIG_LOCAL_RERANKER_MODEL_ID),
		QStringLiteral("qwen3-reranker-0.6b-q8_0")));
	localModelsForm->addRow(obsText("Settings.LocalRerankerModel"), localRerankerModelInput);

	QFormLayout *feedbackForm = createSection(obsText("Settings.SectionFeedbackLearning"));

	QLineEdit *feedbackRankerModelPathInput = new QLineEdit(advancedContainer);
	feedbackRankerModelPathInput->setPlaceholderText(
		QStringLiteral("Optional feedback-ranker.json or GBDT ranker JSON path"));
	feedbackRankerModelPathInput->setText(PluginConfig::getValue(
		QString::fromLatin1(Curation::Scoring::CONFIG_FEEDBACK_RANKER_MODEL_PATH)));
	feedbackRankerModelPathInput->setToolTip(QStringLiteral(
		"Leave empty to use the default feedback/feedback-ranker.json. This can point to a logistic_regression "
		"or gbdt_tree_ensemble artifact produced by tools/train_feedback_ranker.py or tools/train_gbdt_ranker.py."));
	feedbackForm->addRow(obsText("Settings.FeedbackRankerModelPath"), feedbackRankerModelPathInput);

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

	auto updateOpusModeFields = [opusUploadEnabledInput, apiKeyInput, brandTemplateIdInput,
				    localExportDirectoryInput, localExportBrowseButton]() {
		const bool opusEnabled = opusUploadEnabledInput->isChecked();
		apiKeyInput->setEnabled(opusEnabled);
		brandTemplateIdInput->setEnabled(opusEnabled);
		localExportDirectoryInput->setEnabled(!opusEnabled);
		localExportBrowseButton->setEnabled(!opusEnabled);
	};

	QObject::connect(opusUploadEnabledInput, &QCheckBox::toggled, &dialog,
			 [&updateOpusModeFields](bool) { updateOpusModeFields(); });

	QObject::connect(localEmbeddingBackendInput, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
			 [&updateLocalEmbeddingFields](int) { updateLocalEmbeddingFields(); });
	QObject::connect(localRerankerBackendInput, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
			 [&updateLocalRerankerFields](int) { updateLocalRerankerFields(); });
	QObject::connect(whisperXBackendInput, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
			 [&updateWhisperXFields](int) { updateWhisperXFields(); });
	updateWhisperXFields();
	updateLocalEmbeddingFields();
	updateLocalRerankerFields();
	updateOpusModeFields();

	QFormLayout *maintenanceForm = createSection(obsText("Settings.SectionMaintenance"));

	QPushButton *purgeCachesButton = new QPushButton(obsText("Button.PurgePluginCaches"), advancedContainer);
	purgeCachesButton->setToolTip(obsText("Tooltip.PurgePluginCaches"));
	maintenanceForm->addRow(obsText("Settings.PurgePluginCaches"), purgeCachesButton);

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

	advancedLayout->addStretch();
	advancedScrollArea->setWidget(advancedContainer);

	QPushButton *btn = new QPushButton(obsText("Button.Save"), &dialog);

	mainLayout->addLayout(formLayout);
	mainLayout->addWidget(advancedScrollArea, 1);
	mainLayout->addWidget(btn);

	QObject::connect(
		btn, &QPushButton::clicked,
		[&dialog, apiKeyInput, opusUploadEnabledInput, localExportDirectoryInput, videoQualityInput, whisperModelInput,
		 brandTemplateIdInput, resampleThresholdInput,
		 whisperXBackendInput, whisperXPythonPathInput, whisperXWorkerPathInput, whisperXDeviceInput,
		 whisperXFfmpegPathInput, whisperXModelInput, whisperXComputeTypeInput, whisperXBatchSizeInput,
		 localEmbeddingBackendInput, localEmbeddingEndpointInput, localEmbeddingModelInput,
		 localRerankerBackendInput, localRerankerEndpointInput, localRerankerModelInput,
		 feedbackRankerModelPathInput]() {
			PluginConfig::setValue("opus_api_key", apiKeyInput->text().trimmed());
			PluginConfig::setValue("opus_upload_enabled", opusUploadEnabledInput->isChecked() ? QStringLiteral("true") : QStringLiteral("false"));
			PluginConfig::setValue("local_export_directory", localExportDirectoryInput->text().trimmed());
			PluginConfig::setValue("video_quality", videoQualityInput->currentData().toString().trimmed());
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
			PluginConfig::setValue(QString::fromLatin1(Curation::Scoring::CONFIG_FEEDBACK_RANKER_MODEL_PATH),
					       feedbackRankerModelPathInput->text().trimmed());

			blog(LOG_INFO, "Clip Cropper settings saved. Opus Clip settings updated.");

			dialog.accept();
		});

	dialog.exec();
}

void ensure_opus_api_key_impl(QWidget *parent)
{
	if (!opus_upload_enabled()) {
		blog(LOG_INFO, "Opus Clip upload is disabled; local export mode does not require an API key.");
		return;
	}

	const QString apiKey = get_opus_api_key();

	if (!apiKey.isEmpty()) {
		blog(LOG_INFO, "Opus Clip API key already configured");
		return;
	}

	QMessageBox::information(parent, title, obsText("Message.ConfigureApiKeyBeforeCuts"));

	open_settings(nullptr);
}
