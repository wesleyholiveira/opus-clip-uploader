#include "curation/scoring/feedback-trained-ranker.hpp"

#include "utils/config.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace Curation::Scoring {

namespace {

static double bounded(double value)
{
	if (!std::isfinite(value))
		return 0.0;
	return std::clamp(value, 0.0, 1.0);
}

static double sigmoid(double value)
{
	if (value >= 36.0)
		return 1.0;
	if (value <= -36.0)
		return 0.0;
	return 1.0 / (1.0 + std::exp(-value));
}

static double normalizedSeconds(double value, double reference)
{
	if (!std::isfinite(value) || reference <= 0.0)
		return 0.0;
	return bounded(value / reference);
}

static bool jsonBool(const QJsonObject &object, const QString &key, bool fallback = false)
{
	const QJsonValue value = object.value(key);
	return value.isBool() ? value.toBool() : fallback;
}

} // namespace

QString FeedbackTrainedRanker::defaultModelPath()
{
	const QString configured = PluginConfig::getValue(QString::fromLatin1(CONFIG_FEEDBACK_RANKER_MODEL_PATH), QString()).trimmed();
	if (!configured.isEmpty())
		return configured;
	return QDir(Curation::Feedback::CurationFeedbackStore::feedbackDirectoryPath())
		.filePath(QStringLiteral("feedback-ranker.json"));
}

FeedbackTrainedRankerModel FeedbackTrainedRanker::loadDefaultModel()
{
	return loadModel(defaultModelPath());
}

FeedbackTrainedRankerModel FeedbackTrainedRanker::loadModel(const QString &path)
{
	FeedbackTrainedRankerModel model;
	model.path = path;
	QFile file(path);
	if (path.trimmed().isEmpty() || !file.exists() || !file.open(QIODevice::ReadOnly))
		return model;

	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
		blog(LOG_WARNING, "[clip-cropper] Failed to parse feedback ranker model. path=%s error=%s",
		     path.toUtf8().constData(), parseError.errorString().toUtf8().constData());
		return model;
	}

	const QJsonObject root = document.object();
	if (root.value(QStringLiteral("schema_version")).toInt() != 1)
		return model;
	model.modelType = root.value(QStringLiteral("model_type")).toString();
	if (model.modelType != QStringLiteral("logistic_regression") &&
	    model.modelType != QStringLiteral("gbdt_tree_ensemble"))
		return model;
	model.presetId = root.value(QStringLiteral("preset")).toString();
	model.trainedAtUtc = root.value(QStringLiteral("trained_at_utc")).toString();
	model.records = root.value(QStringLiteral("records")).toInt();
	model.positives = root.value(QStringLiteral("positives")).toInt();
	model.negatives = root.value(QStringLiteral("negatives")).toInt();
	model.intercept = root.value(QStringLiteral("intercept")).toDouble(0.0);
	model.baseScore = root.value(QStringLiteral("base_score")).toDouble(model.intercept);

	const QJsonObject thresholds = root.value(QStringLiteral("thresholds")).toObject();
	model.rejectBelow = thresholds.value(QStringLiteral("reject_below")).toDouble(0.0);
	model.acceptAbove = thresholds.value(QStringLiteral("accept_above")).toDouble(0.52);
	model.strongAcceptAbove = thresholds.value(QStringLiteral("strong_accept_above")).toDouble(1.0);
	if (model.acceptAbove <= 0.0)
		model.acceptAbove = 0.52;

	const QJsonArray features = root.value(QStringLiteral("feature_order")).toArray();
	for (const QJsonValue &value : features) {
		const QString feature = value.toString().trimmed();
		if (!feature.isEmpty() && !model.featureOrder.contains(feature))
			model.featureOrder.append(feature);
	}

	if (model.modelType == QStringLiteral("logistic_regression")) {
		const QJsonObject weights = root.value(QStringLiteral("weights")).toObject();
		for (auto it = weights.begin(); it != weights.end(); ++it)
			model.weights.insert(it.key(), it.value().toDouble(0.0));
		model.loaded = !model.featureOrder.isEmpty() && !model.weights.isEmpty() && model.records > 0;
		return model;
	}

	const QJsonArray trees = root.value(QStringLiteral("trees")).toArray();
	for (const QJsonValue &treeValue : trees) {
		if (!treeValue.isObject())
			continue;
		const QJsonObject treeObject = treeValue.toObject();
		FeedbackTrainedRankerTree tree;
		tree.weight = treeObject.value(QStringLiteral("weight")).toDouble(1.0);
		const QJsonArray nodes = treeObject.value(QStringLiteral("nodes")).toArray();
		for (const QJsonValue &nodeValue : nodes) {
			if (!nodeValue.isObject())
				continue;
			const QJsonObject nodeObject = nodeValue.toObject();
			FeedbackTrainedRankerTreeNode node;
			node.leaf = nodeObject.value(QStringLiteral("leaf")).toBool(false);
			node.value = nodeObject.value(QStringLiteral("value")).toDouble(0.0);
			node.feature = nodeObject.value(QStringLiteral("feature")).toString();
			node.threshold = nodeObject.value(QStringLiteral("threshold")).toDouble(0.0);
			node.left = nodeObject.value(QStringLiteral("left")).toInt(-1);
			node.right = nodeObject.value(QStringLiteral("right")).toInt(-1);
			tree.nodes.append(node);
		}
		if (!tree.nodes.isEmpty())
			model.trees.append(tree);
	}
	model.loaded = !model.featureOrder.isEmpty() && !model.trees.isEmpty() && model.records > 0;
	return model;
}

