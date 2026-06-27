#include "ui/curation-transcript-preparation.hpp"

#include "ui/ui-common.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <transcription/realtime-transcription-service.hpp>
#include <transcription/transcript-store.hpp>
#include <transcription/whisperx-settings.hpp>

#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QProgressDialog>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QWidget>
#include <QtGlobal>

#include <atomic>
#include <memory>
#include <utility>

static QString normalize_transcription_language(QString language)
{
	language = language.trimmed().toLower();
	if (language.isEmpty() || language == QStringLiteral("auto"))
		return QStringLiteral("auto");

	if (language == QStringLiteral("pt-br") || language == QStringLiteral("portuguese"))
		return QStringLiteral("pt");

	if (language == QStringLiteral("en-us") || language == QStringLiteral("english"))
		return QStringLiteral("en");

	return language;
}

static QObject *async_context(QWidget *parent)
{
	if (parent)
		return parent;

	return QCoreApplication::instance();
}

static void configure_foreground_progress_window(QProgressDialog *progressDialog, QWidget *parent, bool allowClose)
{
	if (!progressDialog)
		return;

	if (!parent) {
		configure_background_progress_window(progressDialog, allowClose);
		return;
	}

	QWidget *owner = parent->window() ? parent->window() : parent;
	progressDialog->setParent(owner);
	progressDialog->setAttribute(Qt::WA_QuitOnClose, false);
	progressDialog->setModal(true);
	progressDialog->setWindowModality(Qt::WindowModal);

	Qt::WindowFlags flags = Qt::Dialog | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
				Qt::MSWindowsFixedSizeDialogHint | Qt::WindowStaysOnTopHint;
	if (allowClose)
		flags |= Qt::WindowCloseButtonHint;
	progressDialog->setWindowFlags(flags);
}

static void show_foreground_progress_window(QProgressDialog *progressDialog, QWidget *parent, const QString &videoPath)
{
	if (!progressDialog)
		return;

	progressDialog->show();
	progressDialog->raise();
	progressDialog->activateWindow();

	QPointer<QProgressDialog> safeProgress(progressDialog);
	QTimer::singleShot(0, progressDialog, [safeProgress, videoPath]() {
		if (!safeProgress)
			return;

		safeProgress->show();
		safeProgress->raise();
		safeProgress->activateWindow();
		blog(LOG_INFO, "Transcription progress dialog shown in front of review dialog. video=%s",
		     videoPath.toUtf8().constData());
	});

	if (parent) {
		QPointer<QProgressDialog> safeProgressForParent(progressDialog);
		QTimer::singleShot(150, parent, [safeProgressForParent]() {
			if (!safeProgressForParent)
				return;

			safeProgressForParent->show();
			safeProgressForParent->raise();
			safeProgressForParent->activateWindow();
		});
	}
}

static void invoke_finished(QWidget *parent, std::function<void()> finishedCallback)
{
	if (!finishedCallback)
		return;

	QObject *context = async_context(parent);
	if (!context) {
		blog(LOG_WARNING, "Could not continue review flow because no Qt async context is available.");
		return;
	}

	if (QThread::currentThread() == context->thread()) {
		blog(LOG_INFO, "Continuing review flow on current Qt thread.");
		finishedCallback();
		return;
	}

	QPointer<QObject> safeContext(context);
	QMetaObject::invokeMethod(
		context,
		[safeContext, finishedCallback = std::move(finishedCallback)]() mutable {
			if (!safeContext || !finishedCallback) {
				blog(LOG_WARNING,
				     "Could not continue review flow because the Qt async context was destroyed.");
				return;
			}

			blog(LOG_INFO, "Continuing review flow on queued Qt callback.");
			finishedCallback();
		},
		Qt::QueuedConnection);
}

static void align_transcript_with_progress_dialog_async(
	QWidget *parent, const QString &videoPath, const QString &transcriptionLanguage,
	const RecordingTranscript &baseTranscript, std::function<void(RecordingTranscript, bool)> finishedCallback);

