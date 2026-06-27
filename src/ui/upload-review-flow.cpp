#include "ui/ui.hpp"

#include "ui/ui-common.hpp"
#include "ui/upload-flow.hpp"
#include "ui/upload-review-dialog.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <utils/config.hpp>

#include <QDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QObject>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

static const QString &title = clipCropperTitle();
static bool reviewFlowActive = false;

static void start_reviewed_upload(QWidget *parent, const QStringList &paths, const QString &apiKey,
				  const CurationSettings &curationSettings)
{
	auto *progressDialog = new BackgroundProgressDialog(parent);
	progressDialog->setWindowTitle(title);
	configure_background_progress_window(progressDialog, true);
	QObject::connect(progressDialog, &QDialog::finished, progressDialog, &QObject::deleteLater);

	QVBoxLayout *progressMainLayout = new QVBoxLayout(progressDialog);
	progressMainLayout->setContentsMargins(20, 14, 20, 16);
	progressMainLayout->setSpacing(8);

	auto *progressContainer = new QFrame(progressDialog);
	progressContainer->setFrameShape(QFrame::NoFrame);

	auto *progressLayout = new QVBoxLayout(progressContainer);
	progressLayout->setContentsMargins(0, 0, 0, 0);
	progressLayout->setSpacing(6);

	QLabel *uploadStatusLabel = new QLabel(QString(), progressContainer);
	uploadStatusLabel->setWordWrap(false);
	uploadStatusLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	uploadStatusLabel->setMinimumHeight(20);

	QProgressBar *progressBar = new QProgressBar(progressContainer);
	progressBar->setRange(0, 100);
	progressBar->setValue(0);
	progressBar->setFixedHeight(22);
	progressBar->setTextVisible(false);

	progressLayout->addWidget(uploadStatusLabel);
	progressLayout->addWidget(progressBar);
	progressMainLayout->addWidget(progressContainer);

	QPushButton *hiddenUploadButton = new QPushButton(progressDialog);
	QPushButton *cancelUploadButton = new QPushButton(obsText("Button.Cancel"), progressDialog);
	hiddenUploadButton->hide();
	progressMainLayout->addWidget(cancelUploadButton);

	progressDialog->setMinimumWidth(540);
	progressDialog->adjustSize();

	QTimer::singleShot(0, progressDialog,
			   [progressDialog, hiddenUploadButton, cancelUploadButton, progressBar, uploadStatusLabel,
			    apiKey, curationSettings]() {
				   start_upload(progressDialog, hiddenUploadButton, cancelUploadButton, progressBar,
						uploadStatusLabel, apiKey, curationSettings);
			   });

	progressDialog->show();
	progressDialog->raise();
}

static void upload_after_review(QWidget *parent, const QStringList &paths, const QString &apiKey,
				const CurationSettings &curationSettings)
{
	if (paths.isEmpty()) {
		clear_pending_recording_paths();
		return;
	}

	blog(LOG_INFO, "Opening Opus upload progress dialog after review: %s", paths.first().toUtf8().constData());
	start_reviewed_upload(parent, paths, apiKey, curationSettings);
}

static void open_review_and_upload_with_settings(QWidget *parent, const QStringList &paths, const QString &apiKey,
						 const CurationSettings &initialSettings)
{
	if (paths.isEmpty()) {
		clear_pending_recording_paths();
		return;
	}

	blog(LOG_INFO, "Opening upload review dialog directly before Opus upload flow: %s",
	     paths.first().toUtf8().constData());

	UploadReviewDialog reviewDialog(paths.first(), initialSettings, false, parent);

	if (reviewDialog.exec() != QDialog::Accepted) {
		blog(LOG_INFO, "Upload review canceled before Opus upload flow: %s", paths.first().toUtf8().constData());
		clear_pending_recording_paths();
		return;
	}

	const CurationSettings curationSettings = reviewDialog.curationSettings();
	blog(LOG_INFO, "Upload review accepted. Starting Opus upload with reviewed ranges: %s",
	     paths.first().toUtf8().constData());
	upload_after_review(parent, paths, apiKey, curationSettings);
}

static void open_review_and_upload(QWidget *parent, const QStringList &paths, const QString &apiKey)
{
	if (paths.isEmpty()) {
		clear_pending_recording_paths();
		return;
	}

	open_review_and_upload_with_settings(parent, paths, apiKey, CurationSettings{});
}

void open_upload_review_flow_impl(void *private_data)
{
	UNUSED_PARAMETER(private_data);

	if (reviewFlowActive) {
		blog(LOG_INFO, "Upload review dialog is already open. Ignoring duplicate request.");
		return;
	}

	const QStringList paths = get_recording_paths_for_upload();
	if (paths.isEmpty()) {
		blog(LOG_INFO, "No pending recording paths. Review dialog will not be shown.");
		return;
	}

	QWidget *parent = reinterpret_cast<QWidget *>(obs_frontend_get_main_window());
	const QString apiKey = get_opus_api_key();
	if (apiKey.trimmed().isEmpty()) {
		clear_pending_recording_paths();
		QMessageBox::warning(parent, title, obsText("Message.ConfigureApiKeyInSettings"));
		open_settings(nullptr);
		return;
	}

	reviewFlowActive = true;
	struct ReviewDialogGuard {
		~ReviewDialogGuard() { reviewFlowActive = false; }
	} guard;

	open_review_and_upload(parent, paths, apiKey);
}