bool FeedbackTrainedRanker::hasEvidence(const ClipCandidate &candidate, const QString &needle)
{
	for (const QString &evidence : candidate.evidence) {
		if (evidence == needle || evidence.contains(needle))
			return true;
	}
	return false;
}

bool FeedbackTrainedRanker::isPositiveGroundTruthCandidate(const ClipCandidate &candidate)
{
	return candidate.source.contains(QStringLiteral("feedback_positive_exact_seed")) ||
	       hasEvidence(candidate, QStringLiteral("feedback_positive_exact_seed_preserved")) ||
	       hasEvidence(candidate, QStringLiteral("complete_viewer_arc_gate_passed_by_user_feedback"));
}

bool FeedbackTrainedRanker::isNegativeContaminated(const FeedbackSimilarityFeatures &features)
{
	return features.negativeRangeContamination && features.negativeScore >= 0.64 && features.margin <= 0.08;
}

static bool rankerHasEvidence(const ClipCandidate &candidate, const QString &needle)
{
	for (const QString &evidence : candidate.evidence) {
		if (evidence == needle || evidence.contains(needle))
			return true;
	}
	return false;
}

static QString foldedRankerText(QString value)
{
	QString normalized = value.toLower().normalized(QString::NormalizationForm_D);
	QString folded;
	folded.reserve(normalized.size());
	for (const QChar ch : normalized) {
		const QChar::Category category = ch.category();
		if (category == QChar::Mark_NonSpacing || category == QChar::Mark_SpacingCombining ||
		    category == QChar::Mark_Enclosing)
			continue;
		folded.append(ch);
	}
	return folded.simplified();
}

static bool rankerTextContainsAny(const QString &text, std::initializer_list<const char *> needles)
{
	for (const char *needle : needles) {
		if (needle && text.contains(QString::fromUtf8(needle)))
			return true;
	}
	return false;
}

