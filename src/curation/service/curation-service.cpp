#include "curation/service/curation-service.hpp"

#include "curation/compiler/opus-prompt-compiler.hpp"
#include "curation/compiler/prompt-quality-gate.hpp"
#include "curation/curation-rules.hpp"
#include "curation/opus-prompt-renderer.hpp"

namespace Curation {

CurationDecision CurationService::decide(const RecordingTranscript &selectedRangeTranscript,
						 const CurationSettings &settings, const QString &gptOutput) const
{
	CurationDecision decision;
	const Intent intent = resolveIntent(settings, selectedRangeTranscript, gptOutput);
	decision.plan.presetId = intent.resolvedPresetId;

	QString prompt;
	if (!gptOutput.trimmed().isEmpty()) {
		decision.usedGpt = true;
		prompt = applyResolvedPresetPromptGuard(normalizePlanOutput(gptOutput, selectedRangeTranscript, settings),
					       selectedRangeTranscript, settings);
	}

	if (prompt.trimmed().isEmpty()) {
		decision.usedFallback = true;
		prompt = OpusPromptRenderer::renderIntentPrompt(intent);
	}

	decision.blockedBySemanticGate = isSemanticGateFailurePrompt(prompt);
	decision.validationIssues = promptQualityIssues(prompt, selectedRangeTranscript, settings);
	if (!decision.validationIssues.isEmpty() && !decision.blockedBySemanticGate) {
		decision.usedFallback = true;
		prompt = deterministicOpusPromptFallback(prompt, selectedRangeTranscript, settings);
		decision.validationIssues = promptQualityIssues(prompt, selectedRangeTranscript, settings);
	}

	decision.opusPrompt = prompt.trimmed();
	return decision;
}

} // namespace Curation
