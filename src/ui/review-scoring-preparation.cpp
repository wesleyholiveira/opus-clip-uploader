#include "ui/ui-common.hpp"
#include "ui/review-scoring-preparation.hpp"

#include "ui/curation-transcript-preparation.hpp"

#include "curation/curation-preset.hpp"
#include "curation/curation-preset-profile.hpp"
#include "curation/feedback/curation-feedback-store.hpp"
#include "curation/scoring/clip-scoring-pipeline.hpp"
#include "curation/scoring/llama-server-embedding-provider.hpp"
#include "curation/scoring/llama-server-reranker-provider.hpp"
#include "curation/scoring/semantic-embedding-settings.hpp"
#include "curation/scoring/semantic-prototypes.hpp"
#include "models/transcript.hpp"

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

#include <QByteArray>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

using namespace Curation::Scoring;

namespace {

constexpr int kMaxPrototypeTexts = 16;
constexpr int kMaxCandidatesBeforeEmbedding = 64;
constexpr int kMaxCandidateTextChars = 4000;
constexpr int kMaxRerankerTextChars = 3200;
constexpr double DEFAULT_MIN_DURATION_SEC = 8.0;
constexpr double DEFAULT_MAX_DURATION_SEC = 180.0;

struct ReviewSuggestionBudget {
	int maxReviewMarkers = 12;
	int maxCandidatesBeforeEmbedding = kMaxCandidatesBeforeEmbedding;
	int maxRawCandidates = 240;
	int maxCoarseWindowsToEmbed = 72;
	int maxCoarseRegions = 16;
	double coarseWindowSec = 60.0;
	double coarseStrideSec = 36.0;
	double coarseRegionPaddingSec = 36.0;
	double minSpacingSec = 60.0;
	double preSemanticMinSpacingSec = 0.0;
};

QObject *callbackContext(QWidget *parent)
{
	if (parent)
		return static_cast<QObject *>(parent);

	return QCoreApplication::instance();
}

void finishOnUiContext(QPointer<QObject> safeContext, std::function<void(ReviewScoringPreparationResult)> callback,
		       ReviewScoringPreparationResult result)
{
	if (!callback || !safeContext)
		return;

	QObject *context = safeContext.data();
	QTimer::singleShot(0, context,
			   [safeContext, callback = std::move(callback), result = std::move(result)]() mutable {
				   if (!safeContext)
					   return;
				   callback(std::move(result));
			   });
}

void reportProgressToContext(QPointer<QObject> safeContext, ReviewScoringProgressCallback callback,
			     const QString &message, int value, int maximum = 100)
{
	if (!callback || !safeContext)
		return;

	ReviewScoringProgressUpdate update;
	update.message = message;
	update.value = std::clamp(value, 0, std::max(0, maximum));
	update.maximum = std::max(0, maximum);

	QObject *context = safeContext.data();
	QTimer::singleShot(0, context,
			   [safeContext, callback = std::move(callback), update = std::move(update)]() mutable {
				   if (!safeContext)
					   return;
				   callback(std::move(update));
			   });
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
	QTimer::singleShot(0, context,
			   [safeContext, callback = std::move(callback), result = std::move(result)]() mutable {
				   if (!safeContext)
					   return;
				   callback(std::move(result));
			   });
}

void reportProgress(QWidget *parent, ReviewScoringProgressCallback callback, const QString &message, int value,
		    int maximum = 100)
{
	if (!callback)
		return;

	ReviewScoringProgressUpdate update;
	update.message = message;
	update.value = std::clamp(value, 0, std::max(0, maximum));
	update.maximum = std::max(0, maximum);

	QObject *context = callbackContext(parent);
	if (!context) {
		callback(std::move(update));
		return;
	}

	QPointer<QObject> safeContext(context);
	QTimer::singleShot(0, context,
			   [safeContext, callback = std::move(callback), update = std::move(update)]() mutable {
				   if (!safeContext)
					   return;
				   callback(std::move(update));
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

ReviewSuggestionBudget reviewSuggestionBudgetForDuration(double durationSec)
{
	const double minutes = durationSec / 60.0;
	ReviewSuggestionBudget budget;

	// Review suggestions are not the final upload decision. They should provide enough
	// visible markers for the user to accept, adjust or reject, so feedback calibration
	// has real examples. Keep the expensive semantic/reranker beam bounded, but avoid
	// the old 6+ minute spacing that made long Q&A lives produce only a handful of
	// reviewable markers.
	if (minutes <= 3.0) {
		budget.maxReviewMarkers = 4;
		budget.maxCandidatesBeforeEmbedding = 16;
		budget.maxRawCandidates = 80;
		budget.maxCoarseWindowsToEmbed = 16;
		budget.maxCoarseRegions = 6;
		budget.coarseWindowSec = 24.0;
		budget.coarseStrideSec = 12.0;
		budget.coarseRegionPaddingSec = 12.0;
		budget.minSpacingSec = 25.0;
	} else if (minutes <= 10.0) {
		budget.maxReviewMarkers = 8;
		budget.maxCandidatesBeforeEmbedding = 32;
		budget.maxRawCandidates = 140;
		budget.maxCoarseWindowsToEmbed = 32;
		budget.maxCoarseRegions = 10;
		budget.coarseWindowSec = 30.0;
		budget.coarseStrideSec = 18.0;
		budget.coarseRegionPaddingSec = 18.0;
		budget.minSpacingSec = 35.0;
	} else if (minutes <= 30.0) {
		budget.maxReviewMarkers = 16;
		budget.maxCandidatesBeforeEmbedding = 48;
		budget.maxRawCandidates = 220;
		budget.maxCoarseWindowsToEmbed = 56;
		budget.maxCoarseRegions = 18;
		budget.coarseWindowSec = 42.0;
		budget.coarseStrideSec = 26.0;
		budget.coarseRegionPaddingSec = 24.0;
		budget.minSpacingSec = 45.0;
	} else if (minutes <= 60.0) {
		budget.maxReviewMarkers = 24;
		budget.maxCandidatesBeforeEmbedding = 64;
		budget.maxRawCandidates = 320;
		budget.maxCoarseWindowsToEmbed = 80;
		budget.maxCoarseRegions = 28;
		budget.coarseWindowSec = 64.0;
		budget.coarseStrideSec = 38.0;
		budget.coarseRegionPaddingSec = 52.0;
		budget.minSpacingSec = 55.0;
	} else if (minutes <= 120.0) {
		budget.maxReviewMarkers = 48;
		budget.maxCandidatesBeforeEmbedding = 112;
		budget.maxRawCandidates = 640;
		budget.maxCoarseWindowsToEmbed = 128;
		budget.maxCoarseRegions = 56;
		budget.coarseWindowSec = 72.0;
		budget.coarseStrideSec = 42.0;
		budget.coarseRegionPaddingSec = 72.0;
		budget.minSpacingSec = 35.0;
	} else if (minutes <= 180.0) {
		budget.maxReviewMarkers = 60;
		budget.maxCandidatesBeforeEmbedding = 128;
		budget.maxRawCandidates = 780;
		budget.maxCoarseWindowsToEmbed = 160;
		budget.maxCoarseRegions = 72;
		budget.coarseWindowSec = 78.0;
		budget.coarseStrideSec = 42.0;
		budget.coarseRegionPaddingSec = 84.0;
		budget.minSpacingSec = 30.0;
	} else {
		budget.maxReviewMarkers = 72;
		budget.maxCandidatesBeforeEmbedding = 144;
		budget.maxRawCandidates = 920;
		budget.maxCoarseWindowsToEmbed = 192;
		budget.maxCoarseRegions = 88;
		budget.coarseWindowSec = 84.0;
		budget.coarseStrideSec = 48.0;
		budget.coarseRegionPaddingSec = 90.0;
		budget.minSpacingSec = 30.0;
	}

	budget.preSemanticMinSpacingSec = 0.0;
	return budget;
}

struct TranscriptScaleProfile {
	bool longTranscript = false;
	double maxDurationSec = DEFAULT_MAX_DURATION_SEC;
	double defaultAfterSec = 34.0;
	double slidingWindowStepSec = 15.0;
	int minTextChars = 60;
	double minFinalScore = 0.38;
	double rankingMinFinalScore = 0.44;
	double preSemanticMinFinalScore = 0.20;
};

// These values are calibration profiles, not business rules. Keep them named and
// centralized so future feedback data can tune the profile without changing the
// pipeline assembly code.
struct SemanticQualityProfile {
	double rerankerContributionWeight = 0.18;
	double minViewerResponse = 0.34;
	double minRerankerScore = 0.28;
	double minRerankerRawScore = 0.28;
	double minStrongRerankerRawScore = 0.45;
	double minConditionalRerankerRawScore = 0.36;
	double minClipValue = 0.64;
	double minHook = 0.52;
	double minResolution = 0.52;
	double minTopicContinuity = 0.50;
	double maxSemanticMetaNoise = 0.74;
	double hardMaxSemanticMetaNoise = 0.88;
	double strongPositiveRerankerRaw = 0.72;
	double strongPositiveClipValue = 0.68;
	double strongPositiveHook = 0.50;
	double strongPositiveResolution = 0.50;
	double maxSemanticNoise = 0.80;
	double minOpeningHook = 0.52;
	double minEndingResolution = 0.52;
	double maxOpeningMetaNoise = 0.74;
	double maxEndingMetaNoise = 0.76;
	double maxEndingTopicShift = 0.76;
	double maxRerankerBadClip = 0.90;
	double minRerankerGoodBadMargin = -1.0;
	double mmrRelevanceWeight = 0.74;
};

TranscriptScaleProfile transcriptScaleProfileForDuration(double durationSec)
{
	TranscriptScaleProfile profile;
	profile.longTranscript = durationSec >= 1800.0;
	if (!profile.longTranscript)
		return profile;

	profile.maxDurationSec = DEFAULT_MAX_DURATION_SEC;
	profile.defaultAfterSec = 90.0;
	profile.slidingWindowStepSec = 30.0;
	profile.minTextChars = 56;
	profile.minFinalScore = 0.34;
	profile.rankingMinFinalScore = 0.36;
	profile.preSemanticMinFinalScore = 0.16;
	return profile;
}

SemanticQualityProfile semanticQualityProfile(bool hasReranker)
{
	SemanticQualityProfile profile;
	if (!hasReranker)
		return profile;

	// The local Qwen reranker is a useful ordering signal, but it should not veto
	// semantically complete arcs by dragging their final score below the ranker floor.
	profile.rerankerContributionWeight = 0.34;
	profile.minViewerResponse = 0.62;
	profile.minRerankerScore = 0.66;
	profile.minRerankerRawScore = 0.72;
	profile.minStrongRerankerRawScore = 0.86;
	profile.minConditionalRerankerRawScore = 0.76;
	profile.minClipValue = 0.70;
	profile.minHook = 0.58;
	profile.minResolution = 0.58;
	profile.minTopicContinuity = 0.58;
	profile.maxSemanticMetaNoise = 0.70;
	profile.hardMaxSemanticMetaNoise = 0.80;
	profile.strongPositiveRerankerRaw = 0.90;
	profile.strongPositiveClipValue = 0.74;
	profile.strongPositiveHook = 0.60;
	profile.strongPositiveResolution = 0.58;
	profile.maxSemanticNoise = 0.76;
	profile.minOpeningHook = 0.60;
	profile.minEndingResolution = 0.58;
	profile.maxOpeningMetaNoise = 0.66;
	profile.maxEndingMetaNoise = 0.70;
	profile.maxEndingTopicShift = 0.68;
	profile.maxRerankerBadClip = 0.64;
	profile.minRerankerGoodBadMargin = 0.18;
	profile.mmrRelevanceWeight = 0.68;
	return profile;
}

void configureGenerationOptions(ClipScoringPipelineOptions &options, double durationSec,
				const ReviewSuggestionBudget &budget, const TranscriptScaleProfile &scaleProfile,
				const Curation::CurationPresetProfile &presetProfile)
{
	options.generation.searchRange = {0.0, durationSec};
	options.generation.presetId = presetProfile.id;
	options.generation.minDurationSec = DEFAULT_MIN_DURATION_SEC;
	options.generation.maxDurationSec = scaleProfile.maxDurationSec;
	options.generation.defaultAfterSec = std::min(scaleProfile.defaultAfterSec, options.generation.maxDurationSec);
	options.generation.emotionalAfterSec = std::min(120.0, options.generation.maxDurationSec);
	options.generation.adviceAfterSec = std::min(150.0, options.generation.maxDurationSec);
	options.generation.boundaryMinDurationSec = 8.0;
	options.generation.slidingWindowStepSec = scaleProfile.slidingWindowStepSec;
	options.generation.maxRawCandidates = budget.maxRawCandidates;
}

void configureCoarseRetrievalOptions(ClipScoringPipelineOptions &options, const ReviewSuggestionBudget &budget)
{
	options.coarseRetrieval.enabled = true;
	options.coarseRetrieval.searchRange = options.generation.searchRange;
	options.coarseRetrieval.windowSec = budget.coarseWindowSec;
	options.coarseRetrieval.strideSec = budget.coarseStrideSec;
	options.coarseRetrieval.regionPaddingSec = budget.coarseRegionPaddingSec;
	options.coarseRetrieval.maxWindowsToEmbed = budget.maxCoarseWindowsToEmbed;
	options.coarseRetrieval.maxRegions = budget.maxCoarseRegions;
	options.coarseRetrieval.maxPrototypeTexts = kMaxPrototypeTexts;
	options.coarseRetrieval.maxWindowTextChars = kMaxCandidateTextChars;
	options.coarseRetrieval.minRegionSpacingSec = std::clamp(budget.minSpacingSec * 0.35, 12.0, 90.0);
}

void configureSemanticOptions(ClipScoringPipelineOptions &options)
{
	options.semantic.enabled = true;
	options.semantic.maxPrototypeTexts = kMaxPrototypeTexts;
	options.semantic.enablePairwiseTopicContinuity = true;
}

void configureRerankerOptions(ClipScoringPipelineOptions &options, const SemanticQualityProfile &qualityProfile)
{
	options.rerankerOptions.enabled = true;
	options.rerankerOptions.contributionWeight = qualityProfile.rerankerContributionWeight;
}

void configureQualityGateOptions(ClipScoringPipelineOptions &options, const TranscriptScaleProfile &scaleProfile,
				 const SemanticQualityProfile &qualityProfile,
				 const Curation::CurationPresetProfile &presetProfile)
{
	options.qualityGate.presetId = presetProfile.id;
	options.qualityGate.minDurationSec = DEFAULT_MIN_DURATION_SEC;
	options.qualityGate.minTextChars = scaleProfile.minTextChars;
	options.qualityGate.minFinalScore = scaleProfile.minFinalScore;
	options.qualityGate.minViewerResponseForViewerPreset = qualityProfile.minViewerResponse;
	options.qualityGate.minRerankerScoreWhenAvailable = qualityProfile.minRerankerScore;
	options.qualityGate.minRerankerRawScoreWhenAvailable = qualityProfile.minRerankerRawScore;
	options.qualityGate.minStrongRerankerRawScoreWhenAvailable = qualityProfile.minStrongRerankerRawScore;
	options.qualityGate.minConditionalRerankerRawScoreWhenAvailable = qualityProfile.minConditionalRerankerRawScore;
	options.qualityGate.minConditionalSemanticTargetWhenAvailable = 0.70;
	options.qualityGate.minConditionalBoundaryWhenAvailable = 0.88;
	options.qualityGate.minClipValueForViewerPreset = qualityProfile.minClipValue;
	options.qualityGate.minHookForViewerPreset = qualityProfile.minHook;
	options.qualityGate.minResolutionForViewerPreset = qualityProfile.minResolution;
	options.qualityGate.minTopicContinuityForViewerPreset = qualityProfile.minTopicContinuity;
	options.qualityGate.maxSemanticMetaNoiseWhenAvailable = qualityProfile.maxSemanticMetaNoise;
	options.qualityGate.hardMaxSemanticMetaNoiseWhenAvailable = qualityProfile.hardMaxSemanticMetaNoise;
	options.qualityGate.semanticNoiseMarginTolerance = 0.02;
	options.qualityGate.strongPositiveRerankerRaw = qualityProfile.strongPositiveRerankerRaw;
	options.qualityGate.strongPositiveClipValue = qualityProfile.strongPositiveClipValue;
	options.qualityGate.strongPositiveHook = qualityProfile.strongPositiveHook;
	options.qualityGate.strongPositiveResolution = qualityProfile.strongPositiveResolution;
	options.qualityGate.maxSemanticNoiseWhenAvailable = qualityProfile.maxSemanticNoise;
	options.qualityGate.minOpeningHookForViewerPreset = qualityProfile.minOpeningHook;
	options.qualityGate.minEndingResolutionForViewerPreset = qualityProfile.minEndingResolution;
	options.qualityGate.maxOpeningMetaNoiseWhenAvailable = qualityProfile.maxOpeningMetaNoise;
	options.qualityGate.maxEndingMetaNoiseWhenAvailable = qualityProfile.maxEndingMetaNoise;
	options.qualityGate.maxEndingTopicShiftWhenAvailable = qualityProfile.maxEndingTopicShift;
	options.qualityGate.maxRerankerBadClipWhenAvailable = qualityProfile.maxRerankerBadClip;
	options.qualityGate.minRerankerGoodBadMarginWhenAvailable = qualityProfile.minRerankerGoodBadMargin;
	options.qualityGate.minArcOpeningForViewerPreset = presetProfile.arcPolicy.minOpening;
	options.qualityGate.minArcDevelopmentForViewerPreset = presetProfile.arcPolicy.minDevelopment;
	options.qualityGate.minArcConclusionForViewerPreset = presetProfile.arcPolicy.minConclusion;
	options.qualityGate.minArcCompletenessForViewerPreset = presetProfile.arcPolicy.minCompleteness;
	options.qualityGate.minArcBoundaryCleanlinessForViewerPreset = presetProfile.arcPolicy.minBoundaryCleanliness;
	options.qualityGate.maxArcTailRiskForViewerPreset = presetProfile.arcPolicy.maxTailRisk;
}

void configureRankingOptions(ClipScoringPipelineOptions &options, const ReviewSuggestionBudget &budget,
			     const TranscriptScaleProfile &scaleProfile, const SemanticQualityProfile &qualityProfile)
{
	options.ranking.maxCandidates = budget.maxReviewMarkers;
	options.ranking.minFinalScore = scaleProfile.rankingMinFinalScore;
	options.ranking.overlapToleranceSec = 8.0;
	options.ranking.minSpacingSec = budget.minSpacingSec;
	options.ranking.useMmr = true;
	options.ranking.mmrRelevanceWeight = qualityProfile.mmrRelevanceWeight;
	options.ranking.mmrTemporalSimilarityWeight = 0.66;
	options.ranking.mmrTextSimilarityWeight = 0.34;
}

void configurePipelineBudget(ClipScoringPipelineOptions &options, const ReviewSuggestionBudget &budget,
			     const TranscriptScaleProfile &scaleProfile)
{
	options.budget.maxCandidatesBeforeEmbedding = budget.maxCandidatesBeforeEmbedding;
	options.budget.preSemanticMinFinalScore = scaleProfile.preSemanticMinFinalScore;
	options.budget.preSemanticMinSpacingSec = budget.preSemanticMinSpacingSec;
	options.budget.requireSemanticScoringWhenEmbeddingProviderEnabled = true;
}

QString semanticTargetFromReviewSettings(const CurationSettings &settings)
{
	QStringList focusTerms;
	for (const QString &keyword : settings.topicKeywords) {
		const QString trimmed = keyword.simplified();
		if (!trimmed.isEmpty() && !focusTerms.contains(trimmed, Qt::CaseInsensitive))
			focusTerms.append(trimmed);
	}

	const QString prompt = settings.aiPrompt.simplified();
	if (!prompt.isEmpty() && focusTerms.isEmpty())
		focusTerms.append(prompt.left(240));

	const QString genre = settings.genre.simplified();
	if (!genre.isEmpty() && genre.compare(QStringLiteral("auto"), Qt::CaseInsensitive) != 0 &&
	    !focusTerms.contains(genre, Qt::CaseInsensitive))
		focusTerms.append(genre);

	return focusTerms.join(QStringLiteral(", ")).simplified();
}

void applyClipLengthBoundsToOptions(ClipScoringPipelineOptions &options, const CurationSettings &settings)
{
	const CurationPreset::ClipLengthBounds bounds = CurationPreset::clipLengthBoundsForSettings(settings);
	if (!bounds.enabled)
		return;

	const double minSec = std::clamp(bounds.minSec, 5.0, DEFAULT_MAX_DURATION_SEC);
	const double maxSec = std::clamp(bounds.maxSec, std::max(minSec, 8.0), DEFAULT_MAX_DURATION_SEC);
	options.generation.minDurationSec = minSec;
	options.generation.boundaryMinDurationSec = std::min(options.generation.boundaryMinDurationSec, minSec);
	options.generation.maxDurationSec = maxSec;
	options.generation.defaultAfterSec = std::min(options.generation.defaultAfterSec, maxSec);
	options.generation.emotionalAfterSec = std::min(options.generation.emotionalAfterSec, maxSec);
	options.generation.adviceAfterSec = std::min(options.generation.adviceAfterSec, maxSec);
	options.qualityGate.minDurationSec = minSec;
	options.qualityGate.longCandidateDurationSec = std::min(90.0, maxSec);
	options.ranking.overlapToleranceSec =
		std::min(options.ranking.overlapToleranceSec, std::max(3.0, maxSec * 0.30));
}

ClipScoringPipelineOptions optionsForTranscript(const RecordingTranscript &transcript,
						const CurationSettings &reviewSettings,
						const SemanticEmbeddingProvider *embeddingProvider,
						const SemanticReranker *reranker)
{
	const double durationSec = transcriptDurationSec(transcript);
	const ReviewSuggestionBudget budget = reviewSuggestionBudgetForDuration(durationSec);
	const TranscriptScaleProfile scaleProfile = transcriptScaleProfileForDuration(durationSec);
	const SemanticQualityProfile qualityProfile = semanticQualityProfile(reranker != nullptr);

	const Curation::CurationPresetProfile presetProfile =
		Curation::presetProfileForSettings(reviewSettings, reviewSettings.aiPrompt);

	ClipScoringPipelineOptions options;
	configureGenerationOptions(options, durationSec, budget, scaleProfile, presetProfile);
	options.scoring.presetId = presetProfile.id;
	options.scoring.transcriptionLanguage = reviewSettings.transcriptionLanguage;
	options.scoring.sourceLanguage = reviewSettings.sourceLanguage;
	const QString semanticTarget = semanticTargetFromReviewSettings(reviewSettings);
	if (!semanticTarget.isEmpty()) {
		options.scoring.mainTarget = semanticTarget;
		options.scoring.reliableMainTarget = true;
	}
	configureCoarseRetrievalOptions(options, budget);
	configureSemanticOptions(options);
	configureRerankerOptions(options, qualityProfile);
	configureQualityGateOptions(options, scaleProfile, qualityProfile, presetProfile);
	configureRankingOptions(options, budget, scaleProfile, qualityProfile);
	configurePipelineBudget(options, budget, scaleProfile);
	applyClipLengthBoundsToOptions(options, reviewSettings);
	options.embeddingProvider = embeddingProvider;
	options.reranker = reranker;
	return options;
}

CurationSettings scoringSettingsFromReviewSettings(CurationSettings settings)
{
	if (CurationPreset::normalizeId(settings.curationPreset) == CurationPreset::autoPresetId())
		settings.curationPreset = CurationPreset::viewerMessageResponsePresetId();
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
	settings.uploadClipRangesIndependently = settings.clipDurations.size() > 1;
	if (!settings.clipDurations.isEmpty()) {
		settings.rangeStartSec = settings.clipDurations.first().startSec;
		settings.rangeEndSec = settings.clipDurations.last().endSec;
	}
	return settings;
}

static QJsonObject scoresToJson(const ClipCandidateScores &scores)
{
	QJsonObject object;
	object.insert(QStringLiteral("duration"), scores.duration);
	object.insert(QStringLiteral("boundary"), scores.boundary);
	object.insert(QStringLiteral("hook"), scores.hook);
	object.insert(QStringLiteral("emotional"), scores.emotional);
	object.insert(QStringLiteral("advice"), scores.advice);
	object.insert(QStringLiteral("viewerResponse"), scores.viewerResponse);
	object.insert(QStringLiteral("semanticTarget"), scores.semanticTarget);
	object.insert(QStringLiteral("semanticViewerMessage"), scores.semanticViewerMessage);
	object.insert(QStringLiteral("semanticDirectAnswer"), scores.semanticDirectAnswer);
	object.insert(QStringLiteral("semanticTopicShift"), scores.semanticTopicShift);
	object.insert(QStringLiteral("semanticClipValue"), scores.semanticClipValue);
	object.insert(QStringLiteral("semanticOpeningHook"), scores.semanticOpeningHook);
	object.insert(QStringLiteral("semanticOpeningMetaNoise"), scores.semanticOpeningMetaNoise);
	object.insert(QStringLiteral("semanticEndingResolution"), scores.semanticEndingResolution);
	object.insert(QStringLiteral("semanticEndingMetaNoise"), scores.semanticEndingMetaNoise);
	object.insert(QStringLiteral("semanticEndingTopicShift"), scores.semanticEndingTopicShift);
	object.insert(QStringLiteral("topicContinuity"), scores.topicContinuity);
	object.insert(QStringLiteral("arcOpening"), scores.arcOpening);
	object.insert(QStringLiteral("arcDevelopment"), scores.arcDevelopment);
	object.insert(QStringLiteral("arcConclusion"), scores.arcConclusion);
	object.insert(QStringLiteral("arcBoundaryCleanliness"), scores.arcBoundaryCleanliness);
	object.insert(QStringLiteral("arcTailRisk"), scores.arcTailRisk);
	object.insert(QStringLiteral("arcCompleteness"), scores.arcCompleteness);
	object.insert(QStringLiteral("final"), scores.final);
	return object;
}

static QString evidenceValue(const QStringList &evidence, const QString &prefix)
{
	for (const QString &item : evidence) {
		if (item.startsWith(prefix))
			return item.mid(prefix.size()).trimmed();
	}
	return {};
}

static QJsonArray stringListToJson(const QStringList &items, int maxItems = 48)
{
	QJsonArray array;
	const int limit = std::min(static_cast<int>(items.size()), maxItems);
	for (int i = 0; i < limit; ++i)
		array.append(items.at(i).left(300));
	return array;
}

QJsonArray diagnosticsFromScoring(const ClipScoringResult &scoring)
{
	QJsonArray array;
	for (int i = 0; i < scoring.candidates.size(); ++i) {
		const ClipCandidate &candidate = scoring.candidates.at(i);
		if (candidate.range.endSec <= candidate.range.startSec)
			continue;
		QJsonObject object;
		object.insert(QStringLiteral("index"), i);
		object.insert(QStringLiteral("start_sec"), candidate.range.startSec);
		object.insert(QStringLiteral("end_sec"), candidate.range.endSec);
		object.insert(QStringLiteral("duration_sec"), candidate.range.endSec - candidate.range.startSec);
		object.insert(QStringLiteral("source"), candidate.source);
		object.insert(QStringLiteral("final_score"), candidate.scores.final);
		object.insert(QStringLiteral("selected_rank"), candidate.selectedRank);
		object.insert(QStringLiteral("rejected"), candidate.rejectedByQualityGate || candidate.rejectedAsNoise);
		object.insert(QStringLiteral("rejection_reason"), candidate.rejectionReason);
		object.insert(QStringLiteral("scores"), scoresToJson(candidate.scores));
		QJsonObject labels;
		labels.insert(QStringLiteral("boundary_state"),
			      evidenceValue(candidate.evidence, QStringLiteral("boundary_state:")));
		labels.insert(QStringLiteral("boundary_reasons"),
			      evidenceValue(candidate.evidence, QStringLiteral("boundary_reasons:")));
		labels.insert(QStringLiteral("arc_dp"),
			      evidenceValue(candidate.evidence, QStringLiteral("arc_dp_boundary_refined")));
		object.insert(QStringLiteral("classifier_labels"), labels);
		object.insert(QStringLiteral("evidence"), stringListToJson(candidate.evidence));
		array.append(object);
	}
	return array;
}

} // namespace

void prepare_review_scoring_async(QWidget *parent, const QString &videoPath, const CurationSettings &baseSettings,
				  std::function<void(ReviewScoringPreparationResult)> finishedCallback,
				  ReviewScoringProgressCallback progressCallback)
{
	reportProgress(parent, progressCallback, obsText("Status.SuggestClipsCheckingSettings"), 2);

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
	reportProgress(parent, progressCallback, obsText("Status.SuggestClipsPreparingTranscript"), 8);

	blog(LOG_INFO,
	     "Preparing semantic review suggestions from review settings. video=%s transcriptionLanguage=%s sourceLanguage=%s",
	     videoPath.toUtf8().constData(), transcriptSettings.transcriptionLanguage.toUtf8().constData(),
	     transcriptSettings.sourceLanguage.toUtf8().constData());

	ensure_transcript_for_curation_async(
		parent, videoPath, transcriptSettings, true,
		[parent, videoPath, transcriptSettings, startedResult = std::move(startedResult), progressCallback,
		 finishedCallback = std::move(finishedCallback)](RecordingTranscript transcript,
								 bool canceled) mutable {
			ReviewScoringPreparationResult result = std::move(startedResult);
			result.canceled = canceled;
			reportProgress(parent, progressCallback, obsText("Status.SuggestClipsTranscriptReady"), 22);
			if (canceled && transcript.segments.isEmpty()) {
				result.summary = QStringLiteral("transcription_canceled");
				blog(LOG_INFO,
				     "Semantic review suggestion preparation canceled during transcription: %s",
				     videoPath.toUtf8().constData());
				finishOnUiThread(parent, std::move(finishedCallback), std::move(result));
				return;
			}

			if (canceled) {
				result.canceled = false;
				blog(LOG_INFO,
				     "Ignoring late transcription cancel because a usable transcript was produced. video=%s segments=%d",
				     videoPath.toUtf8().constData(), static_cast<int>(transcript.segments.size()));
			}

			if (transcript.segments.isEmpty()) {
				result.summary = QStringLiteral("empty_transcript");
				blog(LOG_WARNING, "Semantic review suggestions skipped because transcript is empty: %s",
				     videoPath.toUtf8().constData());
				finishOnUiThread(parent, std::move(finishedCallback), std::move(result));
				return;
			}

			const QString transcriptContentId =
				Curation::Feedback::CurationFeedbackStore::transcriptContentId(transcript);
			const QString fileContentId =
				Curation::Feedback::CurationFeedbackStore::fileContentId(videoPath);
			result.contentId = transcriptContentId.isEmpty() ? fileContentId : transcriptContentId;
			if (!fileContentId.isEmpty() && fileContentId != result.contentId)
				result.contentIdAliases.append(fileContentId);
			blog(LOG_INFO,
			     "Prepared content identity for semantic review. video=%s contentId=%s aliases=%d",
			     videoPath.toUtf8().constData(), result.contentId.toUtf8().constData(),
			     static_cast<int>(result.contentIdAliases.size()));

			reportProgress(parent, progressCallback, obsText("Status.SuggestClipsPreparingScoring"), 30);

			LlamaServerEmbeddingProviderOptions llamaOptions = llamaServerEmbeddingOptionsFromConfig();
			LlamaServerRerankerProviderOptions rerankerOptions = llamaServerRerankerOptionsFromConfig();

			QObject *context = callbackContext(parent);
			QPointer<QObject> safeContext(context);
			auto cancelRequested = std::make_shared<std::atomic_bool>(false);
			if (!context) {
				result.summary = QStringLiteral("ui_context_unavailable");
				finishOnUiThread(parent, std::move(finishedCallback), std::move(result));
				return;
			}

			auto *workerThread = QThread::create([safeContext, videoPath, transcriptSettings,
							      transcript = std::move(transcript),
							      result = std::move(result), progressCallback,
							      llamaOptions = std::move(llamaOptions),
							      rerankerOptions = std::move(rerankerOptions),
							      cancelRequested,
							      finishedCallback =
								      std::move(finishedCallback)]() mutable {
				reportProgressToContext(safeContext, progressCallback,
							QObject::tr("Starting background clip scoring..."), 32);

				blog(LOG_INFO,
				     "Starting local semantic review scoring on worker thread. video=%s segments=%d thread=%p",
				     videoPath.toUtf8().constData(), static_cast<int>(transcript.segments.size()),
				     QThread::currentThread());

				reportProgressToContext(safeContext, progressCallback,
							QObject::tr("Connecting to local embedding/reranker models..."),
							36);

				std::unique_ptr<SemanticEmbeddingProvider> embeddingProvider;
				std::unique_ptr<SemanticReranker> semanticReranker;
				if (cancelRequested && cancelRequested->load()) {
					result.summary = QStringLiteral("suggest_clips_canceled_before_model_setup");
					finishOnUiContext(safeContext, std::move(finishedCallback), std::move(result));
					return;
				}

				if (llamaOptions.enabled) {
					llamaOptions.cancellationCallback = [cancelRequested]() {
						return cancelRequested && cancelRequested->load();
					};
					// Review scoring needs enough context to see both the opening and
					// the resolution of one full exchange. Use the review cap even when
					// an older saved config still has the previous short-fragment value.
					if (llamaOptions.maxTextChars != kMaxCandidateTextChars)
						llamaOptions.maxTextChars = kMaxCandidateTextChars;
					blog(LOG_INFO,
					     "Local embedding backend enabled for semantic review scoring. endpoint=%s model=%s maxTextChars=%d",
					     llamaOptions.endpoint.toUtf8().constData(),
					     llamaOptions.modelId.toUtf8().constData(), llamaOptions.maxTextChars);
					embeddingProvider =
						std::make_unique<LlamaServerEmbeddingProvider>(llamaOptions);

					if (rerankerOptions.enabled) {
						rerankerOptions.cancellationCallback = [cancelRequested]() {
							return cancelRequested && cancelRequested->load();
						};
						// The reranker must see enough of the candidate to judge whether
						// it actually reaches the first local resolution.
						if (rerankerOptions.maxTextChars != kMaxRerankerTextChars)
							rerankerOptions.maxTextChars = kMaxRerankerTextChars;
						blog(LOG_INFO,
						     "Local Qwen3 reranker backend enabled for semantic review scoring. endpoint=%s model=%s maxTextChars=%d",
						     rerankerOptions.endpoint.toUtf8().constData(),
						     rerankerOptions.modelId.toUtf8().constData(),
						     rerankerOptions.maxTextChars);
						semanticReranker =
							std::make_unique<LlamaServerRerankerProvider>(rerankerOptions);
					} else {
						blog(LOG_INFO,
						     "Local Qwen3 reranker backend disabled. Semantic scoring will rely on embeddings without cross-encoder reranking.");
					}
				}

				reportProgressToContext(safeContext, progressCallback,
							QObject::tr("Building independent marker candidates..."), 42);

				ClipScoringPipelineOptions options =
					optionsForTranscript(transcript, transcriptSettings, embeddingProvider.get(),
							     semanticReranker.get());
				options.videoPath = videoPath;
				options.contentIds = QStringList{result.contentId} + result.contentIdAliases;
				options.cancellationCallback = [cancelRequested]() {
					return cancelRequested && cancelRequested->load();
				};
				options.progressCallback = [safeContext, progressCallback](
								   ClipScoringPipelineProgressUpdate update) mutable {
					reportProgressToContext(safeContext, progressCallback, update.message,
								update.value, update.maximum);
				};

				const QByteArray semanticTargetLog = options.scoring.mainTarget.left(160).toUtf8();
				blog(LOG_INFO,
				     "Semantic review suggestion budget. video=%s durationSec=%.2f maxRawCandidates=%d "
				     "maxCandidatesBeforeEmbedding=%d maxReviewMarkers=%d minSpacingSec=%.2f "
				     "clipBounds=%.2f-%.2f boundaryMin=%.2f coarseWindows=%d coarseRegions=%d "
				     "coarseWindowSec=%.2f coarseStrideSec=%.2f maxPrototypeTexts=%d mmr=%s "
				     "mmrRelevanceWeight=%.2f semanticLanguage=%s presetProfile=%s reliableTarget=%s reviewBudgetProfile=v4_many_markers semanticTarget=%s",
				     videoPath.toUtf8().constData(), transcriptDurationSec(transcript),
				     options.generation.maxRawCandidates, options.budget.maxCandidatesBeforeEmbedding,
				     options.ranking.maxCandidates, options.ranking.minSpacingSec,
				     options.generation.minDurationSec, options.generation.maxDurationSec,
				     options.generation.boundaryMinDurationSec,
				     options.coarseRetrieval.maxWindowsToEmbed, options.coarseRetrieval.maxRegions,
				     options.coarseRetrieval.windowSec, options.coarseRetrieval.strideSec,
				     options.semantic.maxPrototypeTexts, options.ranking.useMmr ? "true" : "false",
				     options.ranking.mmrRelevanceWeight,
				     normalizedSemanticLanguageCode(options.scoring.transcriptionLanguage,
								    options.scoring.sourceLanguage)
					     .toUtf8()
					     .constData(),
				     options.scoring.presetId.toUtf8().constData(),
				     options.scoring.reliableMainTarget ? "true" : "false",
				     semanticTargetLog.constData());

				reportProgressToContext(safeContext, progressCallback,
							QObject::tr("Running semantic marker analysis..."), 48);

				const ClipScoringPipeline pipeline;
				const ClipScoringResult scoring = pipeline.score(transcript, options);
				if (cancelRequested && cancelRequested->load()) {
					result.canceled = true;
					result.summary = scoring.summary.isEmpty()
								 ? QStringLiteral("suggest_clips_canceled")
								 : scoring.summary;
					finishOnUiContext(safeContext, std::move(finishedCallback), std::move(result));
					return;
				}
				reportProgressToContext(safeContext, progressCallback,
							QObject::tr("Ranking marker suggestions..."), 88);
				result.settings = settingsFromScoring(scoring);
				result.applied = !result.settings.clipDurations.isEmpty();
				result.summary = scoring.summary;
				result.candidateDiagnostics = diagnosticsFromScoring(scoring);
				reportProgressToContext(safeContext, progressCallback,
							result.applied ? QObject::tr("Applying suggested markers...")
								       : QObject::tr("No viable clip markers found."),
							96);

				blog(result.applied ? LOG_INFO : LOG_WARNING,
				     "Semantic review suggestions finished. video=%s applied=%s ranges=%d backend=%s summary=%s",
				     videoPath.toUtf8().constData(), result.applied ? "true" : "false",
				     static_cast<int>(result.settings.clipDurations.size()),
				     embeddingProvider ? "llama_server" : "disabled",
				     result.summary.toUtf8().constData());

				reportProgressToContext(safeContext, progressCallback,
							QObject::tr("Clip suggestion analysis finished."), 100);
				finishOnUiContext(safeContext, std::move(finishedCallback), std::move(result));
			});

			QObject::connect(workerThread, &QThread::finished, workerThread, &QObject::deleteLater);
			QObject::connect(context, &QObject::destroyed, workerThread, [workerThread, cancelRequested]() {
				if (cancelRequested)
					cancelRequested->store(true);
				workerThread->requestInterruption();
			});
			workerThread->start();
		});
}

void prepare_review_scoring_async(QWidget *parent, const QString &videoPath,
				  std::function<void(ReviewScoringPreparationResult)> finishedCallback,
				  ReviewScoringProgressCallback progressCallback)
{
	prepare_review_scoring_async(parent, videoPath, CurationSettings{}, std::move(finishedCallback),
				     std::move(progressCallback));
}
