#pragma once

#include "curation/rules/curation-preset-rule.hpp"

namespace Curation {

class AutoRule final : public CurationPresetRule {
public:
	QString presetId() const override;
	ClipArchetype archetype() const override;
	ContentKind contentKind(const Signals &result) const override;
	CurationPromptRecipe promptRecipe() const override;
};

} // namespace Curation
