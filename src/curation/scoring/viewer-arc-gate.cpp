#include "curation/scoring/viewer-arc-gate.hpp"

#include "curation/feedback/boundary-calibration.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace Curation::Scoring {

namespace {

static bool hasEvidence(const ClipCandidate &candidate, const QString &value)
{
	for (const QString &evidence : candidate.evidence) {
		if (evidence == value)
			return true;
	}
	return false;
}

static bool hasEvidenceStartingWith(const ClipCandidate &candidate, const QString &prefix)
{
	for (const QString &evidence : candidate.evidence) {
		if (evidence.startsWith(prefix))
			return true;
	}
	return false;
}

static bool hasEvidenceContaining(const ClipCandidate &candidate, const QString &needle)
{
	for (const QString &evidence : candidate.evidence) {
		if (evidence.contains(needle))
			return true;
	}
	return false;
}

static double evidenceScoreValue(const ClipCandidate &candidate, const QString &prefix)
{
	for (const QString &evidence : candidate.evidence) {
		if (!evidence.startsWith(prefix))
			continue;
		bool ok = false;
		const double value = evidence.mid(prefix.size()).toDouble(&ok);
		if (ok)
			return value;
	}
	return 0.0;
}

static QString foldedCueText(QString value)
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

static bool textContainsAny(const QString &text, std::initializer_list<const char *> needles)
{
	for (const char *needle : needles) {
		if (needle && text.contains(QString::fromUtf8(needle)))
			return true;
	}
	return false;
}

static bool isRecoverableOpeningReason(const QString &reason)
{
	return reason == QStringLiteral("missing_explicit_opening") ||
		reason == QStringLiteral("missing_viewer_message_cue") ||
		reason == QStringLiteral("invalid") || reason.trimmed().isEmpty();
}

static bool candidateTextLooksLikeMusicOrMetaPrelude(const ClipCandidate &candidate)
{
	const QString text = foldedCueText(candidate.text + QStringLiteral(" ") + candidate.anchorText);
	if (text.isEmpty())
		return false;
	const bool musicOnly = text.contains(QStringLiteral("musica")) || text.contains(QStringLiteral("música"));
	const bool setupTalk = textContainsAny(text, {
		"acertando o negocio", "acertando o negócio", "configurando", "setup", "abrindo live",
		"comecando a live", "começando a live", "ja volto", "já volto", "pera ai", "pera aí",
		"calma ai", "calma aí", "so um minuto", "só um minuto", "testando", "volume"
	});
	const bool hasPersonalOrQuestion = text.contains(QLatin1Char('?')) || textContainsAny(text, {
		"o que", "como", "devo", "deveria", "me ajuda", "conselho", "sou ", "eu tenho",
		"nao consigo", "não consigo", "me sinto", "minha ", "meu "
	});
	return (musicOnly || setupTalk) && !hasPersonalOrQuestion;
}

static QString arcInvalidReasonFromEvidence(const ClipCandidate &candidate)
{
	for (const QString &evidence : candidate.evidence) {
		if (!evidence.startsWith(QStringLiteral("exchange_arc_state_machine:invalid")))
			continue;
		const int reasonIndex = evidence.indexOf(QStringLiteral("reason:"));
		return reasonIndex >= 0 ? evidence.mid(reasonIndex + 7).trimmed() : QStringLiteral("invalid");
	}
	return {};
}

static bool hasRecoveredOpening(const ClipCandidate &candidate)
{
	return hasEvidence(candidate, QStringLiteral("viewer_arc_opening_recovered")) ||
		hasEvidenceStartingWith(candidate, QStringLiteral("viewer_arc_opening_recovered_from_segment:")) ||
		hasEvidenceStartingWith(candidate, QStringLiteral("exchange_arc_state_machine:repaired_strong"));
}

static bool hasStrongRecoveredOpening(const ClipCandidate &candidate)
{
	return hasRecoveredOpening(candidate) &&
		evidenceScoreValue(candidate, QStringLiteral("viewer_arc_opening_recovered_score:")) >= 4.0;
}

static bool hasConcreteViewerOriginForLearnedArc(const ClipCandidate &candidate)
{
	return ((candidate.startsNearViewerCue && (!hasRecoveredOpening(candidate) || hasStrongRecoveredOpening(candidate)))) ||
		hasEvidenceContaining(candidate, QStringLiteral("targeted_viewer_message_cue")) ||
		hasEvidenceContaining(candidate, QStringLiteral("exchange_arc_viewer_message_cue_confirmed")) ||
		hasEvidenceContaining(candidate, QStringLiteral("flags=origin")) ||
		hasEvidenceContaining(candidate, QStringLiteral("viewer_response_cue")) ||
		candidate.scores.semanticViewerMessage >= 0.70;
}

static bool hasHardContextBlockerForLearnedArc(const ClipCandidate &candidate)
{
	const bool explicitOrigin = hasConcreteViewerOriginForLearnedArc(candidate);
	const bool metaOrMusicEvidence = hasEvidenceContaining(candidate, QStringLiteral("meta_prelude")) ||
		hasEvidenceContaining(candidate, QStringLiteral("drift+block")) ||
		hasEvidenceContaining(candidate, QStringLiteral("Música")) ||
		hasEvidenceContaining(candidate, QStringLiteral("Musica"));
	const bool introMeta = candidate.range.startSec <= 75.0 &&
		(metaOrMusicEvidence || candidateTextLooksLikeMusicOrMetaPrelude(candidate));
	const bool missingViewerOrigin = arcInvalidReasonFromEvidence(candidate) == QStringLiteral("missing_viewer_message_cue") &&
		!explicitOrigin;
	const bool noOriginOrAnswer = hasEvidence(candidate, QStringLiteral("exchange_arc_window_dfs_no_valid_origin_or_answer")) ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_no_valid_subspan"));
	const bool noArcShape = candidate.scores.arcCompleteness <= 0.02 && candidate.scores.arcOpening <= 0.04 &&
		candidate.scores.arcDevelopment <= 0.04 && candidate.scores.arcConclusion <= 0.04;
	// Missing viewer origin is usually a soft arc failure, not a hard blocker.
	// Keep hard blockers for intro/music/meta/setup and completely shapeless early/prelude spans;
	// learned feedback support will decide whether a pattern/prototype can survive without a perfect state machine.
	return introMeta ||
		(noArcShape && !explicitOrigin && (metaOrMusicEvidence || candidate.range.startSec <= 75.0));
}


static bool targetAllowsSelfContainedAdviceArc(const ViewerArcGateOptions &options)
{
	if (options.presetId != QStringLiteral("viewer_message_response"))
		return false;
	const QString target = foldedCueText(options.mainTarget);
	if (target.isEmpty())
		return false;
	return textContainsAny(target, {
		"motivational speech", "motivational", "mental health", "self care", "personal growth",
		"emotional pain", "relationships", "relationship", "terapia", "ansiedade", "depress",
		"dor emocional", "crescimento pessoal", "auto cuidado", "autocuidado", "relacionamento"
	});
}

static bool hasFeedbackGuidedSelfContainedSignal(const ClipCandidate &candidate)
{
	const double positiveRange = evidenceScoreValue(candidate, QStringLiteral("feedback_positive_range:"));
	const double positiveText = evidenceScoreValue(candidate, QStringLiteral("feedback_positive_text:"));
	const double positiveGuided = evidenceScoreValue(candidate, QStringLiteral("feedback_positive_guided_score:"));
	const double feedbackMargin = evidenceScoreValue(candidate, QStringLiteral("feedback_margin:"));
	const double prototypeSimilarity = evidenceScoreValue(candidate, QStringLiteral("feedback_prototype_similarity:"));
	const double patternScore = evidenceScoreValue(candidate, QStringLiteral("feedback_pattern_score:"));
	const bool feedbackPatternOrPrototype = candidate.source.contains(QStringLiteral("feedback_positive_pattern")) ||
		candidate.source.contains(QStringLiteral("feedback_positive_semantic_prototype")) ||
		hasEvidence(candidate, QStringLiteral("feedback_positive_pattern_search")) ||
		hasEvidence(candidate, QStringLiteral("feedback_positive_semantic_prototype")) ||
		hasEvidenceContaining(candidate, QStringLiteral("feedback_consistency_positive_margin"));
	return feedbackPatternOrPrototype || positiveGuided >= 0.18 || positiveRange >= 0.18 ||
		positiveText >= 0.30 || feedbackMargin >= 0.12 || prototypeSimilarity >= 0.24 || patternScore >= 0.24;
}

} // namespace

