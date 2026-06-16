#include "ui/ui.hpp"

#include "ui/gpt-review-prompt.hpp"
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
static bool confirmDialogActive = false;

static bool is_openai_model_enabled()
{
	const QString model = get_openai_model();
	return !model.isEmpty() && model != OPENAI_MODEL_DISABLED;
}

static void open_review_and_upload(QWidget *parent, const QStringList &paths, const QString &apiKey)
{
	if (paths.isEmpty()) {
		clear_pending_recording_paths();
		return;
	}

	obs_log(LOG_INFO, "Opening upload review dialog after transcript/GPT flow: %s",
		paths.first().toUtf8().constData());

	UploadReviewDialog reviewDialog(paths.first(), parent);

	if (reviewDialog.exec() != QDialog::Accepted) {
		obs_log(LOG_INFO, "Upload review canceled after transcript/GPT flow: %s",
			paths.first().toUtf8().constData());
		clear_pending_recording_paths();
		return;
	}

	obs_log(LOG_INFO, "Upload review accepted. Opening Opus upload progress dialog: %s",
		paths.first().toUtf8().constData());

	const CurationSettings curationSettings = reviewDialog.curationSettings();

	auto *progressDialog = new BackgroundProgressDialog(parent);
	progressDialog->setWindowTitle(title);
	configure_background_progress_window(progressDialog, true);
	QObject::connect(progressDialog, &QDialog::finished, progressDialog, &QObject::deleteLater);

	QVBoxLayout *progressMainLayout = new QVBoxLayout(progressDialog);
	progressMainLayout->setContentsMargins(22, 16, 22, 16);
	progressMainLayout->setSpacing(8);

	auto *progressContainer = new QFrame(progressDialog);
	progressContainer->setFrameShape(QFrame::NoFrame);

	auto *progressLayout = new QVBoxLayout(progressContainer);
	progressLayout->setContentsMargins(0, 8, 0, 8);
	progressLayout->setSpacing(8);

	QLabel *uploadStatusLabel = new QLabel(QString(), progressContainer);
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
	progressMainLayout->addWidget(progressContainer);

	QPushButton *hiddenUploadButton = new QPushButton(progressDialog);
	QPushButton *cancelUploadButton = new QPushButton(obsText("Button.Cancel"), progressDialog);
	hiddenUploadButton->hide();
	progressMainLayout->addWidget(cancelUploadButton);

	progressDialog->setMinimumWidth(560);
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

void open_confirm_dialog_impl(void *private_data)
{

	UNUSED_PARAMETER(private_data);

	if (confirmDialogActive) {
		obs_log(LOG_INFO, "Upload confirm dialog is already open. Ignoring duplicate request.");
		return;
	}

	const QStringList paths = get_recording_paths_for_upload();

	if (paths.isEmpty()) {
		obs_log(LOG_INFO, "No pending recording paths. Upload confirm dialog will not be shown.");
		return;
	}

	confirmDialogActive = true;
	struct ConfirmDialogGuard {
		~ConfirmDialogGuard() { confirmDialogActive = false; }
	} guard;

	QWidget *parent = reinterpret_cast<QWidget *>(obs_frontend_get_main_window());

	QDialog confirmDialog(parent);
	confirmDialog.setWindowTitle(title + " - " + obsText("Dialog.ConfirmUploadTitle"));

	QVBoxLayout *mainLayout = new QVBoxLayout(&confirmDialog);
	mainLayout->setContentsMargins(22, 16, 22, 16);
	mainLayout->setSpacing(8);

	QLabel *label = new QLabel(obsText("Dialog.ConfirmUploadQuestion"), &confirmDialog);
	label->setWordWrap(true);
	mainLayout->addWidget(label);

	QPushButton *btnUpload = new QPushButton(obsText("Button.Yes"), &confirmDialog);
	QPushButton *btnCancel = new QPushButton(obsText("Button.No"), &confirmDialog);

	btnUpload->setMinimumHeight(32);
	btnCancel->setMinimumHeight(32);

	QHBoxLayout *btnLayout = new QHBoxLayout();
	btnLayout->setSpacing(12);
	btnLayout->addWidget(btnUpload);
	btnLayout->addWidget(btnCancel);

	mainLayout->addLayout(btnLayout);

	confirmDialog.setMinimumWidth(520);
	confirmDialog.adjustSize();
	confirmDialog.setFixedSize(confirmDialog.sizeHint());

	bool uploadRequested = false;

	QObject::connect(btnCancel, &QPushButton::clicked, &confirmDialog, [&confirmDialog]() {
		obs_log(LOG_INFO, "Fechando Dialog de Upload");
		confirmDialog.reject();
	});

	QObject::connect(btnUpload, &QPushButton::clicked, &confirmDialog, [&confirmDialog, &uploadRequested]() {
		uploadRequested = true;
		confirmDialog.accept();
	});

	if (confirmDialog.exec() != QDialog::Accepted || !uploadRequested) {
		clear_pending_recording_paths();
		return;
	}

	const QString apiKey = get_opus_api_key();

	if (apiKey.trimmed().isEmpty()) {
		clear_pending_recording_paths();
		QMessageBox::warning(parent, title, obsText("Message.ConfigureApiKeyInSettings"));
		open_settings(nullptr);
		return;
	}

	if (is_openai_model_enabled()) {
		generate_custom_prompt_before_review_async(parent, paths.first(), true, [parent, paths, apiKey]() {
			open_review_and_upload(parent, paths, apiKey);
		});
		return;
	}

	obs_log(LOG_INFO, "OpenAI model is disabled. Skipping transcript wait and GPT prompt generation.");
	open_review_and_upload(parent, paths, apiKey);
}
