#include "curation/rules/curation-rule-engine.hpp"

#include "curation/curation-preset.hpp"
#include "curation/rules/advice-answer-rule.hpp"
#include "curation/rules/auto-rule.hpp"
#include "curation/rules/emotional-reaction-rule.hpp"
#include "curation/rules/explanation-rule.hpp"
#include "curation/rules/opinion-rule.hpp"
#include "curation/rules/story-arc-rule.hpp"
#include "curation/rules/tutorial-step-rule.hpp"
#include "curation/rules/viewer-message-response-rule.hpp"

namespace Curation {

CurationRuleEngine::CurationRuleEngine()
{
	rules.append(std::make_shared<ViewerMessageResponseRule>());
	rules.append(std::make_shared<AdviceAnswerRule>());
	rules.append(std::make_shared<EmotionalReactionRule>());
	rules.append(std::make_shared<ExplanationRule>());
	rules.append(std::make_shared<StoryArcRule>());
	rules.append(std::make_shared<OpinionRule>());
	rules.append(std::make_shared<TutorialStepRule>());
	autoRule = std::make_shared<AutoRule>();
}

std::shared_ptr<CurationPresetRule> CurationRuleEngine::ruleForPresetId(const QString &presetId) const
{
	const QString normalized = CurationPreset::normalizeId(presetId);
	if (normalized == CurationPreset::autoPresetId())
		return autoRule;
	for (const auto &rule : rules) {
		if (rule && rule->presetId() == normalized)
			return rule;
	}
	return autoRule;
}

std::shared_ptr<CurationPresetRule> CurationRuleEngine::resolveRule(const CurationSettings &settings,
								    const Signals &result, const QString &hint) const
{
	const QString requestedPresetId = CurationPreset::normalizeId(settings.curationPreset);
	if (requestedPresetId != CurationPreset::autoPresetId())
		return ruleForPresetId(requestedPresetId);

	std::shared_ptr<CurationPresetRule> bestRule;
	int bestScore = 0;
	for (const auto &rule : rules) {
		const int score = rule ? rule->autoMatchScore(settings, result, hint) : 0;
		if (score > bestScore) {
			bestScore = score;
			bestRule = rule;
		}
	}

	if (bestRule && bestScore >= 48)
		return bestRule;

	const QString fallbackPresetId = CurationPreset::resolveId(settings, hint);
	return ruleForPresetId(fallbackPresetId);
}

Intent CurationRuleEngine::resolveIntent(const CurationSettings &settings,
					 const RecordingTranscript &selectedRangeTranscript, const QString &hint) const
{
	const Signals result = analyzeSignals(selectedRangeTranscript, settings, hint);
	const QString requestedPresetId = CurationPreset::normalizeId(settings.curationPreset);
	const auto rule = resolveRule(settings, result, hint);
	const QString resolvedPresetId = rule ? rule->presetId() : CurationPreset::autoPresetId();
	const QString scope = scopeForDuration(result.selectedDurationSec);

	Intent intent;
	intent.requestedPresetId = requestedPresetId;
	intent.resolvedPresetId = resolvedPresetId;
	intent.selectedDurationSec = result.selectedDurationSec;
	intent.scope = scope;
	intent.viewerSignals = result.likelyViewerExchange;
	intent.viewerExchangeScore = result.viewerExchangeScore;
	intent.adviceScore = result.adviceScore;
	intent.emotionalScore = result.emotionalScore;
	intent.explanationScore = result.explanationScore;
	intent.storyScore = result.storyScore;
	intent.opinionScore = result.opinionScore;
	intent.tutorialScore = result.tutorialScore;
	intent.archetype = rule ? rule->archetype() : ClipArchetype::Auto;
	intent.contentKind = rule ? rule->contentKind(result) : ContentKind::Unknown;
	intent.strategy = rule ? rule->clipStrategy(settings, result, scope) : defaultClipStrategyForScope(scope);
	intent.boundaryPolicies = rule ? rule->boundaryPolicies(result)
				       : QVector<BoundaryPolicy>{BoundaryPolicy::NaturalResolution};
	return intent;
}

CurationPromptRecipe CurationRuleEngine::promptRecipeForPresetId(const QString &presetId) const
{
	const auto rule = ruleForPresetId(presetId);
	return rule ? rule->promptRecipe() : autoRule->promptRecipe();
}

const CurationRuleEngine &ruleEngine()
{
	static const CurationRuleEngine engine;
	return engine;
}

} // namespace Curation
