#include "curation/compiler/prompt-compiler-internal.hpp"

#include <QString>

using namespace Curation;

QString Curation::normalizePlanOutput(const QString &rawOutput, const RecordingTranscript &selectedRangeTranscript,
				       const CurationSettings &curationSettings,
				       const TemplateSectionLoader &templateLoader)
{
	const QString trimmedOutput = rawOutput.trimmed();
	if (trimmedOutput.isEmpty())
		return {};

	if (trimmedOutput.startsWith(QString::fromLatin1(semanticGateFailurePrefixLiteral()), Qt::CaseInsensitive)) {
		if (allowNoStrongFailure(selectedRangeTranscript, curationSettings))
			return trimmedOutput;

		return fallbackRubricOpusPrompt(curationSettings, templateLoader);
	}

	const QJsonObject plan = extractJsonObjectFromText(trimmedOutput);
	if (!plan.isEmpty()) {
		const QString renderedPrompt = renderPlanToOpusPrompt(plan, selectedRangeTranscript, curationSettings).trimmed();
		if (renderedPrompt.startsWith(QString::fromLatin1(semanticGateFailurePrefixLiteral()), Qt::CaseInsensitive) &&
		    !allowNoStrongFailure(selectedRangeTranscript, curationSettings))
			return fallbackRubricOpusPrompt(curationSettings, templateLoader);
		if (!hasEnglishOnlyPromptText(renderedPrompt))
			return englishOnlyPresetFallback(selectedRangeTranscript, curationSettings, renderedPrompt);
		return renderedPrompt;
	}

	const QString opusPrompt = lineValueForPrefix(trimmedOutput, opusPromptPrefixLiteral());
	if (!opusPrompt.isEmpty())
		return hasEnglishOnlyPromptText(opusPrompt)
			       ? opusPrompt
			       : englishOnlyPresetFallback(selectedRangeTranscript, curationSettings, opusPrompt);

	return hasEnglishOnlyPromptText(trimmedOutput)
		       ? trimmedOutput
		       : englishOnlyPresetFallback(selectedRangeTranscript, curationSettings, trimmedOutput);
}
