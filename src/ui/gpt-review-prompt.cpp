#include "ui/gpt-review-prompt.hpp"

#include "ui/ui-common.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <gpt/gpt-prompt-client.hpp>
#include <models/transcript.hpp>
#include <transcription/realtime-transcription-service.hpp>
#include <transcription/transcript-store.hpp>
#include <utils/config.hpp>

#include <QCoreApplication>
#include <QMessageBox>
#include <QMetaObject>
#include <QObject>
#include <QProgressDialog>
#include <QPointer>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>

static const QString &title = clipCropperTitle();

static bool is_openai_model_enabled()
{
	const QString model = get_openai_model().trimmed();
	return !model.isEmpty() && model != OPENAI_MODEL_DISABLED;
}

static QObject *async_context(QWidget *parent)
{
	if (parent)
		return parent;

	return QCoreApplication::instance();
}

static void invoke_finished(QWidget *parent, std::function<void()> finishedCallback)
{
	if (!finishedCallback)
		return;

	QObject *context = async_context(parent);
	if (!context) {
		obs_log(LOG_WARNING, "Could not continue review flow because no Qt async context is available.");
		return;
	}

	if (QThread::currentThread() == context->thread()) {
		obs_log(LOG_INFO, "Continuing review flow on current Qt thread.");
		finishedCallback();
		return;
	}

	QPointer<QObject> safeContext(context);
	QMetaObject::invokeMethod(
		context,
		[safeContext, finishedCallback = std::move(finishedCallback)]() mutable {
			if (!safeContext || !finishedCallback) {
				obs_log(LOG_WARNING,
					"Could not continue review flow because the Qt async context was destroyed.");
				return;
			}

			obs_log(LOG_INFO, "Continuing review flow on queued Qt callback.");
			finishedCallback();
		},
		Qt::QueuedConnection);
}

static void invoke_prompt_finished(QWidget *parent, std::function<void(QString)> finishedCallback, QString prompt)
{
	if (!finishedCallback)
		return;

	invoke_finished(parent, [finishedCallback = std::move(finishedCallback), prompt = std::move(prompt)]() mutable {
		finishedCallback(prompt);
	});
}

static QVector<ClipDuration> valid_prompt_ranges(const CurationSettings &curationSettings)
{
	QVector<ClipDuration> ranges;

	for (const ClipDuration &range : curationSettings.clipDurations) {
		if (range.endSec > range.startSec)
			ranges.append(range);
	}

	if (ranges.isEmpty() && curationSettings.rangeEndSec > curationSettings.rangeStartSec)
		ranges.append({curationSettings.rangeStartSec, curationSettings.rangeEndSec});

	return ranges;
}

static RecordingTranscript transcript_for_curation_ranges(const RecordingTranscript &transcript,
							  const CurationSettings &curationSettings)
{
	const QVector<ClipDuration> ranges = valid_prompt_ranges(curationSettings);
	if (ranges.isEmpty())
		return transcript;

	RecordingTranscript filtered;
	filtered.videoFileName = transcript.videoFileName;
	filtered.videoPath = transcript.videoPath;
	filtered.segments.reserve(transcript.segments.size());

	for (const TranscriptSegment &segment : transcript.segments) {
		if (segment.text.trimmed().isEmpty())
			continue;

		for (const ClipDuration &range : ranges) {
			const double startSec = std::max(segment.startSec, range.startSec);
			const double endSec = std::min(segment.endSec, range.endSec);

			if (endSec <= startSec)
				continue;

			TranscriptSegment filteredSegment = segment;
			filteredSegment.startSec = startSec;
			filteredSegment.endSec = endSec;
			filtered.segments.append(filteredSegment);
			break;
		}
	}

	obs_log(LOG_INFO,
		"Filtered transcript for GPT curation prompt. originalSegments=%d filteredSegments=%d ranges=%d",
		transcript.segments.size(), filtered.segments.size(), ranges.size());

	return filtered;
}

