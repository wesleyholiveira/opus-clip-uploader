#include "curation/rules/viewer-message-response-rule.hpp"

#include "curation/curation-preset.hpp"

namespace Curation {

QString ViewerMessageResponseRule::presetId() const
{
	return CurationPreset::viewerMessageResponsePresetId();
}

ClipArchetype ViewerMessageResponseRule::archetype() const
{
	return ClipArchetype::ViewerMessageResponse;
}

ContentKind ViewerMessageResponseRule::contentKind(const Signals &) const
{
	return ContentKind::LiveChat;
}

int ViewerMessageResponseRule::autoMatchScore(const CurationSettings &settings, const Signals &result,
					      const QString &hint) const
{
	const QString text = combinedRuleText(settings, hint);
	if (CurationPreset::isViewerMessageResponsePrompt(hint) ||
	    CurationPreset::isViewerMessageResponsePrompt(settings.aiPrompt))
		return 100;
	if (result.hasFragmentedViewerChatSignals)
		return 90;
	if (result.likelyViewerExchange)
		return 80;
	if (containsAny(text, {QStringLiteral("viewer"), QStringLiteral("live chat"), QStringLiteral("comentario"),
			       QStringLiteral("comentário"), QStringLiteral("mensagem"), QStringLiteral("pergunta")}))
		return 70;
	return 0;
}

ClipStrategy ViewerMessageResponseRule::clipStrategy(const CurationSettings &settings, const Signals &,
						     const QString &scope) const
{
	return hasSingleConfirmedRange(settings) ? ClipStrategy::BestMoment : defaultClipStrategyForScope(scope);
}

QVector<BoundaryPolicy> ViewerMessageResponseRule::boundaryPolicies(const Signals &) const
{
	return {BoundaryPolicy::NaturalResolution, BoundaryPolicy::StopBeforeNextViewerMessage,
		BoundaryPolicy::StopBeforeHousekeeping, BoundaryPolicy::StopBeforeTopicShift};
}

CurationPromptRecipe ViewerMessageResponseRule::promptRecipe() const
{
	return {presetId(),
		QStringLiteral(
			"Find one continuous, unbroken clip built from one complete response to a single viewer message."),
		QStringLiteral(
			"Find continuous, unbroken clips, each built from one complete response to a single viewer message."),
		QStringLiteral(
			"Prefer the clearest emotionally consequential viewer issue and start at the self-contained sentence that states it."),
		QStringLiteral(
			"Follow the speaker's direct answer through its first local resolution, ending as soon as the speaker leaves that exchange.")};
}

} // namespace Curation
