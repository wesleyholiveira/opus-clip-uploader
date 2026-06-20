#include "curation/compiler/prompt-compiler-internal.hpp"

#include "curation/analysis/named-reference-detector.hpp"
#include "curation/curation-preset.hpp"
#include "curation/curation-rules.hpp"
#include "curation/curation-signals.hpp"
#include "curation/opus-prompt-renderer.hpp"

using namespace Curation;

QString Curation::applyResolvedPresetPromptGuard(const QString &prompt,
						 const RecordingTranscript &selectedRangeTranscript,
						 const CurationSettings &curationSettings)
{
	if (isSemanticGateFailurePrompt(prompt))
		return prompt;

	if (!hasEnglishOnlyPromptText(prompt))
		return englishOnlyPresetFallback(selectedRangeTranscript, curationSettings, prompt);

	const Intent intent = resolveIntent(curationSettings, selectedRangeTranscript, prompt);
	if (intent.resolvedPresetId == CurationPreset::autoPresetId() ||
	    transcriptHasReferenceBackedUnlockMethod(selectedRangeTranscript))
		return prompt;

	const QString lowerPrompt = prompt.toLower();
	const bool genericLocalIdeaPrompt =
		lowerPrompt.contains(QStringLiteral("one clear local idea")) ||
		lowerPrompt.contains(QStringLiteral("local idea from setup to conclusion")) ||
		lowerPrompt.contains(QStringLiteral("local point resolves"));

	if (intent.resolvedPresetId == CurationPreset::viewerMessageResponsePresetId() &&
	    !CurationPreset::isViewerMessageResponsePrompt(prompt) && !isStructuredViewerMessagePrompt(prompt))
		return OpusPromptRenderer::renderIntentPrompt(intent);

	if (genericLocalIdeaPrompt && intent.resolvedPresetId != CurationPreset::autoPresetId())
		return OpusPromptRenderer::renderIntentPrompt(intent);

	return prompt;
}
