#include "curation/rules/opinion-rule.hpp"

namespace Curation {

QString OpinionRule::presetId() const
{
	return QStringLiteral("opinion");
}

ClipArchetype OpinionRule::archetype() const
{
	return ClipArchetype::Opinion;
}

ContentKind OpinionRule::contentKind(const Signals &) const
{
	return ContentKind::Opinion;
}

int OpinionRule::autoMatchScore(const CurationSettings &settings, const Signals &result, const QString &hint) const
{
	const QString text = combinedRuleText(settings, hint);
	if (containsAny(text, {QStringLiteral("opinion"), QStringLiteral("take"), QStringLiteral("opinião"),
			       QStringLiteral("opiniao")}))
		return 60;
	return result.opinionScore >= 0.6 ? 48 : 0;
}

QVector<BoundaryPolicy> OpinionRule::boundaryPolicies(const Signals &) const
{
	return {BoundaryPolicy::NaturalResolution, BoundaryPolicy::StopAfterClaimResolution,
		BoundaryPolicy::StopBeforeTopicShift};
}

CurationPromptRecipe OpinionRule::promptRecipe() const
{
	return {presetId(),
		QStringLiteral(
			"Find the strongest self-contained clip where the speaker gives one focused opinion or take."),
		QStringLiteral("Find self-contained clips where the speaker gives one focused opinion or take."),
		QStringLiteral(
			"Prioritize moments that state the claim, develop the reasoning, and reach a clear conclusion."),
		QStringLiteral(
			"Choose clips that stop after that take resolves instead of drifting into adjacent topics.")};
}

} // namespace Curation