double ViewerArcGate::calibratedMinArcConfidence(const ViewerArcGateOptions &options) const
{
	const Curation::Feedback::BoundaryCalibrationProfile calibration =
		Curation::Feedback::BoundaryCalibration::profileForPreset(options.presetId);
	if (calibration.loaded && calibration.minArcConfidence >= 0.0)
		return std::clamp(calibration.minArcConfidence, 0.24, 0.62);
	return 0.32;
}

bool ViewerArcGate::isUserAcceptedFeedbackSeed(const ClipCandidate &candidate) const
{
	return candidate.source == QStringLiteral("feedback_positive_exact_seed") ||
		candidate.source == QStringLiteral("feedback_positive_seed") ||
		hasEvidence(candidate, QStringLiteral("feedback_positive_exact_seed")) ||
		hasEvidence(candidate, QStringLiteral("feedback_positive_seed")) ||
		hasEvidence(candidate, QStringLiteral("complete_viewer_arc_gate_passed_by_user_feedback"));
}

bool ViewerArcGate::isFeedbackSuppressed(const ClipCandidate &candidate) const
{
	return candidate.rejectionReason == QStringLiteral("feedback_rejected_range") ||
		candidate.rejectionReason == QStringLiteral("feedback_negative_range_contamination") ||
		hasEvidence(candidate, QStringLiteral("feedback_negative_suppressed")) ||
		hasEvidence(candidate, QStringLiteral("feedback_consistency_rejected"));
}

QString ViewerArcGate::invalidReason(const ClipCandidate &candidate) const
{
	for (const QString &evidence : candidate.evidence) {
		if (!evidence.startsWith(QStringLiteral("exchange_arc_state_machine:invalid")))
			continue;
		const int reasonIndex = evidence.indexOf(QStringLiteral("reason:"));
		return reasonIndex >= 0 ? evidence.mid(reasonIndex + 7).trimmed() : QStringLiteral("invalid");
	}
	return {};
}

