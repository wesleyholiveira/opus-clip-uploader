#ifdef __cplusplus
extern "C" {
#endif

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#ifdef __cplusplus
}
#endif

#include "ui/upload-review-dialog.hpp"
#include "ui/video-marker-editor.hpp"

#include <utils/config.hpp>
#include <utils/file.hpp>
#include <worker/upload-worker.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QTimer>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QObject>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

#include <functional>

static QString obsText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

static const QString title("Clip Cropper");

static QStringList pendingRecordingPaths;

void set_pending_recording_paths(const QStringList &paths)
{
	pendingRecordingPaths = paths;

	for (const QString &path : pendingRecordingPaths) {
		obs_log(LOG_INFO, "Pending recording path set: %s", path.toUtf8().constData());
	}
}

void clear_pending_recording_paths()
{
	pendingRecordingPaths.clear();
	obs_log(LOG_INFO, "Pending recording paths cleared");
}

static QStringList get_recording_paths_for_upload()
{
	return pendingRecordingPaths;
}

static QString get_opus_api_key()
{
	return PluginConfig::getValue("opus_api_key").trimmed();
}

static void resize_upload_dialog(QDialog *dialog, bool expanded)
{
	dialog->setMinimumWidth(560);

	if (expanded) {
		dialog->setMinimumHeight(240);
		dialog->setMaximumHeight(QWIDGETSIZE_MAX);
	} else {
		dialog->setMinimumHeight(0);
		dialog->setMaximumHeight(QWIDGETSIZE_MAX);
	}

	dialog->adjustSize();
}

