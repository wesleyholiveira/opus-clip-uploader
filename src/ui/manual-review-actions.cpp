#include "ui/ui.hpp"

#include "ui/ui-common.hpp"
#include "ui/upload-flow.hpp"
#include "ui/upload-review-dialog.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QDialog>
#include <QFileDialog>
#include <QFrame>
#include <QLabel>
#include <QMessageBox>
#include <QObject>
#include <QProgressBar>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

static const QString &title = clipCropperTitle();

static void upload_reviewed_video(QWidget *parent, const QString &videoPath, const CurationSettings &curationSettings)
{
	const QString apiKey = get_opus_api_key();

	if (apiKey.trimmed().isEmpty()) {
		QMessageBox::warning(parent, title, obsText("Message.ConfigureApiKeyInSettings"));
		open_settings(nullptr);
		return;
	}

	set_pending_recording_paths(QStringList{videoPath});

	auto *uploadDialog = new BackgroundProgressDialog(parent);
	uploadDialog->setWindowTitle(title);
	configure_background_progress_window(uploadDialog, true);
	QObject::connect(uploadDialog, &QDialog::finished, uploadDialog, &QObject::deleteLater);

	auto *mainLayout = new QVBoxLayout(uploadDialog);
	mainLayout->setContentsMargins(20, 14, 20, 16);
	mainLayout->setSpacing(8);

	auto *progressContainer = new QFrame(uploadDialog);
	progressContainer->setFrameShape(QFrame::NoFrame);

	auto *progressLayout = new QVBoxLayout(progressContainer);
	progressLayout->setContentsMargins(0, 0, 0, 0);
	progressLayout->setSpacing(6);

	auto *uploadStatusLabel = new QLabel(obsText("Status.PreparingUpload").arg(1), progressContainer);
	uploadStatusLabel->setWordWrap(false);
	uploadStatusLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	uploadStatusLabel->setMinimumHeight(20);

	auto *progressBar = new QProgressBar(progressContainer);
	progressBar->setRange(0, 100);
	progressBar->setValue(0);
	progressBar->setFixedHeight(22);
	progressBar->setTextVisible(false);

	progressLayout->addWidget(uploadStatusLabel);
	progressLayout->addWidget(progressBar);
	mainLayout->addWidget(progressContainer);

	auto *hiddenUploadButton = new QPushButton(uploadDialog);
	auto *cancelUploadButton = new QPushButton(obsText("Button.Cancel"), uploadDialog);
	hiddenUploadButton->hide();
	mainLayout->addWidget(cancelUploadButton);

	uploadDialog->setMinimumWidth(540);
	uploadDialog->adjustSize();

	QTimer::singleShot(0, uploadDialog,
			   [uploadDialog, hiddenUploadButton, cancelUploadButton, progressBar, uploadStatusLabel, apiKey,
			    curationSettings]() {
				   start_upload(uploadDialog, hiddenUploadButton, cancelUploadButton, progressBar,
						uploadStatusLabel, apiKey, curationSettings);
			   });

	uploadDialog->show();
	uploadDialog->raise();
}

void open_video_review_impl(void *private_data)
{
	UNUSED_PARAMETER(private_data);

	QWidget *parent = reinterpret_cast<QWidget *>(obs_frontend_get_main_window());

	const QString videoPath = QFileDialog::getOpenFileName(parent, obsText("Dialog.SelectVideoTitle"), QString(),
							       obsText("Dialog.VideoFileFilter"));

	if (videoPath.trimmed().isEmpty())
		return;

	blog(LOG_INFO, "Opening upload review dialog directly for selected video: %s", videoPath.toUtf8().constData());

	UploadReviewDialog reviewDialog(videoPath, CurationSettings{}, false, parent);
	if (reviewDialog.exec() == QDialog::Accepted) {
		blog(LOG_INFO, "Upload review accepted. Starting Opus upload with reviewed ranges: %s",
		     videoPath.toUtf8().constData());
		upload_reviewed_video(parent, videoPath, reviewDialog.curationSettings());
	} else {
		blog(LOG_INFO, "Upload review canceled before Opus upload flow: %s", videoPath.toUtf8().constData());
	}
}