static bool hasHardRankerContextBlocker(const ClipCandidate &candidate)
{
	const bool exactUserSeed =
		candidate.source.contains(QStringLiteral("feedback_positive_exact_seed")) ||
		rankerHasEvidence(candidate, QStringLiteral("feedback_positive_exact_seed_preserved")) ||
		rankerHasEvidence(candidate, QStringLiteral("complete_viewer_arc_gate_passed_by_user_feedback"));
	if (exactUserSeed)
		return false;
	const QString text = foldedRankerText(candidate.text + QStringLiteral(" ") + candidate.anchorText);
	const bool personalOrQuestion =
		text.contains(QLatin1Char('?')) ||
		rankerTextContainsAny(text, {"o que", "como", "devo", "deveria", "me ajuda", "conselho", "sou ",
					     "eu tenho", "nao consigo", "não consigo", "me sinto", "minha ", "meu "});
	const bool musicOrSetupText =
		(text.contains(QStringLiteral("musica")) || text.contains(QStringLiteral("música")) ||
		 rankerTextContainsAny(text, {"acertando o negocio", "acertando o negócio", "configurando", "setup",
					      "abrindo live", "testando"})) &&
		!personalOrQuestion;
	const bool metaEvidence = rankerHasEvidence(candidate, QStringLiteral("meta_prelude")) ||
				  rankerHasEvidence(candidate, QStringLiteral("drift+block")) ||
				  rankerHasEvidence(candidate, QStringLiteral("Música")) ||
				  rankerHasEvidence(candidate, QStringLiteral("Musica"));
	const bool recoveredOpening =
		rankerHasEvidence(candidate, QStringLiteral("viewer_arc_opening_recovered")) ||
		rankerHasEvidence(candidate, QStringLiteral("exchange_arc_state_machine:repaired_strong"));
	const bool explicitOrigin =
		(!recoveredOpening && candidate.startsNearViewerCue) ||
		rankerHasEvidence(candidate, QStringLiteral("targeted_viewer_message_cue")) ||
		rankerHasEvidence(candidate, QStringLiteral("exchange_arc_viewer_message_cue_confirmed")) ||
		rankerHasEvidence(candidate, QStringLiteral("flags=origin")) ||
		rankerHasEvidence(candidate, QStringLiteral("viewer_response_cue")) ||
		candidate.scores.semanticViewerMessage >= 0.70;
	const bool noOriginOrAnswer =
		rankerHasEvidence(candidate, QStringLiteral("exchange_arc_window_dfs_no_valid_origin_or_answer")) ||
		rankerHasEvidence(candidate, QStringLiteral("exchange_arc_no_valid_subspan")) ||
		rankerHasEvidence(candidate, QStringLiteral("missing_viewer_message_cue"));
	const bool noArcShape = candidate.scores.arcCompleteness <= 0.02 && candidate.scores.arcOpening <= 0.04 &&
				candidate.scores.arcDevelopment <= 0.04 && candidate.scores.arcConclusion <= 0.04;
	// Missing viewer origin is handled by ViewerArcGate as a soft arc failure.
	// The ranker should only hard-reject obvious intro/music/meta/setup spans or
	// completely shapeless early/prelude spans, otherwise positive feedback patterns
	// never get a chance to recover a valid opening.
	return (candidate.range.startSec <= 75.0 && (musicOrSetupText || metaEvidence)) ||
	       (noArcShape && !explicitOrigin &&
		(metaEvidence || musicOrSetupText || candidate.range.startSec <= 75.0));
}

static bool hasFeedbackGuidedPositiveSource(const ClipCandidate &candidate)
{
	const bool exactUserSeed =
		candidate.source.contains(QStringLiteral("feedback_positive_exact_seed")) ||
		rankerHasEvidence(candidate, QStringLiteral("feedback_positive_exact_seed_preserved")) ||
		rankerHasEvidence(candidate, QStringLiteral("complete_viewer_arc_gate_passed_by_user_feedback"));
	return exactUserSeed || candidate.source == QStringLiteral("feedback_positive_pattern_search") ||
	       candidate.source == QStringLiteral("feedback_positive_semantic_prototype") ||
	       rankerHasEvidence(candidate, QStringLiteral("feedback_positive_pattern_search")) ||
	       rankerHasEvidence(candidate, QStringLiteral("feedback_positive_semantic_prototype")) ||
	       rankerHasEvidence(candidate, QStringLiteral("feedback_consistency_positive_margin"));
}

static bool positiveFeedbackDominates(const FeedbackSimilarityFeatures &features)
{
	return features.margin >= 0.10 && features.positiveScore >= 0.26 &&
	       features.positiveScore >= (features.negativeScore + 0.08);
}

static double rankerFeatureCoverage(const ClipCandidate &candidate)
{
	int nonZero = 0;
	constexpr int total = 18;
	const ClipCandidateScores &s = candidate.scores;
	const double values[total] = {
		s.semanticClipValue,
		s.semanticDirectAnswer,
		s.semanticViewerMessage,
		s.semanticHook,
		s.semanticResolution,
		s.semanticEmpathy,
		s.rerankerRaw,
		s.rerankerClipQualityMargin,
		s.arcCompleteness,
		s.arcOpening,
		s.arcDevelopment,
		s.arcConclusion,
		s.coarseSemantic,
		s.viewerResponse,
		s.hook,
		s.advice,
		s.explanation,
		s.emotional,
	};
	for (double value : values) {
		if (std::abs(value) > 1e-6)
			++nonZero;
	}
	return bounded(static_cast<double>(nonZero) / static_cast<double>(total));
}

