#include "opus/opus-curation-payload-builder.hpp"

#include "curation/curation-preset.hpp"

#include <QJsonArray>

#include <algorithm>

namespace {

QJsonArray clipDurationsForBounds(double minSec, double maxSec)
{
	QJsonArray bounds;
	bounds.append(minSec);
	bounds.append(maxSec);

	QJsonArray clipDurations;
	clipDurations.append(bounds);
	return clipDurations;
}

} // namespace

QJsonObject OpusCurationPayloadBuilder::build(const ClipDuration &rangeValue,
						     const CurationSettings &settings) const
{
	QJsonObject curationPref;

	QJsonObject range;
	range.insert("startSec", rangeValue.startSec);
	range.insert("endSec", rangeValue.endSec);
	curationPref.insert("range", range);

	const double rangeDurationSec = std::max(0.0, rangeValue.endSec - rangeValue.startSec);
	const bool createFixedClip = settings.skipCurate;
	const CurationPreset::ClipLengthBounds clipLengthBounds = CurationPreset::clipLengthBoundsForSettings(settings);
	const bool hasPreferredClipLength = !createFixedClip && clipLengthBounds.enabled;

	if (createFixedClip) {
		QJsonArray clipDurations;
		QJsonArray item;
		item.append(rangeValue.startSec);
		item.append(rangeValue.endSec);
		clipDurations.append(item);
		curationPref.insert("clipDurations", clipDurations);

		QJsonArray clipStarts;
		clipStarts.append(rangeValue.startSec);
		curationPref.insert("clip_start", clipStarts);

		QJsonArray clipDurationSeconds;
		clipDurationSeconds.append(rangeDurationSec);
		curationPref.insert("clip_duration", clipDurationSeconds);
	} else if (hasPreferredClipLength) {
		curationPref.insert("clipDurations", clipDurationsForBounds(clipLengthBounds.minSec, clipLengthBounds.maxSec));
	}

	QJsonArray topicKeywords;
	for (const QString &keyword : settings.topicKeywords)
		topicKeywords.append(keyword);

	curationPref.insert("model", settings.model.trimmed().isEmpty() ? "ClipAnything" : settings.model.trimmed());
	curationPref.insert("topicKeywords", topicKeywords);
	curationPref.insert("genre", settings.genre.trimmed().isEmpty() ? "Auto" : settings.genre);
	curationPref.insert("skipCurate", settings.skipCurate);

	if (!settings.aiPrompt.trimmed().isEmpty()) {
		curationPref.insert("prompt", settings.aiPrompt.trimmed());
		curationPref.insert("userPrompt", settings.aiPrompt.trimmed());
	}

	return curationPref;
}
