#pragma once

#include "models/transcript.hpp"
#include "transcription/realtime-transcription-service.hpp"
#include "transcription/whisperx-settings.hpp"

#include <QString>

namespace Transcription {

class WhisperXAlignmentService {
public:
	RecordingTranscript transcribeVideo(const QString &videoPath, const QString &language,
		const WhisperXSettings &settings, const TranscriptionProgressCallback &progressCallback = {},
		const TranscriptionCancelCallback &cancelCallback = {}) const;

	RecordingTranscript alignVideoTranscript(const QString &videoPath, const QString &language,
		const RecordingTranscript &baseTranscript, const WhisperXSettings &settings,
		const TranscriptionProgressCallback &progressCallback = {},
		const TranscriptionCancelCallback &cancelCallback = {}) const;
};

} // namespace Transcription
