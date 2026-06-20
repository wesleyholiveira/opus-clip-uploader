#pragma once

#include "curation/curation-intent.hpp"
#include "curation/curation-signals.hpp"
#include "curation/prompt/curation-prompt-recipe.hpp"
#include "curation/rules/curation-preset-rule.hpp"
#include "models/curation-settings.hpp"
#include "models/transcript.hpp"

#include <QVector>

#include <memory>

namespace Curation {

class CurationRuleEngine {
public:
	CurationRuleEngine();

	std::shared_ptr<CurationPresetRule> ruleForPresetId(const QString &presetId) const;
	std::shared_ptr<CurationPresetRule> resolveRule(const CurationSettings &settings, const Signals &result,
							const QString &hint) const;
	Intent resolveIntent(const CurationSettings &settings, const RecordingTranscript &selectedRangeTranscript,
			     const QString &hint) const;
	CurationPromptRecipe promptRecipeForPresetId(const QString &presetId) const;

private:
	QVector<std::shared_ptr<CurationPresetRule>> rules;
	std::shared_ptr<CurationPresetRule> autoRule;
};

const CurationRuleEngine &ruleEngine();

} // namespace Curation
