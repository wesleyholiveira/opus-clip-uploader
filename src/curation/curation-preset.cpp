#include "curation/curation-preset.hpp"

#include "curation/curation-preset-profile.hpp"

#include <QStringList>

#include <utility>

namespace {

static QString normalizedClipLengthPreset(QString preset)
{
	preset = preset.trimmed().toLower();

	if (preset == QStringLiteral("short"))
		return QStringLiteral("Short");

	if (preset == QStringLiteral("long"))
		return QStringLiteral("Long");

	if (preset == QStringLiteral("medium") || preset == QStringLiteral("recommended") ||
	    preset == QStringLiteral("complete"))
		return QStringLiteral("Medium");

	return QStringLiteral("Auto");
}

static bool clipLengthBoundsForPreset(const QString &preset, double &minSec, double &maxSec)
{
	const QString normalizedPreset = normalizedClipLengthPreset(preset);

	if (normalizedPreset == QStringLiteral("Short")) {
		minSec = 30.0;
		maxSec = 60.0;
		return true;
	}

	if (normalizedPreset == QStringLiteral("Medium")) {
		minSec = 60.0;
		maxSec = 120.0;
		return true;
	}

	if (normalizedPreset == QStringLiteral("Long")) {
		minSec = 120.0;
		maxSec = 180.0;
		return true;
	}

	minSec = 0.0;
	maxSec = 0.0;
	return false;
}

static bool shouldApplyProfileClipLength(const Curation::CurationPresetProfile &profile,
					 const QString &clipLengthPreset)
{
	const QString normalized = normalizedClipLengthPreset(clipLengthPreset);
	switch (profile.clipLengthPolicy) {
	case Curation::PresetClipLengthPolicy::Always:
		return true;
	case Curation::PresetClipLengthPolicy::AutoOnly:
		return normalized == QStringLiteral("Auto");
	case Curation::PresetClipLengthPolicy::UnlessLong:
		return normalized != QStringLiteral("Long");
	case Curation::PresetClipLengthPolicy::None:
		return false;
	}
	return false;
}

} // namespace

namespace CurationPreset {

QString autoPresetId()
{
	return Curation::autoPresetProfileId();
}

QString viewerMessageResponsePresetId()
{
	return Curation::viewerMessageResponsePresetProfileId();
}

QString normalizeId(QString presetId)
{
	return Curation::normalizePresetProfileId(std::move(presetId));
}

QVector<QPair<QString, QString>> options()
{
	return Curation::presetProfileOptions();
}

QString labelForId(const QString &presetId)
{
	return Curation::presetProfileLabelForId(presetId);
}

bool isViewerMessageResponsePrompt(const QString &prompt)
{
	return Curation::isViewerMessageResponsePrompt(prompt);
}

QString resolveId(const CurationSettings &settings, const QString &prompt)
{
	return Curation::resolvePresetProfileId(settings, prompt);
}

ClipLengthBounds clipLengthBoundsForSettings(const CurationSettings &settings)
{
	ClipLengthBounds result;
	if (settings.skipCurate)
		return result;

	const Curation::CurationPresetProfile profile = Curation::presetProfileForSettings(settings);
	if (shouldApplyProfileClipLength(profile, settings.clipLengthPreset) && profile.maxDurationSec > 0.0 &&
	    profile.maxDurationSec >= profile.minDurationSec) {
		result.enabled = true;
		result.minSec = profile.minDurationSec;
		result.maxSec = profile.maxDurationSec;
		result.source = profile.clipLengthSource;
		return result;
	}

	double minSec = 0.0;
	double maxSec = 0.0;
	if (clipLengthBoundsForPreset(settings.clipLengthPreset, minSec, maxSec)) {
		result.enabled = true;
		result.minSec = minSec;
		result.maxSec = maxSec;
		result.source = QStringLiteral("clip-length-preset");
		return result;
	}

	return result;
}

} // namespace CurationPreset
