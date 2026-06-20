#pragma once

#include "curation/rules/curation-preset-rule.hpp"

namespace Curation {

class EmotionalReactionRule final : public CurationPresetRule {
public:
	QString presetId() const override;
	ClipArchetype archetype() const override;
	ContentKind contentKind(const Signals &result) const override;
	int autoMatchScore(const CurationSettings &settings, const Signals &result, const QString &hint) const override;
	QVector<BoundaryPolicy> boundaryPolicies(const Signals &result) const override;
	CurationPromptRecipe promptRecipe() const override;
};

} // namespace Curation
