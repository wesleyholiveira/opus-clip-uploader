#pragma once

#include "curation/curation-intent.hpp"
#include "curation/curation-signals.hpp"
#include "models/curation-settings.hpp"
#include "models/transcript.hpp"

#include <QString>

namespace Curation {

Intent resolveIntent(const CurationSettings &settings, const RecordingTranscript &selectedRangeTranscript,
		     const QString &hint = QString());
QString resolvePresetId(const CurationSettings &settings, const RecordingTranscript &selectedRangeTranscript,
			const QString &hint = QString());
bool shouldUseMultipleClips(const Intent &intent);

} // namespace Curation
