#include "curation/scoring/candidate-quality-rules.hpp"

#include "curation/scoring/evidence-view.hpp"

#include <algorithm>
#include <QStringList>

namespace Curation::Scoring::CandidateQualityRules {

bool isNeutralRerankerFailureReason(const QString &failureReason)
{
	const QString failure = failureReason.trimmed().toLower();
	if (failure.isEmpty())
		return true;
	return failure.contains(QStringLiteral("operation canceled")) ||
	       failure.contains(QStringLiteral("operation cancelled")) ||
	       failure.contains(QStringLiteral("http_error")) || failure.contains(QStringLiteral("network")) ||
	       failure.contains(QStringLiteral("timeout")) || failure.contains(QStringLiteral("timed out")) ||
	       failure.contains(QStringLiteral("invalid_batch_response"));
}

double boundedScore(double value)
{
	return std::clamp(value, 0.0, 1.0);
}

double durationSec(const ClipCandidate &candidate)
{
	return candidate.range.endSec - candidate.range.startSec;
}

bool hasEvidence(const ClipCandidate &candidate, const QString &value)
{
	return EvidenceView::anyEquals(candidate.evidence, value);
}

bool hasEvidenceContaining(const ClipCandidate &candidate, const QString &fragment)
{
	return EvidenceView::anyContains(candidate.evidence, fragment);
}

bool isFeedbackPositiveGroundTruth(const ClipCandidate &candidate)
{
	return candidate.source == QStringLiteral("feedback_positive_exact_seed") ||
	       candidate.source == QStringLiteral("feedback_positive_exact_seed_beam") ||
	       hasEvidence(candidate, QStringLiteral("feedback_positive_exact_seed_preserved")) ||
	       hasEvidence(candidate, QStringLiteral("complete_viewer_arc_gate_passed_by_user_feedback"));
}

bool isFeedbackNegativeBlocked(const ClipCandidate &candidate)
{
	return candidate.rejectionReason == QStringLiteral("feedback_negative_range_contamination") ||
	       candidate.rejectionReason == QStringLiteral("feedback_rejected_range") ||
	       hasEvidence(candidate, QStringLiteral("feedback_negative_suppressed")) ||
	       hasEvidence(candidate, QStringLiteral("feedback_consistency_rejected")) ||
	       hasEvidenceContaining(candidate, QStringLiteral("feedback_negative_range_contamination"));
}

QString foldedQualityText(QString value)
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

bool qualityTextContainsAny(const QString &text, std::initializer_list<const char *> needles)
{
	for (const char *needle : needles) {
		if (needle && text.contains(QString::fromUtf8(needle)))
			return true;
	}
	return false;
}

bool hasHardLearnedContextBlocker(const ClipCandidate &candidate)
{
	const bool exactUserSeed = isFeedbackPositiveGroundTruth(candidate);
	if (exactUserSeed)
		return false;
	const QString text = foldedQualityText(candidate.text + QStringLiteral(" ") + candidate.anchorText);
	const bool personalOrQuestion =
		text.contains(QLatin1Char('?')) ||
		qualityTextContainsAny(text, {"o que", "como", "devo", "deveria", "me ajuda", "conselho", "sou ",
					      "eu tenho", "nao consigo", "não consigo", "me sinto", "minha ", "meu "});
	const bool musicOrSetupText =
		(text.contains(QStringLiteral("musica")) || text.contains(QStringLiteral("música")) ||
		 qualityTextContainsAny(text, {"acertando o negocio", "acertando o negócio", "configurando", "setup",
					       "abrindo live", "testando"})) &&
		!personalOrQuestion;
	const bool metaEvidence = hasEvidenceContaining(candidate, QStringLiteral("meta_prelude")) ||
				  hasEvidenceContaining(candidate, QStringLiteral("drift+block")) ||
				  hasEvidenceContaining(candidate, QStringLiteral("Música")) ||
				  hasEvidenceContaining(candidate, QStringLiteral("Musica"));
	const bool explicitOrigin =
		candidate.startsNearViewerCue ||
		hasEvidenceContaining(candidate, QStringLiteral("targeted_viewer_message_cue")) ||
		hasEvidenceContaining(candidate, QStringLiteral("exchange_arc_viewer_message_cue_confirmed")) ||
		hasEvidenceContaining(candidate, QStringLiteral("flags=origin")) ||
		candidate.scores.semanticViewerMessage >= 0.66;
	const bool noOriginOrAnswer =
		hasEvidence(candidate, QStringLiteral("exchange_arc_window_dfs_no_valid_origin_or_answer")) ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_no_valid_subspan")) ||
		hasEvidenceContaining(candidate, QStringLiteral("missing_viewer_message_cue"));
	const bool noArcShape = candidate.scores.arcCompleteness <= 0.02 && candidate.scores.arcOpening <= 0.04 &&
				candidate.scores.arcDevelopment <= 0.04 && candidate.scores.arcConclusion <= 0.04;
	return (candidate.range.startSec <= 75.0 && (musicOrSetupText || metaEvidence)) ||
	       ((noOriginOrAnswer || noArcShape) && !explicitOrigin && (metaEvidence || musicOrSetupText));
}

bool evidenceContainsFragment(const ClipCandidate &candidate, const QString &fragment)
{
	return EvidenceView::anyContains(candidate.evidence, fragment);
}

bool hasArcRole(const ClipCandidate &candidate, const QString &role)
{
	return evidenceContainsFragment(candidate, QStringLiteral(":%1").arg(role));
}

bool arcRolesStartWith(const ClipCandidate &candidate, const QString &role)
{
	return evidenceContainsFragment(candidate, QStringLiteral("exchange_arc_roles:0:%1").arg(role));
}

bool hasResolutionOnlyArcRoles(const ClipCandidate &candidate)
{
	return arcRolesStartWith(candidate, QStringLiteral("resolution")) &&
	       !hasArcRole(candidate, QStringLiteral("opening")) &&
	       !hasArcRole(candidate, QStringLiteral("development"));
}

bool hasDevelopmentOnlyArcRoles(const ClipCandidate &candidate)
{
	return arcRolesStartWith(candidate, QStringLiteral("development")) &&
	       !hasArcRole(candidate, QStringLiteral("opening")) &&
	       !hasArcRole(candidate, QStringLiteral("resolution"));
}

bool hasInvalidContextualStateMachine(const ClipCandidate &candidate)
{
	return evidenceContainsFragment(candidate, QStringLiteral("exchange_arc_state_machine:invalid"));
}

bool hasValidContextualStateMachine(const ClipCandidate &candidate)
{
	return evidenceContainsFragment(candidate, QStringLiteral("exchange_arc_state_machine:valid"));
}

bool requiresStrictContextualArc(const CandidateQualityGateOptions &options)
{
	return options.presetId == QStringLiteral("viewer_message_response");
}

bool hasCompleteContextualArc(const ClipCandidate &candidate)
{
	return candidate.scores.arcCompleteness > 0.0 && hasValidContextualStateMachine(candidate) &&
	       !hasInvalidContextualStateMachine(candidate) &&
	       !evidenceContainsFragment(candidate, QStringLiteral("flags:implicit_opening"));
}

bool hasImplicitContextualOpening(const ClipCandidate &candidate)
{
	return evidenceContainsFragment(candidate, QStringLiteral("flags:implicit_opening"));
}

QString arcRolesEvidence(const ClipCandidate &candidate)
{
	for (const QString &evidence : candidate.evidence) {
		if (EvidenceView::startsWith(QStringView{evidence}, QStringLiteral("exchange_arc_roles:")))
			return evidence;
	}
	return {};
}

int arcRoleCount(const ClipCandidate &candidate, const QString &role)
{
	const QString roles = arcRolesEvidence(candidate);
	if (roles.isEmpty())
		return 0;
	return static_cast<int>(roles.count(QStringLiteral(":%1").arg(role)));
}

bool hasRecoveredContextualDpSubspan(const ClipCandidate &candidate)
{
	return hasEvidence(candidate, QStringLiteral("exchange_arc_contextual_dp_subspan_recovered")) &&
	       hasValidContextualStateMachine(candidate);
}

bool hasStrongRecoveredContextualDpArc(const ClipCandidate &candidate)
{
	if (!hasRecoveredContextualDpSubspan(candidate))
		return false;

	const double duration = durationSec(candidate);
	const double semanticMeta =
		std::max({candidate.scores.semanticMetaNoise, candidate.scores.semanticOpeningMetaNoise,
			  candidate.scores.semanticEndingMetaNoise});
	const double semanticUseful = std::max({candidate.scores.semanticClipValue, candidate.scores.semanticHook,
						candidate.scores.semanticResolution, candidate.scores.semanticTarget});

	// DP-recovered viewer arcs may still have local roles reported as resolution
	// because multilingual embedding role scores are nearly tied. Treat the DP
	// state-machine contract as authoritative only when it is compact, has a real
	// body/payoff score, and does not look meta dominated. This prevents the old
	// resolution-only recovery from returning, while allowing valid Q&A answers
	// whose first short viewer question was mislabeled as resolution.
	return duration <= 42.0 && candidate.scores.arcCompleteness >= 0.50 && candidate.scores.arcOpening >= 0.48 &&
	       candidate.scores.arcDevelopment >= 0.60 && candidate.scores.arcConclusion >= 0.56 &&
	       candidate.scores.arcBoundaryCleanliness >= 0.44 && candidate.scores.arcTailRisk <= 0.58 &&
	       semanticUseful >= semanticMeta - 0.02 && !hasImplicitContextualOpening(candidate);
}

bool hasResolutionPlateauWithoutOpening(const ClipCandidate &candidate)
{
	const QString roles = arcRolesEvidence(candidate);
	if (roles.isEmpty())
		return false;
	const int resolutionCount = arcRoleCount(candidate, QStringLiteral("resolution"));
	const int openingCount = arcRoleCount(candidate, QStringLiteral("opening"));
	const int developmentCount = arcRoleCount(candidate, QStringLiteral("development"));
	const bool startsInsideBody = roles.contains(QStringLiteral("exchange_arc_roles:0:development")) ||
				      roles.contains(QStringLiteral("exchange_arc_roles:0:resolution"));
	return startsInsideBody && openingCount <= 0 && resolutionCount >= 3 &&
	       (durationSec(candidate) >= 26.0 || candidate.scores.maxInternalPauseSec >= 6.0 || developmentCount <= 1);
}

bool hasSpecificMainTarget(const QString &target)
{
	QString value = target.toLower().simplified();
	if (value.isEmpty())
		return false;
	const bool looksLikeGeneratedInstruction =
		value.contains(QStringLiteral("find one")) || value.contains(QStringLiteral("continuous")) ||
		value.contains(QStringLiteral("unbroken")) || value.contains(QStringLiteral("single viewer message")) ||
		value.contains(QStringLiteral("complete response")) ||
		value.contains(QStringLiteral("prefer the clearest")) || value.contains(QStringLiteral("clip built"));
	const bool hasRealTopic =
		value.contains(QStringLiteral("mental")) || value.contains(QStringLiteral("saude")) ||
		value.contains(QStringLiteral("saúde")) || value.contains(QStringLiteral("empathy")) ||
		value.contains(QStringLiteral("empatia")) || value.contains(QStringLiteral("therapy")) ||
		value.contains(QStringLiteral("terapia")) || value.contains(QStringLiteral("anxiety")) ||
		value.contains(QStringLiteral("ansiedade")) || value.contains(QStringLiteral("depress")) ||
		value.contains(QStringLiteral("relationship")) || value.contains(QStringLiteral("relacionamento"));
	if (looksLikeGeneratedInstruction && !hasRealTopic)
		return false;
	const QStringList genericTokens = {
		QStringLiteral("q&a"),
		QStringLiteral("qna"),
		QStringLiteral("just chatting"),
		QStringLiteral("viewer_message_response"),
		QStringLiteral("viewer message"),
		QStringLiteral("single viewer message"),
		QStringLiteral("viewer issue"),
		QStringLiteral("complete response"),
		QStringLiteral("one continuous"),
		QStringLiteral("unbroken clip"),
		QStringLiteral("continuous clip"),
		QStringLiteral("clip built"),
		QStringLiteral("find one"),
		QStringLiteral("prefer the clearest"),
		QStringLiteral("emotionally consequential"),
		QStringLiteral("motivational speech"),
		QStringLiteral("advices"),
		QStringLiteral("advice"),
	};
	for (const QString &token : genericTokens)
		value.remove(token);
	value.remove(QLatin1Char(','));
	value.remove(QLatin1Char(';'));
	value.remove(QLatin1Char('.'));
	return value.simplified().size() >= 5;
}

bool hasStrictReliableTargetSupport(const ClipCandidate &candidate, const CandidateQualityGateOptions &options)
{
	if (!options.reliableMainTarget || options.mainTarget.trimmed().isEmpty() ||
	    !candidate.semanticScoringAvailable)
		return true;

	const double semanticNegative =
		std::max({candidate.scores.semanticNoise, candidate.scores.semanticMetaNoise,
			  candidate.scores.semanticOpeningMetaNoise, candidate.scores.semanticEndingMetaNoise,
			  candidate.scores.semanticEndingTopicShift});
	const double target = candidate.scores.semanticTarget;
	const double clipValue = candidate.scores.semanticClipValue;
	const double empathy = candidate.scores.semanticEmpathy;
	const double humanAnswer =
		std::max(candidate.scores.semanticDirectAnswer, candidate.scores.semanticViewerMessage);
	const double boundaryValue =
		std::max(candidate.scores.semanticOpeningHook, candidate.scores.semanticEndingResolution);

	const bool targetClearlyWins = target >= 0.70 && target >= semanticNegative + 0.060 &&
				       clipValue >= semanticNegative - 0.010;
	const bool empathyAdviceClearlyWins =
		empathy >= 0.71 && empathy >= semanticNegative + 0.045 && clipValue >= 0.67 &&
		clipValue >= candidate.scores.semanticMetaNoise - 0.005 &&
		(candidate.scores.topicContinuity <= 0.0 || candidate.scores.topicContinuity >= 0.66);
	const bool directTargetedAnswer = humanAnswer >= 0.68 && target >= semanticNegative + 0.050 &&
					  clipValue >= semanticNegative + 0.015 && boundaryValue >= 0.64;

	return targetClearlyWins || empathyAdviceClearlyWins || directTargetedAnswer;
}

bool hasStrongHumanContext(const ClipCandidate &candidate)
{
	const double strongestContext =
		std::max({candidate.scores.semanticViewerMessage, candidate.scores.semanticDirectAnswer,
			  candidate.scores.semanticTarget});
	const double strongestMeta =
		std::max({candidate.scores.semanticMetaNoise, candidate.scores.semanticOpeningMetaNoise,
			  candidate.scores.semanticEndingMetaNoise});
	const bool explicitContext = strongestContext >= 0.66;
	const bool empathyClearlyWins = candidate.scores.semanticEmpathy >= 0.66 &&
					candidate.scores.semanticEmpathy >= strongestMeta + 0.08 &&
					candidate.scores.semanticClipValue >= candidate.scores.semanticMetaNoise + 0.03;
	return explicitContext || empathyClearlyWins;
}

bool hasStrongRepairedResolutionOnlyArc(const ClipCandidate &candidate)
{
	if (!candidate.semanticScoringAvailable || !hasResolutionOnlyArcRoles(candidate))
		return false;

	// A resolution-only role sequence means the local arc classifier did not see a
	// real opening/body/payoff progression. That is acceptable only for compact
	// micro-clips where the whole answer fits inside one short local arc. Longer
	// resolution-only spans are almost always mid-answer windows or ranges that bleed
	// into the next subject, exactly the failure mode seen in dale.mp4.
	const double candidateDurationSec = durationSec(candidate);
	if (candidateDurationSec > 22.0)
		return false;

	const double opening = candidate.scores.semanticOpeningHook;
	const double hook = std::max(candidate.scores.semanticHook, candidate.scores.semanticOpeningHook);
	const double resolution =
		std::max(candidate.scores.semanticResolution, candidate.scores.semanticEndingResolution);
	const double endingDefect =
		std::max(candidate.scores.semanticEndingMetaNoise, candidate.scores.semanticEndingTopicShift);
	const double meta = std::max({candidate.scores.semanticMetaNoise, candidate.scores.semanticOpeningMetaNoise,
				      candidate.scores.semanticEndingMetaNoise});
	const bool compactBoundary = candidateDurationSec <= 18.5 || candidate.scores.pauseBeforeSec >= 0.55 ||
				     candidate.scores.pauseAfterSec >= 0.55;
	const bool localArcLooksComplete =
		candidate.scores.arcOpening >= 0.40 && candidate.scores.arcDevelopment >= 0.49 &&
		candidate.scores.arcConclusion >= 0.52 && candidate.scores.arcBoundaryCleanliness >= 0.48;
	const bool strongOpeningBoundary = opening >= 0.68 && hook >= 0.68 &&
					   opening >= candidate.scores.semanticOpeningMetaNoise - 0.02;
	const bool strongUsefulBody = candidate.scores.semanticClipValue >= 0.70 &&
				      candidate.scores.semanticClipValue >= meta - 0.005;
	const bool strongLocalPayoff = resolution >= 0.68 && resolution >= endingDefect - 0.04;
	const bool coherentTopic = candidate.scores.topicContinuity <= 0.0 || candidate.scores.topicContinuity >= 0.64;
	const bool tailContinuationIsSafe = !hasEvidence(candidate, QStringLiteral("exchange_arc_tail_continuation")) ||
					    (candidateDurationSec <= 18.5 && candidate.scores.arcTailRisk <= 0.28 &&
					     candidate.scores.semanticEndingResolution >= 0.70);
	const bool rerankerAllowsRepair =
		!candidate.rerankerAvailable ||
		(candidate.scores.rerankerRaw >= 0.96 && candidate.scores.rerankerClipQualityMargin >= 0.38 &&
		 candidate.scores.rerankerOpeningDefect <= 0.56 && candidate.scores.rerankerStructureDefect <= 0.58 &&
		 candidate.scores.rerankerEndingDefect <= 0.66) ||
		candidate.scores.rerankerClipQualityMargin >= 0.46;
	const bool notAllDefectsHigh =
		!candidate.rerankerAvailable ||
		!(candidate.scores.rerankerOpeningDefect >= 0.70 && candidate.scores.rerankerEndingDefect >= 0.70 &&
		  candidate.scores.rerankerStructureDefect >= 0.70);
	const bool noObviousTail = candidate.scores.arcTailRisk <= 0.62;

	return candidate.scores.final >= 0.54 && compactBoundary && localArcLooksComplete && strongOpeningBoundary &&
	       strongUsefulBody && strongLocalPayoff && coherentTopic && tailContinuationIsSafe &&
	       rerankerAllowsRepair && notAllDefectsHigh && noObviousTail;
}

bool semanticSupportsCollapsedArcRoles(const ClipCandidate &candidate)
{
	if (!candidate.semanticScoringAvailable)
		return false;
	if (hasResolutionOnlyArcRoles(candidate))
		return hasStrongRepairedResolutionOnlyArc(candidate);

	// A collapsed non-resolution sequence can be a cheap-classifier false negative,
	// but it must not become a free pass. Only soften it when independent semantic
	// evidence says this is a real human-context clip with a clean hook/payoff, not
	// stream setup, social check-in, gift thanks or background-game chatter.
	const double opening = std::max(candidate.scores.semanticOpeningHook, candidate.scores.semanticHook);
	const double resolution =
		std::max(candidate.scores.semanticEndingResolution, candidate.scores.semanticResolution);
	const double endingDefect =
		std::max(candidate.scores.semanticEndingMetaNoise, candidate.scores.semanticEndingTopicShift);
	const bool strongHumanContext = hasStrongHumanContext(candidate);
	const bool contextBoundaryWasImproved =
		hasEvidence(candidate, QStringLiteral("exchange_arc_context_start_extended")) ||
		hasEvidence(candidate, QStringLiteral("exchange_arc_essential_context_prepended"));
	const bool openingBeatsMeta = opening >=
				      candidate.scores.semanticOpeningMetaNoise + (strongHumanContext ? 0.00 : 0.035);
	const bool bodyBeatsMeta = candidate.scores.semanticClipValue >=
				   candidate.scores.semanticMetaNoise + (strongHumanContext ? -0.01 : 0.025);
	const bool usefulSemanticArc =
		candidate.scores.semanticClipValue >= 0.66 && opening >= 0.64 && resolution >= 0.64 &&
		resolution >= endingDefect - 0.06 && openingBeatsMeta && bodyBeatsMeta &&
		(candidate.scores.topicContinuity <= 0.0 || candidate.scores.topicContinuity >= 0.64);
	const bool arcHasSomeOpeningEvidence = candidate.scores.arcOpening >= 0.42 ||
					       (contextBoundaryWasImproved && candidate.scores.arcOpening >= 0.38);
	const bool rerankerDoesNotDisagree =
		!candidate.rerankerAvailable ||
		(candidate.scores.rerankerRaw >= 0.94 && candidate.scores.rerankerClipQualityMargin >= 0.30 &&
		 candidate.scores.rerankerStructureDefect <= 0.62);
	return usefulSemanticArc && arcHasSomeOpeningEvidence && rerankerDoesNotDisagree && strongHumanContext;
}

bool isWeakConclusionReason(const QString &reason)
{
	return reason == QStringLiteral("exchange_arc_weak_conclusion") ||
	       reason == QStringLiteral("exchange_arc_incomplete");
}

} // namespace Curation::Scoring::CandidateQualityRules
