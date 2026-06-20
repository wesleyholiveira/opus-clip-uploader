#include "curation/curation-intent.hpp"

#include "curation/curation-preset.hpp"

namespace Curation {

ClipArchetype archetypeFromPresetId(const QString &presetId)
{
	const QString id = CurationPreset::normalizeId(presetId);
	if (id == CurationPreset::viewerMessageResponsePresetId())
		return ClipArchetype::ViewerMessageResponse;
	if (id == QStringLiteral("advice_answer"))
		return ClipArchetype::AdviceAnswer;
	if (id == QStringLiteral("emotional_reaction"))
		return ClipArchetype::EmotionalReaction;
	if (id == QStringLiteral("explanation"))
		return ClipArchetype::Explanation;
	if (id == QStringLiteral("story_arc"))
		return ClipArchetype::StoryArc;
	if (id == QStringLiteral("opinion"))
		return ClipArchetype::Opinion;
	if (id == QStringLiteral("tutorial_step"))
		return ClipArchetype::TutorialStep;
	return ClipArchetype::Auto;
}

ContentKind contentKindFromArchetype(ClipArchetype archetype, bool viewerSignals)
{
	if (archetype == ClipArchetype::ViewerMessageResponse || (viewerSignals && archetype == ClipArchetype::Auto))
		return ContentKind::LiveChat;
	if (archetype == ClipArchetype::AdviceAnswer)
		return ContentKind::Advice;
	if (archetype == ClipArchetype::EmotionalReaction)
		return ContentKind::EmotionalReaction;
	if (archetype == ClipArchetype::Explanation)
		return ContentKind::Explanation;
	if (archetype == ClipArchetype::StoryArc)
		return ContentKind::Story;
	if (archetype == ClipArchetype::Opinion)
		return ContentKind::Opinion;
	if (archetype == ClipArchetype::TutorialStep)
		return ContentKind::Tutorial;
	return ContentKind::Unknown;
}

bool isMultipleClipStrategy(ClipStrategy strategy)
{
	return strategy == ClipStrategy::MultipleIndependentClips;
}

} // namespace Curation
