#ifdef __cplusplus
extern "C" {
#endif

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#ifdef __cplusplus
}
#endif

#include <utils/config.hpp>
#include <utils/file.hpp>
#include <worker/upload-worker.hpp>

#include <QVector>
#include <QStringList>
#include <QDialog>
#include <QProgressBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QWidget>
#include <QThread>
#include <QMessageBox>
#include <QFileInfo>
#include <QObject>

#include <functional>

static const QString title("Clip Cropper");
static const QString opusApiKeyKey("opus_api_key");
static QStringList pendingRecordingPaths;

static QString get_opus_api_key()
{
	return PluginConfig::getValue(opusApiKeyKey).trimmed();
}

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

void ensure_opus_api_key(QWidget *parent)
{
	UNUSED_PARAMETER(parent);

	if (get_opus_api_key().isEmpty()) {
		obs_log(LOG_WARNING, "Opus Clip API key is not configured yet");
		return;
	}

	obs_log(LOG_INFO, "Opus Clip API key is configured");
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
			 QLabel *uploadStatusLabel, const QString &apiKey)
{
	const QStringList recordingPaths = get_recording_paths_for_upload();

	if (recordingPaths.isEmpty()) {
		obs_log(LOG_ERROR, "No recording path found for upload.");
		QMessageBox::critical(dialog, title, "Nenhum arquivo de gravação válido foi encontrado.");

		btnUpload->setEnabled(true);
		btnCancel->setEnabled(true);
		progressBar->hide();

		if (uploadStatusLabel) {
			uploadStatusLabel->hide();
			uploadStatusLabel->clear();
		}

		return;
	}

	static constexpr int MAX_PARALLEL_UPLOADS = 3;

	struct UploadBatchState {
		QStringList paths;
		QVector<int> progress;
		int nextIndex = 0;
		int running = 0;
		int completed = 0;
		int failed = 0;
		QStringList projectIds;
	};

	auto *state = new UploadBatchState();
	state->paths = recordingPaths;
	state->progress = QVector<int>(recordingPaths.size(), 0);

	btnUpload->setEnabled(false);
	btnCancel->setEnabled(false);

	progressBar->show();
	progressBar->setValue(0);

	if (uploadStatusLabel) {
		uploadStatusLabel->show();
		uploadStatusLabel->setText(QString("Preparando envio de %1 vídeo(s) para o Opus Clip...").arg(state->paths.size()));
	}

	resize_upload_dialog(dialog, true);

	auto updateProgress = [=]() {
		int totalProgress = 0;
		for (int value : state->progress) {
			totalProgress += value;
		}

		const int globalProgress = state->paths.isEmpty() ? 0 : totalProgress / state->paths.size();
		progressBar->setValue(globalProgress);

		if (uploadStatusLabel) {
			uploadStatusLabel->setText(
				QString("Enviando para o Opus Clip: %1/%2")
					.arg(qMin(state->completed + state->running, state->paths.size()))
					.arg(state->paths.size()));
		}
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
				continue;
			}

			FileInfo fInfo(recordingPath.toStdString());
			fInfo.parseFile();

			obs_log(LOG_INFO, "Uploading video %d/%d to Opus Clip - Path: %s, Name: %s, Mime: %s",
				index + 1, state->paths.size(), fInfo.filePath.c_str(), fInfo.fileName.c_str(),
				fInfo.mimeType.c_str());

			auto *thread = new QThread;
			auto *worker = new UploadWorker(apiKey, QString::fromStdString(fInfo.filePath),
							QString::fromStdString(fInfo.fileName),
							QString::fromStdString(fInfo.mimeType));

			state->running++;
			worker->moveToThread(thread);

			QObject::connect(thread, &QThread::started, worker, &UploadWorker::run);

			QObject::connect(
				worker, &UploadWorker::progressChanged, progressBar,
				[=](int value) {
					state->progress[index] = value;
					updateProgress();
				},
				Qt::QueuedConnection);

			QObject::connect(worker, &UploadWorker::finished, thread, &QThread::quit);
			QObject::connect(worker, &UploadWorker::finished, worker, &QObject::deleteLater);
			QObject::connect(worker, &UploadWorker::failed, thread, &QThread::quit);
			QObject::connect(worker, &UploadWorker::failed, worker, &QObject::deleteLater);
			QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);

			QObject::connect(
				worker, &UploadWorker::failed, dialog,
				[=](const QString &message) mutable {
					obs_log(LOG_ERROR, "Opus Clip upload failed: %s", message.toUtf8().constData());
					state->running--;
					state->completed++;
					state->failed++;
					state->progress[index] = 100;
					updateProgress();

					if (state->completed >= state->paths.size()) {
						clear_pending_recording_paths();
						QMessageBox::warning(
							dialog, title,
							QString("Envio para o Opus Clip finalizado com %1 falha(s).")
								.arg(state->failed));
						delete state;
						delete startNext;
						dialog->accept();
						return;
					}

					(*startNext)();
				},
				Qt::QueuedConnection);

			QObject::connect(
				worker, &UploadWorker::finished, dialog,
				[=](const QString &projectId) mutable {
					state->running--;
					state->completed++;
					state->progress[index] = 100;
					if (!projectId.trimmed().isEmpty()) {
						state->projectIds.append(projectId);
					}

					updateProgress();

					if (state->completed >= state->paths.size()) {
						clear_pending_recording_paths();

						if (state->failed > 0) {
							QMessageBox::warning(
								dialog, title,
								QString("Envio para o Opus Clip finalizado com %1 falha(s).")
									.arg(state->failed));
						} else {
							QMessageBox::information(dialog, title,
										 "Vídeo enviado e projeto criado no Opus Clip.");
						}

						delete state;
						delete startNext;
						dialog->accept();
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

	QWidget *parent = (QWidget *)obs_frontend_get_main_window();

	QDialog dialog(parent);
	dialog.setWindowTitle(title);
	dialog.resize(520, 160);

	QVBoxLayout *mainLayout = new QVBoxLayout(&dialog);
	mainLayout->setContentsMargins(20, 20, 20, 0);
	mainLayout->setSpacing(12);

	QFormLayout *formLayout = new QFormLayout();
	formLayout->setLabelAlignment(Qt::AlignLeft);

	QLineEdit *apiKeyInput = new QLineEdit(&dialog);
	apiKeyInput->setEchoMode(QLineEdit::Password);
	apiKeyInput->setPlaceholderText("Opus Clip API Key");
	apiKeyInput->setText(get_opus_api_key());

	formLayout->addRow("Opus Clip API Key:", apiKeyInput);

	QPushButton *btn = new QPushButton("Salvar", &dialog);

	mainLayout->addLayout(formLayout);
	mainLayout->addWidget(btn);
	mainLayout->addStretch();

	QObject::connect(btn, &QPushButton::clicked, [&dialog, apiKeyInput]() {
		PluginConfig::setValue(opusApiKeyKey, apiKeyInput->text().trimmed());
		obs_log(LOG_INFO, "Clip Cropper Opus Clip settings saved.");
		dialog.accept();
	});

	dialog.exec();
}

void open_confirm_dialog(void *private_data)
{
	UNUSED_PARAMETER(private_data);

	QWidget *parent = (QWidget *)obs_frontend_get_main_window();

	QDialog dialog(parent);
	dialog.setWindowTitle(title + " - Confirmar envio");

	QVBoxLayout *mainLayout = new QVBoxLayout(&dialog);
	mainLayout->setContentsMargins(22, 16, 22, 16);
	mainLayout->setSpacing(8);

	QLabel *label = new QLabel("Deseja enviar o vídeo para o Opus Clip para gerar os cortes?", &dialog);
	mainLayout->addWidget(label);

	QLabel *uploadStatusLabel = new QLabel("", &dialog);
	uploadStatusLabel->setWordWrap(true);
	uploadStatusLabel->setMinimumHeight(36);
	uploadStatusLabel->hide();
	mainLayout->addWidget(uploadStatusLabel);

	QProgressBar *progressBar = new QProgressBar(&dialog);
	progressBar->setRange(0, 100);
	progressBar->setValue(0);
	progressBar->hide();
	mainLayout->addWidget(progressBar);

	QPushButton *btnUpload = new QPushButton("Sim", &dialog);
	QPushButton *btnCancel = new QPushButton("Não", &dialog);

	btnUpload->setMinimumHeight(32);
	btnCancel->setMinimumHeight(32);

	QHBoxLayout *btnLayout = new QHBoxLayout();
	btnLayout->setSpacing(12);
	btnLayout->addWidget(btnUpload);
	btnLayout->addWidget(btnCancel);

	mainLayout->addLayout(btnLayout);

	dialog.setMinimumWidth(560);
	dialog.adjustSize();
	dialog.setFixedSize(dialog.sizeHint());

	QObject::connect(btnCancel, &QPushButton::clicked, [&dialog]() {
		obs_log(LOG_INFO, "Fechando Dialog de Upload Opus Clip");
		dialog.reject();
	});

	QObject::connect(btnUpload, &QPushButton::clicked, [&dialog, btnUpload, btnCancel, progressBar, uploadStatusLabel]() {
		const QString apiKey = get_opus_api_key();
		if (apiKey.isEmpty()) {
			QMessageBox::warning(&dialog, title, "Configure a Opus Clip API Key antes de enviar o vídeo.");
			return;
		}

		obs_log(LOG_INFO, "Starting Opus Clip upload flow.");
		start_upload(&dialog, btnUpload, btnCancel, progressBar, uploadStatusLabel, apiKey);
	});

	dialog.exec();
}
