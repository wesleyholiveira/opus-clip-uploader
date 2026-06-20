#include "ui/review-scoring-preparation.hpp"

#include "ui/gpt-review-prompt.hpp"

#include "curation/scoring/clip-scoring-pipeline.hpp"
#include "curation/scoring/embedding-semantic-reranker.hpp"
#include "curation/scoring/llama-server-embedding-provider.hpp"
#include "curation/scoring/semantic-embedding-settings.hpp"
#include "models/transcript.hpp"

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

#include <QCoreApplication>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <memory>

using namespace Curation::Scoring;

namespace {

constexpr int MAX_REVIEW_SUGGESTED_CLIPS = 8;
constexpr double DEFAULT_MIN_DURATION_SEC = 18.0;
constexpr double DEFAULT_MAX_DURATION_SEC = 75.0;

QObject *callbackContext(QWidget *parent)
{
	if (parent)
		return static_cast<QObject *>(parent);

	return QCoreApplication::instance();
}

void finishOnUiThread(QWidget *parent, std::function<void(ReviewScoringPreparationResult)> callback,
			      ReviewScoringPreparationResult result)
{
	if (!callback)
		return;

	QObject *context = callbackContext(parent);
	if (!context) {
		callback(std::move(result));
		return;
	}

	QPointer<QObject> safeContext(context);
	QTimer::singleShot(0, context, [safeContext, callback = std::move(callback), result = std::move(result)]() mutable {
		if (!safeContext)
			return;
		callback(std::move(result));
	});
}

double transcriptDurationSec(const RecordingTranscript &transcript)
{
	double endSec = 0.0;
	for (const TranscriptSegment &segment : transcript.segments) {
		if (std::isfinite(segment.endSec))
			endSec = std::max(endSec, segment.endSec);
	}
	return endSec;
}

ClipScoringPipelineOptions optionsForTranscript(const RecordingTranscript &transcript,
						       const SemanticEmbeddingProvider *embeddingProvider,
						       const SemanticReranker *reranker)
{
	const double durationSec = transcriptDurationSec(transcript);
	ClipScoringPipelineOptions options;
	options.generation.searchRange = {0.0, durationSec};
	options.generation.presetId = QStringLiteral("viewer_message_response");
	options.generation.minDurationSec = DEFAULT_MIN_DURATION_SEC;
	options.generation.maxDurationSec = durationSec >= 1800.0 ? 90.0 : DEFAULT_MAX_DURATION_SEC;
	options.generation.defaultAfterSec = durationSec >= 1800.0 ? 42.0 : 34.0;
	options.generation.emotionalAfterSec = durationSec >= 1800.0 ? 48.0 : 42.0;
	options.generation.adviceAfterSec = durationSec >= 1800.0 ? 48.0 : 42.0;
	options.generation.boundaryMinDurationSec = 8.0;
	options.generation.slidingWindowStepSec = durationSec >= 1800.0 ? 24.0 : 15.0;
	options.generation.maxRawCandidates = durationSec >= 7200.0 ? 480 : durationSec >= 1800.0 ? 360 : 220;

	options.scoring.presetId = QStringLiteral("viewer_message_response");
	options.semantic.enabled = true;
	options.rerankerOptions.enabled = true;
	options.qualityGate.presetId = QStringLiteral("viewer_message_response");
	options.qualityGate.minFinalScore = durationSec >= 1800.0 ? 0.30 : 0.36;
	options.qualityGate.minViewerResponseForViewerPreset = 0.24;
	options.ranking.maxCandidates = durationSec >= 1800.0 ? MAX_REVIEW_SUGGESTED_CLIPS : 5;
	options.ranking.minFinalScore = durationSec >= 1800.0 ? 0.32 : 0.38;
	options.ranking.overlapToleranceSec = 8.0;
	options.embeddingProvider = embeddingProvider;
	options.reranker = reranker;
	return options;
}

CurationSettings scoringSettingsFromReviewSettings(CurationSettings settings)
{
	settings.curationPreset = QStringLiteral("viewer_message_response");
	if (settings.transcriptionLanguage.trimmed().isEmpty())
		settings.transcriptionLanguage = QStringLiteral("auto");
	if (settings.sourceLanguage.trimmed().isEmpty())
		settings.sourceLanguage = QStringLiteral("auto");

	// Semantic suggestions should scan the video/transcript, not only the current manual
	// review markers. Keep language/preset fields, but clear range filters.
	settings.clipDurations.clear();
	settings.rangeStartSec = 0.0;
	settings.rangeEndSec = 0.0;
	return settings;
}

CurationSettings settingsFromScoring(const ClipScoringResult &scoring)
{
	CurationSettings settings;
	settings.clipDurations = scoring.ranges();
	if (!settings.clipDurations.isEmpty()) {
		settings.rangeStartSec = settings.clipDurations.first().startSec;
		settings.rangeEndSec = settings.clipDurations.last().endSec;
	}
	return settings;
}

} // namespace