static void align_transcript_with_progress_dialog_async(QWidget *parent, const QString &videoPath,
							const QString &transcriptionLanguage,
							const RecordingTranscript &baseTranscript,
							std::function<void(RecordingTranscript, bool)> finishedCallback)
{
	QObject *context = async_context(parent);
	if (!context) {
		if (finishedCallback)
			finishedCallback(baseTranscript, false);
		return;
	}

	auto *progressDialog = new QProgressDialog(obsText("Status.WhisperXAligning"), obsText("Button.Cancel"), 0, 100,
						   parent ? parent : nullptr);
	progressDialog->setWindowTitle(obsText("Dialog.TranscribingAudioTitle"));
	configure_foreground_progress_window(progressDialog, parent, true);
	progressDialog->setMinimumDuration(0);
	progressDialog->setAutoClose(false);
	progressDialog->setAutoReset(false);
	progressDialog->setValue(90);
	show_foreground_progress_window(progressDialog, parent, videoPath);

	QPointer<QObject> safeContext(context);
	QPointer<QProgressDialog> safeProgress(progressDialog);
	auto cancelRequested = std::make_shared<std::atomic_bool>(false);
	auto operationFinished = std::make_shared<std::atomic_bool>(false);
	auto transcriptResult = std::make_shared<RecordingTranscript>(baseTranscript);

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

	auto *thread = QThread::create([videoPath, transcriptionLanguage, baseTranscript, cancelRequested,
					transcriptResult, reportProgress]() {
		if (transcriptResult) {
			*transcriptResult = global_realtime_transcription_service()->alignTranscriptWithWhisperX(
				videoPath, transcriptionLanguage, baseTranscript, reportProgress,
				[cancelRequested]() { return cancelRequested && cancelRequested->load(); });
		}
	});
	thread->setObjectName(QStringLiteral("ClipCropperWhisperXAlignmentThread"));

	auto requestCancel = [cancelRequested, operationFinished, safeProgress, videoPath]() {
		if (operationFinished && operationFinished->load())
			return;
		cancelRequested->store(true);
		if (safeProgress)
			safeProgress->setLabelText(obsText("Status.CancelingOperation"));
		blog(LOG_INFO, "User canceled WhisperX alignment before local curation scoring: %s",
		     videoPath.toUtf8().constData());
	};

	QObject::connect(progressDialog, &QProgressDialog::canceled, progressDialog, requestCancel);
	bind_progress_window_cancel(progressDialog, requestCancel);
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
				 if (operationFinished)
					 operationFinished->store(true);
				 if (safeProgress) {
					 mark_progress_window_finished(safeProgress);
					 safeProgress->close();
					 safeProgress->deleteLater();
				 }
				 if (finishedCallback)
					 finishedCallback(transcriptResult ? *transcriptResult : RecordingTranscript{},
							  canceled);
			 });
	thread->start();
}

static void transcribe_video_with_progress_dialog_after_cache_async(
	QWidget *parent, const QString &videoPath, const QString &transcriptionLanguage,
	std::function<void(RecordingTranscript, bool)> finishedCallback);

static void load_cached_transcript_async(QWidget *parent, const QString &videoPath, const QString &normalizedLanguage,
					 std::function<void(RecordingTranscript)> finishedCallback)
{
	QObject *context = async_context(parent);
	if (!context) {
		if (finishedCallback)
			finishedCallback({});
		return;
	}

	QPointer<QObject> safeContext(context);
	auto transcriptResult = std::make_shared<RecordingTranscript>();
	auto *thread = QThread::create([videoPath, normalizedLanguage, transcriptResult]() {
		const Transcription::WhisperXSettings whisperXSettings = Transcription::whisperXSettingsFromConfig();
		const bool whisperXPrimary = whisperXSettings.primaryTranscription();
		const QString cacheKey =
			whisperXPrimary ? TranscriptStore::keyForAlignedVideoPath(videoPath, normalizedLanguage)
					: TranscriptStore::keyForVideoPath(videoPath, normalizedLanguage);
		blog(LOG_INFO,
		     "Loading transcript cache on background thread before local curation scoring. video=%s language=%s cacheKey=%s mode=%s thread=%p",
		     videoPath.toUtf8().constData(), normalizedLanguage.toUtf8().constData(),
		     cacheKey.toUtf8().constData(), whisperXPrimary ? "whisperx_primary" : "default",
		     QThread::currentThread());
		if (transcriptResult) {
			*transcriptResult =
				whisperXPrimary
					? TranscriptStore::loadAlignedForVideoPath(videoPath, normalizedLanguage)
					: TranscriptStore::loadForVideoPath(videoPath, normalizedLanguage);
		}
	});
	thread->setObjectName(QStringLiteral("ClipCropperTranscriptCacheLoadThread"));

	QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
	QObject::connect(thread, &QThread::finished, context,
			 [safeContext, transcriptResult, finishedCallback = std::move(finishedCallback)]() mutable {
				 if (!safeContext)
					 return;

				 if (finishedCallback)
					 finishedCallback(transcriptResult ? *transcriptResult : RecordingTranscript{});
			 });
	thread->start();
}

