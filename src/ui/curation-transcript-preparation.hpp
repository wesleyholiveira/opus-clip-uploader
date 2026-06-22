#pragma once

#include "models/curation-settings.hpp"
#include "models/transcript.hpp"

#include <QString>

#include <functional>

class QWidget;

void ensure_transcript_for_curation_async(QWidget *parent, const QString &videoPath,
                                         const CurationSettings &curationSettings, bool transcribeOnDemand,
                                         std::function<void(RecordingTranscript, bool)> finishedCallback);