bool ViewerArcGate::hasValidStateMachine(const ClipCandidate &candidate) const
{
	return hasEvidenceStartingWith(candidate, QStringLiteral("exchange_arc_state_machine:valid")) ||
		hasEvidenceStartingWith(candidate, QStringLiteral("exchange_arc_state_machine:repaired_strong"));
}

bool ViewerArcGate::hasInvalidStateMachine(const ClipCandidate &candidate) const
{
	if (!hasEvidenceStartingWith(candidate, QStringLiteral("exchange_arc_state_machine:invalid")))
		return false;
	if (hasEvidenceStartingWith(candidate, QStringLiteral("exchange_arc_state_machine:repaired_strong")))
		return !isRecoverableOpeningReason(invalidReason(candidate));
	return true;
}

bool ViewerArcGate::hasExplicitViewerOrigin(const ClipCandidate &candidate) const
{
	return candidate.startsNearViewerCue ||
		hasEvidenceContaining(candidate, QStringLiteral("targeted_viewer_message_cue")) ||
		hasEvidenceContaining(candidate, QStringLiteral("exchange_arc_viewer_message_cue_confirmed")) ||
		hasEvidenceContaining(candidate, QStringLiteral("flags=origin"));
}


bool ViewerArcGate::hasDirectPositiveFeedbackArcSupport(const ClipCandidate &candidate) const
{
	if (isFeedbackSuppressed(candidate))
		return false;

	const double positiveRange = evidenceScoreValue(candidate, QStringLiteral("feedback_positive_range:"));
	const double positiveText = evidenceScoreValue(candidate, QStringLiteral("feedback_positive_text:"));
	const double positiveGuided = evidenceScoreValue(candidate, QStringLiteral("feedback_positive_guided_score:"));
	const bool exactPositiveSeed = isUserAcceptedFeedbackSeed(candidate);
	if (!exactPositiveSeed && hasHardContextBlockerForLearnedArc(candidate))
		return false;

	// A high similarity to a corrected/accepted range is not enough to override the
	// viewer-arc gate. Boundary variants often overlap a positive range perfectly
	// while still missing the original viewer message or answer origin. Let those
	// variants surface as diagnostics/recoverable review items instead of applying
	// them as final markers.
	if (!exactPositiveSeed) {
		const bool hasOriginOrValidArc = hasExplicitViewerOrigin(candidate) ||
			candidate.scores.semanticViewerMessage >= 0.70 || hasValidStateMachine(candidate) ||
			hasRecoveredOpening(candidate) || hasStrongRecoveredOpening(candidate);
		if (!hasOriginOrValidArc)
			return false;
		if (hasInvalidStateMachine(candidate) && !hasRecoveredOpening(candidate) && !hasStrongRecoveredOpening(candidate))
			return false;
	}

	const QString reason = invalidReason(candidate);
	if (reason == QStringLiteral("multiple_viewer_messages_inside_arc") ||
	    reason == QStringLiteral("topic_shift_before_resolution"))
		return false;

	if (hasEvidence(candidate, QStringLiteral("feedback_negative_contamination")) ||
	    hasEvidence(candidate, QStringLiteral("feedback_negative_suppressed")) ||
	    hasEvidence(candidate, QStringLiteral("feedback_consistency_rejected")) ||
	    hasEvidence(candidate, QStringLiteral("feedback_trained_ranker_rejected_negative_contamination")))
		return false;

	const double negativeRange = evidenceScoreValue(candidate, QStringLiteral("feedback_negative_range:"));
	const double negativeText = evidenceScoreValue(candidate, QStringLiteral("feedback_negative_text:"));
	const double feedbackMargin = evidenceScoreValue(candidate, QStringLiteral("feedback_margin:"));
	const double positiveSum = positiveRange + positiveText + positiveGuided;
	const double negativeSum = negativeRange + negativeText;
	const bool exactPositiveRange = positiveRange >= 0.92;
	const bool strongPositiveText = positiveText >= 0.82;
	const bool explicitPositiveEvidence = hasEvidence(candidate, QStringLiteral("feedback_positive_range_explains_candidate"));
	const bool positiveDominates = feedbackMargin >= 0.12 && positiveSum >= negativeSum + 0.18;
	if (!(exactPositiveRange && (strongPositiveText || explicitPositiveEvidence) && positiveDominates))
		return false;

	const bool viewerOrAnswer = hasExplicitViewerOrigin(candidate) || candidate.scores.semanticViewerMessage >= 0.60 ||
		candidate.scores.viewerResponse >= 0.60 || candidate.scores.semanticDirectAnswer >= 0.62 ||
		(exactPositiveSeed && positiveText >= 0.82);
	const bool hasUsefulEnding = candidate.scores.semanticEndingResolution >= 0.62 ||
		candidate.scores.semanticResolution >= 0.62 || candidate.endsBeforeNextCue ||
		(exactPositiveSeed && candidate.scores.semanticEndingResolution >= 0.56);
	const bool notBadlyContaminated = negativeRange < 0.55 && negativeText < 0.58;
	return viewerOrAnswer && hasUsefulEnding && notBadlyContaminated;
}

