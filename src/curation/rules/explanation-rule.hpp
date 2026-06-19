#pragma once

#include "curation/rules/curation-preset-rule.hpp"

namespace Curation {

class ExplanationRule final : public CurationPresetRule {
public:
	QString presetId() const override;
	ClipArchetype archetype() const override;
	ContentKind contentKind(const Signals &result) const override;
	int autoMatchScore(const CurationSettings &settings, const Signals &result, const QString &hint) const override;
	ClipStrategy clipStrategy(const CurationSettings &settings, const Signals &result,
				  const QString &scope) const override;
	CurationPromptRecipe promptRecipe() const override;
};

} // namespace Curation
