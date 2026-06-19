#include "ui/gpt-review-prompt.hpp"

#include "ui/ui-common.hpp"

#include "curation/range/curation-range-strategy.hpp"

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
#include <cmath>
#include <memory>
#include <utility>

static const QString &title = clipCropperTitle();
static constexpr const char *CONFIG_GPT_TRANSCRIPT_CONTEXT_PADDING_SEC = "gpt_transcript_context_padding_sec";
static constexpr double DEFAULT_GPT_TRANSCRIPT_CONTEXT_PADDING_SEC = 60.0;

static QString gptPromptBlockedReason(const QString &details = {})
{
	const QString base = QStringLiteral(
		"No usable transcript segments were produced, so the GPT Opus prompt could not be generated.");
	const QString trimmedDetails = details.trimmed();
	return trimmedDetails.isEmpty() ? base : QStringLiteral("%1 %2").arg(base, trimmedDetails);
}

static QString gptPromptBlockedPrompt(const QString &details = {})
{
	return GptPromptClient::promptGenerationBlockedPrompt(gptPromptBlockedReason(details));
}

static double configured_gpt_transcript_context_padding_seconds()
{
	bool ok = false;
	const double value = PluginConfig::getValue(QString::fromLatin1(CONFIG_GPT_TRANSCRIPT_CONTEXT_PADDING_SEC),
						    QString::number(DEFAULT_GPT_TRANSCRIPT_CONTEXT_PADDING_SEC, 'f', 0))
				     .toDouble(&ok);

	if (!ok || !std::isfinite(value))
		return DEFAULT_GPT_TRANSCRIPT_CONTEXT_PADDING_SEC;

	return std::clamp(value, 0.0, 600.0);
}

static QVector<ClipDuration> expanded_context_ranges_for_transcription(const QVector<ClipDuration> &ranges,
								       double paddingSec)
{
	QVector<ClipDuration> expanded;
	if (ranges.isEmpty())
		return expanded;

	for (const ClipDuration &range : ranges) {
		const double startSec = std::max(0.0, range.startSec);
		const double endSec = std::max(startSec, range.endSec);
		if (endSec <= startSec)
			continue;

		expanded.append({std::max(0.0, startSec - paddingSec), endSec + paddingSec});
	}

	std::sort(expanded.begin(), expanded.end(),
		  [](const ClipDuration &left, const ClipDuration &right) { return left.startSec < right.startSec; });

	QVector<ClipDuration> merged;
	for (const ClipDuration &range : expanded) {
		if (merged.isEmpty() || range.startSec > merged.last().endSec) {
			merged.append(range);
			continue;
		}

		merged.last().endSec = std::max(merged.last().endSec, range.endSec);
	}

	return merged;
}

static QString curation_ranges_log_string(const CurationSettings &settings)
{
	QStringList parts;
	double totalSec = 0.0;

	if (!settings.clipDurations.isEmpty()) {
		for (const ClipDuration &range : settings.clipDurations) {
			const double startSec = std::max(0.0, range.startSec);
			const double endSec = std::max(startSec, range.endSec);
			totalSec += std::max(0.0, endSec - startSec);
			parts << QStringLiteral("%1-%2").arg(startSec, 0, 'f', 2).arg(endSec, 0, 'f', 2);
		}
	} else {
		const double startSec = std::max(0.0, settings.rangeStartSec);
		const double endSec = std::max(startSec, settings.rangeEndSec);
		totalSec = std::max(0.0, endSec - startSec);
		parts << QStringLiteral("%1-%2").arg(startSec, 0, 'f', 2).arg(endSec, 0, 'f', 2);
	}

	return QStringLiteral("ranges=%1 totalSelectedSec=%2")
		.arg(parts.join(QStringLiteral(",")))
		.arg(totalSec, 0, 'f', 2);
}

static bool is_openai_model_enabled()
{
	const QString model = get_openai_model().trimmed();
	return !model.isEmpty() && model != OPENAI_MODEL_DISABLED;
}

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

static void invoke_prompt_finished(QWidget *parent, std::function<void(QString)> finishedCallback, QString prompt)
{
	if (!finishedCallback)
		return;

	invoke_finished(parent, [finishedCallback = std::move(finishedCallback), prompt = std::move(prompt)]() mutable {
		finishedCallback(prompt);
	});
}

