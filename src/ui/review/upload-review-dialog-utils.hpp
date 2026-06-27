#pragma once

#include "models/curation-settings.hpp"
#include "models/transcript.hpp"
#include "curation/feedback/curation-feedback-store.hpp"
#include "curation/scoring/cheap-clip-scorer.hpp"

#include <QByteArray>
#include <QComboBox>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QTableWidgetItem>
#include <QVariant>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <utility>


namespace ReviewDialogUtils {

static constexpr const char *CONFIG_REVIEW_SETTINGS_PREFIX = "review.settings";
static constexpr const char *CONFIG_OPUS_SOURCE_LANGUAGE = "opus_source_lang";
static constexpr const char *LANGUAGE_AUTO = "auto";
static constexpr double DEFAULT_NO_MARKER_PLACEHOLDER_SEC = 90.0;
static constexpr int REVIEW_ROW_KIND_ROLE = Qt::UserRole + 10;
static constexpr int REVIEW_DIAGNOSTIC_INDEX_ROLE = Qt::UserRole + 11;
static constexpr int REVIEW_ROW_KIND_MARKER = 0;
static constexpr int REVIEW_ROW_KIND_DIAGNOSTIC = 1;
static constexpr const char *DIAGNOSTIC_STATE_LIKED = "liked";
static constexpr const char *DIAGNOSTIC_STATE_DISLIKED = "disliked";
static constexpr const char *DIAGNOSTIC_STATE_ADJUSTED = "adjusted";
static constexpr const char *DIAGNOSTIC_STATE_APPROVED_ADJUSTED = "approved_adjusted";
static constexpr const char *DIAGNOSTIC_STATE_IGNORED = "ignored_diagnostic";

static int tableItemIntDataOr(const QTableWidgetItem *item, int role, int fallback)
{
	if (!item)
		return fallback;
	const QVariant value = item->data(role);
	if (!value.isValid())
		return fallback;
	bool ok = false;
	const int parsed = value.toInt(&ok);
	return ok ? parsed : fallback;
}

static bool diagnosticRangeFromObject(const QJsonObject &object, ClipDuration *outRange)
{
	if (!outRange)
		return false;
	const double startSec =
		object.value(QStringLiteral("start_sec")).toDouble(std::numeric_limits<double>::quiet_NaN());
	const double endSec =
		object.value(QStringLiteral("end_sec")).toDouble(std::numeric_limits<double>::quiet_NaN());
	if (!std::isfinite(startSec) || !std::isfinite(endSec) || endSec <= startSec)
		return false;
	outRange->startSec = startSec;
	outRange->endSec = endSec;
	return true;
}

static bool diagnosticRangeMeaningfullyEdited(const ClipDuration &originalRange, const ClipDuration &reviewedRange)
{
	if (!std::isfinite(originalRange.startSec) || !std::isfinite(originalRange.endSec) ||
	    !std::isfinite(reviewedRange.startSec) || !std::isfinite(reviewedRange.endSec))
		return false;
	const double startDelta = std::fabs(originalRange.startSec - reviewedRange.startSec);
	const double endDelta = std::fabs(originalRange.endSec - reviewedRange.endSec);
	const double durationDelta = std::fabs((originalRange.endSec - originalRange.startSec) -
					       (reviewedRange.endSec - reviewedRange.startSec));
	return startDelta > 0.25 || endDelta > 0.25 || durationDelta > 0.25;
}

static QString diagnosticReasonLabel(const QJsonObject &diagnostic)
{
	const QString reason = diagnostic.value(QStringLiteral("rejection_reason")).toString().trimmed();
	if (!reason.isEmpty())
		return reason;
	const QString status = diagnostic.value(QStringLiteral("review_status")).toString().trimmed();
	return status.isEmpty() ? QStringLiteral("diagnostic") : status;
}

static bool isReviewDiagnosticCandidateObject(const QJsonObject &diagnostic)
{
	ClipDuration ignored;
	if (!diagnosticRangeFromObject(diagnostic, &ignored))
		return false;
	if (diagnostic.contains(QStringLiteral("diagnostic_feedback_source_index")))
		return false;
	const QString kind = diagnostic.value(QStringLiteral("diagnostic_kind")).toString().trimmed();
	if (kind == QStringLiteral("selected_candidate") ||
	    kind == QStringLiteral("marker_created_from_rejected_candidate"))
		return false;
	const QString status = diagnostic.value(QStringLiteral("review_status")).toString().trimmed();
	if (status == QStringLiteral("created_marker_from_rejected_diagnostic"))
		return false;
	return true;
}

static QString diagnosticStateLabel(const QString &state)
{
	if (state == QString::fromLatin1(DIAGNOSTIC_STATE_LIKED))
		return QStringLiteral("Approved");
	if (state == QString::fromLatin1(DIAGNOSTIC_STATE_DISLIKED))
		return QStringLiteral("Rejected");
	if (state == QString::fromLatin1(DIAGNOSTIC_STATE_ADJUSTED))
		return QStringLiteral("Needs adjustment");
	if (state == QString::fromLatin1(DIAGNOSTIC_STATE_APPROVED_ADJUSTED))
		return QStringLiteral("Good edited clip");
	if (state == QString::fromLatin1(DIAGNOSTIC_STATE_IGNORED))
		return QStringLiteral("Ignored for training");
	return QStringLiteral("Pending");
}

static bool isDefaultNoMarkerPlaceholderRange(bool hasExplicitClipMarkers, qint64 durationMs,
                                               const QVector<ClipDuration> &ranges)
{
	if (hasExplicitClipMarkers || ranges.size() != 1 || durationMs <= 0)
		return false;

	const double durationSec = durationMs / 1000.0;
	const double expectedEnd = std::min(DEFAULT_NO_MARKER_PLACEHOLDER_SEC, durationSec);
	const ClipDuration &range = ranges.first();
	return std::isfinite(range.startSec) && std::isfinite(range.endSec) && std::fabs(range.startSec) <= 0.05 &&
	       std::fabs(range.endSec - expectedEnd) <= 0.25;
}

static QString normalizeLanguageSetting(QString value)
{
	value = value.trimmed().toLower();
	if (value.isEmpty())
		return QString::fromLatin1(LANGUAGE_AUTO);

	if (value == QStringLiteral("pt-br") || value == QStringLiteral("portuguese"))
		return QStringLiteral("pt");

	if (value == QStringLiteral("en-us") || value == QStringLiteral("english"))
		return QStringLiteral("en");

	return value;
}

static void addLanguageOptions(QComboBox *combo)
{
	combo->addItem(QStringLiteral("auto"), QStringLiteral("auto"));
	combo->addItem(QStringLiteral("pt"), QStringLiteral("pt"));
	combo->addItem(QStringLiteral("en"), QStringLiteral("en"));
}

static QString safeReviewFileKey(const QString &videoPath)
{
	QString fileName = QFileInfo(videoPath).fileName().trimmed();
	if (fileName.isEmpty())
		fileName = videoPath.trimmed();

	if (fileName.isEmpty())
		return QStringLiteral("unknown");

	return QString::fromLatin1(
		fileName.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

static QString formatReviewTimeToken(double seconds)
{
	const qint64 milliseconds = static_cast<qint64>(std::max(0.0, std::round(seconds * 1000.0)));
	return QString::number(milliseconds);
}

static QString reviewRangeKeyFromMarkers(QVector<double> markers)
{
	std::sort(markers.begin(), markers.end());

	if (markers.isEmpty())
		return QStringLiteral("no_markers");

	QStringList values;
	values.reserve(markers.size());
	for (double marker : markers) {
		if (std::isfinite(marker))
			values.append(formatReviewTimeToken(marker));
	}

	return values.isEmpty() ? QStringLiteral("no_markers") : values.join(QStringLiteral("_"));
}

static QVector<double> markerPositionsFromRanges(const QVector<ClipDuration> &ranges)
{
	QVector<double> markers;
	markers.reserve(ranges.size() * 2);
	for (const ClipDuration &range : ranges) {
		if (!std::isfinite(range.startSec) || !std::isfinite(range.endSec) || range.endSec <= range.startSec)
			continue;
		markers.append(std::max(0.0, range.startSec));
		markers.append(std::max(0.0, range.endSec));
	}
	std::sort(markers.begin(), markers.end());
	return markers;
}

static QString reviewRangeKeyFromRanges(const QVector<ClipDuration> &ranges)
{
	if (ranges.isEmpty())
		return QStringLiteral("no_markers");

	QStringList values;
	values.reserve(ranges.size());
	for (const ClipDuration &range : ranges) {
		if (!std::isfinite(range.startSec) || !std::isfinite(range.endSec))
			continue;

		values.append(QStringLiteral("%1-%2").arg(formatReviewTimeToken(range.startSec),
							  formatReviewTimeToken(range.endSec)));
	}

	return values.isEmpty() ? QStringLiteral("no_markers") : values.join(QStringLiteral("_"));
}

static QString reviewSettingsConfigKey(const QString &videoPath, const QString &rangeKey)
{
	return QStringLiteral("%1.%2.%3")
		.arg(QString::fromLatin1(CONFIG_REVIEW_SETTINGS_PREFIX), safeReviewFileKey(videoPath), rangeKey);
}

static void setComboCurrentTextIfExists(QComboBox *combo, const QString &value)
{
	if (!combo || value.trimmed().isEmpty())
		return;

	const int index = combo->findText(value, Qt::MatchFixedString);
	if (index >= 0)
		combo->setCurrentIndex(index);
}

static void setComboCurrentDataIfExists(QComboBox *combo, const QString &value)
{
	if (!combo || value.trimmed().isEmpty())
		return;

	const int index = combo->findData(value.trimmed(), Qt::UserRole, Qt::MatchFixedString);
	if (index >= 0) {
		combo->setCurrentIndex(index);
		return;
	}

	setComboCurrentTextIfExists(combo, value);
}

static void setOpusModelOrDefault(QComboBox *combo, const QString &value = {})
{
	if (!combo)
		return;

	const QString requested = value.trimmed().isEmpty() ? QStringLiteral("ClipAnything") : value.trimmed();
	setComboCurrentTextIfExists(combo, requested);

	if (combo->currentText().trimmed().isEmpty() || combo->currentText() == QStringLiteral("ClipBasic"))
		setComboCurrentTextIfExists(combo, QStringLiteral("ClipAnything"));
}

static double reviewOverlapSec(const ClipDuration &a, const ClipDuration &b)
{
	return std::max(0.0, std::min(a.endSec, b.endSec) - std::max(a.startSec, b.startSec));
}

static double reviewUnionSec(const ClipDuration &a, const ClipDuration &b)
{
	return std::max(a.endSec, b.endSec) - std::min(a.startSec, b.startSec);
}

static double reviewRangeSimilarity(const ClipDuration &a, const ClipDuration &b)
{
	const double uni = reviewUnionSec(a, b);
	if (uni <= 0.0)
		return 0.0;
	const double iou = reviewOverlapSec(a, b) / uni;
	const double boundaryDistance = std::fabs(a.startSec - b.startSec) + std::fabs(a.endSec - b.endSec);
	const double boundaryBonus = std::max(0.0, 1.0 - (boundaryDistance / 30.0));
	return (iou * 0.78) + (boundaryBonus * 0.22);
}

static bool reviewRangeValid(const ClipDuration &range)
{
	return std::isfinite(range.startSec) && std::isfinite(range.endSec) && range.endSec > range.startSec;
}

static double reviewRangeCenter(const ClipDuration &range)
{
	return (range.startSec + range.endSec) * 0.5;
}

static bool reviewRangeAlreadyInList(const QVector<ClipDuration> &ranges, const ClipDuration &candidate,
				     double similarityThreshold = 0.88)
{
	if (!reviewRangeValid(candidate))
		return true;
	for (const ClipDuration &range : ranges) {
		if (!reviewRangeValid(range))
			continue;
		if (reviewRangeSimilarity(range, candidate) >= similarityThreshold)
			return true;
		const double boundaryDistance =
			std::fabs(range.startSec - candidate.startSec) + std::fabs(range.endSec - candidate.endSec);
		if (boundaryDistance <= 8.0)
			return true;
	}
	return false;
}

static QVector<ClipDuration> mergeReviewRangesPreservingExisting(QVector<ClipDuration> existing,
								 const QVector<ClipDuration> &suggested,
								 int *addedCount = nullptr)
{
	int added = 0;
	for (const ClipDuration &range : suggested) {
		if (!reviewRangeValid(range))
			continue;
		if (reviewRangeAlreadyInList(existing, range))
			continue;
		existing.append(range);
		++added;
	}
	std::sort(existing.begin(), existing.end(),
		  [](const ClipDuration &left, const ClipDuration &right) { return left.startSec < right.startSec; });
	if (addedCount)
		*addedCount = added;
	return existing;
}

static bool reviewRangeMatchesPersistedFeedback(const ClipDuration &range,
						const Curation::Feedback::FeedbackRangeSignal &signal)
{
	if (!reviewRangeValid(range) || !reviewRangeValid(signal.range))
		return false;
	if (reviewRangeSimilarity(range, signal.range) >= 0.94)
		return true;
	const double boundaryDistance =
		std::fabs(range.startSec - signal.range.startSec) + std::fabs(range.endSec - signal.range.endSec);
	if (boundaryDistance <= 6.0)
		return true;
	const double minDuration = std::min(range.endSec - range.startSec, signal.range.endSec - signal.range.startSec);
	return std::fabs(reviewRangeCenter(range) - reviewRangeCenter(signal.range)) <= 8.0 && minDuration > 0.0 &&
	       reviewOverlapSec(range, signal.range) >= minDuration * 0.90;
}

static QString persistedFeedbackDecisionForRange(const ClipDuration &range,
						 const Curation::Feedback::FeedbackRangeMemory &memory)
{
	if (!memory.loaded || !reviewRangeValid(range))
		return {};
	for (const Curation::Feedback::FeedbackRangeSignal &signal : memory.positiveRanges) {
		if (reviewRangeMatchesPersistedFeedback(range, signal))
			return signal.decision.trimmed().isEmpty() ? QStringLiteral("positive")
								   : signal.decision.trimmed();
	}
	for (const Curation::Feedback::FeedbackRangeSignal &signal : memory.negativeRanges) {
		if (reviewRangeMatchesPersistedFeedback(range, signal))
			return signal.decision.trimmed().isEmpty() ? QStringLiteral("negative")
								   : signal.decision.trimmed();
	}
	return {};
}

static bool reviewedPositiveSignalCanRestoreMarker(const Curation::Feedback::FeedbackRangeSignal &signal)
{
	if (!reviewRangeValid(signal.range))
		return false;
	const QString decision = signal.decision.trimmed().toLower();
	if (decision == QStringLiteral("accepted") || decision == QStringLiteral("added_by_user") ||
	    decision == QStringLiteral("approved_adjusted"))
		return true;
	if (decision == QStringLiteral("adjusted") &&
	    signal.reason.contains(QStringLiteral("complete_clip"), Qt::CaseInsensitive))
		return true;
	return false;
}

static bool betterReviewedPositiveRestoreSignal(const Curation::Feedback::FeedbackRangeSignal &candidate,
						const Curation::Feedback::FeedbackRangeSignal &current)
{
	const auto priority = [](const Curation::Feedback::FeedbackRangeSignal &signal) {
		const QString decision = signal.decision.trimmed().toLower();
		if (decision == QStringLiteral("approved_adjusted"))
			return 5;
		if (decision == QStringLiteral("accepted"))
			return 4;
		if (decision == QStringLiteral("added_by_user"))
			return 3;
		if (decision == QStringLiteral("adjusted"))
			return 2;
		return 1;
	};
	const int candidatePriority = priority(candidate);
	const int currentPriority = priority(current);
	if (candidatePriority != currentPriority)
		return candidatePriority > currentPriority;
	if (candidate.weight != current.weight)
		return candidate.weight > current.weight;
	return candidate.sequence > current.sequence;
}

static QVector<ClipDuration>
restoredReviewedPositiveRangesFromMemory(const Curation::Feedback::FeedbackRangeMemory &memory, int maxRanges = 32)
{
	QVector<Curation::Feedback::FeedbackRangeSignal> representatives;
	for (const Curation::Feedback::FeedbackRangeSignal &signal : memory.positiveRanges) {
		if (!reviewedPositiveSignalCanRestoreMarker(signal))
			continue;
		bool merged = false;
		for (Curation::Feedback::FeedbackRangeSignal &representative : representatives) {
			if (!reviewRangeMatchesPersistedFeedback(signal.range, representative))
				continue;
			if (betterReviewedPositiveRestoreSignal(signal, representative))
				representative = signal;
			merged = true;
			break;
		}
		if (!merged)
			representatives.append(signal);
	}

	std::sort(representatives.begin(), representatives.end(),
		  [](const Curation::Feedback::FeedbackRangeSignal &left,
		     const Curation::Feedback::FeedbackRangeSignal &right) {
			  if (left.sequence != right.sequence)
				  return left.sequence > right.sequence;
			  return left.range.startSec < right.range.startSec;
		  });

	QVector<ClipDuration> restored;
	for (const Curation::Feedback::FeedbackRangeSignal &signal : std::as_const(representatives)) {
		if (maxRanges > 0 && restored.size() >= maxRanges)
			break;
		if (reviewRangeAlreadyInList(restored, signal.range, 0.86))
			continue;
		restored.append(signal.range);
	}
	std::sort(restored.begin(), restored.end(),
		  [](const ClipDuration &left, const ClipDuration &right) { return left.startSec < right.startSec; });
	return restored;
}

static QString reviewDecisionDisplayLabel(const QString &decision)
{
	const QString normalized = decision.trimmed().toLower();
	if (normalized == QStringLiteral("liked") || normalized == QStringLiteral("accepted"))
		return QStringLiteral("Reviewed: positive");
	if (normalized == QStringLiteral("approved_adjusted"))
		return QStringLiteral("Reviewed: good edited");
	if (normalized == QStringLiteral("adjusted"))
		return QStringLiteral("Reviewed: adjusted");
	if (normalized == QStringLiteral("disliked") || normalized == QStringLiteral("rejected"))
		return QStringLiteral("Reviewed: rejected");
	if (normalized == QStringLiteral("ignored_diagnostic"))
		return QStringLiteral("Reviewed: ignored");
	if (!normalized.isEmpty())
		return QStringLiteral("Reviewed: %1").arg(normalized.left(24));
	return QStringLiteral("Reviewed");
}

static double totalRangeDurationSeconds(const QVector<ClipDuration> &ranges)
{
	double totalSeconds = 0.0;
	for (const ClipDuration &range : ranges)
		totalSeconds += std::max(0.0, range.endSec - range.startSec);

	return totalSeconds;
}
static QString formatEditableReviewTime(double seconds)
{
	seconds = std::max(0.0, seconds);
	const int wholeSeconds = static_cast<int>(std::floor(seconds));
	const int centiseconds = static_cast<int>(std::round((seconds - wholeSeconds) * 100.0));
	const int normalizedCentiseconds = centiseconds >= 100 ? 0 : centiseconds;
	const int adjustedWholeSeconds = centiseconds >= 100 ? wholeSeconds + 1 : wholeSeconds;
	const int hours = adjustedWholeSeconds / 3600;
	const int minutes = (adjustedWholeSeconds % 3600) / 60;
	const int secs = adjustedWholeSeconds % 60;

	const QString suffix = normalizedCentiseconds > 0
				       ? QStringLiteral(".%1").arg(normalizedCentiseconds, 2, 10, QLatin1Char('0'))
				       : QString{};
	if (hours > 0)
		return QStringLiteral("%1:%2:%3%4")
			.arg(hours)
			.arg(minutes, 2, 10, QLatin1Char('0'))
			.arg(secs, 2, 10, QLatin1Char('0'))
			.arg(suffix);

	return QStringLiteral("%1:%2%3").arg(minutes).arg(secs, 2, 10, QLatin1Char('0')).arg(suffix);
}

static bool parseEditableReviewTime(QString value, double *outSeconds)
{
	value = value.trimmed().toLower();
	if (value.isEmpty() || !outSeconds)
		return false;

	value.replace(QStringLiteral(","), QStringLiteral("."));
	value.replace(QStringLiteral(" "), QString{});
	value.remove(QStringLiteral("sec"));
	value.remove(QStringLiteral("secs"));
	value.remove(QStringLiteral("seg"));
	value.remove(QStringLiteral("s"));

	bool ok = false;
	if (value.contains(QStringLiteral(":"))) {
		const QStringList parts = value.split(QStringLiteral(":"), Qt::SkipEmptyParts);
		if (parts.isEmpty() || parts.size() > 3)
			return false;

		double multiplier = 1.0;
		double seconds = 0.0;
		for (int i = parts.size() - 1; i >= 0; --i) {
			const double part = parts.at(i).toDouble(&ok);
			if (!ok || part < 0.0)
				return false;
			seconds += part * multiplier;
			multiplier *= 60.0;
		}
		*outSeconds = seconds;
		return std::isfinite(seconds);
	}

	if (value.contains(QStringLiteral("m"))) {
		const QStringList parts = value.split(QStringLiteral("m"), Qt::KeepEmptyParts);
		if (parts.size() != 2)
			return false;
		const double minutes = parts.at(0).isEmpty() ? 0.0 : parts.at(0).toDouble(&ok);
		if (!ok || minutes < 0.0)
			return false;
		const double seconds = parts.at(1).isEmpty() ? 0.0 : parts.at(1).toDouble(&ok);
		if (!ok || seconds < 0.0)
			return false;
		*outSeconds = (minutes * 60.0) + seconds;
		return std::isfinite(*outSeconds);
	}

	const double seconds = value.toDouble(&ok);
	if (!ok || !std::isfinite(seconds) || seconds < 0.0)
		return false;

	*outSeconds = seconds;
	return true;
}

static bool jsonReviewTimeToSeconds(const QJsonValue &value, double *outSeconds)
{
	if (!outSeconds)
		return false;

	if (value.isDouble()) {
		const double seconds = value.toDouble();
		if (!std::isfinite(seconds) || seconds < 0.0)
			return false;
		*outSeconds = seconds;
		return true;
	}

	if (value.isString())
		return parseEditableReviewTime(value.toString(), outSeconds);

	return false;
}

static bool appendReviewRangeIfValid(QVector<ClipDuration> *ranges, double startSec, double endSec,
				     double durationSec = 0.0)
{
	if (!ranges || !std::isfinite(startSec) || !std::isfinite(endSec))
		return false;

	startSec = std::max(0.0, startSec);
	endSec = std::max(0.0, endSec);
	if (durationSec > 0.0) {
		startSec = std::min(startSec, durationSec);
		endSec = std::min(endSec, durationSec);
	}

	if (endSec <= startSec)
		return false;

	ranges->append({startSec, endSec});
	return true;
}

static bool parseReviewRangeObject(const QJsonObject &object, QVector<ClipDuration> *ranges, double durationSec = 0.0)
{
	double startSec = 0.0;
	double endSec = 0.0;
	const QJsonValue startValue = object.value(QStringLiteral("start_sec")).isUndefined()
					      ? object.value(QStringLiteral("start"))
					      : object.value(QStringLiteral("start_sec"));
	const QJsonValue endValue = object.value(QStringLiteral("end_sec")).isUndefined()
					    ? object.value(QStringLiteral("end"))
					    : object.value(QStringLiteral("end_sec"));

	if (!jsonReviewTimeToSeconds(startValue, &startSec) || !jsonReviewTimeToSeconds(endValue, &endSec))
		return false;

	return appendReviewRangeIfValid(ranges, startSec, endSec, durationSec);
}

static QVector<ClipDuration> reviewRangesFromMarkerSeconds(QVector<double> markers, double durationSec = 0.0)
{
	QVector<ClipDuration> ranges;
	std::sort(markers.begin(), markers.end());
	ranges.reserve((markers.size() + 1) / 2);

	for (int i = 0; i < markers.size(); i += 2) {
		const double startSec = markers.at(i);
		const double endSec = i + 1 < markers.size() ? markers.at(i + 1) : startSec + 90.0;
		appendReviewRangeIfValid(&ranges, startSec, endSec, durationSec);
	}
	return ranges;
}

static QVector<ClipDuration> parseReviewRangesFromJson(const QJsonDocument &document, double durationSec = 0.0)
{
	QVector<ClipDuration> ranges;
	QVector<double> markers;

	auto readRangeArray = [&ranges, durationSec](const QJsonArray &array) {
		for (const QJsonValue &value : array) {
			if (value.isObject())
				parseReviewRangeObject(value.toObject(), &ranges, durationSec);
		}
	};

	auto readMarkerArray = [&markers](const QJsonArray &array) {
		for (const QJsonValue &value : array) {
			double marker = 0.0;
			if (jsonReviewTimeToSeconds(value, &marker))
				markers.append(marker);
		}
	};

	if (document.isArray()) {
		const QJsonArray array = document.array();
		bool hasObject = false;
		for (const QJsonValue &value : array) {
			if (value.isObject()) {
				hasObject = true;
				break;
			}
		}
		if (hasObject)
			readRangeArray(array);
		else
			readMarkerArray(array);
	} else if (document.isObject()) {
		const QJsonObject object = document.object();
		if (object.value(QStringLiteral("ranges")).isArray())
			readRangeArray(object.value(QStringLiteral("ranges")).toArray());
		if (object.value(QStringLiteral("clip_durations")).isArray())
			readRangeArray(object.value(QStringLiteral("clip_durations")).toArray());
		if (object.value(QStringLiteral("markers_sec")).isArray())
			readMarkerArray(object.value(QStringLiteral("markers_sec")).toArray());
		if (object.value(QStringLiteral("markers")).isArray())
			readMarkerArray(object.value(QStringLiteral("markers")).toArray());
	}

	if (!ranges.isEmpty())
		return ranges;

	return reviewRangesFromMarkerSeconds(markers, durationSec);
}

static QVector<ClipDuration> parseReviewRangesFromPlainText(const QString &text, double durationSec = 0.0)
{
	QVector<ClipDuration> ranges;
	QVector<double> markers;
	QString normalized = text;
	normalized.replace(QStringLiteral("\r"), QStringLiteral("\n"));
	normalized.replace(QStringLiteral(";"), QStringLiteral("\n"));
	normalized.replace(QStringLiteral(","), QStringLiteral("\n"));

	for (QString line : normalized.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
		line = line.trimmed();
		if (line.isEmpty())
			continue;

		QString separator;
		if (line.contains(QStringLiteral("->")))
			separator = QStringLiteral("->");
		else if (line.contains(QStringLiteral("-")))
			separator = QStringLiteral("-");

		if (!separator.isEmpty()) {
			const QStringList parts = line.split(separator, Qt::SkipEmptyParts);
			if (parts.size() == 2) {
				double startSec = 0.0;
				double endSec = 0.0;
				if (parseEditableReviewTime(parts.at(0), &startSec) &&
				    parseEditableReviewTime(parts.at(1), &endSec)) {
					appendReviewRangeIfValid(&ranges, startSec, endSec, durationSec);
					continue;
				}
			}
		}

		double marker = 0.0;
		if (parseEditableReviewTime(line, &marker))
			markers.append(marker);
	}

	if (!ranges.isEmpty())
		return ranges;

	return reviewRangesFromMarkerSeconds(markers, durationSec);
}

static QJsonArray reviewStringListToJson(const QStringList &items, int maxItems = 80)
{
	QJsonArray array;
	const int limit = std::min(static_cast<int>(items.size()), maxItems);
	for (int i = 0; i < limit; ++i)
		array.append(items.at(i).left(300));
	return array;
}

static QJsonObject reviewScoresToJson(const Curation::Scoring::ClipCandidateScores &scores)
{
	QJsonObject object;
	object.insert(QStringLiteral("duration"), scores.duration);
	object.insert(QStringLiteral("boundary"), scores.boundary);
	object.insert(QStringLiteral("hook"), scores.hook);
	object.insert(QStringLiteral("emotional"), scores.emotional);
	object.insert(QStringLiteral("advice"), scores.advice);
	object.insert(QStringLiteral("explanation"), scores.explanation);
	object.insert(QStringLiteral("story"), scores.story);
	object.insert(QStringLiteral("opinion"), scores.opinion);
	object.insert(QStringLiteral("tutorial"), scores.tutorial);
	object.insert(QStringLiteral("viewerResponse"), scores.viewerResponse);
	object.insert(QStringLiteral("semanticTarget"), scores.semanticTarget);
	object.insert(QStringLiteral("semanticViewerMessage"), scores.semanticViewerMessage);
	object.insert(QStringLiteral("semanticDirectAnswer"), scores.semanticDirectAnswer);
	object.insert(QStringLiteral("semanticTopicShift"), scores.semanticTopicShift);
	object.insert(QStringLiteral("semanticClipValue"), scores.semanticClipValue);
	object.insert(QStringLiteral("semanticEmpathy"), scores.semanticEmpathy);
	object.insert(QStringLiteral("semanticHook"), scores.semanticHook);
	object.insert(QStringLiteral("semanticResolution"), scores.semanticResolution);
	object.insert(QStringLiteral("semanticMetaNoise"), scores.semanticMetaNoise);
	object.insert(QStringLiteral("semanticOpeningHook"), scores.semanticOpeningHook);
	object.insert(QStringLiteral("semanticOpeningMetaNoise"), scores.semanticOpeningMetaNoise);
	object.insert(QStringLiteral("semanticEndingResolution"), scores.semanticEndingResolution);
	object.insert(QStringLiteral("semanticEndingMetaNoise"), scores.semanticEndingMetaNoise);
	object.insert(QStringLiteral("semanticEndingTopicShift"), scores.semanticEndingTopicShift);
	object.insert(QStringLiteral("topicContinuity"), scores.topicContinuity);
	object.insert(QStringLiteral("arcOpening"), scores.arcOpening);
	object.insert(QStringLiteral("arcDevelopment"), scores.arcDevelopment);
	object.insert(QStringLiteral("arcConclusion"), scores.arcConclusion);
	object.insert(QStringLiteral("arcBoundaryCleanliness"), scores.arcBoundaryCleanliness);
	object.insert(QStringLiteral("arcTailRisk"), scores.arcTailRisk);
	object.insert(QStringLiteral("arcCompleteness"), scores.arcCompleteness);
	object.insert(QStringLiteral("pauseBeforeSec"), scores.pauseBeforeSec);
	object.insert(QStringLiteral("pauseAfterSec"), scores.pauseAfterSec);
	object.insert(QStringLiteral("maxInternalPauseSec"), scores.maxInternalPauseSec);
	object.insert(QStringLiteral("final"), scores.final);
	return object;
}

static QString reviewEvidenceValue(const QStringList &evidence, const QString &prefix)
{
	for (const QString &item : evidence) {
		if (item.startsWith(prefix))
			return item.mid(prefix.size()).trimmed();
	}
	return {};
}



} // namespace ReviewDialogUtils
