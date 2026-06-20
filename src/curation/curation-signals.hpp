#pragma once

#include "models/curation-settings.hpp"
#include "models/transcript.hpp"

#include <QString>
#include <QStringList>

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
	double viewerExchangeScore = 0.0;
	double adviceScore = 0.0;
	double emotionalScore = 0.0;
	double explanationScore = 0.0;
	double storyScore = 0.0;
	double opinionScore = 0.0;
	double tutorialScore = 0.0;
	QStringList emotionalCues;
};

QString joinedTranscriptText(const RecordingTranscript &transcript);
double selectedDurationSeconds(const RecordingTranscript &selectedRangeTranscript,
			       const CurationSettings &curationSettings);
QString scopeForDuration(double durationSec);
bool textHasViewerExchangeSignals(const QString &text);
bool transcriptLooksLikeFragmentedViewerChat(const RecordingTranscript &transcript);
double emotionalScoreForText(const QString &text, QStringList *matchedCues = nullptr);
double adviceScoreForText(const QString &text);
double explanationScoreForText(const QString &text);
double storyScoreForText(const QString &text);
double opinionScoreForText(const QString &text);
double tutorialScoreForText(const QString &text);
Signals analyzeSignals(const RecordingTranscript &transcript, const CurationSettings &curationSettings,
		       const QString &generatedPrompt = QString());
bool looksLikeViewerExchange(const RecordingTranscript &transcript, const CurationSettings &curationSettings,
			     const QString &generatedPrompt = QString());

} // namespace Curation