static void invoke_prompt_result_finished(QWidget *parent,
					  std::function<void(GeneratedCurationPromptResult)> finishedCallback,
					  GeneratedCurationPromptResult result)
{
	if (!finishedCallback)
		return;

	invoke_finished(parent, [finishedCallback = std::move(finishedCallback), result = std::move(result)]() mutable {
		finishedCallback(result);
	});
}

static QString range_log(const ClipDuration &range)
{
	return QStringLiteral("%1-%2").arg(range.startSec, 0, 'f', 2).arg(range.endSec, 0, 'f', 2);
}

static bool is_prompt_blocked_or_empty(const QString &prompt)
{
	const QString trimmed = prompt.trimmed();
	return trimmed.isEmpty() || GptPromptClient::isPromptGenerationBlockedPrompt(trimmed) ||
	       GptPromptClient::isSemanticGateFailurePrompt(trimmed);
}

static QString ranges_log(const QVector<ClipDuration> &ranges)
{
	QStringList values;
	for (const ClipDuration &range : ranges)
		values << range_log(range);
	return values.join(QStringLiteral(","));
}

static const char *range_resolution_mode_name(CurationRangeStrategyResolution::Mode mode)
{
	switch (mode) {
	case CurationRangeStrategyResolution::Mode::FocusRange:
		return "focus_range";
	case CurationRangeStrategyResolution::Mode::CandidateRanges:
		return "candidate_ranges";
	case CurationRangeStrategyResolution::Mode::Unchanged:
		return "unchanged";
	}

	return "unknown";
}

static GeneratedCurationPromptResult result_with_strategy_ranges(const QString &videoPath,
							 const RecordingTranscript &transcript,
							 const CurationSettings &settings,
							 const QString &prompt)
{
	GeneratedCurationPromptResult result;
	result.prompt = prompt.trimmed();
	result.curationSettings = settings;

	if (is_prompt_blocked_or_empty(result.prompt))
		return result;

	const CurationRangeStrategyResolver resolver;
	const CurationRangeStrategyResolution resolution = resolver.resolve(transcript, settings, result.prompt);

	if (resolution.applied()) {
		result.curationSettings = resolver.apply(settings, resolution);

		if (resolution.mode == CurationRangeStrategyResolution::Mode::FocusRange) {
			blog(LOG_INFO,
			     "[clip-cropper] Curation range strategy applied. video=%s strategy=%s mode=%s target=%s confidence=%.2f manualRange=%s ranges=%s startAdjusted=%s anchor=%s",
			     videoPath.toUtf8().constData(), resolution.strategyName.toUtf8().constData(),
			     range_resolution_mode_name(resolution.mode), resolution.target.toUtf8().constData(),
			     resolution.confidence, range_log(resolution.manualRange).toUtf8().constData(),
			     ranges_log(resolution.ranges).toUtf8().constData(),
			     resolution.startAdjusted ? "true" : "false", resolution.anchorText.toUtf8().constData());
			return result;
		}

		blog(LOG_INFO,
		     "[clip-cropper] Curation range strategy applied. video=%s strategy=%s mode=%s reason=%s projects=%d ranges=%s details=%s",
		     videoPath.toUtf8().constData(), resolution.strategyName.toUtf8().constData(),
		     range_resolution_mode_name(resolution.mode), resolution.reason.toUtf8().constData(),
		     static_cast<int>(resolution.ranges.size()), ranges_log(resolution.ranges).toUtf8().constData(),
		     resolution.details.toUtf8().constData());
		return result;
	}

	blog(LOG_INFO,
	     "[clip-cropper] Curation range strategy skipped. video=%s strategy=%s reason=%s mode=%s",
	     videoPath.toUtf8().constData(), resolution.strategyName.toUtf8().constData(),
	     resolution.reason.toUtf8().constData(), range_resolution_mode_name(resolution.mode));
	return result;
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

	blog(LOG_INFO, "Filtered transcript for GPT curation prompt. originalSegments=%d filteredSegments=%d ranges=%d",
	     transcript.segments.size(), filtered.segments.size(), ranges.size());

	return filtered;
}