static void transcribe_video_with_progress_dialog_async(QWidget *parent, const QString &videoPath,
							const QString &transcriptionLanguage,
							std::function<void(RecordingTranscript, bool)> finishedCallback)
{
	const QString normalizedLanguage = normalize_transcription_language(transcriptionLanguage);
	auto callback = std::make_shared<std::function<void(RecordingTranscript, bool)>>(std::move(finishedCallback));

	load_cached_transcript_async(
		parent, videoPath, normalizedLanguage,
		[parent, videoPath, normalizedLanguage, callback](RecordingTranscript cached) mutable {
			if (!cached.segments.isEmpty()) {
				const Transcription::WhisperXSettings whisperXSettings =
					Transcription::whisperXSettingsFromConfig();
				const bool whisperXPrimary = whisperXSettings.primaryTranscription();
				blog(LOG_INFO,
				     "Transcript cache hit before local curation scoring. Skipping transcription. video=%s segments=%d language=%s cacheKey=%s mode=%s wordAligned=%s",
				     videoPath.toUtf8().constData(), static_cast<int>(cached.segments.size()),
				     normalizedLanguage.toUtf8().constData(),
				     (whisperXPrimary
					      ? TranscriptStore::keyForAlignedVideoPath(videoPath, normalizedLanguage)
					      : TranscriptStore::keyForVideoPath(videoPath, normalizedLanguage))
					     .toUtf8()
					     .constData(),
				     whisperXPrimary ? "whisperx_primary" : "default",
				     cached.hasWordTimings() ? "true" : "false");
				if (whisperXSettings.alignmentOnly() && !cached.hasWordTimings()) {
					align_transcript_with_progress_dialog_async(
						parent, videoPath, normalizedLanguage, cached,
						[callback](RecordingTranscript aligned, bool canceled) mutable {
							if (callback && *callback)
								(*callback)(std::move(aligned), canceled);
						});
					return;
				}
				if (callback && *callback)
					invoke_finished(parent, [cached = std::move(cached), callback]() mutable {
						if (callback && *callback)
							(*callback)(std::move(cached), false);
					});
				return;
			}

			transcribe_video_with_progress_dialog_after_cache_async(
				parent, videoPath, normalizedLanguage,
				[callback](RecordingTranscript transcript, bool canceled) mutable {
					if (callback && *callback)
						(*callback)(std::move(transcript), canceled);
				});
		});
}

