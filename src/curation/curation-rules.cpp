#include "curation/curation-rules.hpp"

#include "curation/rules/curation-rule-engine.hpp"

namespace Curation {

Intent resolveIntent(const CurationSettings &settings, const RecordingTranscript &selectedRangeTranscript,
		     const QString &hint)
{
	return ruleEngine().resolveIntent(settings, selectedRangeTranscript, hint);
}

QString resolvePresetId(const CurationSettings &settings, const RecordingTranscript &selectedRangeTranscript,
			const QString &hint)
{
	return resolveIntent(settings, selectedRangeTranscript, hint).resolvedPresetId;
}

bool shouldUseMultipleClips(const Intent &intent)
{
	return isMultipleClipStrategy(intent.strategy);
}

CurationPromptRecipe promptRecipeForIntent(const Intent &intent)
{
	return ruleEngine().promptRecipeForPresetId(intent.resolvedPresetId);
}

CurationPromptRecipe promptRecipeForPresetId(const QString &presetId)
{
	return ruleEngine().promptRecipeForPresetId(presetId);
}

} // namespace Curation