static void transcribe_video_with_progress_dialog_async(QWidget *parent, const QString &videoPath,
							const QString &transcriptionLanguage,
							const CurationSettings &curationSettings,
							std::function<void(RecordingTranscript, bool)> finishedCallback)
{
	const QString normalizedLanguage = normalize_transcription_language(transcriptionLanguage);
	RecordingTranscript cached = TranscriptStore::loadForVideoPath(videoPath, normalizedLanguage);
	if (!cached.segments.isEmpty()) {
		blog(LOG_INFO,
		     "Transcript cache hit before GPT curation prompt. Skipping Whisper/GPU transcription. video=%s segments=%d language=%s cacheKey=%s",
		     videoPath.toUtf8().constData(), static_cast<int>(cached.segments.size()),
		     normalizedLanguage.toUtf8().constData(),
		     TranscriptStore::keyForVideoPath(videoPath, normalizedLanguage).toUtf8().constData());
		if (finishedCallback)
			invoke_finished(parent, [cached, finishedCallback = std::move(finishedCallback)]() mutable {
				finishedCallback(cached, false);
			});
		return;
	}

	const QVector<ClipDuration> ranges = valid_prompt_ranges(curationSettings);
	const double contextPaddingSec = configured_gpt_transcript_context_padding_seconds();
	const QVector<ClipDuration> transcriptionRanges =
		expanded_context_ranges_for_transcription(ranges, contextPaddingSec);

	if (!transcriptionRanges.isEmpty()) {
		const RecordingTranscript rangeCached =
			TranscriptStore::loadForVideoRanges(videoPath, normalizedLanguage, transcriptionRanges);
		if (!rangeCached.segments.isEmpty()) {
			blog(LOG_INFO,
			     "Context-range transcript cache hit before GPT curation prompt. Skipping Whisper/GPU transcription. video=%s segments=%d language=%s paddingSec=%.0f cacheKey=%s",
			     videoPath.toUtf8().constData(), static_cast<int>(rangeCached.segments.size()),
			     normalizedLanguage.toUtf8().constData(), contextPaddingSec,
			     TranscriptStore::keyForVideoRanges(videoPath, normalizedLanguage, transcriptionRanges)
				     .toUtf8()
				     .constData());
			if (finishedCallback)
				invoke_finished(parent, [rangeCached,
							 finishedCallback = std::move(finishedCallback)]() mutable {
					finishedCallback(rangeCached, false);
				});
			return;
		}
	}

	const QString contextRangeCacheKey =
		transcriptionRanges.isEmpty()
			? QStringLiteral("<none>")
			: TranscriptStore::keyForVideoRanges(videoPath, normalizedLanguage, transcriptionRanges);
	blog(LOG_INFO,
	     "Transcript cache miss before GPT curation prompt. Starting Whisper/GPU transcription if available. video=%s language=%s cacheKey=%s contextRangeCacheKey=%s selectedRanges=%d contextRanges=%d paddingSec=%.0f",
	     videoPath.toUtf8().constData(), normalizedLanguage.toUtf8().constData(),
	     TranscriptStore::keyForVideoPath(videoPath, normalizedLanguage).toUtf8().constData(),
	     contextRangeCacheKey.toUtf8().constData(), static_cast<int>(ranges.size()),
	     static_cast<int>(transcriptionRanges.size()), contextPaddingSec);

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

	auto *thread = QThread::create([videoPath, modelPath, normalizedLanguage, transcriptionRanges, cancelRequested,
					transcriptResult, reportProgress]() {
		reportProgress(0, obsText("Status.PreparingTranscription"));

		if (transcriptResult) {
			if (!transcriptionRanges.isEmpty()) {
				*transcriptResult = global_realtime_transcription_service()->transcribeVideoRanges(
					videoPath, modelPath, normalizedLanguage, transcriptionRanges, reportProgress,
					[cancelRequested]() { return cancelRequested && cancelRequested->load(); });
			} else {
				*transcriptResult = global_realtime_transcription_service()->transcribeVideoFile(
					videoPath, modelPath, normalizedLanguage, reportProgress,
					[cancelRequested]() { return cancelRequested && cancelRequested->load(); });
			}
		}
	});
	thread->setObjectName(QStringLiteral("ClipCropperTranscriptionThread"));

	auto requestTranscriptionCancel = [cancelRequested, operationFinished, safeProgress, videoPath]() {
		if (operationFinished && operationFinished->load())
			return;

		cancelRequested->store(true);
		if (safeProgress)
			safeProgress->setLabelText(obsText("Status.CancelingOperation"));
		blog(LOG_INFO, "User canceled on-demand transcription before GPT curation prompt: %s",
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
		 normalizedLanguage, transcriptionRanges, finishedCallback = std::move(finishedCallback)]() mutable {
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

			if (!canceled && !transcriptionRanges.isEmpty() && !transcript.segments.isEmpty()) {
				TranscriptStore::saveForVideoRanges(videoPath, normalizedLanguage, transcriptionRanges,
								    transcript);
				blog(LOG_INFO,
				     "Context-range transcript saved after on-demand transcription. video=%s segments=%d language=%s cacheKey=%s",
				     videoPath.toUtf8().constData(), static_cast<int>(transcript.segments.size()),
				     normalizedLanguage.toUtf8().constData(),
				     TranscriptStore::keyForVideoRanges(videoPath, normalizedLanguage,
									transcriptionRanges)
					     .toUtf8()
					     .constData());
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
	const QString rangeLog = curation_ranges_log_string(curationSettings);
	blog(LOG_INFO, "Preparing GPT curation transcript. video=%s originalSegments=%d selectedSegments=%d %s",
	     videoPath.toUtf8().constData(), transcript.segments.size(), promptTranscript.segments.size(),
	     rangeLog.toUtf8().constData());
	if (promptTranscript.segments.isEmpty()) {
		blog(LOG_WARNING,
		     "No transcript segments inside the selected curation ranges for %s. Blocking upload without GPT prompt.",
		     videoPath.toUtf8().constData());
		invoke_prompt_finished(
			parent, std::move(finishedCallback),
			gptPromptBlockedPrompt(QStringLiteral(
				"Try selecting a range with audible speech or check the transcription language/audio stream.")));
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
					 blog(LOG_WARNING, "GPT curation prompt generation failed: %s",
					      message.toUtf8().constData());

				 if (safeProgress) {
					 mark_progress_window_finished(safeProgress);
					 safeProgress->close();
					 safeProgress->deleteLater();
				 }

				 if (callback && *callback)
					 (*callback)({});
			 });

	blog(LOG_INFO,
	     "Sending transcript context to GPT after review. video=%s fullSegments=%d selectedSegments=%d model=%s",
	     videoPath.toUtf8().constData(), transcript.segments.size(), promptTranscript.segments.size(),
	     get_openai_model().toUtf8().constData());
	client->createOpusPromptAsync(videoPath, transcript, promptTranscript, curationSettings);
}

void generate_custom_prompt_for_curation_async(QWidget *parent, const QString &videoPath,
					       const CurationSettings &curationSettings, bool transcribeOnDemand,
					       std::function<void(QString)> finishedCallback)
{
	generate_custom_prompt_for_curation_result_async(
		parent, videoPath, curationSettings, transcribeOnDemand,
		[finishedCallback = std::move(finishedCallback)](GeneratedCurationPromptResult result) mutable {
			if (finishedCallback)
				finishedCallback(result.prompt);
		});
}

void generate_custom_prompt_for_curation_result_async(QWidget *parent, const QString &videoPath,
						      const CurationSettings &curationSettings,
						      bool transcribeOnDemand,
						      std::function<void(GeneratedCurationPromptResult)> finishedCallback)
{
	auto callback = std::make_shared<std::function<void(GeneratedCurationPromptResult)>>(std::move(finishedCallback));
	auto finish = [parent, callback](GeneratedCurationPromptResult result) mutable {
		if (!callback || !*callback)
			return;

		auto finalCallback = std::move(*callback);
		*callback = {};
		invoke_prompt_result_finished(parent, std::move(finalCallback), std::move(result));
	};

	auto finishPromptOnly = [finish, curationSettings](QString prompt) mutable {
		GeneratedCurationPromptResult result;
		result.prompt = prompt.trimmed();
		result.curationSettings = curationSettings;
		finish(std::move(result));
	};

	if (!curationSettings.aiPrompt.trimmed().isEmpty()) {
		blog(LOG_INFO, "Using custom Opus prompt provided in review dialog: %s",
		     videoPath.toUtf8().constData());
		finishPromptOnly(curationSettings.aiPrompt.trimmed());
		return;
	}

	const QString openAiApiKey = get_openai_api_key();
	if (openAiApiKey.trimmed().isEmpty()) {
		blog(LOG_WARNING, "OpenAI API key is empty. Skipping GPT curation prompt generation.");
		finishPromptOnly({});
		return;
	}

	if (!is_openai_model_enabled()) {
		blog(LOG_INFO, "OpenAI model is disabled. Skipping GPT curation prompt generation.");
		finishPromptOnly({});
		return;
	}

	auto continueWithTranscript = [parent, videoPath, curationSettings,
				       finish](const RecordingTranscript &transcript,
				       bool transcriptionCanceled) mutable {
		if (transcriptionCanceled) {
			blog(LOG_INFO, "GPT curation prompt generation blocked because transcription was canceled: %s",
			     videoPath.toUtf8().constData());
			GeneratedCurationPromptResult result;
			result.prompt = gptPromptBlockedPrompt(
				QStringLiteral("Transcription was canceled before a prompt could be generated."));
			result.curationSettings = curationSettings;
			finish(std::move(result));
			return;
		}

		if (transcript.segments.isEmpty()) {
			blog(LOG_WARNING,
			     "No transcript available for %s. Blocking upload without GPT curation prompt.",
			     videoPath.toUtf8().constData());
			GeneratedCurationPromptResult result;
			result.prompt = gptPromptBlockedPrompt(QStringLiteral(
				"Try selecting a range with audible speech or check the transcription language/audio stream."));
			result.curationSettings = curationSettings;
			finish(std::move(result));
			return;
		}

		auto finishWithFocus = [videoPath, transcript, curationSettings,
					    finish](QString prompt) mutable {
			GeneratedCurationPromptResult result = result_with_strategy_ranges(videoPath, transcript,
											  curationSettings, prompt);
			finish(std::move(result));
		};

		generate_gpt_prompt_with_progress_dialog_async(parent, videoPath, transcript, curationSettings,
								     std::move(finishWithFocus));
	};

	blog(LOG_INFO, "Loading transcript after review before GPT curation prompt generation: %s",
	     videoPath.toUtf8().constData());
	const QString transcriptionLanguage = normalize_transcription_language(curationSettings.transcriptionLanguage);
	const RecordingTranscript transcript = TranscriptStore::loadForVideoPath(videoPath, transcriptionLanguage);
	if (!transcript.segments.isEmpty()) {
		blog(LOG_INFO,
		     "Transcript cache hit after review. Skipping on-demand Whisper/GPU transcription. video=%s segments=%d language=%s cacheKey=%s",
		     videoPath.toUtf8().constData(), static_cast<int>(transcript.segments.size()),
		     transcriptionLanguage.toUtf8().constData(),
		     TranscriptStore::keyForVideoPath(videoPath, transcriptionLanguage).toUtf8().constData());
		continueWithTranscript(transcript, false);
		return;
	}

	blog(LOG_INFO,
	     "Transcript cache miss after review. On-demand transcription is required. video=%s language=%s cacheKey=%s",
	     videoPath.toUtf8().constData(), transcriptionLanguage.toUtf8().constData(),
	     TranscriptStore::keyForVideoPath(videoPath, transcriptionLanguage).toUtf8().constData());

	if (!transcribeOnDemand) {
		blog(LOG_WARNING,
		     "No cached transcript available for %s and on-demand transcription is disabled. Blocking upload without GPT curation prompt.",
		     videoPath.toUtf8().constData());
		GeneratedCurationPromptResult result;
		result.prompt = gptPromptBlockedPrompt(
			QStringLiteral("No cached transcript is available and on-demand transcription is disabled."));
		result.curationSettings = curationSettings;
		finish(std::move(result));
		return;
	}

	blog(LOG_INFO, "No cached transcript found. Starting on-demand transcription after review: %s",
	     videoPath.toUtf8().constData());
	transcribe_video_with_progress_dialog_async(parent, videoPath, transcriptionLanguage, curationSettings,
						    std::move(continueWithTranscript));
}

void generate_custom_prompt_before_review_async(QWidget *parent, const QString &videoPath, bool transcribeOnDemand,
						std::function<void()> finishedCallback)
{
	Q_UNUSED(videoPath);
	Q_UNUSED(transcribeOnDemand);
	blog(LOG_INFO,
	     "Legacy pre-review GPT flow was skipped. GPT prompt is now generated after review ranges are confirmed.");
	invoke_finished(parent, std::move(finishedCallback));
}
