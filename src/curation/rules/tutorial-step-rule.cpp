#include "curation/rules/tutorial-step-rule.hpp"

namespace Curation {

QString TutorialStepRule::presetId() const
{
	return QStringLiteral("tutorial_step");
}

ClipArchetype TutorialStepRule::archetype() const
{
	return ClipArchetype::TutorialStep;
}

ContentKind TutorialStepRule::contentKind(const Signals &) const
{
	return ContentKind::Tutorial;
}

int TutorialStepRule::autoMatchScore(const CurationSettings &settings, const Signals &result, const QString &hint) const
{
	const QString text = combinedRuleText(settings, hint);
	if (containsAny(text, {QStringLiteral("tutorial"), QStringLiteral("step"), QStringLiteral("passo"),
			       QStringLiteral("walkthrough"), QStringLiteral("walk through")}))
		return 60;
	return result.tutorialScore >= 0.6 ? 48 : 0;
}

QVector<BoundaryPolicy> TutorialStepRule::boundaryPolicies(const Signals &) const
{
	return {BoundaryPolicy::NaturalResolution, BoundaryPolicy::StopAfterStepCompletion,
		BoundaryPolicy::StopBeforeTopicShift};
}

CurationPromptRecipe TutorialStepRule::promptRecipe() const
{
	return {presetId(),
		QStringLiteral(
			"Find the strongest self-contained clip where the speaker walks through one actionable step or demo segment."),
		QStringLiteral(
			"Find self-contained clips where the speaker walks through one actionable step or demo segment."),
		QStringLiteral(
			"Prioritize moments that introduce the step, show or explain it clearly, and reach a useful stopping point."),
		QStringLiteral(
			"Choose clips that finish the step cleanly before moving into the next step or adjacent topic.")};
}

} // namespace Curation
