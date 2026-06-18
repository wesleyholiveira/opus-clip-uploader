#pragma once

#include "models/curation-settings.hpp"
#include "models/transcript.hpp"

#include <QJsonObject>
#include <QString>

#include <functional>

namespace Curation {

using TemplateSectionLoader = std::function<QString(const QString &sectionName, const QString &logLabel)>;

QString semanticGateFailurePrefix();
QString promptGenerationBlockedPrefix();
bool isSemanticGateFailurePrompt(const QString &prompt);
QString semanticGateFailureReason(const QString &prompt);
QString promptGenerationBlockedPrompt(const QString &reason);
bool isPromptGenerationBlockedPrompt(const QString &prompt);
QString promptGenerationBlockedReason(const QString &prompt);
QString fallbackRubricOpusPrompt(const CurationSettings &curationSettings,
					const TemplateSectionLoader &templateLoader = {});
QString renderPlanToOpusPrompt(const QJsonObject &plan, const RecordingTranscript &selectedRangeTranscript,
			       const CurationSettings &curationSettings);
QString normalizePlanOutput(const QString &rawOutput, const RecordingTranscript &selectedRangeTranscript,
			   const CurationSettings &curationSettings, const TemplateSectionLoader &templateLoader = {});
QString deterministicOpusPromptFallback(const QString &generatedPrompt,
				       const RecordingTranscript &selectedRangeTranscript,
				       const CurationSettings &curationSettings);
QString applyResolvedPresetPromptGuard(const QString &prompt, const RecordingTranscript &selectedRangeTranscript,
				      const CurationSettings &curationSettings);

} // namespace Curation
