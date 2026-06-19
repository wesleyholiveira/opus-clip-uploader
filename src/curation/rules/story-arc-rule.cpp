#include "curation/rules/story-arc-rule.hpp"

namespace Curation {

QString StoryArcRule::presetId() const
{
	return QStringLiteral("story_arc");
}

ClipArchetype StoryArcRule::archetype() const
{
	return ClipArchetype::StoryArc;
}

ContentKind StoryArcRule::contentKind(const Signals &) const
{
	return ContentKind::Story;
}

int StoryArcRule::autoMatchScore(const CurationSettings &settings, const Signals &result, const QString &hint) const
{
	const QString text = combinedRuleText(settings, hint);
	if (containsAny(text, {QStringLiteral("story"), QStringLiteral("história"), QStringLiteral("historia")}))
		return 64;
	return result.storyScore >= 0.6 ? 50 : 0;
}

QVector<BoundaryPolicy> StoryArcRule::boundaryPolicies(const Signals &) const
{
	return {BoundaryPolicy::NaturalResolution, BoundaryPolicy::StopAfterStoryPayoff,
		BoundaryPolicy::StopBeforeTopicShift};
}

CurationPromptRecipe StoryArcRule::promptRecipe() const
{
	return {presetId(),
		QStringLiteral(
			"Find the strongest self-contained clip where the speaker tells one self-contained story."),
		QStringLiteral("Find self-contained clips where the speaker tells one self-contained story."),
		QStringLiteral(
			"Prioritize moments with clear setup, development, and payoff, keeping the story arc continuous."),
		QStringLiteral(
			"Choose clips that end after the payoff or lesson resolves without merging into another story or topic.")};
}

} // namespace Curation
