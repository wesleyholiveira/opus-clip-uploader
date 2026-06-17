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

static void generate_prompt_and_upload(QWidget *parent, const QStringList &paths, const QString &apiKey,
				       const CurationSettings &curationSettings)
{
	if (paths.isEmpty()) {
		clear_pending_recording_paths();
		return;
	}

	auto startUpload = [parent, paths, apiKey, curationSettings](const QString &generatedPrompt) mutable {
		CurationSettings finalSettings = curationSettings;
		const bool promptWasGenerated = finalSettings.aiPrompt.trimmed().isEmpty() &&
						!generatedPrompt.trimmed().isEmpty();

		if (promptWasGenerated)
			finalSettings.aiPrompt = generatedPrompt.trimmed();

		if (promptWasGenerated) {
			obs_log(LOG_INFO, "Opening GPT-generated Opus prompt confirmation dialog before upload: %s",
				paths.first().toUtf8().constData());

			UploadReviewDialog promptReviewDialog(paths.first(), finalSettings, true, parent);
			if (promptReviewDialog.exec() != QDialog::Accepted) {
				obs_log(LOG_INFO, "GPT-generated Opus prompt confirmation canceled before upload: %s",
					paths.first().toUtf8().constData());
				clear_pending_recording_paths();
				return;
			}

			finalSettings = promptReviewDialog.curationSettings();
		}

		obs_log(LOG_INFO, "Opening Opus upload progress dialog after review/GPT flow: %s",
			paths.first().toUtf8().constData());
		start_reviewed_upload(parent, paths, apiKey, finalSettings);
	};

	if (!is_openai_model_enabled() || !curationSettings.aiPrompt.trimmed().isEmpty()) {
		if (!is_openai_model_enabled())
			obs_log(LOG_INFO, "OpenAI model is disabled. Skipping GPT prompt generation after review.");
		startUpload(curationSettings.aiPrompt);
		return;
	}

	generate_custom_prompt_for_curation_async(parent, paths.first(), curationSettings, true, startUpload);
}

static void open_review_and_upload(QWidget *parent, const QStringList &paths, const QString &apiKey)
{
	if (paths.isEmpty()) {
		clear_pending_recording_paths();
		return;
	}

	obs_log(LOG_INFO, "Opening upload review dialog before GPT/Opus upload flow: %s",
		paths.first().toUtf8().constData());

	UploadReviewDialog reviewDialog(paths.first(), parent);

	if (reviewDialog.exec() != QDialog::Accepted) {
		obs_log(LOG_INFO, "Upload review canceled before GPT/Opus upload flow: %s",
			paths.first().toUtf8().constData());
		clear_pending_recording_paths();
		return;
	}

	const CurationSettings curationSettings = reviewDialog.curationSettings();
	obs_log(LOG_INFO, "Upload review accepted. Generating GPT prompt after review ranges were confirmed: %s",
		paths.first().toUtf8().constData());
	generate_prompt_and_upload(parent, paths, apiKey, curationSettings);
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
	confirmDialog.resize(confirmDialog.sizeHint());

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

	open_review_and_upload(parent, paths, apiKey);
}
