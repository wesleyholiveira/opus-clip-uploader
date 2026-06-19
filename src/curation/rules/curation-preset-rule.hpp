#pragma once

#include "curation/curation-intent.hpp"
#include "curation/curation-signals.hpp"
#include "curation/prompt/curation-prompt-recipe.hpp"
#include "curation/rules/curation-rule-helpers.hpp"
#include "models/curation-settings.hpp"

#include <QVector>

namespace Curation {

class CurationPresetRule {
public:
	virtual ~CurationPresetRule() = default;

	virtual QString presetId() const = 0;
	virtual ClipArchetype archetype() const = 0;
	virtual ContentKind contentKind(const Signals &result) const = 0;
	virtual CurationPromptRecipe promptRecipe() const = 0;

	virtual int autoMatchScore(const CurationSettings &settings, const Signals &result, const QString &hint) const
	{
		Q_UNUSED(settings);
		Q_UNUSED(result);
		Q_UNUSED(hint);
		return 0;
	}

	virtual ClipStrategy clipStrategy(const CurationSettings &settings, const Signals &result,
					  const QString &scope) const
	{
		Q_UNUSED(settings);
		Q_UNUSED(result);
		return defaultClipStrategyForScope(scope);
	}

	virtual QVector<BoundaryPolicy> boundaryPolicies(const Signals &result) const
	{
		Q_UNUSED(result);
		return {BoundaryPolicy::NaturalResolution};
	}
};

} // namespace Curation
