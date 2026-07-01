#include "curation/feedback/boundary-calibration.hpp"

#include "curation/feedback/curation-feedback-store.hpp"
#include "curation/curation-preset-profile.hpp"

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <cmath>

namespace Curation::Feedback {

namespace {

static double numericValue(const QJsonObject &object, const QString &key, double fallback)
{
	const QJsonValue value = object.value(key);
	if (!value.isDouble())
		return fallback;
	const double result = value.toDouble(fallback);
	return std::isfinite(result) ? result : fallback;
}

static BoundaryCalibrationProfile fromObject(const QJsonObject &object)
{
	BoundaryCalibrationProfile profile;
	profile.maxQuestionLookbackSec = numericValue(object, QStringLiteral("max_question_lookback_sec"), -1.0);
	profile.lookaheadSec = numericValue(object, QStringLiteral("lookahead_sec"), -1.0);
	profile.contextWeight = std::clamp(numericValue(object, QStringLiteral("context_weight"), 1.0), 0.25, 3.0);
	profile.hookWeight = std::clamp(numericValue(object, QStringLiteral("hook_weight"), 1.0), 0.25, 3.0);
	profile.developmentWeight =
		std::clamp(numericValue(object, QStringLiteral("development_weight"), 1.0), 0.25, 3.0);
	profile.resolutionWeight =
		std::clamp(numericValue(object, QStringLiteral("resolution_weight"), 1.0), 0.25, 3.0);
	profile.targetWeight = std::clamp(numericValue(object, QStringLiteral("target_weight"), 1.0), 0.25, 3.0);
	profile.defectPenalty = std::clamp(numericValue(object, QStringLiteral("defect_penalty"), 1.0), 0.25, 4.0);
	profile.minArcConfidence = numericValue(object, QStringLiteral("min_arc_confidence"), -1.0);
	profile.loaded = true;
	return profile;
}

} // namespace

BoundaryCalibrationProfile BoundaryCalibration::profileForPreset(const QString &presetId)
{
	const QString normalizedPreset = Curation::normalizePresetProfileId(presetId);
	static QMutex cacheMutex;
	static QHash<QString, BoundaryCalibrationProfile> cache;
	{
		QMutexLocker locker(&cacheMutex);
		const auto it = cache.constFind(normalizedPreset);
		if (it != cache.constEnd())
			return it.value();
	}

	BoundaryCalibrationProfile profile;
	const QJsonObject root = CurationFeedbackStore::loadCalibrationRootForProfile(normalizedPreset);
	if (!root.isEmpty()) {
		if (root.value(normalizedPreset).isObject())
			profile = fromObject(root.value(normalizedPreset).toObject());
		else if (root.value(QStringLiteral("profile")).isObject())
			profile = fromObject(root.value(QStringLiteral("profile")).toObject());
		else if (root.value(QStringLiteral("default")).isObject())
			profile = fromObject(root.value(QStringLiteral("default")).toObject());
	}

	QMutexLocker locker(&cacheMutex);
	cache.insert(normalizedPreset, profile);
	return profile;
}

} // namespace Curation::Feedback