static void start_upload(QDialog *dialog, QPushButton *btnUpload, QPushButton *btnCancel, QProgressBar *progressBar,
			 QLabel *uploadStatusLabel, const QString &apiKey, const CurationSettings &curationSettings)
{
	const QStringList recordingPaths = get_recording_paths_for_upload();

	if (apiKey.trimmed().isEmpty()) {
		QMessageBox::warning(dialog, title, obsText("Message.ConfigureApiKeyBeforeUpload"));

		btnUpload->setEnabled(true);
		btnCancel->setEnabled(true);
		progressBar->parentWidget()->hide();

		if (uploadStatusLabel) {
			uploadStatusLabel->clear();
		}

		return;
	}

	if (recordingPaths.isEmpty()) {
		obs_log(LOG_ERROR, "No recording path found for upload.");

		QMessageBox::critical(dialog, title, obsText("Message.NoValidRecordingFile"));

		btnUpload->setEnabled(true);
		btnCancel->setEnabled(true);
		progressBar->parentWidget()->hide();

		if (uploadStatusLabel) {
			uploadStatusLabel->clear();
		}

		return;
	}

	static constexpr int MAX_PARALLEL_UPLOADS = 3;

	struct UploadBatchState {
		QStringList paths;
		QVector<int> progress;
		QVector<QString> statusMessages;
		int nextIndex = 0;
		int running = 0;
		int completed = 0;
		int failed = 0;
		bool finished = false;
	};

	auto *state = new UploadBatchState();
	state->paths = recordingPaths;
	state->progress = QVector<int>(recordingPaths.size(), 0);
	state->statusMessages = QVector<QString>(recordingPaths.size());

	btnUpload->setEnabled(false);
	btnCancel->setEnabled(false);

	progressBar->parentWidget()->show();
	progressBar->show();
	progressBar->setValue(0);
	progressBar->setFormat(obsText("Status.ProgressPreparing"));

	if (uploadStatusLabel) {
		uploadStatusLabel->show();
		uploadStatusLabel->setText(obsText("Status.PreparingUpload").arg(state->paths.size()));
	}

	resize_upload_dialog(dialog, true);

	auto updateProgress = [=]() {
		int totalProgress = 0;

		for (int value : state->progress) {
			totalProgress += value;
		}

		const int globalProgress = state->paths.isEmpty() ? 0 : totalProgress / state->paths.size();

		progressBar->setValue(globalProgress);

		QString currentStatus;
		for (const QString &message : state->statusMessages) {
			if (!message.trimmed().isEmpty())
				currentStatus = message;
		}

		if (currentStatus.trimmed().isEmpty()) {
			currentStatus = obsText("Status.UploadingVideos")
						.arg(qMin(state->completed + state->running, state->paths.size()))
						.arg(state->paths.size());
		}

		progressBar->setFormat(QString("%1 - %p%").arg(currentStatus));

		if (uploadStatusLabel)
			uploadStatusLabel->setText(currentStatus);
	};

	auto finishBatchIfNeeded = [=]() {
		if (state->finished)
			return true;

		if (state->completed < state->paths.size())
			return false;

		state->finished = true;

		clear_pending_recording_paths();

		if (state->failed > 0) {
			QMessageBox::warning(dialog, title,
					     obsText("Message.UploadFinishedWithFailures").arg(state->failed));
		} else {
			QMessageBox::information(dialog, title, obsText("Message.UploadSuccess"));
		}

		delete state;

		dialog->accept();
		return true;
	};

	auto *startNext = new std::function<void()>();

	*startNext = [=]() mutable {
		while (state->running < MAX_PARALLEL_UPLOADS && state->nextIndex < state->paths.size()) {
			const int index = state->nextIndex++;
			const QString recordingPath = state->paths.at(index);
			const QFileInfo qFileInfo(recordingPath);

			if (!qFileInfo.exists() || !qFileInfo.isFile()) {
				obs_log(LOG_ERROR, "Invalid upload path. Expected file, got: %s",
					recordingPath.toUtf8().constData());

				state->progress[index] = 100;
				state->completed++;
				state->failed++;

				updateProgress();

				if (finishBatchIfNeeded()) {
					delete startNext;
					return;
				}

				continue;
			}

			FileInfo fInfo(recordingPath.toStdString());
			fInfo.parseFile();

			obs_log(LOG_INFO, "Uploading part %d/%d to Opus Clip - Path: %s, Name: %s, Mime: %s", index + 1,
				state->paths.size(), fInfo.filePath.c_str(), fInfo.fileName.c_str(),
				fInfo.mimeType.c_str());

			auto *thread = new QThread(dialog);
			const QString brandTemplateId = PluginConfig::getValue("opus_brand_template_id").trimmed();
			const QString sourceLang = PluginConfig::getValue("opus_source_lang", "auto").trimmed();

			auto *worker = new UploadWorker(apiKey, QString::fromStdString(fInfo.filePath),
							QString::fromStdString(fInfo.fileName),
							QString::fromStdString(fInfo.mimeType), brandTemplateId,
							sourceLang, curationSettings);

			state->running++;

			worker->moveToThread(thread);

			QObject::connect(thread, &QThread::started, worker, &UploadWorker::run);

			QObject::connect(
				worker, &UploadWorker::progressChanged, progressBar,
				[=](int value, const QString &message) {
					state->progress[index] = value;
					if (!message.trimmed().isEmpty())
						state->statusMessages[index] = message;
					updateProgress();
				},
				Qt::QueuedConnection);

			QObject::connect(worker, &UploadWorker::finished, thread, &QThread::quit);
			QObject::connect(worker, &UploadWorker::failed, thread, &QThread::quit);

			QObject::connect(worker, &UploadWorker::finished, worker, &QObject::deleteLater);
			QObject::connect(worker, &UploadWorker::failed, worker, &QObject::deleteLater);
			QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);

			QObject::connect(
				worker, &UploadWorker::failed, dialog,
				[=](const QString &message) mutable {
					obs_log(LOG_ERROR, "Upload failed: %s", message.toUtf8().constData());

					state->running--;
					state->completed++;
					state->failed++;
					state->progress[index] = 100;
					state->statusMessages[index] = message;

					updateProgress();

					if (finishBatchIfNeeded()) {
						delete startNext;
						return;
					}

					(*startNext)();
				},
				Qt::QueuedConnection);

			QObject::connect(
				worker, &UploadWorker::finished, dialog,
				[=](const QString &projectId) mutable {
					obs_log(LOG_INFO, "Opus Clip project created: %s",
						projectId.toUtf8().constData());

					state->running--;
					state->completed++;
					state->progress[index] = 100;
					state->statusMessages[index] = obsText("Status.ProjectCreated").arg(projectId);

					updateProgress();

					if (finishBatchIfNeeded()) {
						delete startNext;
						return;
					}

					(*startNext)();
				},
				Qt::QueuedConnection);

			thread->start();
		}
	};

	(*startNext)();
}

