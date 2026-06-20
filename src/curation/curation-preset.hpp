#pragma once

#include "models/curation-settings.hpp"

#include <QPair>
#include <QString>
#include <QVector>

namespace CurationPreset {

struct ClipLengthBounds {
	bool enabled = false;
	double minSec = 0.0;
	double maxSec = 0.0;
	QString source = QStringLiteral("auto");
};

QString autoPresetId();
QString viewerMessageResponsePresetId();
QString normalizeId(QString presetId);
QVector<QPair<QString, QString>> options();
QString labelForId(const QString &presetId);
QString resolveId(const CurationSettings &settings, const QString &prompt = QString());
QString gptContextForId(const QString &presetId);
QString opusPromptForId(const QString &presetId, bool multipleClips);
QString fallbackOpusPrompt(const CurationSettings &settings, bool multipleClips);
ClipLengthBounds clipLengthBoundsForSettings(const CurationSettings &settings);
bool isViewerMessageResponsePrompt(const QString &prompt);

} // namespace CurationPreset