bool ViewerArcGate::hasLearnedFeedbackArcSupport(const ClipCandidate &candidate) const
{
	if (isFeedbackSuppressed(candidate))
		return false;

	const QString reason = invalidReason(candidate);
	if (reason == QStringLiteral("multiple_viewer_messages_inside_arc") ||
	    reason == QStringLiteral("topic_shift_before_resolution"))
		return false;
	if (hasHardContextBlockerForLearnedArc(candidate))
		return false;

	if (hasEvidence(candidate, QStringLiteral("feedback_negative_contamination")) ||
	    hasEvidence(candidate, QStringLiteral("feedback_negative_suppressed")) ||
	    hasEvidence(candidate, QStringLiteral("feedback_consistency_rejected")) ||
	    hasEvidence(candidate, QStringLiteral("feedback_trained_ranker_rejected_negative_contamination")))
		return false;

	const double learnedScore = evidenceScoreValue(candidate, QStringLiteral("feedback_trained_ranker_score:"));
	const bool strongLearnedAccept = hasEvidence(candidate, QStringLiteral("feedback_trained_ranker_strong_accept")) || learnedScore >= 0.74;
	const bool learnedAccept = strongLearnedAccept || hasEvidence(candidate, QStringLiteral("feedback_trained_ranker_accept")) || learnedScore >= 0.50;
	if (!learnedAccept)
		return false;

	const bool explicitPositiveSeed = candidate.source.contains(QStringLiteral("feedback_positive_exact_seed")) ||
		hasEvidence(candidate, QStringLiteral("feedback_positive_exact_seed")) ||
		hasEvidence(candidate, QStringLiteral("feedback_positive_exact_seed_preserved"));
	const bool patternOrPrototype = candidate.source == QStringLiteral("feedback_positive_pattern_search") ||
		candidate.source == QStringLiteral("feedback_positive_semantic_prototype") ||
		hasEvidence(candidate, QStringLiteral("feedback_positive_pattern_search")) ||
		hasEvidence(candidate, QStringLiteral("feedback_positive_semantic_prototype"));
	const bool feedbackGuidedSource = explicitPositiveSeed || patternOrPrototype ||
		hasEvidenceContaining(candidate, QStringLiteral("feedback_consistency_positive_margin"));

	const double positiveGuided = evidenceScoreValue(candidate, QStringLiteral("feedback_positive_guided_score:"));
	const double positiveRange = evidenceScoreValue(candidate, QStringLiteral("feedback_positive_range:"));
	const double positiveText = evidenceScoreValue(candidate, QStringLiteral("feedback_positive_text:"));
	const double negativeRange = evidenceScoreValue(candidate, QStringLiteral("feedback_negative_range:"));
	const double negativeText = evidenceScoreValue(candidate, QStringLiteral("feedback_negative_text:"));
	const double feedbackMargin = evidenceScoreValue(candidate, QStringLiteral("feedback_margin:"));
	const double prototypeSimilarity = evidenceScoreValue(candidate, QStringLiteral("feedback_prototype_similarity:"));
	const double patternScore = evidenceScoreValue(candidate, QStringLiteral("feedback_pattern_score:"));
	const double positiveFeedbackSum = positiveRange + positiveText + positiveGuided;
	const double negativeFeedbackSum = negativeRange + negativeText;
	const bool positiveDominatesNegative = feedbackMargin >= 0.08 &&
		positiveFeedbackSum >= (negativeFeedbackSum + 0.05);
	const bool strongLearnedPositiveSource = feedbackGuidedSource && learnedScore >= 0.76 &&
		feedbackMargin >= 0.04 && positiveFeedbackSum >= negativeFeedbackSum;
	const bool feedbackConfidence = (positiveDominatesNegative || strongLearnedPositiveSource) &&
		(positiveGuided >= 0.24 || positiveRange >= 0.18 || positiveText >= 0.20 ||
		 feedbackMargin >= 0.12 || prototypeSimilarity >= 0.24 || patternScore >= 0.24 ||
		 hasEvidenceContaining(candidate, QStringLiteral("feedback_consistency_positive_margin")) ||
		 strongLearnedPositiveSource);
	if (!feedbackGuidedSource && !feedbackConfidence)
		return false;
	if (!explicitPositiveSeed && !feedbackConfidence)
		return false;

	const bool missingViewerCue = reason == QStringLiteral("missing_viewer_message_cue") ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_window_dfs_no_valid_origin_or_answer")) ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_no_valid_subspan"));
	if (missingViewerCue && !hasConcreteViewerOriginForLearnedArc(candidate) && !explicitPositiveSeed) {
		// A feedback-guided pattern/prototype can survive a weak state-machine origin only when
		// the user's positive feedback clearly dominates negatives. Generic semantic coarse
		// candidates still need a concrete viewer origin.
		const bool positivePatternCanSupplyOrigin = patternOrPrototype && feedbackConfidence &&
			learnedScore >= 0.50 && (positiveGuided >= 0.24 || positiveRange >= 0.18 ||
				prototypeSimilarity >= 0.24 || patternScore >= 0.24 || feedbackMargin >= 0.12 ||
				strongLearnedPositiveSource);
		if (!positivePatternCanSupplyOrigin)
			return false;
	}

	const bool semanticBody = candidate.scores.semanticClipValue >= 0.60 ||
		candidate.scores.semanticDirectAnswer >= 0.58 || candidate.scores.viewerResponse >= 0.52 ||
		candidate.scores.advice >= 0.56 || candidate.scores.explanation >= 0.56 ||
		candidate.scores.emotional >= 0.56 || candidate.scores.hook >= 0.60 ||
		candidate.scores.semanticHook >= 0.58;
	const bool learnedBody = learnedScore >= 0.50 && feedbackConfidence &&
		(candidate.scores.rerankerRaw >= 0.60 || candidate.scores.coarseSemantic >= 0.18 ||
		 candidate.scores.boundary >= 0.62 || candidate.scores.viewerResponse >= 0.30 ||
		 candidate.scores.semanticEmpathy >= 0.48 || patternOrPrototype);
	const bool hasUsefulBody = semanticBody || learnedBody;

	const bool semanticResolution = candidate.scores.semanticResolution >= 0.58 ||
		candidate.scores.semanticEndingResolution >= 0.58 ||
		candidate.scores.arcConclusion >= 0.34 || candidate.endsBeforeNextCue ||
		hasEvidenceContaining(candidate, QStringLiteral(":resolution"));
	const bool learnedResolution = learnedScore >= 0.50 && feedbackConfidence &&
		(candidate.scores.rerankerRaw >= 0.60 || candidate.scores.boundary >= 0.62 ||
		 candidate.scores.semanticEndingTopicShift <= 0.44 || patternOrPrototype);
	const bool hasEndingOrResolution = semanticResolution || learnedResolution;
	const bool durationOk = candidate.range.endSec > candidate.range.startSec &&
		(candidate.range.endSec - candidate.range.startSec) >= 8.0 &&
		(candidate.range.endSec - candidate.range.startSec) <= 180.0;

	return durationOk && hasUsefulBody && hasEndingOrResolution;
}