double FeedbackTrainedRanker::featureValue(const QString &name, const ClipCandidate &candidate,
					   const FeedbackSimilarityFeatures &features)
{
	const ClipCandidateScores &s = candidate.scores;
	if (name == QStringLiteral("duration"))
		return bounded(s.duration);
	if (name == QStringLiteral("boundary"))
		return bounded(s.boundary);
	if (name == QStringLiteral("hook"))
		return bounded(s.hook);
	if (name == QStringLiteral("emotional"))
		return bounded(s.emotional);
	if (name == QStringLiteral("advice"))
		return bounded(s.advice);
	if (name == QStringLiteral("explanation"))
		return bounded(s.explanation);
	if (name == QStringLiteral("story"))
		return bounded(s.story);
	if (name == QStringLiteral("opinion"))
		return bounded(s.opinion);
	if (name == QStringLiteral("tutorial"))
		return bounded(s.tutorial);
	if (name == QStringLiteral("viewerResponse"))
		return bounded(s.viewerResponse);
	if (name == QStringLiteral("coarseSemantic"))
		return bounded(s.coarseSemantic);
	if (name == QStringLiteral("semanticTarget"))
		return bounded(s.semanticTarget);
	if (name == QStringLiteral("embeddingTarget"))
		return bounded(s.embeddingTarget);
	if (name == QStringLiteral("semanticViewerMessage"))
		return bounded(s.semanticViewerMessage);
	if (name == QStringLiteral("semanticDirectAnswer"))
		return bounded(s.semanticDirectAnswer);
	if (name == QStringLiteral("semanticNoise"))
		return bounded(s.semanticNoise);
	if (name == QStringLiteral("semanticTopicShift"))
		return bounded(s.semanticTopicShift);
	if (name == QStringLiteral("semanticClipValue"))
		return bounded(s.semanticClipValue);
	if (name == QStringLiteral("semanticEmpathy"))
		return bounded(s.semanticEmpathy);
	if (name == QStringLiteral("semanticHook"))
		return bounded(s.semanticHook);
	if (name == QStringLiteral("semanticResolution"))
		return bounded(s.semanticResolution);
	if (name == QStringLiteral("semanticMetaNoise"))
		return bounded(s.semanticMetaNoise);
	if (name == QStringLiteral("semanticOpeningHook"))
		return bounded(s.semanticOpeningHook);
	if (name == QStringLiteral("semanticOpeningMetaNoise"))
		return bounded(s.semanticOpeningMetaNoise);
	if (name == QStringLiteral("semanticEndingResolution"))
		return bounded(s.semanticEndingResolution);
	if (name == QStringLiteral("semanticEndingMetaNoise"))
		return bounded(s.semanticEndingMetaNoise);
	if (name == QStringLiteral("semanticEndingTopicShift"))
		return bounded(s.semanticEndingTopicShift);
	if (name == QStringLiteral("topicContinuity"))
		return bounded(s.topicContinuity);
	if (name == QStringLiteral("semanticFocusContinuity"))
		return bounded(s.semanticFocusContinuity);
	if (name == QStringLiteral("reranker"))
		return bounded(s.reranker);
	if (name == QStringLiteral("rerankerRaw"))
		return bounded(s.rerankerRaw);
	if (name == QStringLiteral("rerankerBadClip"))
		return bounded(s.rerankerBadClip);
	if (name == QStringLiteral("rerankerOpeningDefect"))
		return bounded(s.rerankerOpeningDefect);
	if (name == QStringLiteral("rerankerEndingDefect"))
		return bounded(s.rerankerEndingDefect);
	if (name == QStringLiteral("rerankerStructureDefect"))
		return bounded(s.rerankerStructureDefect);
	if (name == QStringLiteral("rerankerClipQualityMargin"))
		return bounded((s.rerankerClipQualityMargin + 1.0) * 0.5);
	if (name == QStringLiteral("qualityGate"))
		return bounded(s.qualityGate);
	if (name == QStringLiteral("noise"))
		return bounded(s.noise);
	if (name == QStringLiteral("pauseBeforeSec"))
		return normalizedSeconds(s.pauseBeforeSec, 12.0);
	if (name == QStringLiteral("pauseAfterSec"))
		return normalizedSeconds(s.pauseAfterSec, 12.0);
	if (name == QStringLiteral("maxInternalPauseSec"))
		return normalizedSeconds(s.maxInternalPauseSec, 16.0);
	if (name == QStringLiteral("pauseBoundary"))
		return bounded(s.pauseBoundary);
	if (name == QStringLiteral("arcOpening"))
		return bounded(s.arcOpening);
	if (name == QStringLiteral("arcDevelopment"))
		return bounded(s.arcDevelopment);
	if (name == QStringLiteral("arcConclusion"))
		return bounded(s.arcConclusion);
	if (name == QStringLiteral("arcBoundaryCleanliness"))
		return bounded(s.arcBoundaryCleanliness);
	if (name == QStringLiteral("arcTailRisk"))
		return bounded(s.arcTailRisk);
	if (name == QStringLiteral("arcCompleteness"))
		return bounded(s.arcCompleteness);
	if (name == QStringLiteral("final"))
		return bounded(s.final);

	if (name == QStringLiteral("feedbackPositiveRange"))
		return bounded(features.positiveRangeSimilarity);
	if (name == QStringLiteral("feedbackNegativeRange"))
		return bounded(features.negativeRangeSimilarity);
	if (name == QStringLiteral("feedbackPositiveText"))
		return bounded(features.positiveTextSimilarity);
	if (name == QStringLiteral("feedbackNegativeText"))
		return bounded(features.negativeTextSimilarity);
	if (name == QStringLiteral("feedbackPositiveScore"))
		return bounded(features.positiveScore);
	if (name == QStringLiteral("feedbackNegativeScore"))
		return bounded(features.negativeScore);
	if (name == QStringLiteral("feedbackMargin"))
		return bounded((features.margin + 1.0) * 0.5);
	if (name == QStringLiteral("feedbackPositiveDominates"))
		return positiveFeedbackDominates(features) ? 1.0 : 0.0;
	if (name == QStringLiteral("feedbackRawMarginPositive"))
		return bounded(std::max(0.0, features.margin));
	if (name == QStringLiteral("feedbackPositiveOverlap"))
		return normalizedSeconds(features.positiveOverlapSec, 60.0);
	if (name == QStringLiteral("feedbackNegativeOverlap"))
		return normalizedSeconds(features.negativeOverlapSec, 60.0);
	if (name == QStringLiteral("feedbackNegativeContamination"))
		return features.negativeRangeContamination ? 1.0 : 0.0;
	if (name == QStringLiteral("feedbackExplainedByPositive"))
		return features.explainedByPositiveRange ? 1.0 : 0.0;

	if (name == QStringLiteral("evidenceViewerResponseCue"))
		return hasEvidence(candidate, QStringLiteral("viewer_response_cue")) ? 1.0 : 0.0;
	if (name == QStringLiteral("evidenceCleanBoundary"))
		return hasEvidence(candidate, QStringLiteral("clean_boundary")) ? 1.0 : 0.0;
	if (name == QStringLiteral("evidenceQualityPassed"))
		return hasEvidence(candidate, QStringLiteral("quality_gate_passed")) ? 1.0 : 0.0;
	if (name == QStringLiteral("evidenceArcInvalid"))
		return hasEvidence(candidate, QStringLiteral("exchange_arc_state_machine:invalid")) ? 1.0 : 0.0;
	if (name == QStringLiteral("evidenceMissingViewerCue"))
		return hasEvidence(candidate, QStringLiteral("missing_viewer_message_cue")) ? 1.0 : 0.0;
	if (name == QStringLiteral("evidenceMultipleViewerMessages"))
		return hasEvidence(candidate, QStringLiteral("multiple_viewer_messages_inside_arc")) ? 1.0 : 0.0;
	if (name == QStringLiteral("evidenceTopicShift"))
		return hasEvidence(candidate, QStringLiteral("topic_shift")) ? 1.0 : 0.0;
	if (name == QStringLiteral("evidencePositiveSeed"))
		return isPositiveGroundTruthCandidate(candidate) ? 1.0 : 0.0;
	if (name == QStringLiteral("evidencePatternSearch"))
		return candidate.source == QStringLiteral("feedback_positive_pattern_search") ||
				       hasEvidence(candidate, QStringLiteral("feedback_positive_pattern_search"))
			       ? 1.0
			       : 0.0;
	if (name == QStringLiteral("evidenceSemanticResolution"))
		return hasEvidence(candidate, QStringLiteral("semantic_resolution")) ? 1.0 : 0.0;
	if (name == QStringLiteral("evidenceSemanticHook"))
		return hasEvidence(candidate, QStringLiteral("semantic_hook")) ? 1.0 : 0.0;
	if (name == QStringLiteral("evidenceRerankerStrongMatch"))
		return hasEvidence(candidate, QStringLiteral("reranker_strong_match")) ? 1.0 : 0.0;
	if (name == QStringLiteral("evidenceMetaPrelude"))
		return hasEvidence(candidate, QStringLiteral("meta_prelude")) ||
				       hasEvidence(candidate, QStringLiteral("drift+block"))
			       ? 1.0
			       : 0.0;
	if (name == QStringLiteral("evidenceMusicOrIntro"))
		return hasHardRankerContextBlocker(candidate) && candidate.range.startSec <= 75.0 ? 1.0 : 0.0;
	if (name == QStringLiteral("evidenceHardContextBlocker"))
		return hasHardRankerContextBlocker(candidate) ? 1.0 : 0.0;
	if (name == QStringLiteral("featureCoverage"))
		return rankerFeatureCoverage(candidate);

	return 0.0;
}

