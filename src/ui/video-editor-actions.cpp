#include "ui/ui.hpp"

#include "ui/ui-common.hpp"

#include "ui/gpt-review-prompt.hpp"
#include "ui/upload-flow.hpp"
#include "ui/upload-review-dialog.hpp"
#include "ui/video-marker-editor.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QDialog>
#include <QFileDialog>
#include <QFrame>
#include <QLabel>
#include <QMessageBox>
#include <QObject>
#include <QProgressBar>
#include <QPointer>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

static const QString &title = clipCropperTitle();

static bool is_openai_model_enabled()
{
	const QString model = get_openai_model().trimmed();
	return !model.isEmpty() && model != OPENAI_MODEL_DISABLED;
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

	auto *uploadDialog = new BackgroundProgressDialog(parent);
	uploadDialog->setWindowTitle(title + " - " + obsText("Dialog.ConfirmUploadTitle"));
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

	auto *btnUpload = new QPushButton(uploadDialog);
	auto *btnCancel = new QPushButton(obsText("Button.Cancel"), uploadDialog);
	btnUpload->hide();
	mainLayout->addWidget(btnCancel);

	uploadDialog->setMinimumWidth(540);
	uploadDialog->adjustSize();

	QTimer::singleShot(0, uploadDialog,
			   [uploadDialog, btnUpload, btnCancel, progressBar, uploadStatusLabel, apiKey,
			    curationSettings]() {
				   start_upload(uploadDialog, btnUpload, btnCancel, progressBar, uploadStatusLabel,
						apiKey, curationSettings);
			   });

	uploadDialog->show();
	uploadDialog->raise();
}

void open_video_editor_impl(void *private_data)
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

		QPointer<QDialog> editorDialog(&dialog);
		QPointer<QWidget> reviewParent(dialog.parentWidget());
		dialog.hide();

		auto openReview = [editorDialog, reviewParent, videoPath]() {
			QWidget *parentWidget = reviewParent
							? reviewParent.data()
							: reinterpret_cast<QWidget *>(obs_frontend_get_main_window());

			blog(LOG_INFO, "Opening upload review dialog after transcript/GPT flow: %s",
			     videoPath.toUtf8().constData());

			UploadReviewDialog reviewDialog(videoPath, parentWidget);
			if (reviewDialog.exec() == QDialog::Accepted) {
				blog(LOG_INFO, "Upload review accepted. Opening Opus upload progress dialog: %s",
				     videoPath.toUtf8().constData());
				const CurationSettings curationSettings = reviewDialog.curationSettings();
				upload_reviewed_video(parentWidget, videoPath, curationSettings);
			} else {
				blog(LOG_INFO, "Upload review canceled after transcript/GPT flow: %s",
				     videoPath.toUtf8().constData());
			}

			if (editorDialog)
				editorDialog->accept();
		};

		if (is_openai_model_enabled()) {
			generate_custom_prompt_before_review_async(reviewParent, videoPath, true, openReview);
			return;
		}

		blog(LOG_INFO, "OpenAI model is disabled. Skipping transcript wait and GPT prompt generation.");
		openReview();
	});

	QTimer::singleShot(0, editor, [editor]() { editor->toggleFullScreen(); });

	dialog.exec();
}