bool ViewerArcGate::hasSelfContainedAdviceArcSupport(const ClipCandidate &candidate,
	const ViewerArcGateOptions &options) const
{
	if (!targetAllowsSelfContainedAdviceArc(options))
		return false;
	if (isFeedbackSuppressed(candidate) || hasHardContextBlockerForLearnedArc(candidate))
		return false;

	const QString reason = invalidReason(candidate);
	if (reason == QStringLiteral("multiple_viewer_messages_inside_arc") ||
	    reason == QStringLiteral("topic_shift_before_resolution"))
		return false;

	if (hasEvidence(candidate, QStringLiteral("feedback_negative_contamination")) ||
	    hasEvidence(candidate, QStringLiteral("feedback_negative_suppressed")) ||
	    hasEvidence(candidate, QStringLiteral("feedback_consistency_rejected")) ||
	    hasEvidence(candidate, QStringLiteral("feedback_trained_ranker_rejected_negative_contamination")))
		return false;

	const double negativeRange = evidenceScoreValue(candidate, QStringLiteral("feedback_negative_range:"));
	const double negativeText = evidenceScoreValue(candidate, QStringLiteral("feedback_negative_text:"));
	if (negativeRange >= 0.42 || negativeText >= 0.55)
		return false;

	const double durationSec = candidate.range.endSec - candidate.range.startSec;
	if (durationSec < 12.0 || durationSec > options.maxDurationSec || durationSec > 120.0)
		return false;

	const bool feedbackGuided = hasFeedbackGuidedSelfContainedSignal(candidate);
	if (!feedbackGuided)
		return false;

	const bool noValidOriginOrSubspan =
		hasEvidence(candidate, QStringLiteral("exchange_arc_window_dfs_no_valid_origin_or_answer")) ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_no_valid_subspan"));
	const bool missingOpeningOrCue = reason == QStringLiteral("missing_viewer_message_cue") ||
		reason == QStringLiteral("missing_explicit_opening") || noValidOriginOrSubspan ||
		hasEvidence(candidate, QStringLiteral("arc_gate_cleared_prior_quality_rejection_from:missing_contextual_arc")) ||
		hasEvidence(candidate, QStringLiteral("quality_rejected:missing_contextual_arc"));
	const bool hasRealArcShape = hasValidStateMachine(candidate) ||
		candidate.scores.arcCompleteness >= std::max(0.20, calibratedMinArcConfidence(options) * 0.75) ||
		(candidate.scores.arcOpening >= 0.30 &&
		 (candidate.scores.arcDevelopment >= 0.32 || candidate.scores.arcConclusion >= 0.32)) ||
		hasEvidence(candidate, QStringLiteral("viewer_arc_opening_recovered")) ||
		hasEvidenceStartingWith(candidate, QStringLiteral("exchange_arc_state_machine:repaired_strong"));
	if (missingOpeningOrCue && !hasRealArcShape)
		return false;

	const bool hasUsefulBody = candidate.scores.semanticClipValue >= 0.62 ||
		candidate.scores.semanticTarget >= 0.62 || candidate.scores.semanticDirectAnswer >= 0.58 ||
		candidate.scores.advice >= 0.58 || candidate.scores.explanation >= 0.58 ||
		candidate.scores.emotional >= 0.58 || candidate.scores.semanticEmpathy >= 0.56 ||
		candidate.scores.rerankerRaw >= 0.54 || candidate.scores.coarseSemantic >= 0.22;
	const bool hasHookOrOpening = candidate.scores.semanticOpeningHook >= 0.60 ||
		candidate.scores.hook >= 0.64 || candidate.scores.semanticHook >= 0.60 ||
		candidate.scores.rerankerOpeningDefect <= 0.22 || hasEvidenceContaining(candidate, QStringLiteral(":opening"));
	const bool hasEndingOrResolution = candidate.scores.semanticEndingResolution >= 0.60 ||
		candidate.scores.semanticResolution >= 0.60 || candidate.scores.arcConclusion >= 0.34 ||
		candidate.endsBeforeNextCue || candidate.scores.rerankerEndingDefect <= 0.28 ||
		hasEvidenceContaining(candidate, QStringLiteral(":resolution"));
	const bool notTooDefective = candidate.scores.rerankerStructureDefect <= 0.82 &&
		candidate.scores.rerankerOpeningDefect <= 0.78 && candidate.scores.rerankerEndingDefect <= 0.72;
	const bool textHasEnoughBody = candidate.text.simplified().size() >= 80 || durationSec >= 24.0;

	return hasUsefulBody && hasHookOrOpening && hasEndingOrResolution && notTooDefective && textHasEnoughBody;
}

