#include "curation/rules/advice-answer-rule.hpp"

namespace Curation {

QString AdviceAnswerRule::presetId() const
{
	return QStringLiteral("advice_answer");
}

ClipArchetype AdviceAnswerRule::archetype() const
{
	return ClipArchetype::AdviceAnswer;
}

ContentKind AdviceAnswerRule::contentKind(const Signals &) const
{
	return ContentKind::Advice;
}

int AdviceAnswerRule::autoMatchScore(const CurationSettings &settings, const Signals &result, const QString &hint) const
{
	const QString text = combinedRuleText(settings, hint);
	if (containsAny(text, {QStringLiteral("advice"), QStringLiteral("conselho"), QStringLiteral("relationship"),
			       QStringLiteral("relacionamento")}))
		return 72;
	return result.adviceScore >= 0.5 ? 62 : 0;
}

QVector<BoundaryPolicy> AdviceAnswerRule::boundaryPolicies(const Signals &) const
{
	return {BoundaryPolicy::NaturalResolution, BoundaryPolicy::StopBeforeNextViewerMessage,
		BoundaryPolicy::StopBeforeHousekeeping, BoundaryPolicy::StopBeforeTopicShift};
}

CurationPromptRecipe AdviceAnswerRule::promptRecipe() const
{
	return {presetId(),
		QStringLiteral(
			"Find the strongest self-contained clip where the speaker answers one concrete question or problem with useful advice."),
		QStringLiteral(
			"Find self-contained clips where the speaker answers one concrete question or problem with useful advice."),
		QStringLiteral(
			"Prioritize moments that include enough setup for that problem, then follow one coherent answer while it stays on the same issue."),
		QStringLiteral(
			"Choose clips that end at the first resolved advice, caveat, or takeaway instead of continuing into adjacent chat or a new problem.")};
}

} // namespace Curation
