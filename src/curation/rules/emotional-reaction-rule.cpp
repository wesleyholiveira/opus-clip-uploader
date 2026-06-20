#include "curation/rules/emotional-reaction-rule.hpp"

namespace Curation {

QString EmotionalReactionRule::presetId() const
{
	return QStringLiteral("emotional_reaction");
}

ClipArchetype EmotionalReactionRule::archetype() const
{
	return ClipArchetype::EmotionalReaction;
}

ContentKind EmotionalReactionRule::contentKind(const Signals &) const
{
	return ContentKind::EmotionalReaction;
}

int EmotionalReactionRule::autoMatchScore(const CurationSettings &settings, const Signals &result,
					  const QString &hint) const
{
	const QString text = combinedRuleText(settings, hint);
	if (containsAny(text, {QStringLiteral("emotional"), QStringLiteral("emotion"), QStringLiteral("emocional"),
			       QStringLiteral("reaction"), QStringLiteral("reação"), QStringLiteral("reacao")}))
		return 74;
	if (result.emotionalScore >= 0.75)
		return 70;
	if (result.emotionalScore >= 0.55)
		return 58;
	return 0;
}

QVector<BoundaryPolicy> EmotionalReactionRule::boundaryPolicies(const Signals &) const
{
	return {BoundaryPolicy::NaturalResolution, BoundaryPolicy::StopAfterEmotionalPayoff,
		BoundaryPolicy::StopBeforeNextViewerMessage, BoundaryPolicy::StopBeforeHousekeeping,
		BoundaryPolicy::StopBeforeTopicShift};
}

CurationPromptRecipe EmotionalReactionRule::promptRecipe() const
{
	return {presetId(),
		QStringLiteral(
			"Find the strongest self-contained clip where the speaker reacts to one emotionally consequential message or moment."),
		QStringLiteral(
			"Find self-contained clips where the speaker reacts to emotionally consequential messages or moments."),
		QStringLiteral(
			"Prioritize clips that include enough setup for that moment, then follow the immediate reaction until its first natural emotional payoff."),
		QStringLiteral(
			"Choose shorter complete reactions that resolve cleanly before the speaker moves into another message or topic.")};
}

} // namespace Curation