bool ViewerArcGate::looksLikeViewerOriginText(const QString &rawText) const
{
	const QString text = foldedCueText(rawText);
	if (text.size() < 8)
		return false;

	const bool questionShape = text.contains(QLatin1Char('?')) || textContainsAny(text, {
		"o que voce", "o que vc", "o que acha", "oq voce", "oq vc", "como eu", "como que",
		"como faco", "como faço", "devo ", "deveria", "sera que", "será que", "vale a pena",
		"qual sua opiniao", "qual a sua opiniao", "me ajuda", "conselho"
	});
	const bool personalProblem = textContainsAny(text, {
		"sou ", "eu sou", "eu tenho", "tenho ", "meu ", "minha ", "meus ", "minhas ",
		"comigo", "pra mim", "para mim", "me sinto", "nao consigo", "não consigo", "tentei",
		"ansiedade", "depress", "relacionamento", "namorada", "namorado", "meu pai", "minha mae",
		"minha mãe", "vicio", "vício", "aposta", "terapia"
	});
	const bool directAddress = textContainsAny(text, {
		"opa ", "mano", "cara", "gami", "dino", "chat"
	});
	const int colon = text.indexOf(QLatin1Char(':'));
	const bool attribution = colon >= 2 && colon <= 36;
	return (questionShape && (personalProblem || directAddress || attribution)) ||
		(personalProblem && (attribution || questionShape));
}

bool ViewerArcGate::shouldAttemptOpeningRecovery(const ClipCandidate &candidate, const ViewerArcGateOptions &options) const
{
	if (!options.enabled || candidate.range.endSec <= candidate.range.startSec || candidate.text.trimmed().isEmpty())
		return false;
	if (isFeedbackSuppressed(candidate) || isUserAcceptedFeedbackSeed(candidate))
		return false;

	const QString reason = invalidReason(candidate);
	if (reason == QStringLiteral("multiple_viewer_messages_inside_arc") ||
	    reason == QStringLiteral("topic_shift_before_resolution"))
		return false;

	const bool hasAnswerBody = candidate.scores.semanticDirectAnswer >= 0.60 || candidate.scores.viewerResponse >= 0.60 ||
		candidate.scores.advice >= 0.55 || candidate.scores.explanation >= 0.55 || candidate.scores.emotional >= 0.55 ||
		hasEvidenceContaining(candidate, QStringLiteral(":development"));
	const bool hasResolution = candidate.scores.semanticEndingResolution >= 0.60 || candidate.scores.semanticResolution >= 0.60 ||
		candidate.scores.arcConclusion >= 0.40 || hasEvidenceContaining(candidate, QStringLiteral(":resolution"));
	const bool hasValue = candidate.scores.semanticClipValue >= 0.58 || candidate.scores.semanticEmpathy >= 0.58 ||
		candidate.scores.semanticTarget >= 0.58 || candidate.scores.coarseSemantic >= 0.20;
	return hasAnswerBody && hasResolution && hasValue &&
		(!hasValidStateMachine(candidate) || isRecoverableOpeningReason(reason));
}

