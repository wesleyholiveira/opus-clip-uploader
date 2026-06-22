#include "ui/upload-flow.hpp"

#include "ui/ui-common.hpp"
#include "ui/ui.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <utils/config.hpp>
#include <utils/file.hpp>
#include <worker/upload-worker.hpp>

#include <QDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QObject>
#include <QProgressBar>
#include <QPushButton>
#include <QSize>
#include <QPointer>
#include <QThread>
#include <QVector>
#include <QWidget>

#include <functional>
#include <memory>

static const QString &title = clipCropperTitle();

static void resize_upload_dialog(QDialog *dialog, bool expanded)
{
	static constexpr int uploadDialogMinWidth = 540;
	static constexpr int uploadDialogMinExpandedHeight = 138;

	dialog->setMinimumWidth(uploadDialogMinWidth);
	dialog->setMinimumHeight(expanded ? uploadDialogMinExpandedHeight : 0);
	dialog->setMaximumHeight(QWIDGETSIZE_MAX);

	dialog->adjustSize();
	const QSize preferredSize = dialog->sizeHint();
	dialog->resize(qMax(dialog->width(), uploadDialogMinWidth),
		       qMax(preferredSize.height(), dialog->minimumHeight()));
}

void start_upload(QDialog *dialog, QPushButton *btnUpload, QPushButton *btnCancel, QProgressBar *progressBar,
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
		blog(LOG_ERROR, "No recording path found for upload.");

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
		bool canceled = false;
		QVector<QPointer<UploadWorker>> workers;
	};

	auto state = std::make_shared<UploadBatchState>();
	state->paths = recordingPaths;
	state->progress = QVector<int>(recordingPaths.size(), 0);
	state->statusMessages = QVector<QString>(recordingPaths.size());

	btnUpload->setEnabled(false);
	btnCancel->setText(obsText("Button.Cancel"));
	btnCancel->setEnabled(true);
	btnCancel->show();

	progressBar->parentWidget()->show();
	progressBar->show();
	progressBar->setValue(0);
	progressBar->setFormat(QStringLiteral("%p%"));

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

		progressBar->setFormat(QStringLiteral("%p%"));

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

		if (state->canceled) {
			QMessageBox::information(dialog, title, obsText("Message.UploadCanceled"));
		} else if (state->failed > 0) {
			QMessageBox::warning(dialog, title,
					     obsText("Message.UploadFinishedWithFailures").arg(state->failed));
		} else {
			QMessageBox::information(dialog, title, obsText("Message.UploadSuccess"));
		}

		mark_progress_window_finished(dialog);
		dialog->accept();
		return true;
	};

	auto cancelUpload = [=]() {
		if (state->finished || state->canceled)
			return;

		state->canceled = true;
		btnCancel->setEnabled(false);
		progressBar->setFormat(QStringLiteral("%p%"));
		if (uploadStatusLabel)
			uploadStatusLabel->setText(obsText("Status.CancelingOperation"));

		const int pending = state->paths.size() - state->nextIndex;
		if (pending > 0) {
			state->completed += pending;
			state->failed += pending;
			state->nextIndex = state->paths.size();
		}

		for (const QPointer<UploadWorker> &worker : state->workers) {
			if (worker)
				worker->cancel();
		}

		if (state->running == 0)
			finishBatchIfNeeded();
	};

	QObject::connect(btnCancel, &QPushButton::clicked, dialog, cancelUpload);
	bind_progress_window_cancel(dialog, cancelUpload);

	auto *startNext = new std::function<void()>();
	QObject::connect(dialog, &QObject::destroyed, dialog, [startNext]() { delete startNext; });

	*startNext = [=]() mutable {
		if (state->canceled)
			return;

		while (!state->canceled && state->running < MAX_PARALLEL_UPLOADS &&
		       state->nextIndex < state->paths.size()) {
			const int index = state->nextIndex++;
			const QString recordingPath = state->paths.at(index);
			const QFileInfo qFileInfo(recordingPath);

			if (!qFileInfo.exists() || !qFileInfo.isFile()) {
				blog(LOG_ERROR, "Invalid upload path. Expected file, got: %s",
				     recordingPath.toUtf8().constData());

				state->progress[index] = 100;
				state->completed++;
				state->failed++;

				updateProgress();

				if (finishBatchIfNeeded())
					return;

				continue;
			}

			FileInfo fInfo(recordingPath.toStdString());
			fInfo.parseFile();

			blog(LOG_INFO, "Uploading part %d/%d to Opus Clip - Path: %s, Name: %s, Mime: %s", index + 1,
			     state->paths.size(), fInfo.filePath.c_str(), fInfo.fileName.c_str(),
			     fInfo.mimeType.c_str());

			auto *thread = new QThread(dialog);
			const QString brandTemplateId = PluginConfig::getValue("opus_brand_template_id").trimmed();
			QString sourceLang = curationSettings.sourceLanguage.trimmed();
			if (sourceLang.isEmpty())
				sourceLang = QStringLiteral("auto");


			auto *worker = new UploadWorker(apiKey, QString::fromStdString(fInfo.filePath),
							QString::fromStdString(fInfo.fileName),
							QString::fromStdString(fInfo.mimeType), brandTemplateId,
							sourceLang, curationSettings);

			state->running++;
			state->workers.append(worker);

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
					blog(LOG_ERROR, "Upload failed: %s", message.toUtf8().constData());

					state->running--;
					state->completed++;
					state->failed++;
					state->progress[index] = 100;
					state->statusMessages[index] =
						state->canceled ? obsText("Message.UploadCanceled") : message;

					updateProgress();

					if (finishBatchIfNeeded())
						return;

					(*startNext)();
				},
				Qt::QueuedConnection);

			QObject::connect(
				worker, &UploadWorker::finished, dialog,
				[=](const QString &projectId) mutable {
					blog(LOG_INFO, "Opus Clip project created: %s", projectId.toUtf8().constData());

					state->running--;
					state->completed++;
					state->progress[index] = 100;
					state->statusMessages[index] = obsText("Status.ProjectCreated").arg(projectId);

					updateProgress();

					if (finishBatchIfNeeded())
						return;

					(*startNext)();
				},
				Qt::QueuedConnection);

			thread->start();
		}
	};

	(*startNext)();
}
