#include "curation/opus-prompt-renderer.hpp"

#include "curation/curation-preset.hpp"
#include "curation/curation-rules.hpp"
#include "curation/prompt/curation-prompt-recipe.hpp"

namespace OpusPromptRenderer {

QString renderPresetPrompt(const QString &presetId, bool multipleClips)
{
	const Curation::CurationPromptRecipe recipe =
		Curation::promptRecipeForPresetId(CurationPreset::normalizeId(presetId));
	return Curation::renderPromptRecipe(recipe, multipleClips);
}

QString renderIntentPrompt(const Curation::Intent &intent)
{
	return Curation::renderPromptRecipe(Curation::promptRecipeForIntent(intent), Curation::shouldUseMultipleClips(intent));
}

} // namespace OpusPromptRenderer