bool ViewerArcGate::recoverMissingOpening(const TranscriptIndex &index, ClipCandidate &candidate,
	const ViewerArcGateOptions &options) const
{
	if (!shouldAttemptOpeningRecovery(candidate, options))
		return false;

	const int first = std::max(0, candidate.firstSegmentIndex);
	const double lookbackStartSec = std::max(options.searchRange.startSec, candidate.range.startSec - 90.0);
	int bestIndex = -1;
	int bestScore = 0;
	for (int i = first; i >= 0; --i) {
		const TranscriptSegment *segment = index.segmentAt(i);
		if (!segment)
			continue;
		if (segment->endSec < lookbackStartSec)
			break;
		if (segment->startSec > candidate.range.startSec + 8.0)
			continue;
		const QString text = segment->text.simplified();
		if (!looksLikeViewerOriginText(text))
			continue;
		int score = 1;
		const QString folded = foldedCueText(text);
		if (folded.contains(QLatin1Char('?')))
			++score;
		if (textContainsAny(folded, {"o que", "como", "devo", "deveria", "me ajuda", "conselho"}))
			++score;
		if (textContainsAny(folded, {"meu ", "minha ", "sou ", "eu tenho", "nao consigo", "não consigo"}))
			++score;
		if (score > bestScore || (score == bestScore && bestIndex >= 0 && segment->startSec > index.segmentAt(bestIndex)->startSec)) {
			bestScore = score;
			bestIndex = i;
		}
	}

	if (bestIndex < 0 || bestScore < 4)
		return false;
	const TranscriptSegment *origin = index.segmentAt(bestIndex);
	if (!origin)
		return false;
	ClipDuration repairedRange{std::max(options.searchRange.startSec, origin->startSec - 0.35), candidate.range.endSec};
	if ((repairedRange.endSec - repairedRange.startSec) > options.maxDurationSec)
		repairedRange.startSec = std::max(options.searchRange.startSec, repairedRange.endSec - options.maxDurationSec);
	if ((repairedRange.endSec - repairedRange.startSec) < options.minDurationSec)
		repairedRange.endSec = std::min(options.searchRange.endSec, repairedRange.startSec + options.minDurationSec);
	repairedRange = index.clampRange(repairedRange, options.searchRange);
	if (repairedRange.endSec <= repairedRange.startSec)
		return false;

	candidate.range = repairedRange;
	candidate.firstSegmentIndex = index.firstSegmentIndexOverlapping(candidate.range);
	candidate.lastSegmentIndex = index.lastSegmentIndexOverlapping(candidate.range);
	candidate.text = index.textForRange(candidate.range).simplified();
	candidate.timedText = index.timedTextForRange(candidate.range);
	candidate.anchorText = candidate.text.left(220);
	candidate.startsNearViewerCue = true;
	candidate.scores.arcOpening = std::max(candidate.scores.arcOpening, 0.44);
	candidate.evidence.append(QStringLiteral("viewer_arc_opening_recovered"));
	candidate.evidence.append(QStringLiteral("viewer_arc_opening_recovered_from_segment:%1").arg(bestIndex));
	candidate.evidence.append(QStringLiteral("viewer_arc_opening_recovered_score:%1").arg(bestScore));
	candidate.evidence.append(QStringLiteral("exchange_arc_state_machine:repaired_strong reason:recovered_explicit_opening"));
	candidate.evidence.removeDuplicates();
	return true;
}

void ViewerArcGate::recoverMissingOpenings(const TranscriptIndex &index, QVector<ClipCandidate> &candidates,
	const ViewerArcGateOptions &options) const
{
	if (!options.enabled)
		return;
	int recovered = 0;
	for (ClipCandidate &candidate : candidates) {
		if (recoverMissingOpening(index, candidate, options))
			++recovered;
	}
	if (recovered <= 0)
		return;
	for (ClipCandidate &candidate : candidates) {
		if (hasEvidence(candidate, QStringLiteral("viewer_arc_opening_recovered")))
			candidate.evidence.append(QStringLiteral("viewer_arc_opening_recovery_batch:%1").arg(recovered));
	}
}

bool ViewerArcGate::hasCompleteArc(const ClipCandidate &candidate, const ViewerArcGateOptions &options) const
{
	if (!options.enabled)
		return true;
	if (candidate.range.endSec <= candidate.range.startSec || candidate.text.trimmed().isEmpty())
		return false;
	if (isUserAcceptedFeedbackSeed(candidate))
		return true;
	if (isFeedbackSuppressed(candidate))
		return false;
	if (hasDirectPositiveFeedbackArcSupport(candidate))
		return true;
	if (hasLearnedFeedbackArcSupport(candidate))
		return true;
	if (hasSelfContainedAdviceArcSupport(candidate, options))
		return true;
	if (hasInvalidStateMachine(candidate))
		return false;

	const double minArc = calibratedMinArcConfidence(options);
	const bool repairedOpening = hasEvidenceStartingWith(candidate, QStringLiteral("exchange_arc_state_machine:repaired_strong"));
	const bool validStateMachine = hasValidStateMachine(candidate);
	const bool hasOrigin = repairedOpening || hasExplicitViewerOrigin(candidate) || candidate.scores.semanticViewerMessage >= 0.70;
	const bool hasOpening = candidate.scores.arcOpening >= 0.32 || candidate.scores.semanticOpeningHook >= 0.62 ||
		hasEvidenceContaining(candidate, QStringLiteral(":opening"));
	const bool hasDevelopment = candidate.scores.arcDevelopment >= 0.46 ||
		candidate.scores.semanticDirectAnswer >= 0.66 || candidate.scores.viewerResponse >= 0.62 ||
		candidate.scores.advice >= 0.60 || candidate.scores.explanation >= 0.60 || candidate.scores.emotional >= 0.60 ||
		hasEvidenceContaining(candidate, QStringLiteral(":development"));
	const bool hasConclusion = candidate.scores.arcConclusion >= 0.46 || candidate.scores.semanticEndingResolution >= 0.64 ||
		candidate.scores.semanticResolution >= 0.64 || candidate.endsBeforeNextCue;
	const bool cleanEnough = candidate.scores.arcBoundaryCleanliness >= 0.28 && candidate.scores.arcTailRisk <= 1.12;
	const bool arcConfident = candidate.scores.arcCompleteness >= minArc ||
		(repairedOpening && hasDevelopment && hasConclusion && candidate.scores.arcBoundaryCleanliness >= 0.28);
	const bool notMostlyOpening = !(hasEvidenceContaining(candidate, QStringLiteral("exchange_arc_roles:0:opening,1:opening,2:opening")) &&
		candidate.scores.arcDevelopment < 0.50);

	return validStateMachine && arcConfident && hasOrigin && hasOpening && hasDevelopment && hasConclusion && cleanEnough && notMostlyOpening;
}

