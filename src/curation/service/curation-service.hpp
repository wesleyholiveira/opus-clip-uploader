#pragma once

#include "curation/domain/curation-plan.hpp"
#include "models/curation-settings.hpp"
#include "models/transcript.hpp"

#include <QString>

namespace Curation {

class CurationService {
public:
	CurationDecision decide(const RecordingTranscript &selectedRangeTranscript,
				const CurationSettings &settings, const QString &gptOutput = QString()) const;
};

} // namespace Curation