static void transcribe_video_with_progress_dialog_async(QWidget *parent, const QString &videoPath,
							std::function<void(RecordingTranscript, bool)> finishedCallback)
{
	RecordingTranscript cached = TranscriptStore::loadForVideoPath(videoPath);
	if (!cached.segments.isEmpty()) {
		obs_log(LOG_INFO, "Transcript cache hit before GPT curation prompt: %s",
			videoPath.toUtf8().constData());
		if (finishedCallback)
			invoke_finished(parent, [cached, finishedCallback = std::move(finishedCallback)]() mutable {
				finishedCallback(cached, false);
			});
		return;
	}

	QObject *context = async_context(parent);
	if (!context) {
		if (finishedCallback)
			finishedCallback({}, true);
		return;
	}

	QPointer<QObject> safeContext(context);

	auto *progressDialog =
		new QProgressDialog(obsText("Status.PreparingTranscription"), obsText("Button.Cancel"), 0, 100, parent);
	QPointer<QProgressDialog> safeProgress(progressDialog);
	progressDialog->setWindowTitle(obsText("Dialog.TranscribingAudioTitle"));
	configure_background_progress_window(progressDialog, true);
	progressDialog->setMinimumDuration(0);
	progressDialog->setAutoClose(false);
	progressDialog->setAutoReset(false);
	progressDialog->setValue(0);
	progressDialog->show();

	auto cancelRequested = std::make_shared<std::atomic_bool>(false);
	auto operationFinished = std::make_shared<std::atomic_bool>(false);
	auto transcriptResult = std::make_shared<RecordingTranscript>();
	const QString modelPath = resolve_whisper_model_path();

	auto reportProgress = [safeProgress](int progress, const QString &message) {
		if (!safeProgress)
			return;

		QMetaObject::invokeMethod(
			safeProgress,
			[safeProgress, progress, message]() {
				if (!safeProgress)
					return;

				if (!message.trimmed().isEmpty())
					safeProgress->setLabelText(message);
				safeProgress->setValue(qBound(0, progress, 100));
			},
			Qt::QueuedConnection);
	};

	auto *thread = QThread::create([videoPath, modelPath, cancelRequested, transcriptResult, reportProgress]() {
		reportProgress(0, obsText("Status.PreparingTranscription"));

		if (transcriptResult) {
			*transcriptResult = global_realtime_transcription_service()->transcribeVideoFile(
				videoPath, modelPath, reportProgress,
				[cancelRequested]() { return cancelRequested && cancelRequested->load(); });
		}
	});
	thread->setObjectName(QStringLiteral("ClipCropperTranscriptionThread"));

	auto requestTranscriptionCancel = [cancelRequested, operationFinished, safeProgress, videoPath]() {
		if (operationFinished && operationFinished->load())
			return;

		cancelRequested->store(true);
		if (safeProgress)
			safeProgress->setLabelText(obsText("Status.CancelingOperation"));
		obs_log(LOG_INFO, "User canceled on-demand transcription before GPT curation prompt: %s",
			videoPath.toUtf8().constData());
	};

	QObject::connect(progressDialog, &QProgressDialog::canceled, progressDialog, requestTranscriptionCancel);
	bind_progress_window_cancel(progressDialog, requestTranscriptionCancel);
	QObject::connect(progressDialog, &QObject::destroyed, progressDialog, [cancelRequested, operationFinished]() {
		if (!operationFinished || !operationFinished->load())
			cancelRequested->store(true);
	});

	QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
	QObject::connect(thread, &QThread::finished, context,
			 [safeContext, safeProgress, transcriptResult, cancelRequested, operationFinished,
			  finishedCallback = std::move(finishedCallback)]() mutable {
				 if (!safeContext)
					 return;

				 const bool canceled = cancelRequested && cancelRequested->load();
				 const RecordingTranscript transcript = transcriptResult ? *transcriptResult
											 : RecordingTranscript{};

				 if (operationFinished)
					 operationFinished->store(true);

				 if (safeProgress) {
					 mark_progress_window_finished(safeProgress);
					 safeProgress->close();
					 safeProgress->deleteLater();
				 }

				 if (finishedCallback)
					 finishedCallback(transcript, canceled);
			 });

	thread->start();
}

static void generate_gpt_prompt_with_progress_dialog_async(QWidget *parent, const QString &videoPath,
							   const RecordingTranscript &transcript,
							   const CurationSettings &curationSettings,
							   std::function<void(QString)> finishedCallback)
{
	const RecordingTranscript promptTranscript = transcript_for_curation_ranges(transcript, curationSettings);
	if (promptTranscript.segments.isEmpty()) {
		obs_log(LOG_WARNING,
			"No transcript segments inside the selected curation ranges for %s. Skipping GPT prompt generation.",
			videoPath.toUtf8().constData());
		QMessageBox::warning(parent, title, obsText("Message.TranscriptUnavailableForGpt"));
		invoke_prompt_finished(parent, std::move(finishedCallback), {});
		return;
	}

	auto *progress =
		new QProgressDialog(obsText("Status.GeneratingGptPrompt"), obsText("Button.Cancel"), 0, 0, parent);
	QPointer<QProgressDialog> safeProgress(progress);
	progress->setWindowTitle(obsText("Dialog.GeneratingGptPromptTitle"));
	configure_background_progress_window(progress, true);
	progress->setMinimumDuration(0);
	progress->setAutoClose(false);
	progress->setAutoReset(false);
	progress->show();

	auto *client = new GptPromptClient(get_openai_api_key(), get_openai_model(), progress);
	QPointer<GptPromptClient> safeClient(client);
	auto canceled = std::make_shared<std::atomic_bool>(false);
	auto callback = std::make_shared<std::function<void(QString)>>(std::move(finishedCallback));

	auto requestGptCancel = [safeClient, safeProgress, canceled]() {
		canceled->store(true);
		if (safeProgress)
			safeProgress->setLabelText(obsText("Status.CancelingOperation"));
		if (safeClient)
			safeClient->cancel();
	};

	QObject::connect(progress, &QProgressDialog::canceled, client, requestGptCancel);
	bind_progress_window_cancel(progress, requestGptCancel);

	QObject::connect(client, &GptPromptClient::promptReady, progress,
			 [safeProgress, callback](const QString &prompt) {
				 const QString generatedPrompt = prompt.trimmed();
				 if (safeProgress) {
					 mark_progress_window_finished(safeProgress);
					 safeProgress->close();
					 safeProgress->deleteLater();
				 }

				 if (callback && *callback)
					 (*callback)(generatedPrompt);
			 });

	QObject::connect(client, &GptPromptClient::promptFailed, progress,
			 [safeProgress, canceled, callback](const QString &message) {
				 if (!canceled->load() && message != QStringLiteral("Canceled"))
					 obs_log(LOG_WARNING, "GPT curation prompt generation failed: %s",
						 message.toUtf8().constData());

				 if (safeProgress) {
					 mark_progress_window_finished(safeProgress);
					 safeProgress->close();
					 safeProgress->deleteLater();
				 }

				 if (callback && *callback)
					 (*callback)({});
			 });

	obs_log(LOG_INFO,
		"Sending range-filtered transcript to GPT after review. video=%s originalSegments=%d promptSegments=%d model=%s",
		videoPath.toUtf8().constData(), transcript.segments.size(), promptTranscript.segments.size(),
		get_openai_model().toUtf8().constData());
	client->createOpusPromptAsync(videoPath, promptTranscript, curationSettings);
}