void open_settings(void *private_data)
{
	UNUSED_PARAMETER(private_data);

	QWidget *parent = reinterpret_cast<QWidget *>(obs_frontend_get_main_window());

	QDialog dialog(parent);
	dialog.setWindowTitle(title);
	dialog.resize(620, 300);

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

	QTreeWidget *treeWidget = new QTreeWidget(&dialog);
	treeWidget->setColumnCount(2);
	treeWidget->setHeaderHidden(true);
	treeWidget->setRootIsDecorated(true);
	treeWidget->setItemsExpandable(true);
	treeWidget->setAnimated(true);
	treeWidget->setMinimumHeight(110);
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

	QObject::connect(btn, &QPushButton::clicked, [&dialog, apiKeyInput, brandTemplateIdInput, sourceLangInput]() {
		PluginConfig::setValue("opus_api_key", apiKeyInput->text().trimmed());
		PluginConfig::setValue("opus_brand_template_id", brandTemplateIdInput->text().trimmed());

		const QString sourceLang = sourceLangInput->currentText().trimmed();
		PluginConfig::setValue("opus_source_lang", sourceLang.isEmpty() ? "auto" : sourceLang);

		obs_log(LOG_INFO, "Clip Cropper settings saved. Opus Clip settings updated.");

		dialog.accept();
	});

	dialog.exec();
}

void open_confirm_dialog(void *private_data)
{
	UNUSED_PARAMETER(private_data);

	QWidget *parent = reinterpret_cast<QWidget *>(obs_frontend_get_main_window());

	QDialog dialog(parent);
	dialog.setWindowTitle(title + " - " + obsText("Dialog.ConfirmUploadTitle"));

	QVBoxLayout *mainLayout = new QVBoxLayout(&dialog);
	mainLayout->setContentsMargins(22, 16, 22, 16);
	mainLayout->setSpacing(8);

	QLabel *label = new QLabel(obsText("Dialog.ConfirmUploadQuestion"), &dialog);
	label->setWordWrap(true);
	mainLayout->addWidget(label);

	auto *progressContainer = new QFrame(&dialog);
	progressContainer->setFrameShape(QFrame::NoFrame);
	progressContainer->hide();

	auto *progressLayout = new QVBoxLayout(progressContainer);
	progressLayout->setContentsMargins(0, 8, 0, 8);
	progressLayout->setSpacing(8);

	QLabel *uploadStatusLabel = new QLabel("", progressContainer);
	uploadStatusLabel->setWordWrap(true);
	uploadStatusLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	uploadStatusLabel->setMinimumHeight(22);

	QProgressBar *progressBar = new QProgressBar(progressContainer);
	progressBar->setRange(0, 100);
	progressBar->setValue(0);
	progressBar->setMinimumHeight(24);
	progressBar->setTextVisible(true);

	progressLayout->addWidget(uploadStatusLabel);
	progressLayout->addWidget(progressBar);
	mainLayout->addWidget(progressContainer);

	QPushButton *btnUpload = new QPushButton(obsText("Button.Yes"), &dialog);
	QPushButton *btnCancel = new QPushButton(obsText("Button.No"), &dialog);

	btnUpload->setMinimumHeight(32);
	btnCancel->setMinimumHeight(32);

	QHBoxLayout *btnLayout = new QHBoxLayout();
	btnLayout->setSpacing(12);
	btnLayout->addWidget(btnUpload);
	btnLayout->addWidget(btnCancel);

	mainLayout->addLayout(btnLayout);

	dialog.setMinimumWidth(520);
	dialog.adjustSize();
	dialog.setFixedSize(dialog.sizeHint());

	QObject::connect(btnCancel, &QPushButton::clicked, [&dialog]() {
		obs_log(LOG_INFO, "Fechando Dialog de Upload");
		dialog.reject();
	});

	QObject::connect(
		btnUpload, &QPushButton::clicked, [&dialog, btnUpload, btnCancel, progressBar, uploadStatusLabel]() {
			const QString apiKey = get_opus_api_key();

			if (apiKey.trimmed().isEmpty()) {
				QMessageBox::warning(&dialog, title, obsText("Message.ConfigureApiKeyInSettings"));
				open_settings(nullptr);
				return;
			}

			const QStringList paths = get_recording_paths_for_upload();

			if (paths.isEmpty()) {
				QMessageBox::critical(&dialog, title, obsText("Message.NoValidRecordingFile"));
				return;
			}

			UploadReviewDialog reviewDialog(paths.first(), &dialog);

			if (reviewDialog.exec() != QDialog::Accepted)
				return;

			const CurationSettings curationSettings = reviewDialog.curationSettings();

			start_upload(&dialog, btnUpload, btnCancel, progressBar, uploadStatusLabel, apiKey,
				     curationSettings);
		});

	dialog.exec();
}