double FeedbackTrainedRanker::logisticScore(const ClipCandidate &candidate,
						 const FeedbackSimilarityFeatures &feedbackFeatures,
						 const FeedbackTrainedRankerModel &model)
{
	double linear = model.intercept;
	for (const QString &feature : model.featureOrder)
		linear += model.weights.value(feature, 0.0) * featureValue(feature, candidate, feedbackFeatures);
	return bounded(sigmoid(linear));
}

double FeedbackTrainedRanker::gbdtScore(const ClipCandidate &candidate,
					     const FeedbackSimilarityFeatures &feedbackFeatures,
					     const FeedbackTrainedRankerModel &model)
{
	double raw = model.baseScore;
	for (const FeedbackTrainedRankerTree &tree : model.trees) {
		if (tree.nodes.isEmpty())
			continue;
		int index = 0;
		int guard = 0;
		while (index >= 0 && index < tree.nodes.size() && guard++ < 128) {
			const FeedbackTrainedRankerTreeNode &node = tree.nodes.at(index);
			if (node.leaf) {
				raw += tree.weight * node.value;
				break;
			}
			const double value = featureValue(node.feature, candidate, feedbackFeatures);
			index = (value <= node.threshold) ? node.left : node.right;
		}
	}
	return bounded(sigmoid(raw));
}

