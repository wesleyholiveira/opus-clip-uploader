#pragma once

#include "models/curation-settings.hpp"
#include "models/transcript.hpp"

#include <QString>

namespace Curation {

struct Signals {
	int segmentCount = 0;
	double selectedDurationSec = 0.0;
	double transcriptDurationSec = 0.0;
	double averageSegmentSec = 0.0;
	double averageCharsPerSegment = 0.0;
	bool hasMetadataViewerSignals = false;
	bool hasTranscriptViewerSignals = false;
	bool hasFragmentedViewerChatSignals = false;
	bool likelyViewerExchange = false;
};

QString joinedTranscriptText(const RecordingTranscript &transcript);
double selectedDurationSeconds(const RecordingTranscript &selectedRangeTranscript,
			       const CurationSettings &curationSettings);
QString scopeForDuration(double durationSec);
bool textHasViewerExchangeSignals(const QString &text);
bool transcriptLooksLikeFragmentedViewerChat(const RecordingTranscript &transcript);
Signals analyzeSignals(const RecordingTranscript &transcript, const CurationSettings &curationSettings,
		       const QString &generatedPrompt = QString());
bool looksLikeViewerExchange(const RecordingTranscript &transcript, const CurationSettings &curationSettings,
			     const QString &generatedPrompt = QString());

} // namespace Curation