QVector<ClipCandidate> ViewerArcGate::apply(QVector<ClipCandidate> candidates, const ViewerArcGateOptions &options) const
{
	if (!options.enabled)
		return candidates;
	for (ClipCandidate &candidate : candidates) {
		if (hasCompleteArc(candidate, options)) {
			const bool userSeed = isUserAcceptedFeedbackSeed(candidate);
			const bool directPositiveSeed = !userSeed && hasDirectPositiveFeedbackArcSupport(candidate);
			const bool learnedSeed = !userSeed && !directPositiveSeed && hasLearnedFeedbackArcSupport(candidate);
			const bool selfContainedAdviceSeed = !userSeed && !directPositiveSeed && !learnedSeed &&
				hasSelfContainedAdviceArcSupport(candidate, options);
			const QString previousReason = candidate.rejectionReason.trimmed();
			if (candidate.rejectedByQualityGate && (userSeed || directPositiveSeed || learnedSeed || selfContainedAdviceSeed)) {
				candidate.rejectedByQualityGate = false;
				candidate.rejectionReason.clear();
				candidate.scores.qualityGate = std::max(candidate.scores.qualityGate,
					userSeed ? 0.92 : directPositiveSeed ? 0.74 : learnedSeed ? 0.68 : 0.62);
				candidate.scores.final = std::max(candidate.scores.final,
					userSeed ? 0.88 : directPositiveSeed ? 0.60 : learnedSeed ? 0.52 : 0.48);
				candidate.evidence.append(QStringLiteral("arc_gate_cleared_prior_quality_rejection"));
				if (!previousReason.isEmpty())
					candidate.evidence.append(QStringLiteral("arc_gate_cleared_prior_quality_rejection_from:%1").arg(previousReason.left(80)));
			}
			candidate.evidence.append(userSeed
				? QStringLiteral("complete_viewer_arc_gate_passed_by_user_feedback")
				: (directPositiveSeed ? QStringLiteral("complete_viewer_arc_gate_passed_by_direct_positive_feedback")
				               : (learnedSeed ? QStringLiteral("complete_viewer_arc_gate_passed_by_feedback_trained_ranker")
				                              : (selfContainedAdviceSeed
									? QStringLiteral("complete_viewer_arc_gate_passed_by_self_contained_advice_feedback")
									: QStringLiteral("complete_viewer_arc_gate_passed")))));
			candidate.evidence.removeDuplicates();
			continue;
		}
		candidate.rejectedByQualityGate = true;
		candidate.qualityGateChecked = true;
		candidate.rejectionReason = QStringLiteral("incomplete_viewer_arc");
		candidate.scores.qualityGate = std::min(candidate.scores.qualityGate, 0.05);
		candidate.scores.final = std::min(candidate.scores.final, 0.08);
		candidate.evidence.append(QStringLiteral("complete_viewer_arc_gate_failed"));
		if (!hasValidStateMachine(candidate))
			candidate.evidence.append(QStringLiteral("arc_gate_missing_valid_state_machine"));
		if (hasInvalidStateMachine(candidate))
			candidate.evidence.append(QStringLiteral("arc_gate_invalid_state_machine"));
		if (!hasExplicitViewerOrigin(candidate) && candidate.scores.semanticViewerMessage < 0.66)
			candidate.evidence.append(QStringLiteral("arc_gate_missing_viewer_origin"));
		if (hasHardContextBlockerForLearnedArc(candidate))
			candidate.evidence.append(QStringLiteral("arc_gate_hard_context_blocker"));
		if (hasEvidence(candidate, QStringLiteral("feedback_trained_ranker_accept")) ||
		    hasEvidence(candidate, QStringLiteral("feedback_trained_ranker_strong_accept"))) {
			candidate.evidence.append(QStringLiteral("arc_gate_learned_accept_rejected"));
			if (isFeedbackSuppressed(candidate))
				candidate.evidence.append(QStringLiteral("arc_gate_learned_rejected_feedback_suppressed"));
			if (hasEvidence(candidate, QStringLiteral("feedback_negative_contamination")) ||
			    hasEvidence(candidate, QStringLiteral("feedback_negative_suppressed")) ||
			    hasEvidence(candidate, QStringLiteral("feedback_consistency_rejected")))
				candidate.evidence.append(QStringLiteral("arc_gate_learned_rejected_negative_feedback"));
			if (!hasHardContextBlockerForLearnedArc(candidate) && !isFeedbackSuppressed(candidate))
				candidate.evidence.append(QStringLiteral("arc_gate_learned_rejected_insufficient_positive_arc_support"));
		}
		candidate.evidence.removeDuplicates();
	}
	return candidates;
}

} // namespace Curation::Scoring