double FeedbackTrainedRanker::scoreCandidate(const ClipCandidate &candidate, const TranscriptIndex &index,
					     const Curation::Feedback::FeedbackRangeMemory &memory,
					     const FeedbackTrainedRankerModel &model,
					     FeedbackSimilarityFeatures *feedbackFeatures)
{
	FeedbackSimilarityScorer feedbackScorer;
	FeedbackSimilarityFeatures features = feedbackScorer.score(candidate, index, memory);
	if (feedbackFeatures)
		*feedbackFeatures = features;

	if (model.modelType == QStringLiteral("gbdt_tree_ensemble"))
		return gbdtScore(candidate, features, model);
	return logisticScore(candidate, features, model);
}

QVector<ClipCandidate> FeedbackTrainedRanker::apply(QVector<ClipCandidate> candidates, const TranscriptIndex &index,
						    const Curation::Feedback::FeedbackRangeMemory &memory,
						    const QString &presetId, const QString &videoPath) const
{
	if (candidates.isEmpty() || !memory.loaded)
		return candidates;
	const FeedbackTrainedRankerModel model = loadDefaultModel();
	if (!model.loaded)
		return candidates;
	if (!model.presetId.isEmpty() && !presetId.isEmpty() && model.presetId != presetId &&
	    model.presetId != QStringLiteral("all"))
		return candidates;

	int scored = 0;
	int rejected = 0;
	double best = 0.0;
	for (ClipCandidate &candidate : candidates) {
		FeedbackSimilarityFeatures features;
		const double learnedScore = scoreCandidate(candidate, index, memory, model, &features);
		best = std::max(best, learnedScore);
		++scored;

		const bool positiveGroundTruth = isPositiveGroundTruthCandidate(candidate);
		const bool negativeContaminated = isNegativeContaminated(features);
		const bool hardContextBlocked = hasHardRankerContextBlocker(candidate);
		const bool positiveBacked = positiveGroundTruth || (hasFeedbackGuidedPositiveSource(candidate) &&
								    positiveFeedbackDominates(features));
		candidate.evidence.append(
			QStringLiteral("feedback_trained_ranker_score:%1").arg(QString::number(learnedScore, 'f', 2)));
		candidate.evidence.append(
			QStringLiteral("feedback_trained_ranker_model_records:%1").arg(model.records));
		candidate.evidence.append(QStringLiteral("feedback_trained_ranker_model_type:%1").arg(model.modelType));
		candidate.evidence.removeDuplicates();

		if (hardContextBlocked && !positiveGroundTruth && !positiveBacked) {
			candidate.rejectedByQualityGate = true;
			candidate.rejectionReason = QStringLiteral("feedback_trained_ranker_hard_context_blocker");
			candidate.evidence.append(
				QStringLiteral("feedback_trained_ranker_rejected_hard_context_blocker"));
			++rejected;
			continue;
		}
		if (hardContextBlocked && positiveBacked)
			candidate.evidence.append(
				QStringLiteral("feedback_trained_ranker_hard_context_blocker_recoverable_positive"));

		if (negativeContaminated && !positiveGroundTruth) {
			candidate.rejectedByQualityGate = true;
			candidate.rejectionReason = QStringLiteral("feedback_trained_ranker_negative_contamination");
			candidate.evidence.append(
				QStringLiteral("feedback_trained_ranker_rejected_negative_contamination"));
			++rejected;
			continue;
		}

		if (!positiveGroundTruth && model.rejectBelow > 0.0 && learnedScore < model.rejectBelow &&
		    features.negativeScore >= features.positiveScore) {
			candidate.rejectedByQualityGate = true;
			candidate.rejectionReason = QStringLiteral("feedback_trained_ranker_below_threshold");
			candidate.evidence.append(QStringLiteral("feedback_trained_ranker_rejected_below_threshold"));
			++rejected;
			continue;
		}

		const double blended = (candidate.scores.final * 0.38) + (learnedScore * 0.62);
		candidate.scores.final = positiveGroundTruth
						 ? std::max(0.88, std::max(candidate.scores.final, learnedScore))
						 : bounded(blended);
		const double acceptThreshold = std::clamp(model.acceptAbove, 0.42, 0.72);
		const double strongThreshold = std::max(model.strongAcceptAbove, 0.76);
		const bool structurallyBacked =
			!candidate.rejectedByQualityGate &&
			(candidate.scores.arcCompleteness >= 0.28 || candidate.scores.semanticViewerMessage >= 0.70 ||
			 candidate.startsNearViewerCue ||
			 rankerHasEvidence(candidate, QStringLiteral("exchange_arc_state_machine:valid")));
		if ((positiveBacked || !hardContextBlocked) && learnedScore >= acceptThreshold &&
		    !(negativeContaminated && !positiveGroundTruth) && (positiveBacked || structurallyBacked)) {
			candidate.evidence.append(QStringLiteral("feedback_trained_ranker_accept"));
		} else if (learnedScore >= acceptThreshold && !positiveGroundTruth && !positiveBacked) {
			candidate.evidence.append(QStringLiteral("feedback_trained_ranker_not_positive_backed"));
		}
		if ((positiveBacked || !hardContextBlocked) && learnedScore >= strongThreshold &&
		    (positiveBacked || structurallyBacked))
			candidate.evidence.append(QStringLiteral("feedback_trained_ranker_strong_accept"));
		candidate.evidence.removeDuplicates();
	}

	blog(LOG_INFO,
	     "[clip-cropper] Feedback-trained ranker applied. video=%s model=%s candidates=%d rejected=%d best=%.2f records=%d positives=%d negatives=%d",
	     videoPath.toUtf8().constData(), model.path.toUtf8().constData(), scored, rejected, best, model.records,
	     model.positives, model.negatives);
	return candidates;
}

} // namespace Curation::Scoring
