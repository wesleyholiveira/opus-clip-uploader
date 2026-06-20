#include "curation/compiler/prompt-compiler-internal.hpp"

#include "curation/analysis/named-reference-detector.hpp"
#include "curation/curation-preset.hpp"
#include "curation/curation-rules.hpp"
#include "curation/curation-signals.hpp"
#include "curation/opus-prompt-renderer.hpp"

using namespace Curation;

QString Curation::fallbackRubricOpusPrompt(const CurationSettings &curationSettings,
					   const TemplateSectionLoader &templateLoader)
{
	if (templateLoader) {
		const QString runtimeTemplate =
			templateLoader(QStringLiteral("fallback_opus_prompt"), QStringLiteral("GPT fallback rubric"));
		if (!runtimeTemplate.trimmed().isEmpty())
			return renderFallbackRubricTemplate(runtimeTemplate, curationSettings);
	}

	return CurationPreset::fallbackOpusPrompt(curationSettings, true);
}

QString Curation::deterministicOpusPromptFallback(const QString &generatedPrompt,
						  const RecordingTranscript &selectedRangeTranscript,
						  const CurationSettings &curationSettings)
{
	const QString scope = scopeForDuration(selectedDurationSeconds(selectedRangeTranscript, curationSettings));
	QString target = explicitViewerTargetFromSettings(curationSettings);
	if (target.isEmpty() &&
	    resolveIntent(curationSettings, selectedRangeTranscript, generatedPrompt).resolvedPresetId !=
		    CurationPreset::viewerMessageResponsePresetId()) {
		target = viewerTargetFromRenderedPrompt(generatedPrompt);
		if (target.isEmpty())
			target = deterministicPromptTarget(generatedPrompt, selectedRangeTranscript, curationSettings);
	}

	const QStringList namedReferences = importantNamedReferences(selectedRangeTranscript);
	const bool needsNamedReference =
		transcriptHasPronounDependentNamedReference(selectedRangeTranscript) && !namedReferences.isEmpty();
	const bool hasReferenceBackedUnlockMethod = transcriptHasReferenceBackedUnlockMethod(selectedRangeTranscript);
	const Intent intent = resolveIntent(curationSettings, selectedRangeTranscript, generatedPrompt);
	const bool viewerExchange = intent.resolvedPresetId == CurationPreset::viewerMessageResponsePresetId() &&
				    !hasReferenceBackedUnlockMethod;

	QString sentence1;
	if (viewerExchange) {
		const QString targetSuffix = viewerMessageTargetSuffix(target, QString());
		if (!targetSuffix.isEmpty()) {
			const bool multipleClips = shouldUseMultipleClips(intent);
			return renderViewerMessagePrompt(multipleClips, targetSuffix, false);
		}

		return OpusPromptRenderer::renderIntentPrompt(intent);
	} else if (shouldUseMultipleClips(intent) && scope == QStringLiteral("large_range_multiple_clips"))
		sentence1 = QStringLiteral("Find multiple strong self-contained clips where the speaker explains %1.")
				    .arg(target);
	else if (scope == QStringLiteral("short_range_best_moment"))
		sentence1 = QStringLiteral("Find the strongest self-contained moment where the speaker explains %1.")
				    .arg(target);
	else
		sentence1 =
			QStringLiteral("Find one clear self-contained clip where the speaker explains %1.").arg(target);

	QString sentence2;
	if (viewerExchange) {
		sentence2 = QStringLiteral(
			"Prioritize emotionally consequential and clearly useful exchanges over casual banter, starting with only enough setup from that message and following one continuous response.");
	} else if (hasReferenceBackedUnlockMethod) {
		sentence2 =
			QStringLiteral(
				"Prioritize moments that introduce %1 with the key/unlock metaphor before later pronouns, then show how one piece of language logic becomes easier to understand with continuous spoken development and minimal dead air.")
				.arg(namedReferences.first());
	} else if (needsNamedReference) {
		sentence2 =
			QStringLiteral(
				"Prioritize moments that introduce %1 or the target idea before later pronouns and indirect references, then develop one continuous local explanation with minimal dead air.")
				.arg(namedReferences.first());
	} else if (scope == QStringLiteral("short_range_best_moment")) {
		sentence2 = QStringLiteral(
			"Prioritize moments that introduce the idea with enough local context and complete one resolved part of the explanation with minimal dead air.");
	} else {
		sentence2 = QStringLiteral(
			"Prioritize moments that introduce the idea with enough local context and develop the same continuous explanation with minimal dead air.");
	}

	const QString sentence3 =
		viewerExchange
			? QStringLiteral("Choose a clip that stays focused on that one exchange from start to finish.")
			: QStringLiteral(
				  "Prefer clips that end after the local point is resolved, with clean natural boundaries and a complete local idea.");

	return QStringLiteral("%1 %2 %3").arg(sentence1, sentence2, sentence3).trimmed();
}
