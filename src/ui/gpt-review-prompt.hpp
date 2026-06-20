#pragma once

#include "models/curation-settings.hpp"
#include "models/transcript.hpp"

#include <QString>

#include <functional>

class QWidget;

struct GeneratedCurationPromptResult {
	QString prompt;
	CurationSettings curationSettings;
};


void ensure_transcript_for_curation_async(QWidget *parent, const QString &videoPath,
					 const CurationSettings &curationSettings, bool transcribeOnDemand,
					 std::function<void(RecordingTranscript, bool)> finishedCallback);

void generate_custom_prompt_for_curation_async(QWidget *parent, const QString &videoPath,
					       const CurationSettings &curationSettings, bool transcribeOnDemand,
					       std::function<void(QString)> finishedCallback);

void generate_custom_prompt_for_curation_result_async(QWidget *parent, const QString &videoPath,
						      const CurationSettings &curationSettings,
						      bool transcribeOnDemand,
						      std::function<void(GeneratedCurationPromptResult)> finishedCallback);

void generate_custom_prompt_before_review_async(QWidget *parent, const QString &videoPath, bool transcribeOnDemand,
						std::function<void()> finishedCallback);