static void
transcribe_video_with_progress_dialog_after_cache_async(QWidget *parent, const QString &videoPath,
							const QString &transcriptionLanguage,
							std::function<void(RecordingTranscript, bool)> finishedCallback)
{
	const QString normalizedLanguage = normalize_transcription_language(transcriptionLanguage);
	const Transcription::WhisperXSettings whisperXSettings = Transcription::whisperXSettingsFromConfig();
	const bool whisperXPrimary = whisperXSettings.primaryTranscription();
	blog(LOG_INFO,
	     "Transcript cache miss before local curation scoring. Starting %s transcription if available. video=%s language=%s cacheKey=%s",
	     whisperXPrimary ? "WhisperX primary" : "Whisper/GPU", videoPath.toUtf8().constData(),
	     normalizedLanguage.toUtf8().constData(),
	     (whisperXPrimary ? TranscriptStore::keyForAlignedVideoPath(videoPath, normalizedLanguage)
			      : TranscriptStore::keyForVideoPath(videoPath, normalizedLanguage))
		     .toUtf8()
		     .constData());

	QObject *context = async_context(parent);
	if (!context) {
		if (finishedCallback)
			finishedCallback({}, false);
		return;
	}

	auto *progressDialog = new QProgressDialog(obsText("Status.TranscribingAudio"), obsText("Button.Cancel"), 0,
						   100, parent ? parent : nullptr);
	progressDialog->setWindowTitle(obsText("Dialog.TranscribingAudioTitle"));
	configure_foreground_progress_window(progressDialog, parent, true);
	progressDialog->setMinimumDuration(0);
	progressDialog->setAutoClose(false);
	progressDialog->setAutoReset(false);
	progressDialog->setValue(0);
	show_foreground_progress_window(progressDialog, parent, videoPath);

	QPointer<QObject> safeContext(context);
	QPointer<QProgressDialog> safeProgress(progressDialog);
	auto cancelRequested = std::make_shared<std::atomic_bool>(false);
	auto transcriptionCompleted = std::make_shared<std::atomic_bool>(false);
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

	auto *thread = QThread::create([videoPath, modelPath, normalizedLanguage, cancelRequested,
					transcriptionCompleted, transcriptResult, reportProgress]() {
		reportProgress(0, obsText("Status.PreparingTranscription"));

		if (transcriptResult) {
			*transcriptResult = global_realtime_transcription_service()->transcribeVideoFile(
				videoPath, modelPath, normalizedLanguage, reportProgress,
				[cancelRequested]() { return cancelRequested && cancelRequested->load(); });
		}

		if (transcriptionCompleted)
			transcriptionCompleted->store(true);
	});
	thread->setObjectName(QStringLiteral("ClipCropperTranscriptionThread"));

	auto requestTranscriptionCancel = [cancelRequested, transcriptionCompleted, operationFinished, safeProgress,
					   videoPath]() {
		if ((operationFinished && operationFinished->load()) ||
		    (transcriptionCompleted && transcriptionCompleted->load()))
			return;

		cancelRequested->store(true);
		if (safeProgress)
			safeProgress->setLabelText(obsText("Status.CancelingOperation"));
		blog(LOG_INFO, "User canceled on-demand transcription before local curation scoring: %s",
		     videoPath.toUtf8().constData());
	};

	QObject::connect(progressDialog, &QProgressDialog::canceled, progressDialog, requestTranscriptionCancel);
	bind_progress_window_cancel(progressDialog, requestTranscriptionCancel);
	QObject::connect(progressDialog, &QObject::destroyed, progressDialog, [cancelRequested, operationFinished]() {
		if (!operationFinished || !operationFinished->load())
			cancelRequested->store(true);
	});

	QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
	QObject::connect(
		thread, &QThread::finished, context,
		[safeContext, safeProgress, transcriptResult, cancelRequested, operationFinished, videoPath,
		 finishedCallback = std::move(finishedCallback)]() mutable {
			if (!safeContext)
				return;

			const bool cancelRequestedBeforeCompletion = cancelRequested && cancelRequested->load();
			const RecordingTranscript transcript = transcriptResult ? *transcriptResult
										: RecordingTranscript{};
			const bool canceled = cancelRequestedBeforeCompletion && transcript.segments.isEmpty();
			if (cancelRequestedBeforeCompletion && !transcript.segments.isEmpty()) {
				blog(LOG_INFO,
				     "Ignoring late transcription cancel because a usable transcript was produced. video=%s segments=%d",
				     videoPath.toUtf8().constData(), static_cast<int>(transcript.segments.size()));
			}

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

void ensure_transcript_for_curation_async(QWidget *parent, const QString &videoPath,
					  const CurationSettings &curationSettings, bool transcribeOnDemand,
					  std::function<void(RecordingTranscript, bool)> finishedCallback)
{
	const QString transcriptionLanguage = normalize_transcription_language(curationSettings.transcriptionLanguage);
	if (transcribeOnDemand) {
		transcribe_video_with_progress_dialog_async(parent, videoPath, transcriptionLanguage,
							    std::move(finishedCallback));
		return;
	}

	load_cached_transcript_async(
		parent, videoPath, transcriptionLanguage,
		[parent, videoPath, transcriptionLanguage,
		 finishedCallback = std::move(finishedCallback)](RecordingTranscript transcript) mutable {
			blog(transcript.segments.isEmpty() ? LOG_WARNING : LOG_INFO,
			     "Transcript cache %s before local curation scoring. video=%s segments=%d language=%s cacheKey=%s",
			     transcript.segments.isEmpty() ? "miss" : "hit", videoPath.toUtf8().constData(),
			     static_cast<int>(transcript.segments.size()), transcriptionLanguage.toUtf8().constData(),
			     TranscriptStore::keyForVideoPath(videoPath, transcriptionLanguage).toUtf8().constData());

			if (finishedCallback)
				invoke_finished(parent, [transcript = std::move(transcript),
							 finishedCallback = std::move(finishedCallback)]() mutable {
					finishedCallback(std::move(transcript), false);
				});
		});
}