void prepare_review_scoring_async(QWidget *parent, const QString &videoPath, const CurationSettings &baseSettings,
					  std::function<void(ReviewScoringPreparationResult)> finishedCallback)
{
	ReviewScoringPreparationResult disabledResult;
	const QString backend = localEmbeddingBackendFromConfig();
	if (backend != QString::fromLatin1(LOCAL_EMBEDDING_BACKEND_LLAMA_SERVER)) {
		disabledResult.summary = QStringLiteral("local_embedding_backend_disabled");
		finishOnUiThread(parent, std::move(finishedCallback), std::move(disabledResult));
		return;
	}

	ReviewScoringPreparationResult startedResult;
	startedResult.attempted = true;

	const CurationSettings transcriptSettings = scoringSettingsFromReviewSettings(baseSettings);

	blog(LOG_INFO,
	     "Preparing semantic review suggestions from review settings. video=%s transcriptionLanguage=%s sourceLanguage=%s",
	     videoPath.toUtf8().constData(), transcriptSettings.transcriptionLanguage.toUtf8().constData(),
	     transcriptSettings.sourceLanguage.toUtf8().constData());

	ensure_transcript_for_curation_async(
		parent, videoPath, transcriptSettings, true,
		[parent, videoPath, startedResult = std::move(startedResult),
		 finishedCallback = std::move(finishedCallback)](RecordingTranscript transcript, bool canceled) mutable {
			ReviewScoringPreparationResult result = std::move(startedResult);
			result.canceled = canceled;
			if (canceled) {
				result.summary = QStringLiteral("transcription_canceled");
				blog(LOG_INFO, "Semantic review suggestion preparation canceled during transcription: %s",
				     videoPath.toUtf8().constData());
				finishOnUiThread(parent, std::move(finishedCallback), std::move(result));
				return;
			}

			if (transcript.segments.isEmpty()) {
				result.summary = QStringLiteral("empty_transcript");
				blog(LOG_WARNING,
				     "Semantic review suggestions skipped because transcript is empty: %s",
				     videoPath.toUtf8().constData());
				finishOnUiThread(parent, std::move(finishedCallback), std::move(result));
				return;
			}

			LlamaServerEmbeddingProviderOptions llamaOptions = llamaServerEmbeddingOptionsFromConfig();
			std::unique_ptr<SemanticEmbeddingProvider> embeddingProvider;
			std::unique_ptr<SemanticReranker> semanticReranker;
			if (llamaOptions.enabled) {
				embeddingProvider = std::make_unique<LlamaServerEmbeddingProvider>(llamaOptions);
				semanticReranker = std::make_unique<EmbeddingSemanticReranker>(embeddingProvider.get());
			}

			const ClipScoringPipelineOptions options = optionsForTranscript(transcript, embeddingProvider.get(),
										      semanticReranker.get());
			const ClipScoringPipeline pipeline;
			const ClipScoringResult scoring = pipeline.score(transcript, options);
			result.settings = settingsFromScoring(scoring);
			result.applied = !result.settings.clipDurations.isEmpty();
			result.summary = scoring.summary;

			blog(result.applied ? LOG_INFO : LOG_WARNING,
			     "Semantic review suggestions finished. video=%s applied=%s ranges=%d backend=%s summary=%s",
			     videoPath.toUtf8().constData(), result.applied ? "true" : "false",
			     static_cast<int>(result.settings.clipDurations.size()),
			     embeddingProvider ? "llama_server" : "disabled",
			     result.summary.toUtf8().constData());

			finishOnUiThread(parent, std::move(finishedCallback), std::move(result));
		});
}


void prepare_review_scoring_async(QWidget *parent, const QString &videoPath,
					  std::function<void(ReviewScoringPreparationResult)> finishedCallback)
{
	prepare_review_scoring_async(parent, videoPath, CurationSettings{}, std::move(finishedCallback));
}
