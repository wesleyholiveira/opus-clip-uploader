#include "curation/rules/explanation-rule.hpp"

namespace Curation {

QString ExplanationRule::presetId() const
{
	return QStringLiteral("explanation");
}

ClipArchetype ExplanationRule::archetype() const
{
	return ClipArchetype::Explanation;
}

ContentKind ExplanationRule::contentKind(const Signals &) const
{
	return ContentKind::Explanation;
}

int ExplanationRule::autoMatchScore(const CurationSettings &settings, const Signals &result, const QString &hint) const
{
	const QString text = combinedRuleText(settings, hint);
	if (containsAny(text, {QStringLiteral("explanation"), QStringLiteral("explain"), QStringLiteral("explicação"),
			       QStringLiteral("explicacao"), QStringLiteral("explicar")}))
		return 66;
	return result.explanationScore >= 0.6 ? 54 : 0;
}

ClipStrategy ExplanationRule::clipStrategy(const CurationSettings &settings, const Signals &,
					   const QString &scope) const
{
	return hasSingleConfirmedRange(settings) ? ClipStrategy::BestMoment : defaultClipStrategyForScope(scope);
}

CurationPromptRecipe ExplanationRule::promptRecipe() const
{
	return {presetId(),
		QStringLiteral(
			"Find the strongest self-contained clip where the speaker develops one clear local idea from setup to conclusion."),
		QStringLiteral(
			"Find self-contained clips where the speaker develops one clear local idea from setup to conclusion."),
		QStringLiteral(
			"Prioritize moments that introduce the idea with enough local context and develop the same continuous explanation with minimal dead air."),
		QStringLiteral(
			"Prefer clips that end after the local point is resolved, with clean natural boundaries and a complete local idea.")};
}

} // namespace Curation