static void upload_reviewed_video(QWidget *parent, const QString &videoPath, const CurationSettings &curationSettings)
{
	const QString apiKey = get_opus_api_key();

	if (apiKey.trimmed().isEmpty()) {
		QMessageBox::warning(parent, title, obsText("Message.ConfigureApiKeyInSettings"));
		open_settings(nullptr);
		return;
	}

	set_pending_recording_paths(QStringList{videoPath});

	QDialog uploadDialog(parent);
	uploadDialog.setWindowTitle(title + " - " + obsText("Dialog.ConfirmUploadTitle"));

	auto *mainLayout = new QVBoxLayout(&uploadDialog);
	mainLayout->setContentsMargins(22, 16, 22, 16);
	mainLayout->setSpacing(10);

	auto *progressContainer = new QFrame(&uploadDialog);
	progressContainer->setFrameShape(QFrame::NoFrame);

	auto *progressLayout = new QVBoxLayout(progressContainer);
	progressLayout->setContentsMargins(0, 0, 0, 0);
	progressLayout->setSpacing(8);

	auto *uploadStatusLabel = new QLabel(obsText("Status.PreparingUpload").arg(1), progressContainer);
	uploadStatusLabel->setWordWrap(true);
	uploadStatusLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	uploadStatusLabel->setMinimumHeight(24);

	auto *progressBar = new QProgressBar(progressContainer);
	progressBar->setRange(0, 100);
	progressBar->setValue(0);
	progressBar->setMinimumHeight(26);
	progressBar->setTextVisible(true);

	progressLayout->addWidget(uploadStatusLabel);
	progressLayout->addWidget(progressBar);
	mainLayout->addWidget(progressContainer);

	auto *btnUpload = new QPushButton(&uploadDialog);
	auto *btnCancel = new QPushButton(&uploadDialog);
	btnUpload->hide();
	btnCancel->hide();

	uploadDialog.setMinimumWidth(540);
	uploadDialog.adjustSize();

	QTimer::singleShot(0, &uploadDialog, [&uploadDialog, btnUpload, btnCancel, progressBar, uploadStatusLabel, apiKey, curationSettings]() {
		start_upload(&uploadDialog, btnUpload, btnCancel, progressBar, uploadStatusLabel, apiKey, curationSettings);
	});

	uploadDialog.exec();
}

void open_video_editor(void *private_data)
{
	UNUSED_PARAMETER(private_data);

	QWidget *parent = reinterpret_cast<QWidget *>(obs_frontend_get_main_window());

	const QString videoPath = QFileDialog::getOpenFileName(parent, obsText("Dialog.SelectVideoTitle"), QString(),
		obsText("Dialog.VideoFileFilter"));

	if (videoPath.trimmed().isEmpty())
		return;

	QDialog dialog(parent);
	dialog.setWindowTitle(title + " - " + obsText("Dialog.VideoEditorTitle"));
	dialog.resize(980, 720);

	auto *mainLayout = new QVBoxLayout(&dialog);
	mainLayout->setContentsMargins(12, 12, 12, 12);
	mainLayout->setSpacing(8);

	auto *editor = new VideoMarkerEditor(videoPath, &dialog);
	editor->setReviewActionVisible(true);
	mainLayout->addWidget(editor, 1);

	QObject::connect(editor, &VideoMarkerEditor::reviewRequested, &dialog, [&dialog, editor, videoPath]() {
		editor->exitFullScreen();

		QWidget *reviewParent = dialog.parentWidget();
		dialog.hide();

		UploadReviewDialog reviewDialog(videoPath, reviewParent);
		if (reviewDialog.exec() == QDialog::Accepted) {
			const CurationSettings curationSettings = reviewDialog.curationSettings();
			upload_reviewed_video(reviewParent, videoPath, curationSettings);
		}

		dialog.accept();
	});

	QTimer::singleShot(0, editor, [editor]() {
		editor->toggleFullScreen();
	});

	dialog.exec();
}

void ensure_opus_api_key(QWidget *parent)
{
	const QString apiKey = get_opus_api_key();

	if (!apiKey.isEmpty()) {
		obs_log(LOG_INFO, "Opus Clip API key already configured");
		return;
	}

	QMessageBox::information(parent, title, obsText("Message.ConfigureApiKeyBeforeCuts"));

	open_settings(nullptr);
}