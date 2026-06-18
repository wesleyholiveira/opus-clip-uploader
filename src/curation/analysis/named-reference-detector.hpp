#pragma once

#include "models/transcript.hpp"

#include <QString>
#include <QStringList>

namespace Curation {

QStringList importantNamedReferences(const RecordingTranscript &transcript);
QString formatImportantNamedReferences(const RecordingTranscript &transcript);
bool transcriptHasPronounDependentNamedReference(const RecordingTranscript &transcript);
bool transcriptHasReferenceBackedUnlockMethod(const RecordingTranscript &transcript);
bool promptContainsAnyNamedReference(const QString &prompt, const RecordingTranscript &transcript);

} // namespace Curation
