#pragma once

#include "curation/curation-intent.hpp"

#include <QString>

namespace OpusPromptRenderer {

QString renderPresetPrompt(const QString &presetId, bool multipleClips);
QString renderIntentPrompt(const Curation::Intent &intent);

} // namespace OpusPromptRenderer
