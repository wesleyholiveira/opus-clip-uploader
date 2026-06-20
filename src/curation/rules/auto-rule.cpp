#include "curation/rules/auto-rule.hpp"

#include "curation/curation-preset.hpp"

namespace Curation {

QString AutoRule::presetId() const
{
	return CurationPreset::autoPresetId();
}

ClipArchetype AutoRule::archetype() const
{
	return ClipArchetype::Auto;
}

ContentKind AutoRule::contentKind(const Signals &result) const
{
	return result.likelyViewerExchange ? ContentKind::LiveChat : ContentKind::Unknown;
}

CurationPromptRecipe AutoRule::promptRecipe() const
{
	return {presetId(),
		QStringLiteral(
			"Find the strongest self-contained clip where the speaker develops one clear local idea from setup to conclusion."),
		QStringLiteral(
			"Find strong self-contained clips where the speaker develops clear local ideas from setup to conclusion."),
		QStringLiteral(
			"Prioritize continuous spoken development with minimal dead air and enough local context to stand alone."),
		QStringLiteral(
			"Choose clips with clean natural boundaries that stop after the local point resolves instead of continuing into another topic.")};
}

} // namespace Curation