void generate_custom_prompt_for_curation_async(QWidget *parent, const QString &videoPath,
					       const CurationSettings &curationSettings, bool transcribeOnDemand,
					       std::function<void(QString)> finishedCallback)
{
	auto callback = std::make_shared<std::function<void(QString)>>(std::move(finishedCallback));
	auto finish = [parent, callback](QString prompt) mutable {
		if (!callback || !*callback)
			return;

		auto finalCallback = std::move(*callback);
		*callback = {};
		invoke_prompt_finished(parent, std::move(finalCallback), std::move(prompt));
	};

	if (!curationSettings.aiPrompt.trimmed().isEmpty()) {
		obs_log(LOG_INFO, "Using custom Opus prompt provided in review dialog: %s",
			videoPath.toUtf8().constData());
		finish(curationSettings.aiPrompt.trimmed());
		return;
	}

	const QString openAiApiKey = get_openai_api_key();
	if (openAiApiKey.trimmed().isEmpty()) {
		obs_log(LOG_WARNING, "OpenAI API key is empty. Skipping GPT curation prompt generation.");
		finish({});
		return;
	}

	if (!is_openai_model_enabled()) {
		obs_log(LOG_INFO, "OpenAI model is disabled. Skipping GPT curation prompt generation.");
		finish({});
		return;
	}

	auto continueWithTranscript = [parent, videoPath, curationSettings,
				       finish](const RecordingTranscript &transcript,
					       bool transcriptionCanceled) mutable {
		if (transcriptionCanceled) {
			obs_log(LOG_INFO,
				"GPT curation prompt generation skipped because transcription was canceled: %s",
				videoPath.toUtf8().constData());
			finish({});
			return;
		}

		if (transcript.segments.isEmpty()) {
			obs_log(LOG_WARNING, "No transcript available for %s. Skipping GPT curation prompt generation.",
				videoPath.toUtf8().constData());
			QMessageBox::warning(parent, title, obsText("Message.TranscriptUnavailableForGpt"));
			finish({});
			return;
		}

		generate_gpt_prompt_with_progress_dialog_async(parent, videoPath, transcript, curationSettings, finish);
	};

	obs_log(LOG_INFO, "Loading transcript after review before GPT curation prompt generation: %s",
		videoPath.toUtf8().constData());
	const RecordingTranscript transcript = TranscriptStore::loadForVideoPath(videoPath);
	if (!transcript.segments.isEmpty()) {
		continueWithTranscript(transcript, false);
		return;
	}

	if (!transcribeOnDemand) {
		obs_log(LOG_WARNING, "No cached transcript available for %s. Skipping GPT curation prompt generation.",
			videoPath.toUtf8().constData());
		QMessageBox::warning(parent, title, obsText("Message.TranscriptUnavailableForGpt"));
		finish({});
		return;
	}

	obs_log(LOG_INFO, "No cached transcript found. Starting on-demand transcription after review: %s",
		videoPath.toUtf8().constData());
	transcribe_video_with_progress_dialog_async(parent, videoPath, std::move(continueWithTranscript));
}

void generate_custom_prompt_before_review_async(QWidget *parent, const QString &videoPath, bool transcribeOnDemand,
						std::function<void()> finishedCallback)
{
	Q_UNUSED(videoPath);
	Q_UNUSED(transcribeOnDemand);
	obs_log(LOG_INFO,
		"Legacy pre-review GPT flow was skipped. GPT prompt is now generated after review ranges are confirmed.");
	invoke_finished(parent, std::move(finishedCallback));
}
