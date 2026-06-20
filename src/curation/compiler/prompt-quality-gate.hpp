#pragma once

#include "models/curation-settings.hpp"
#include "models/transcript.hpp"

#include <QString>
#include <QStringList>

namespace Curation {

QString opusPromptPayload(const QString &prompt);
QStringList promptQualityIssues(const QString &prompt, const RecordingTranscript &selectedRangeTranscript,
					const CurationSettings &curationSettings);
bool shouldRepairPrompt(const QString &prompt, const RecordingTranscript &selectedRangeTranscript,
			const CurationSettings &curationSettings, QStringList *issues = nullptr);

} // namespace Curation
