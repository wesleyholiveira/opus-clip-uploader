#include "ui/upload-review-dialog.hpp"

#include "ui/review/review-clip-table-widget.hpp"
#include "ui/review/diagnostics-dialogs.hpp"

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

#include "ui/advanced-settings-tree.hpp"
#include "ui/review-scoring-preparation.hpp"
#include "ui/ui-common.hpp"
#include "ui/video-marker-editor.hpp"
#include "curation/curation-preset.hpp"
#include "curation/scoring/cheap-clip-scorer.hpp"
#include "curation/scoring/exchange-arc-boundary-refiner.hpp"
#include "curation/scoring/transcript-index.hpp"
#include "transcription/transcript-store.hpp"
#include "utils/config.hpp"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QGroupBox>
#include <QKeyEvent>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QMetaObject>
#include <QMessageBox>
#include <QModelIndex>
#include <QMenu>
#include <QLineEdit>
#include <QProgressDialog>
#include <QPointer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QItemSelectionModel>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <set>
#include <cmath>
#include <functional>
#include <optional>
#include <limits>
#include <utility>

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

static bool isDefaultNoMarkerPlaceholderRange(const VideoMarkerEditor *videoEditor, const QVector<ClipDuration> &ranges)
{
	if (!videoEditor || videoEditor->hasExplicitClipMarkers() || ranges.size() != 1)
		return false;

	const qint64 durationMs = videoEditor->durationMilliseconds();
	if (durationMs <= 0)
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

UploadReviewDialog::UploadReviewDialog(const QString &videoPath, QWidget *parent)
	: UploadReviewDialog(videoPath, CurationSettings{}, false, parent)
{
}

UploadReviewDialog::UploadReviewDialog(const QString &videoPath, const CurationSettings &initialCurationSettings,
				       bool showAdvancedSettingsOnly, QWidget *parent)
	: QDialog(parent),
	  videoPath(videoPath),
	  initialSettings(initialCurationSettings),
	  advancedSettingsOnly(showAdvancedSettingsOnly)
{
	setWindowTitle(QString("Clip Cropper - %1").arg(obsText("Dialog.ReviewVideoTitle")));
	resize(920, advancedSettingsOnly ? 560 : 720);

	auto *mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(14, 14, 14, 12);
	mainLayout->setSpacing(8);

	if (!advancedSettingsOnly) {
		videoEditor = new VideoMarkerEditor(videoPath, this);

		clipTable = new ReviewClipTableWidget(this);
		clipTable->setColumnCount(4);
		clipTable->setHorizontalHeaderLabels({obsText("Table.Marker"), QStringLiteral("Range Start"),
						      QStringLiteral("Range End"), QStringLiteral("Feedback")});
		clipTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
		clipTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
		clipTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
		clipTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
		clipTable->setMinimumHeight(145);
		clipTable->setSelectionBehavior(QAbstractItemView::SelectRows);
		clipTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
		clipTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
		clipTable->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
		clipTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

		creditEstimateLabel = new QLabel(this);
		creditEstimateLabel->setWordWrap(true);
		creditEstimateLabel->setToolTip(obsText("Tooltip.OpusCreditsSelectedEstimate"));
		updateCreditEstimate(videoEditor->clipRanges());

		connect(videoEditor, &VideoMarkerEditor::rangesChanged, this,
			[this](const QVector<ClipDuration> &ranges) {
				boundaryFeedbackSaved = false;
				if (finishReviewButton) {
					finishReviewButton->setEnabled(true);
					finishReviewButton->setText(QStringLiteral("Finish review (save feedback)"));
				}
				Q_UNUSED(ranges);
				refreshClipTable(videoEditor ? videoEditor->clipRanges() : QVector<ClipDuration>{});
			});
		connect(clipTable, &QTableWidget::currentCellChanged, this, [this](int currentRow, int, int, int) {
			if (!videoEditor || currentRow < 0)
				return;
			if (isDiagnosticTableRow(currentRow)) {
				ClipDuration range;
				if (diagnosticRangeForTableRow(currentRow, &range))
					videoEditor->seekToSeconds(range.startSec);
				return;
			}
			videoEditor->selectRange(currentRow);
		});
		connect(clipTable, &QTableWidget::cellClicked, this, [this](int row, int column) {
			if (!videoEditor || row < 0)
				return;
			if (isDiagnosticTableRow(row)) {
				if (column == 2)
					seekReviewTableRowEnd(row);
				else
					seekReviewTableRowStart(row);
				return;
			}
			videoEditor->selectRange(row);
			videoEditor->goToSelectedRange();
		});
		connect(clipTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int column) {
			if (!videoEditor || row < 0)
				return;
			if (isDiagnosticTableRow(row)) {
				if (column == 0)
					showMarkerDiagnostics(row);
				return;
			}
			videoEditor->selectRange(row);
			if (column != 1 && column != 2)
				videoEditor->goToSelectedRange();
		});
		connect(clipTable, &QTableWidget::itemChanged, this, &UploadReviewDialog::handleClipTableItemChanged);
		connect(clipTable, &QTableWidget::itemSelectionChanged, this,
			[this]() { updateDiagnosticModeControls(); });

		if (auto *reviewTable = dynamic_cast<ReviewClipTableWidget *>(clipTable)) {
			const QPointer<UploadReviewDialog> self(this);
			reviewTable->isDiagnosticRow = [self](int row) {
				return self && self->isDiagnosticTableRow(row);
			};
			reviewTable->removeSelectedRows = [self]() {
				if (self)
					self->removeSelectedRangeWithoutFeedbackDecision();
			};
			reviewTable->nudgeSelectedRange = [self](double deltaSec) {
				if (self)
					self->nudgeSelectedRangeOrBoundary(deltaSec);
			};
			reviewTable->showMarkerDiagnostics = [self](int row) {
				if (self)
					self->showMarkerDiagnostics(row);
			};
			reviewTable->approveMarker = [self](int row) {
				if (self)
					self->setClipReviewDecision(row, QStringLiteral("liked"));
			};
			reviewTable->rejectMarker = [self](int row) {
				if (self)
					self->setClipReviewDecision(row, QStringLiteral("disliked"));
			};
			reviewTable->markMarkerAdjusted = [self](int row) {
				if (self)
					self->setClipReviewDecision(row, QStringLiteral("adjusted"));
			};
			reviewTable->approveEditedMarker = [self](int row) {
				if (self)
					self->setClipReviewDecision(row, QStringLiteral("approved_adjusted"));
			};
			reviewTable->ignoreMarker = [self](int row) {
				if (self)
					self->setClipReviewDecision(row, QStringLiteral("ignored_diagnostic"));
			};
			reviewTable->seekMarkerStart = [self](int row) {
				if (self)
					self->seekReviewTableRowStart(row);
			};
			reviewTable->seekMarkerEnd = [self](int row) {
				if (self)
					self->seekReviewTableRowEnd(row);
			};
		}
	}

	topicKeywordsInput = new QLineEdit(this);
	topicKeywordsInput->setPlaceholderText(obsText("Placeholder.TopicKeywords"));

	opusPromptInput = new QPlainTextEdit(this);
	opusPromptInput->setMinimumHeight(advancedSettingsOnly ? 260 : 96);
	opusPromptInput->setPlaceholderText(obsText("Placeholder.OpusPrompt"));

	modelInput = new QComboBox(this);
	modelInput->addItems(QStringList{"ClipBasic", "ClipAnything"});
	setOpusModelOrDefault(modelInput);

	clipLengthInput = new QComboBox(this);
	clipLengthInput->addItem(obsText("Option.ClipLengthAuto"), QStringLiteral("Auto"));
	clipLengthInput->addItem(obsText("Option.ClipLengthShort"), QStringLiteral("Short"));
	clipLengthInput->addItem(obsText("Option.ClipLengthMedium"), QStringLiteral("Medium"));
	clipLengthInput->addItem(obsText("Option.ClipLengthLong"), QStringLiteral("Long"));
	setComboCurrentDataIfExists(clipLengthInput, QStringLiteral("Medium"));

	sourceLanguageInput = new QComboBox(this);
	addLanguageOptions(sourceLanguageInput);
	setComboCurrentDataIfExists(sourceLanguageInput, normalizeLanguageSetting(PluginConfig::getValue(
								 QString::fromLatin1(CONFIG_OPUS_SOURCE_LANGUAGE),
								 QString::fromLatin1(LANGUAGE_AUTO))));

	transcriptionLanguageInput = new QComboBox(this);
	addLanguageOptions(transcriptionLanguageInput);
	setComboCurrentDataIfExists(transcriptionLanguageInput, QString::fromLatin1(LANGUAGE_AUTO));

	genreInput = new QComboBox(this);
	genreInput->addItems(QStringList{"Auto", "Q&A", "Commentary", "Marketing", "Webinar", "Motivational speech",
					 "Podcast", "Academic", "Listicle", "Product reviews", "How-to", "Comedy",
					 "Sports commentary", "Church", "News", "Vlog", "Gaming", "Others"});

	curationPresetInput = new QComboBox(this);
	for (const auto &option : CurationPreset::options())
		curationPresetInput->addItem(option.second, option.first);
	setComboCurrentDataIfExists(curationPresetInput, QStringLiteral("auto"));

	skipCurateInput = new QCheckBox(obsText("Label.SkipCurate"), this);

	if (!advancedSettingsOnly) {
		suggestClipRangesButton = new QPushButton(obsText("Button.SuggestBestClips"), this);
		connect(suggestClipRangesButton, &QPushButton::clicked, this,
			&UploadReviewDialog::requestSemanticClipSuggestions);

		finishReviewButton = new QPushButton(QStringLiteral("Finish review (save feedback)"), this);
		finishReviewButton->setToolTip(
			QStringLiteral("Persist like/dislike and marker adjustments without starting upload."));
		connect(finishReviewButton, &QPushButton::clicked, this, &UploadReviewDialog::finishReviewFeedback);
	}

	QHBoxLayout *languageRow = nullptr;
	if (!advancedSettingsOnly) {
		languageRow = new QHBoxLayout();
		languageRow->setContentsMargins(0, 0, 0, 0);
		languageRow->setSpacing(8);
		languageRow->addWidget(new QLabel(obsText("Settings.SourceLanguage"), this));
		languageRow->addWidget(sourceLanguageInput);
		languageRow->addSpacing(16);
		languageRow->addWidget(new QLabel(obsText("Settings.TranscriptionLanguage"), this));
		languageRow->addWidget(transcriptionLanguageInput);
		if (suggestClipRangesButton) {
			languageRow->addSpacing(16);
			languageRow->addWidget(suggestClipRangesButton);
		}
		languageRow->addStretch();
	}

	auto *advancedTree = new AdvancedSettingsTree(this);
	if (advancedSettingsOnly) {
		advancedTree->addField(obsText("Settings.SourceLanguage"), sourceLanguageInput);
		advancedTree->addField(obsText("Settings.TranscriptionLanguage"), transcriptionLanguageInput);
	}
	advancedTree->addField(obsText("Settings.Model"), modelInput);
	advancedTree->addField(obsText("Settings.ClipLength"), clipLengthInput);
	advancedTree->addField(obsText("Settings.TopicKeywords"), topicKeywordsInput);
	advancedTree->addField(obsText("Settings.OpusPrompt"), opusPromptInput);
	advancedTree->addField(obsText("Settings.Genre"), genreInput);
	advancedTree->addField(obsText("Settings.CurationPreset"), curationPresetInput);
	advancedTree->addField(obsText("Settings.SkipCurate"), skipCurateInput);

	if (advancedSettingsOnly) {
		applyCurationSettings(initialSettings);
		if (advancedTree->rootItem())
			advancedTree->rootItem()->setExpanded(true);
		advancedTree->setMinimumHeight(360);
	} else {
		loadSavedCurationOptions();
		if (videoEditor && initialSettings.clipDurations.isEmpty())
			restoreReviewedPositiveMarkersFromFeedbackIfEmpty(
				QStringLiteral("dialog_open_no_saved_markers"));
		if (videoEditor && !initialSettings.clipDurations.isEmpty()) {
			videoEditor->setMarkerPositions(markerPositionsFromRanges(initialSettings.clipDurations));
			refreshClipTable(videoEditor->clipRanges());
			captureInitialSemanticSuggestion();
			blog(LOG_INFO, "Applied semantic review suggestion ranges in review dialog. video=%s ranges=%d",
			     videoPath.toUtf8().constData(), static_cast<int>(initialSettings.clipDurations.size()));
		} else if (videoEditor) {
			refreshClipTable(videoEditor->clipRanges());
			ensureReviewFeedbackSnapshot(QStringLiteral("existing_review_markers"));
		}
	}

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	if (auto *okButton = buttons->button(QDialogButtonBox::Ok))
		okButton->setText(obsText("Button.ConfirmAndUpload"));
	connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
		saveCurationOptions();
		saveBoundaryFeedback(QStringLiteral("review_accepted"));
		accept();
	});
	connect(buttons, &QDialogButtonBox::rejected, this, [this]() {
		// Cancel closes the review without teaching the scorer. Explicit marker feedback is
		// saved through Finish review; treating a dialog cancel as rejection pollutes
		// calibration and can make rejected/accepted ranges ambiguous.
		reject();
	});

	mainLayout->addWidget(new QLabel(QFileInfo(videoPath).fileName(), this));
	if (languageRow)
		mainLayout->addLayout(languageRow);

	if (advancedSettingsOnly) {
		mainLayout->addWidget(advancedTree, 1);
	} else {
		mainLayout->addWidget(videoEditor, 1);
		mainLayout->addWidget(new QLabel(obsText("Label.Markers"), this));
		mainLayout->addWidget(clipTable);
		auto *reviewNudgeRow = new QHBoxLayout();
		reviewNudgeRow->setContentsMargins(0, 0, 0, 0);
		reviewNudgeRow->setSpacing(6);
		auto *nudgeBackButton = new QPushButton(QStringLiteral("−1s selected"), this);
		nudgeBackButton->setToolTip(QStringLiteral(
			"Nudge the selected marker row or selected Start/End cell back by 1 second. Shortcut: Ctrl+Alt+,"));
		connect(nudgeBackButton, &QPushButton::clicked, this, [this]() { nudgeSelectedRangeOrBoundary(-1.0); });
		auto *nudgeForwardButton = new QPushButton(QStringLiteral("+1s selected"), this);
		nudgeForwardButton->setToolTip(QStringLiteral(
			"Nudge the selected marker row or selected Start/End cell forward by 1 second. Shortcut: Ctrl+Alt+."));
		connect(nudgeForwardButton, &QPushButton::clicked, this,
			[this]() { nudgeSelectedRangeOrBoundary(1.0); });
		auto *importMarkersButton = new QPushButton(QStringLiteral("Import markers..."), this);
		importMarkersButton->setToolTip(
			QStringLiteral("Import review markers from JSON, JSON array, or plain text/CSV time ranges."));
		connect(importMarkersButton, &QPushButton::clicked, this, &UploadReviewDialog::importReviewMarkers);
		auto *exportMarkersButton = new QPushButton(QStringLiteral("Export markers..."), this);
		exportMarkersButton->setToolTip(
			QStringLiteral("Export the current review markers as JSON with ranges and marker positions."));
		connect(exportMarkersButton, &QPushButton::clicked, this, &UploadReviewDialog::exportReviewMarkers);
		reviewNudgeRow->addWidget(nudgeBackButton);
		reviewNudgeRow->addWidget(nudgeForwardButton);
		reviewNudgeRow->addSpacing(12);
		reviewNudgeRow->addWidget(importMarkersButton);
		reviewNudgeRow->addWidget(exportMarkersButton);
		reviewNudgeRow->addStretch(1);
		mainLayout->addLayout(reviewNudgeRow);
		if (finishReviewButton)
			mainLayout->addWidget(finishReviewButton);
		mainLayout->addWidget(creditEstimateLabel);
		mainLayout->addWidget(advancedTree);
	}

	mainLayout->addWidget(buttons);
}

void UploadReviewDialog::refreshClipTable(const QVector<ClipDuration> &ranges)
{
	if (!clipTable || !videoEditor)
		return;

	const int previousRow = clipTable->currentRow();
	updatingClipTable = true;
	clipTable->setRowCount(0);

	Curation::Feedback::FeedbackRangeMemory persistedReviewMemory;
	{
		const CurationSettings settings = curationSettings();
		QString presetId = settings.curationPreset.trimmed();
		if (presetId.isEmpty())
			presetId = QStringLiteral("viewer_message_response");
		QStringList contentIds;
		const QString fileContentId = Curation::Feedback::CurationFeedbackStore::fileContentId(videoPath);
		if (!fileContentId.isEmpty())
			contentIds.append(fileContentId);
		persistedReviewMemory = Curation::Feedback::CurationFeedbackStore::loadRangeMemoryForVideo(
			videoPath, presetId, contentIds);
	}

	for (int i = 0; i < ranges.size(); ++i) {
		const auto &range = ranges[i];
		const double selectedSec = std::max(0.0, range.endSec - range.startSec);
		const int row = clipTable->rowCount();
		clipTable->insertRow(row);
		auto *markerItem = new QTableWidgetItem(QString("#%1  %2 → %3  (%4s)")
								.arg(i + 1)
								.arg(videoEditor->formatTimecode(range.startSec),
								     videoEditor->formatTimecode(range.endSec))
								.arg(selectedSec, 0, 'f', 0));
		markerItem->setFlags(markerItem->flags() & ~Qt::ItemIsEditable);
		markerItem->setData(REVIEW_ROW_KIND_ROLE, REVIEW_ROW_KIND_MARKER);
		clipTable->setItem(row, 0, markerItem);
		auto *startItem = new QTableWidgetItem(formatEditableReviewTime(range.startSec));
		startItem->setToolTip(QStringLiteral(
			"Double-click to type time as seconds, mm:ss, hh:mm:ss, or 1m30. Ctrl+Alt+, / Ctrl+Alt+. nudges the selected cell by 1s."));
		startItem->setData(Qt::UserRole, range.startSec);
		startItem->setFlags(startItem->flags() | Qt::ItemIsEditable);
		auto *endItem = new QTableWidgetItem(formatEditableReviewTime(range.endSec));
		endItem->setToolTip(QStringLiteral(
			"Double-click to type time as seconds, mm:ss, hh:mm:ss, or 1m30. Ctrl+Alt+, / Ctrl+Alt+. nudges the selected cell by 1s."));
		endItem->setData(Qt::UserRole, range.endSec);
		endItem->setFlags(endItem->flags() | Qt::ItemIsEditable);
		clipTable->setItem(row, 1, startItem);
		clipTable->setItem(row, 2, endItem);

		const int suggestedIndex = suggestedIndexForRange(range);
		const QString decision = suggestedIndex >= 0 ? explicitReviewDecisions.value(suggestedIndex)
							     : QString{};
		auto *feedbackWidget = new QWidget(clipTable);
		auto *feedbackLayout = new QHBoxLayout(feedbackWidget);
		feedbackLayout->setContentsMargins(2, 0, 2, 0);
		feedbackLayout->setSpacing(4);
		const QString persistedDecision =
			decision.trimmed().isEmpty() ? persistedFeedbackDecisionForRange(range, persistedReviewMemory)
						     : QString{};
		const bool alreadyReviewed = !decision.trimmed().isEmpty() || !persistedDecision.trimmed().isEmpty();
		if (alreadyReviewed) {
			startItem->setFlags(startItem->flags() & ~Qt::ItemIsEditable);
			endItem->setFlags(endItem->flags() & ~Qt::ItemIsEditable);
			startItem->setToolTip(QStringLiteral(
				"This reviewed marker is locked. Remove it from the list or create a new marker instead of re-editing/re-rating it."));
			endItem->setToolTip(startItem->toolTip());
			const QString displayDecision = !decision.trimmed().isEmpty() ? decision : persistedDecision;
			auto *reviewedLabel = new QLabel(reviewDecisionDisplayLabel(displayDecision), feedbackWidget);
			reviewedLabel->setToolTip(QStringLiteral(
				"This range already has feedback saved or pending. It is locked to avoid re-evaluating and reinforcing the same clip twice."));
			feedbackLayout->addWidget(reviewedLabel);
		} else {
			auto *likeButton = new QPushButton(QStringLiteral("👍"), feedbackWidget);
			auto *dislikeButton = new QPushButton(QStringLiteral("👎"), feedbackWidget);
			auto *adjustedButton = new QPushButton(QStringLiteral("✎"), feedbackWidget);
			auto *approvedAdjustedButton = new QPushButton(QStringLiteral("⭐"), feedbackWidget);
			auto *ignoreButton = new QPushButton(QStringLiteral("∅"), feedbackWidget);
			likeButton->setFocusPolicy(Qt::NoFocus);
			dislikeButton->setFocusPolicy(Qt::NoFocus);
			adjustedButton->setFocusPolicy(Qt::NoFocus);
			approvedAdjustedButton->setFocusPolicy(Qt::NoFocus);
			ignoreButton->setFocusPolicy(Qt::NoFocus);
			likeButton->setToolTip(QStringLiteral(
				"Approve this candidate as-is and describe its structure. This becomes a positive training signal."));
			dislikeButton->setToolTip(QStringLiteral(
				"Reject this candidate, describe why, and remove it from upload ranges. Use only for truly bad patterns."));
			adjustedButton->setToolTip(
				QStringLiteral("Mark this edited candidate as recoverable boundary feedback."));
			approvedAdjustedButton->setToolTip(QStringLiteral(
				"Edit start/end first, then save the corrected marker as a good semantic clip example."));
			ignoreButton->setToolTip(QStringLiteral(
				"Ignore this candidate for dataset/training. It suppresses near-duplicates without becoming a strong negative."));
			connect(likeButton, &QPushButton::clicked, this,
				[this, row]() { setClipReviewDecision(row, QStringLiteral("liked")); });
			connect(dislikeButton, &QPushButton::clicked, this,
				[this, row]() { setClipReviewDecision(row, QStringLiteral("disliked")); });
			connect(adjustedButton, &QPushButton::clicked, this,
				[this, row]() { setClipReviewDecision(row, QStringLiteral("adjusted")); });
			connect(approvedAdjustedButton, &QPushButton::clicked, this,
				[this, row]() { setClipReviewDecision(row, QStringLiteral("approved_adjusted")); });
			connect(ignoreButton, &QPushButton::clicked, this,
				[this, row]() { setClipReviewDecision(row, QStringLiteral("ignored_diagnostic")); });
			feedbackLayout->addWidget(likeButton);
			feedbackLayout->addWidget(dislikeButton);
			feedbackLayout->addWidget(adjustedButton);
			feedbackLayout->addWidget(approvedAdjustedButton);
			feedbackLayout->addWidget(ignoreButton);
		}
		clipTable->setCellWidget(row, 3, feedbackWidget);
	}

	int visibleSupplementalDiagnostics = 0;
	for (int i = 0; i < lastSemanticSuggestion.candidateDiagnostics.size(); ++i) {
		const QJsonValue value = lastSemanticSuggestion.candidateDiagnostics.at(i);
		if (!value.isObject())
			continue;
		const QJsonObject diagnostic = value.toObject();
		if (!isReviewDiagnosticCandidateObject(diagnostic))
			continue;

		ClipDuration originalRange;
		if (!diagnosticRangeFromObject(diagnostic, &originalRange))
			continue;

		const ClipDuration range = diagnosticEditedRanges.contains(i)
						   ? normalizedDiagnosticRange(diagnosticEditedRanges.value(i))
						   : normalizedDiagnosticRange(originalRange);
		const bool rangeEdited = diagnosticRangeMeaningfullyEdited(originalRange, range);

		const int row = clipTable->rowCount();
		clipTable->insertRow(row);
		const double selectedSec = std::max(0.0, range.endSec - range.startSec);
		const QString reason = diagnosticReasonLabel(diagnostic);
		const double finalScore = diagnostic.value(QStringLiteral("final_score")).toDouble(0.0);
		const QString state = diagnosticReviewState(i);
		const QString stateLabel = diagnosticStateLabel(state);

		auto *markerItem =
			new QTableWidgetItem(QStringLiteral("Diagnostic #%1  [%2%3]  %4 → %5  (%6s)  %7  final=%8")
						     .arg(visibleSupplementalDiagnostics + 1)
						     .arg(stateLabel)
						     .arg(rangeEdited ? QStringLiteral(" / range edited") : QString{})
						     .arg(videoEditor->formatTimecode(range.startSec),
							  videoEditor->formatTimecode(range.endSec))
						     .arg(selectedSec, 0, 'f', 0)
						     .arg(reason.left(42))
						     .arg(finalScore, 0, 'f', 2));
		markerItem->setToolTip(QStringLiteral(
			"Supplemental rejected/diagnostic candidate kept for feedback. Double-click this description to open the full diagnostic."));
		markerItem->setFlags(markerItem->flags() & ~Qt::ItemIsEditable);
		markerItem->setData(REVIEW_ROW_KIND_ROLE, REVIEW_ROW_KIND_DIAGNOSTIC);
		markerItem->setData(REVIEW_DIAGNOSTIC_INDEX_ROLE, i);
		clipTable->setItem(row, 0, markerItem);

		auto *startItem = new QTableWidgetItem(formatEditableReviewTime(range.startSec));
		startItem->setToolTip(QStringLiteral(
			"Double-click to edit this diagnostic candidate start time. This does not change the preserved scores/evidence."));
		startItem->setFlags(startItem->flags() | Qt::ItemIsEditable);
		startItem->setData(REVIEW_ROW_KIND_ROLE, REVIEW_ROW_KIND_DIAGNOSTIC);
		startItem->setData(REVIEW_DIAGNOSTIC_INDEX_ROLE, i);
		clipTable->setItem(row, 1, startItem);

		auto *endItem = new QTableWidgetItem(formatEditableReviewTime(range.endSec));
		endItem->setToolTip(QStringLiteral(
			"Double-click to edit this diagnostic candidate end time. This does not change the preserved scores/evidence."));
		endItem->setFlags(endItem->flags() | Qt::ItemIsEditable);
		endItem->setData(REVIEW_ROW_KIND_ROLE, REVIEW_ROW_KIND_DIAGNOSTIC);
		endItem->setData(REVIEW_DIAGNOSTIC_INDEX_ROLE, i);
		clipTable->setItem(row, 2, endItem);

		auto *feedbackWidget = new QWidget(clipTable);
		auto *feedbackLayout = new QHBoxLayout(feedbackWidget);
		feedbackLayout->setContentsMargins(2, 0, 2, 0);
		feedbackLayout->setSpacing(4);
		const QString persistedDecision =
			state.trimmed().isEmpty() ? persistedFeedbackDecisionForRange(range, persistedReviewMemory)
						  : QString{};
		const bool alreadyReviewed = !state.trimmed().isEmpty() || !persistedDecision.trimmed().isEmpty();
		if (alreadyReviewed) {
			startItem->setFlags(startItem->flags() & ~Qt::ItemIsEditable);
			endItem->setFlags(endItem->flags() & ~Qt::ItemIsEditable);
			startItem->setToolTip(QStringLiteral(
				"This reviewed diagnostic/probe is locked. It cannot be edited or rated again in this session."));
			endItem->setToolTip(startItem->toolTip());
			const QString displayDecision = !state.trimmed().isEmpty() ? state : persistedDecision;
			auto *reviewedLabel = new QLabel(reviewDecisionDisplayLabel(displayDecision), feedbackWidget);
			reviewedLabel->setToolTip(QStringLiteral(
				"This diagnostic/probe already has feedback saved or pending. It is locked to avoid asking for feedback twice."));
			feedbackLayout->addWidget(reviewedLabel);
		} else {
			auto *likeButton = new QPushButton(QStringLiteral("👍"), feedbackWidget);
			auto *dislikeButton = new QPushButton(QStringLiteral("👎"), feedbackWidget);
			auto *adjustedButton = new QPushButton(QStringLiteral("✎"), feedbackWidget);
			auto *approvedAdjustedButton = new QPushButton(QStringLiteral("⭐"), feedbackWidget);
			auto *ignoreButton = new QPushButton(QStringLiteral("∅"), feedbackWidget);
			likeButton->setFocusPolicy(Qt::NoFocus);
			dislikeButton->setFocusPolicy(Qt::NoFocus);
			adjustedButton->setFocusPolicy(Qt::NoFocus);
			approvedAdjustedButton->setFocusPolicy(Qt::NoFocus);
			ignoreButton->setFocusPolicy(Qt::NoFocus);
			likeButton->setToolTip(
				QStringLiteral("Approve this diagnostic candidate as-is and describe its structure."));
			dislikeButton->setToolTip(QStringLiteral("Reject this diagnostic candidate and describe why."));
			adjustedButton->setToolTip(QStringLiteral(
				"Edit start/end first, then mark this diagnostic as adjusted boundary feedback."));
			approvedAdjustedButton->setToolTip(QStringLiteral(
				"Edit start/end first, then save the corrected range as a good semantic clip example."));
			ignoreButton->setToolTip(QStringLiteral(
				"Ignore this exploration diagnostic for training. It only suppresses near-duplicates lightly and does not become a strong negative."));
			connect(likeButton, &QPushButton::clicked, this,
				[this, row]() { setDiagnosticReviewDecision(row, QStringLiteral("liked")); });
			connect(dislikeButton, &QPushButton::clicked, this,
				[this, row]() { setDiagnosticReviewDecision(row, QStringLiteral("disliked")); });
			connect(adjustedButton, &QPushButton::clicked, this,
				[this, row]() { setDiagnosticReviewDecision(row, QStringLiteral("adjusted")); });
			connect(approvedAdjustedButton, &QPushButton::clicked, this, [this, row]() {
				setDiagnosticReviewDecision(row, QStringLiteral("approved_adjusted"));
			});
			connect(ignoreButton, &QPushButton::clicked, this, [this, row]() {
				setDiagnosticReviewDecision(row, QStringLiteral("ignored_diagnostic"));
			});
			feedbackLayout->addWidget(likeButton);
			feedbackLayout->addWidget(dislikeButton);
			feedbackLayout->addWidget(adjustedButton);
			feedbackLayout->addWidget(approvedAdjustedButton);
			feedbackLayout->addWidget(ignoreButton);
		}
		clipTable->setCellWidget(row, 3, feedbackWidget);
		++visibleSupplementalDiagnostics;
	}
	if (visibleSupplementalDiagnostics > 0) {
		blog(LOG_INFO,
		     "Displayed supplemental semantic diagnostic candidate rows alongside applied markers. video=%s diagnostics=%d",
		     videoPath.toUtf8().constData(), visibleSupplementalDiagnostics);
	}

	updatingClipTable = false;
	updateCreditEstimate(ranges);

	if (clipTable->rowCount() <= 0)
		return;

	const int rowToSelect = std::clamp(previousRow, 0, clipTable->rowCount() - 1);
	clipTable->selectRow(rowToSelect);
	if (isDiagnosticTableRow(rowToSelect)) {
		ClipDuration diagnosticRange;
		if (diagnosticRangeForTableRow(rowToSelect, &diagnosticRange))
			videoEditor->seekToSeconds(diagnosticRange.startSec);
	} else {
		videoEditor->selectRange(rowToSelect);
	}
	updateSuggestClipRangesButtonState();
	updateDiagnosticModeControls();
}

int UploadReviewDialog::diagnosticIndexForTableRow(int row) const
{
	if (!clipTable || row < 0 || row >= clipTable->rowCount())
		return -1;
	const QTableWidgetItem *item = clipTable->item(row, 0);
	if (!item ||
	    tableItemIntDataOr(item, REVIEW_ROW_KIND_ROLE, REVIEW_ROW_KIND_MARKER) != REVIEW_ROW_KIND_DIAGNOSTIC)
		return -1;
	return tableItemIntDataOr(item, REVIEW_DIAGNOSTIC_INDEX_ROLE, -1);
}

QString UploadReviewDialog::diagnosticReviewState(int diagnosticIndex) const
{
	return diagnosticReviewStates.value(diagnosticIndex).trimmed().toLower();
}

bool UploadReviewDialog::hasReviewDiagnosticCandidates() const
{
	for (const QJsonValue &value : lastSemanticSuggestion.candidateDiagnostics) {
		if (value.isObject() && isReviewDiagnosticCandidateObject(value.toObject()))
			return true;
	}
	return false;
}

QVector<int> UploadReviewDialog::selectedDiagnosticTableRows() const
{
	QVector<int> rows;
	if (!clipTable || !clipTable->selectionModel())
		return rows;
	const QModelIndexList selectedRows = clipTable->selectionModel()->selectedRows();
	rows.reserve(selectedRows.size());
	for (const QModelIndex &index : selectedRows) {
		if (!index.isValid())
			continue;
		const int row = index.row();
		if (isDiagnosticTableRow(row))
			rows.append(row);
	}
	std::sort(rows.begin(), rows.end());
	rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
	return rows;
}

void UploadReviewDialog::updateDiagnosticModeControls() {}

void UploadReviewDialog::updateSuggestClipRangesButtonState()
{
	if (!suggestClipRangesButton)
		return;
	if (semanticSuggestionInProgress) {
		suggestClipRangesButton->setEnabled(false);
		suggestClipRangesButton->setText(obsText("Status.SuggestingBestClips"));
		suggestClipRangesButton->setToolTip(QString{});
		updateDiagnosticModeControls();
		return;
	}
	suggestClipRangesButton->setEnabled(true);
	suggestClipRangesButton->setText(obsText("Button.SuggestBestClips"));
	suggestClipRangesButton->setToolTip(QString{});
	updateDiagnosticModeControls();
}

void UploadReviewDialog::showDiagnosticCandidateTable()
{
	if (!hasReviewDiagnosticCandidates())
		return;
	diagnosticReviewMode = false;
	if (videoEditor)
		refreshClipTable(videoEditor->clipRanges());
}

void UploadReviewDialog::showReviewMarkerTable()
{
	diagnosticReviewMode = false;
	if (videoEditor)
		refreshClipTable(videoEditor->clipRanges());
	updateSuggestClipRangesButtonState();
}

void UploadReviewDialog::refreshDiagnosticCandidateTable()
{
	if (videoEditor) {
		diagnosticReviewMode = false;
		refreshClipTable(videoEditor->clipRanges());
	}
	return;
}

bool UploadReviewDialog::isDiagnosticTableRow(int row) const
{
	if (!clipTable || row < 0 || row >= clipTable->rowCount())
		return false;
	const QTableWidgetItem *item = clipTable->item(row, 0);
	return item &&
	       tableItemIntDataOr(item, REVIEW_ROW_KIND_ROLE, REVIEW_ROW_KIND_MARKER) == REVIEW_ROW_KIND_DIAGNOSTIC;
}

bool UploadReviewDialog::diagnosticRangeForTableRow(int row, ClipDuration *outRange, QJsonObject *outDiagnostic) const
{
	if (!outRange || !clipTable || row < 0 || row >= clipTable->rowCount())
		return false;
	const QTableWidgetItem *item = clipTable->item(row, 0);
	if (!item ||
	    tableItemIntDataOr(item, REVIEW_ROW_KIND_ROLE, REVIEW_ROW_KIND_MARKER) != REVIEW_ROW_KIND_DIAGNOSTIC)
		return false;
	const int diagnosticIndex = tableItemIntDataOr(item, REVIEW_DIAGNOSTIC_INDEX_ROLE, -1);
	if (diagnosticIndex < 0 || diagnosticIndex >= lastSemanticSuggestion.candidateDiagnostics.size())
		return false;
	const QJsonValue value = lastSemanticSuggestion.candidateDiagnostics.at(diagnosticIndex);
	if (!value.isObject())
		return false;
	const QJsonObject diagnostic = value.toObject();
	ClipDuration range;
	if (!diagnosticRangeFromObject(diagnostic, &range))
		return false;
	if (diagnosticEditedRanges.contains(diagnosticIndex))
		range = diagnosticEditedRanges.value(diagnosticIndex);
	*outRange = normalizedDiagnosticRange(range);
	if (outDiagnostic)
		*outDiagnostic = diagnostic;
	return true;
}

void UploadReviewDialog::seekReviewTableRowStart(int row)
{
	if (!videoEditor || row < 0)
		return;
	if (isDiagnosticTableRow(row)) {
		ClipDuration range;
		if (diagnosticRangeForTableRow(row, &range))
			videoEditor->seekToSeconds(range.startSec);
		return;
	}
	const QVector<ClipDuration> ranges = videoEditor->clipRanges();
	if (row >= 0 && row < ranges.size()) {
		videoEditor->selectRange(row);
		videoEditor->seekToSeconds(ranges.at(row).startSec);
	}
}

void UploadReviewDialog::seekReviewTableRowEnd(int row)
{
	if (!videoEditor || row < 0)
		return;
	if (isDiagnosticTableRow(row)) {
		ClipDuration range;
		if (diagnosticRangeForTableRow(row, &range))
			videoEditor->seekToSeconds(range.endSec);
		return;
	}
	const QVector<ClipDuration> ranges = videoEditor->clipRanges();
	if (row >= 0 && row < ranges.size()) {
		videoEditor->selectRange(row);
		videoEditor->seekToSeconds(ranges.at(row).endSec);
	}
}

bool UploadReviewDialog::eventFilter(QObject *watched, QEvent *event)
{
	return QDialog::eventFilter(watched, event);
}

void UploadReviewDialog::handleClipTableItemChanged(QTableWidgetItem *item)
{
	if (updatingClipTable || !item || !clipTable)
		return;

	const int row = item->row();
	const int column = item->column();
	if (column != 1 && column != 2)
		return;

	const QString value = item->text();
	const bool diagnosticRow = isDiagnosticTableRow(row);
	QTimer::singleShot(0, this, [guard = QPointer<UploadReviewDialog>(this), row, column, value, diagnosticRow]() {
		if (!guard || !guard->clipTable || guard->updatingClipTable || row < 0 ||
		    row >= guard->clipTable->rowCount())
			return;
		if (diagnosticRow)
			guard->applyEditedDiagnosticRangeTime(row, column, value);
		else
			guard->applyEditedRangeTime(row, column, value);
	});
}

ClipDuration UploadReviewDialog::normalizedDiagnosticRange(const ClipDuration &range) const
{
	ClipDuration normalized = range;
	const double durationSec = videoEditor && videoEditor->durationMilliseconds() > 0
					   ? videoEditor->durationMilliseconds() / 1000.0
					   : 0.0;
	normalized.startSec = std::max(0.0, normalized.startSec);
	normalized.endSec = std::max(0.0, normalized.endSec);
	if (durationSec > 0.0) {
		normalized.startSec = std::min(normalized.startSec, durationSec);
		normalized.endSec = std::min(normalized.endSec, durationSec);
	}
	static constexpr double MinRangeDurationSec = 0.25;
	if (normalized.endSec <= normalized.startSec) {
		if (durationSec > 0.0 && normalized.startSec + MinRangeDurationSec > durationSec)
			normalized.startSec = std::max(0.0, durationSec - MinRangeDurationSec);
		normalized.endSec =
			std::min(durationSec > 0.0 ? durationSec : normalized.startSec + MinRangeDurationSec,
				 normalized.startSec + MinRangeDurationSec);
	}
	return normalized;
}

void UploadReviewDialog::applyEditedDiagnosticRangeTime(int row, int column, const QString &rawValue)
{
	if (!videoEditor || row < 0 || (column != 1 && column != 2))
		return;

	const int diagnosticIndex = diagnosticIndexForTableRow(row);
	ClipDuration range;
	QJsonObject diagnostic;
	if (diagnosticIndex < 0 || !diagnosticRangeForTableRow(row, &range, &diagnostic))
		return;
	if (!diagnosticReviewState(diagnosticIndex).trimmed().isEmpty()) {
		blog(LOG_INFO,
		     "Ignored edit for already-reviewed diagnostic candidate in current session. video=%s row=%d diagnosticIndex=%d",
		     videoPath.toUtf8().constData(), row, diagnosticIndex);
		refreshDiagnosticCandidateTable();
		return;
	}

	double seconds = 0.0;
	if (!parseEditableReviewTime(rawValue, &seconds)) {
		refreshDiagnosticCandidateTable();
		return;
	}

	if (column == 1)
		range.startSec = seconds;
	else
		range.endSec = seconds;
	range = normalizedDiagnosticRange(range);
	diagnosticEditedRanges.insert(diagnosticIndex, range);
	ClipDuration originalRange;
	const bool edited = diagnosticRangeFromObject(diagnostic, &originalRange) &&
			    diagnosticRangeMeaningfullyEdited(originalRange, range);
	const int previousSuggestedIndex = diagnosticFeedbackSuggestedIndices.value(diagnosticIndex, -1);
	if (previousSuggestedIndex >= 0 && previousSuggestedIndex < lastSemanticSuggestion.ranges.size()) {
		ensureDiagnosticFeedbackSuggestion(diagnosticIndex, range, diagnostic);
		QJsonObject details = explicitReviewFeedbackDetails.value(previousSuggestedIndex);
		if (!details.isEmpty()) {
			details.insert(QStringLiteral("range_start_sec"), range.startSec);
			details.insert(QStringLiteral("range_end_sec"), range.endSec);
			details.insert(QStringLiteral("diagnostic_range_edited"), edited);
			explicitReviewFeedbackDetails.insert(previousSuggestedIndex, details);
		}
	}

	if (column == 2)
		videoEditor->seekToSeconds(range.endSec);
	else
		videoEditor->seekToSeconds(range.startSec);

	refreshDiagnosticCandidateTable();
	if (clipTable && row >= 0 && row < clipTable->rowCount()) {
		clipTable->selectRow(row);
		clipTable->setCurrentCell(row, column);
	}

	blog(LOG_INFO, "Edited diagnostic candidate review range. video=%s row=%d diagnosticIndex=%d range=%.2f-%.2f",
	     videoPath.toUtf8().constData(), row, diagnosticIndex, range.startSec, range.endSec);
}

void UploadReviewDialog::applyEditedRangeTime(int row, int column, const QString &rawValue)
{
	if (!videoEditor || row < 0 || (column != 1 && column != 2))
		return;

	const QVector<ClipDuration> ranges = videoEditor->clipRanges();
	if (row >= ranges.size())
		return;
	{
		const CurationSettings settings = curationSettings();
		QString presetId = settings.curationPreset.trimmed();
		if (presetId.isEmpty())
			presetId = QStringLiteral("viewer_message_response");
		QStringList contentIds;
		const QString fileContentId = Curation::Feedback::CurationFeedbackStore::fileContentId(videoPath);
		if (!fileContentId.isEmpty())
			contentIds.append(fileContentId);
		const Curation::Feedback::FeedbackRangeMemory memory =
			Curation::Feedback::CurationFeedbackStore::loadRangeMemoryForVideo(videoPath, presetId,
											   contentIds);
		const QString persistedDecision = persistedFeedbackDecisionForRange(ranges.at(row), memory);
		if (!persistedDecision.trimmed().isEmpty()) {
			blog(LOG_INFO,
			     "Ignored edit for already-reviewed marker range. video=%s row=%d previous=%s range=%.2f-%.2f",
			     videoPath.toUtf8().constData(), row, persistedDecision.toUtf8().constData(),
			     ranges.at(row).startSec, ranges.at(row).endSec);
			refreshClipTable(ranges);
			return;
		}
	}

	double seconds = 0.0;
	if (!parseEditableReviewTime(rawValue, &seconds)) {
		refreshClipTable(ranges);
		return;
	}

	ClipDuration edited = ranges.at(row);
	if (column == 1)
		edited.startSec = seconds;
	else
		edited.endSec = seconds;

	setClipRangeAtIndex(row, edited.startSec, edited.endSec, true);
}

void UploadReviewDialog::setClipRangeAtIndex(int row, double startSec, double endSec, bool seekToChangedBoundary)
{
	if (!videoEditor || row < 0)
		return;

	QVector<ClipDuration> ranges = videoEditor->clipRanges();
	if (row >= ranges.size())
		return;

	const double durationSec =
		videoEditor->durationMilliseconds() > 0 ? videoEditor->durationMilliseconds() / 1000.0 : 0.0;
	startSec = std::max(0.0, startSec);
	endSec = std::max(0.0, endSec);
	if (durationSec > 0.0) {
		startSec = std::min(startSec, durationSec);
		endSec = std::min(endSec, durationSec);
	}

	static constexpr double MinRangeDurationSec = 0.25;
	if (endSec <= startSec) {
		if (durationSec > 0.0 && startSec + MinRangeDurationSec > durationSec)
			startSec = std::max(0.0, durationSec - MinRangeDurationSec);
		endSec = std::min(durationSec > 0.0 ? durationSec : startSec + MinRangeDurationSec,
				  startSec + MinRangeDurationSec);
	}

	ranges[row].startSec = startSec;
	ranges[row].endSec = endSec;

	QVector<double> markers;
	markers.reserve(ranges.size() * 2);
	for (const ClipDuration &range : ranges) {
		if (!std::isfinite(range.startSec) || !std::isfinite(range.endSec) || range.endSec <= range.startSec)
			continue;
		markers.append(range.startSec);
		markers.append(range.endSec);
	}

	boundaryFeedbackSaved = false;
	if (finishReviewButton) {
		finishReviewButton->setEnabled(true);
		finishReviewButton->setText(QStringLiteral("Finish review (save feedback)"));
	}

	videoEditor->setMarkerPositions(markers);
	videoEditor->selectRange(row);
	if (seekToChangedBoundary)
		videoEditor->seekToSeconds(clipTable && clipTable->currentColumn() == 2 ? endSec : startSec);
	if (clipTable && row < clipTable->rowCount())
		clipTable->selectRow(row);
}

void UploadReviewDialog::nudgeSelectedRangeOrBoundary(double deltaSec)
{
	if (!clipTable || !videoEditor)
		return;

	const int row = clipTable->currentRow();
	if (row < 0)
		return;

	const QVector<ClipDuration> ranges = videoEditor->clipRanges();
	if (row >= ranges.size())
		return;

	ClipDuration edited = ranges.at(row);
	const int column = clipTable->currentColumn();
	if (column == 1) {
		edited.startSec += deltaSec;
	} else if (column == 2) {
		edited.endSec += deltaSec;
	} else {
		edited.startSec += deltaSec;
		edited.endSec += deltaSec;
	}

	setClipRangeAtIndex(row, edited.startSec, edited.endSec, true);
}

void UploadReviewDialog::removeSelectedRangeWithoutFeedbackDecision()
{
	if (!clipTable || !videoEditor || clipTable->rowCount() <= 0)
		return;
	if (clipTable->currentRow() >= 0 && isDiagnosticTableRow(clipTable->currentRow()))
		return;

	QVector<int> rowsToRemove;
	if (clipTable->selectionModel()) {
		const QModelIndexList selectedRows = clipTable->selectionModel()->selectedRows();
		rowsToRemove.reserve(selectedRows.size());
		for (const QModelIndex &index : selectedRows) {
			if (index.isValid())
				rowsToRemove.append(index.row());
		}
	}

	if (rowsToRemove.isEmpty() && clipTable->currentRow() >= 0)
		rowsToRemove.append(clipTable->currentRow());

	if (rowsToRemove.isEmpty())
		return;

	std::sort(rowsToRemove.begin(), rowsToRemove.end());
	rowsToRemove.erase(std::unique(rowsToRemove.begin(), rowsToRemove.end()), rowsToRemove.end());

	ensureReviewFeedbackSnapshot(QStringLiteral("existing_review_markers"));
	const QVector<ClipDuration> ranges = videoEditor->clipRanges();
	if (ranges.isEmpty())
		return;

	QVector<ClipDuration> keptRanges;
	keptRanges.reserve(ranges.size());
	int removedCount = 0;
	for (int i = 0; i < ranges.size(); ++i) {
		const bool shouldRemove = std::binary_search(rowsToRemove.constBegin(), rowsToRemove.constEnd(), i);
		if (!shouldRemove) {
			keptRanges.append(ranges.at(i));
			continue;
		}

		const int suggestedIndex = suggestedIndexForRange(ranges.at(i));
		if (suggestedIndex >= 0) {
			explicitReviewDecisions.remove(suggestedIndex);
			explicitReviewFeedbackDetails.remove(suggestedIndex);
		}
		++removedCount;
	}

	if (removedCount <= 0)
		return;

	boundaryFeedbackSaved = false;
	if (finishReviewButton) {
		finishReviewButton->setEnabled(true);
		finishReviewButton->setText(QStringLiteral("Finish review (save feedback)"));
	}

	const int firstRemovedRow = rowsToRemove.first();
	const int nextRow = keptRanges.isEmpty() ? -1
						 : std::min(firstRemovedRow, static_cast<int>(keptRanges.size()) - 1);
	videoEditor->setMarkerPositions(markerPositionsFromRanges(keptRanges));
	if (nextRow >= 0) {
		videoEditor->selectRange(nextRow);
		if (clipTable && nextRow < clipTable->rowCount())
			clipTable->selectRow(nextRow);
	}

	blog(LOG_INFO, "Removed selected review marker rows without rating. video=%s removed=%d remaining=%d",
	     videoPath.toUtf8().constData(), removedCount, static_cast<int>(keptRanges.size()));
}

void UploadReviewDialog::exportReviewMarkers()
{
	if (!videoEditor)
		return;

	const QVector<ClipDuration> ranges = videoEditor->clipRanges();
	if (ranges.isEmpty()) {
		QMessageBox::information(this, QStringLiteral("Export markers"),
					 QStringLiteral("There are no review markers to export."));
		return;
	}

	const QString baseName = QFileInfo(videoPath).completeBaseName().trimmed().isEmpty()
					 ? QStringLiteral("clip-markers")
					 : QFileInfo(videoPath).completeBaseName().trimmed();
	const QString defaultPath =
		QFileInfo(videoPath).absoluteDir().filePath(baseName + QStringLiteral("-markers.json"));
	const QString filePath = QFileDialog::getSaveFileName(
		this, QStringLiteral("Export review markers"), defaultPath,
		QStringLiteral("Marker JSON (*.json);;Text files (*.txt);;All files (*.*)"));
	if (filePath.trimmed().isEmpty())
		return;

	QJsonArray rangesJson;
	for (int i = 0; i < ranges.size(); ++i) {
		const ClipDuration &range = ranges.at(i);
		QJsonObject item;
		item.insert(QStringLiteral("index"), i);
		item.insert(QStringLiteral("start_sec"), range.startSec);
		item.insert(QStringLiteral("end_sec"), range.endSec);
		item.insert(QStringLiteral("duration_sec"), std::max(0.0, range.endSec - range.startSec));
		item.insert(QStringLiteral("start"), formatEditableReviewTime(range.startSec));
		item.insert(QStringLiteral("end"), formatEditableReviewTime(range.endSec));
		rangesJson.append(item);
	}

	QJsonArray markersJson;
	const QVector<double> markers = videoEditor->markerPositions();
	for (double marker : markers)
		markersJson.append(marker);

	QJsonObject root;
	root.insert(QStringLiteral("schema_version"), 1);
	root.insert(QStringLiteral("type"), QStringLiteral("clip-cropper-review-markers"));
	root.insert(QStringLiteral("video_path"), videoPath);
	root.insert(QStringLiteral("video_file"), QFileInfo(videoPath).fileName());
	root.insert(QStringLiteral("exported_at_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
	root.insert(QStringLiteral("duration_sec"),
		    videoEditor->durationMilliseconds() > 0 ? videoEditor->durationMilliseconds() / 1000.0 : 0.0);
	root.insert(QStringLiteral("markers_sec"), markersJson);
	root.insert(QStringLiteral("ranges"), rangesJson);

	QFile file(filePath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
		QMessageBox::warning(this, QStringLiteral("Export markers"),
				     QStringLiteral("Could not write marker file:\n%1").arg(file.errorString()));
		return;
	}

	file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	file.close();
	blog(LOG_INFO, "Exported review markers. video=%s path=%s ranges=%d markers=%d", videoPath.toUtf8().constData(),
	     filePath.toUtf8().constData(), static_cast<int>(ranges.size()), static_cast<int>(markers.size()));
	QMessageBox::information(this, QStringLiteral("Export markers"),
				 QStringLiteral("Exported %1 marker range(s).").arg(ranges.size()));
}

void UploadReviewDialog::importReviewMarkers()
{
	if (!videoEditor)
		return;

	const QString startDir = QFileInfo(videoPath).absolutePath();
	const QString filePath =
		QFileDialog::getOpenFileName(this, QStringLiteral("Import review markers"), startDir,
					     QStringLiteral("Marker files (*.json *.txt *.csv);;All files (*.*)"));
	if (filePath.trimmed().isEmpty())
		return;

	QFile file(filePath);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		QMessageBox::warning(this, QStringLiteral("Import markers"),
				     QStringLiteral("Could not read marker file:\n%1").arg(file.errorString()));
		return;
	}

	const QByteArray data = file.readAll();
	file.close();

	QVector<ClipDuration> importedRanges;
	const double durationSec =
		videoEditor->durationMilliseconds() > 0 ? videoEditor->durationMilliseconds() / 1000.0 : 0.0;
	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
	if (parseError.error == QJsonParseError::NoError && (!document.isNull()))
		importedRanges = parseReviewRangesFromJson(document, durationSec);
	else
		importedRanges = parseReviewRangesFromPlainText(QString::fromUtf8(data), durationSec);

	if (importedRanges.isEmpty()) {
		QMessageBox::warning(this, QStringLiteral("Import markers"),
				     QStringLiteral("No valid marker ranges were found in this file."));
		return;
	}

	if (!videoEditor->clipRanges().isEmpty()) {
		const QMessageBox::StandardButton answer = QMessageBox::question(
			this, QStringLiteral("Import markers"),
			QStringLiteral("Replace the current review markers with %1 imported range(s)?")
				.arg(importedRanges.size()),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
		if (answer != QMessageBox::Yes)
			return;
	}

	applyImportedReviewRanges(importedRanges, filePath);
}

void UploadReviewDialog::applyImportedReviewRanges(const QVector<ClipDuration> &ranges, const QString &sourcePath)
{
	if (!videoEditor || ranges.isEmpty())
		return;

	lastSemanticSuggestion = Curation::Feedback::FeedbackSuggestionSnapshot{};
	explicitReviewDecisions.clear();
	explicitReviewFeedbackDetails.clear();
	boundaryFeedbackSaved = false;

	videoEditor->setMarkerPositions(markerPositionsFromRanges(ranges));
	lastSemanticSuggestion.ranges = videoEditor->clipRanges();
	lastSemanticSuggestion.contentId = Curation::Feedback::CurationFeedbackStore::fileContentId(videoPath);
	lastSemanticSuggestion.contentIdAliases.clear();
	lastSemanticSuggestion.source = QStringLiteral("imported_review_markers");
	lastSemanticSuggestion.summary = QStringLiteral("review markers imported from file");
	lastSemanticSuggestion.candidateDiagnostics = QJsonArray{};
	diagnosticReviewStates.clear();
	diagnosticReviewMode = false;

	if (finishReviewButton) {
		finishReviewButton->setEnabled(true);
		finishReviewButton->setText(QStringLiteral("Finish review (save feedback)"));
	}
	if (clipTable && clipTable->rowCount() > 0)
		clipTable->selectRow(0);
	videoEditor->selectRange(0);
	blog(LOG_INFO, "Imported review markers. video=%s path=%s ranges=%d", videoPath.toUtf8().constData(),
	     sourcePath.toUtf8().constData(), static_cast<int>(lastSemanticSuggestion.ranges.size()));
}

void UploadReviewDialog::applyCurationSettings(const CurationSettings &settings)
{
	if (topicKeywordsInput)
		topicKeywordsInput->setText(settings.topicKeywords.join(QStringLiteral(", ")));

	setComboCurrentTextIfExists(genreInput, settings.genre);
	setComboCurrentDataIfExists(curationPresetInput, CurationPreset::normalizeId(settings.curationPreset));
	setOpusModelOrDefault(modelInput, settings.model);
	setComboCurrentDataIfExists(clipLengthInput, settings.clipLengthPreset);
	setComboCurrentDataIfExists(sourceLanguageInput, normalizeLanguageSetting(settings.sourceLanguage));
	setComboCurrentDataIfExists(transcriptionLanguageInput,
				    normalizeLanguageSetting(settings.transcriptionLanguage));

	if (skipCurateInput)
		skipCurateInput->setChecked(settings.skipCurate);

	if (opusPromptInput)
		opusPromptInput->setPlainText(settings.aiPrompt.trimmed());
}

void UploadReviewDialog::showSemanticSuggestionProgressDialog()
{
	closeSemanticSuggestionProgressDialog();

	auto *progress = new QProgressDialog(obsText("Status.SuggestClipsCheckingSettings"), QString(), 0, 100, this);
	semanticSuggestionProgressDialog = progress;
	progress->setWindowTitle(obsText("Dialog.SuggestingBestClipsTitle"));
	progress->setWindowModality(Qt::WindowModal);
	progress->setMinimumDuration(0);
	progress->setAutoClose(false);
	progress->setAutoReset(false);
	progress->setValue(0);
	progress->setAttribute(Qt::WA_DeleteOnClose, false);
	progress->setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::CustomizeWindowHint |
				 Qt::MSWindowsFixedSizeDialogHint | Qt::WindowStaysOnTopHint);

	connect(progress, &QObject::destroyed, this, [this, progress]() {
		if (semanticSuggestionProgressDialog == progress)
			semanticSuggestionProgressDialog = nullptr;
	});

	progress->show();
	progress->raise();
	progress->activateWindow();
	QTimer::singleShot(0, progress, [safeProgress = QPointer<QProgressDialog>(progress)]() {
		if (!safeProgress)
			return;
		safeProgress->show();
		safeProgress->raise();
		safeProgress->activateWindow();
	});
}

void UploadReviewDialog::updateSemanticSuggestionProgress(const ReviewScoringProgressUpdate &progressUpdate)
{
	if (QThread::currentThread() != thread()) {
		QPointer<UploadReviewDialog> safeThis(this);
		QMetaObject::invokeMethod(
			this,
			[safeThis, progressUpdate]() {
				if (!safeThis)
					return;
				safeThis->updateSemanticSuggestionProgress(progressUpdate);
			},
			Qt::QueuedConnection);
		return;
	}

	QPointer<QProgressDialog> progress = semanticSuggestionProgressDialog;
	if (!progress)
		return;

	const int maximum = std::max(0, progressUpdate.maximum);
	const int value = std::clamp(progressUpdate.value, 0, maximum);
	progress->setRange(0, maximum);
	if (!progressUpdate.message.isEmpty())
		progress->setLabelText(progressUpdate.message);
	progress->setValue(value);
	if (!progress->isVisible())
		progress->show();
}

void UploadReviewDialog::closeSemanticSuggestionProgressDialog()
{
	if (!semanticSuggestionProgressDialog)
		return;

	QProgressDialog *progress = semanticSuggestionProgressDialog;
	semanticSuggestionProgressDialog = nullptr;
	progress->close();
	progress->deleteLater();
}

void UploadReviewDialog::requestSemanticClipSuggestions()
{
	if (advancedSettingsOnly || !videoEditor || semanticSuggestionInProgress)
		return;

	if (!boundaryFeedbackSaved && !explicitReviewDecisions.isEmpty())
		saveBoundaryFeedback(QStringLiteral("before_new_semantic_suggestions"));

	const CurationSettings reviewSettings = curationSettings();
	semanticSuggestionInProgress = true;
	const int suggestionGeneration = ++semanticSuggestionProgressGeneration;
	showSemanticSuggestionProgressDialog();
	if (suggestClipRangesButton) {
		suggestClipRangesButton->setEnabled(false);
		suggestClipRangesButton->setText(obsText("Status.SuggestingBestClips"));
	}

	blog(LOG_INFO,
	     "Starting semantic review suggestions from review dialog. video=%s sourceLanguage=%s transcriptionLanguage=%s",
	     videoPath.toUtf8().constData(), reviewSettings.sourceLanguage.toUtf8().constData(),
	     reviewSettings.transcriptionLanguage.toUtf8().constData());

	QPointer<UploadReviewDialog> safeThis(this);
	prepare_review_scoring_async(
		this, videoPath, reviewSettings,
		[safeThis, suggestionGeneration](ReviewScoringPreparationResult result) mutable {
			if (!safeThis || safeThis->semanticSuggestionProgressGeneration != suggestionGeneration)
				return;

			safeThis->semanticSuggestionInProgress = false;
			++safeThis->semanticSuggestionProgressGeneration;
			safeThis->closeSemanticSuggestionProgressDialog();
			safeThis->updateSuggestClipRangesButtonState();

			safeThis->applySemanticClipSuggestionResult(result);
		},
		[safeThis, suggestionGeneration](ReviewScoringProgressUpdate progress) mutable {
			if (!safeThis || !safeThis->semanticSuggestionInProgress ||
			    safeThis->semanticSuggestionProgressGeneration != suggestionGeneration)
				return;
			safeThis->updateSemanticSuggestionProgress(progress);
		});
}

void UploadReviewDialog::applySemanticClipSuggestionResult(const ReviewScoringPreparationResult &result)
{
	if (result.attempted) {
		blog(LOG_INFO,
		     "Semantic review suggestions returned to review dialog. video=%s applied=%s canceled=%s summary=%s",
		     videoPath.toUtf8().constData(), result.applied ? "true" : "false",
		     result.canceled ? "true" : "false", result.summary.toUtf8().constData());
	}

	if (!videoEditor)
		return;

	if (!result.applied) {
		restoreReviewedPositiveMarkersFromFeedbackIfEmpty(
			QStringLiteral("diagnostics_only_result_preserve_reviewed_markers"));
		lastSemanticSuggestion.ranges.clear();
		lastSemanticSuggestion.contentId = result.contentId;
		lastSemanticSuggestion.contentIdAliases = result.contentIdAliases;
		lastSemanticSuggestion.summary = result.summary;
		lastSemanticSuggestion.source = QStringLiteral("semantic_review_diagnostics_only");
		lastSemanticSuggestion.candidateDiagnostics = result.candidateDiagnostics;
		diagnosticReviewStates.clear();
		diagnosticEditedRanges.clear();
		diagnosticFeedbackSuggestedIndices.clear();
		diagnosticReviewMode = false;
		explicitReviewDecisions.clear();
		explicitReviewFeedbackDetails.clear();
		refreshClipTable(videoEditor->clipRanges());
		blog(LOG_INFO,
		     "Preserved semantic review diagnostics without applying markers. video=%s diagnostics=%d",
		     videoPath.toUtf8().constData(), static_cast<int>(result.candidateDiagnostics.size()));
		return;
	}

	QVector<ClipDuration> currentRanges = videoEditor->clipRanges();
	if (isDefaultNoMarkerPlaceholderRange(videoEditor, currentRanges))
		currentRanges.clear();
	int addedSuggestionCount = 0;
	const QVector<ClipDuration> mergedRanges = mergeReviewRangesPreservingExisting(
		currentRanges, result.settings.clipDurations, &addedSuggestionCount);

	lastSemanticSuggestion.ranges = mergedRanges;
	lastSemanticSuggestion.contentId = result.contentId;
	lastSemanticSuggestion.contentIdAliases = result.contentIdAliases;
	lastSemanticSuggestion.summary = result.summary;
	lastSemanticSuggestion.source = QStringLiteral("semantic_review_button");
	lastSemanticSuggestion.candidateDiagnostics = result.candidateDiagnostics;
	diagnosticReviewStates.clear();
	diagnosticEditedRanges.clear();
	diagnosticFeedbackSuggestedIndices.clear();
	diagnosticReviewMode = false;
	explicitReviewDecisions.clear();
	explicitReviewFeedbackDetails.clear();
	boundaryFeedbackSaved = false;
	if (finishReviewButton) {
		finishReviewButton->setEnabled(true);
		finishReviewButton->setText(QStringLiteral("Finish review (save feedback)"));
	}
	videoEditor->setMarkerPositions(markerPositionsFromRanges(mergedRanges));
	refreshClipTable(videoEditor->clipRanges());
	blog(LOG_INFO,
	     "Merged semantic review suggestion ranges without deleting existing markers. video=%s existing=%d suggested=%d added=%d total=%d",
	     videoPath.toUtf8().constData(), static_cast<int>(currentRanges.size()),
	     static_cast<int>(result.settings.clipDurations.size()), addedSuggestionCount,
	     static_cast<int>(mergedRanges.size()));
}

bool UploadReviewDialog::restoreReviewedPositiveMarkersFromFeedbackIfEmpty(const QString &reason)
{
	if (advancedSettingsOnly || !videoEditor)
		return false;
	if (videoEditor->hasExplicitClipMarkers())
		return false;

	const CurationSettings settings = curationSettings();
	QString presetId = settings.curationPreset.trimmed();
	if (presetId.isEmpty())
		presetId = QStringLiteral("viewer_message_response");
	QStringList contentIds;
	const QString fileContentId = Curation::Feedback::CurationFeedbackStore::fileContentId(videoPath);
	if (!fileContentId.isEmpty())
		contentIds.append(fileContentId);
	const Curation::Feedback::FeedbackRangeMemory memory =
		Curation::Feedback::CurationFeedbackStore::loadRangeMemoryForVideo(videoPath, presetId, contentIds);
	const QVector<ClipDuration> restored = restoredReviewedPositiveRangesFromMemory(memory, 32);
	if (restored.isEmpty())
		return false;

	videoEditor->setMarkerPositions(markerPositionsFromRanges(restored));
	lastSemanticSuggestion.ranges = restored;
	lastSemanticSuggestion.contentId = fileContentId;
	lastSemanticSuggestion.contentIdAliases.clear();
	lastSemanticSuggestion.source = QStringLiteral("restored_reviewed_positive_markers");
	lastSemanticSuggestion.summary = QStringLiteral("reviewed positive markers restored from feedback memory");
	lastSemanticSuggestion.candidateDiagnostics = QJsonArray{};
	explicitReviewDecisions.clear();
	explicitReviewFeedbackDetails.clear();
	diagnosticReviewStates.clear();
	diagnosticEditedRanges.clear();
	diagnosticFeedbackSuggestedIndices.clear();
	diagnosticReviewMode = false;
	boundaryFeedbackSaved = false;
	restoredReviewedPositiveMarkersFromMemory = true;
	blog(LOG_INFO,
	     "Restored reviewed positive markers from feedback memory. video=%s reason=%s restored=%d positives=%d records=%d",
	     videoPath.toUtf8().constData(), reason.toUtf8().constData(), static_cast<int>(restored.size()),
	     static_cast<int>(memory.positiveRanges.size()), memory.recordsRead);
	return true;
}

void UploadReviewDialog::captureInitialSemanticSuggestion()
{
	if (advancedSettingsOnly || initialSettings.clipDurations.isEmpty())
		return;

	lastSemanticSuggestion.ranges = initialSettings.clipDurations;
	lastSemanticSuggestion.contentId = Curation::Feedback::CurationFeedbackStore::fileContentId(videoPath);
	lastSemanticSuggestion.contentIdAliases.clear();
	lastSemanticSuggestion.source = QStringLiteral("initial_review_suggestion");
	lastSemanticSuggestion.summary = QStringLiteral("initial semantic suggestion ranges");
	diagnosticReviewStates.clear();
	diagnosticEditedRanges.clear();
	diagnosticFeedbackSuggestedIndices.clear();
	diagnosticReviewMode = false;
	explicitReviewDecisions.clear();
	explicitReviewFeedbackDetails.clear();
	boundaryFeedbackSaved = false;
}

void UploadReviewDialog::ensureReviewFeedbackSnapshot(const QString &source)
{
	if (advancedSettingsOnly || !videoEditor)
		return;

	if (Curation::Feedback::CurationFeedbackStore::hasUsefulSuggestion(lastSemanticSuggestion))
		return;

	const QVector<ClipDuration> ranges = videoEditor->clipRanges();
	if (ranges.isEmpty())
		return;
	if (isDefaultNoMarkerPlaceholderRange(videoEditor, ranges)) {
		blog(LOG_INFO,
		     "Skipping baseline review feedback snapshot without explicit markers. video=%s source=%s",
		     videoPath.toUtf8().constData(), source.toUtf8().constData());
		return;
	}

	const QJsonArray preservedDiagnostics = lastSemanticSuggestion.candidateDiagnostics;
	lastSemanticSuggestion.ranges = ranges;
	lastSemanticSuggestion.contentId = Curation::Feedback::CurationFeedbackStore::fileContentId(videoPath);
	lastSemanticSuggestion.contentIdAliases.clear();
	lastSemanticSuggestion.source = source.trimmed().isEmpty() ? QStringLiteral("existing_review_markers")
								   : source.trimmed();
	lastSemanticSuggestion.summary = QStringLiteral("baseline marker list captured for feedback");
	lastSemanticSuggestion.candidateDiagnostics = preservedDiagnostics;
	explicitReviewDecisions.clear();
	explicitReviewFeedbackDetails.clear();
	boundaryFeedbackSaved = false;
	blog(LOG_INFO, "Captured baseline review markers for feedback. video=%s ranges=%d source=%s",
	     videoPath.toUtf8().constData(), static_cast<int>(ranges.size()),
	     lastSemanticSuggestion.source.toUtf8().constData());
}

int UploadReviewDialog::suggestedIndexForRange(const ClipDuration &range) const
{
	int bestIndex = -1;
	double bestScore = 0.0;
	for (int i = 0; i < lastSemanticSuggestion.ranges.size(); ++i) {
		const ClipDuration &suggested = lastSemanticSuggestion.ranges.at(i);
		if (!std::isfinite(suggested.startSec) || !std::isfinite(suggested.endSec) ||
		    suggested.endSec <= suggested.startSec)
			continue;
		const double score = reviewRangeSimilarity(range, suggested);
		if (score > bestScore) {
			bestScore = score;
			bestIndex = i;
		}
	}
	return bestScore >= 0.18 ? bestIndex : -1;
}

int UploadReviewDialog::ensureFeedbackSuggestionForRange(const ClipDuration &range, const QString &source)
{
	if (!std::isfinite(range.startSec) || !std::isfinite(range.endSec) || range.endSec <= range.startSec)
		return -1;

	ensureReviewFeedbackSnapshot(source.trimmed().isEmpty() ? QStringLiteral("existing_review_markers")
								: source.trimmed());
	if (!Curation::Feedback::CurationFeedbackStore::hasUsefulSuggestion(lastSemanticSuggestion) && videoEditor) {
		const QVector<ClipDuration> currentRanges = videoEditor->clipRanges();
		if (isDefaultNoMarkerPlaceholderRange(videoEditor, currentRanges) && currentRanges.size() == 1 &&
		    reviewRangeSimilarity(range, currentRanges.first()) >= 0.99) {
			blog(LOG_INFO,
			     "Skipping default no-marker placeholder as explicit feedback candidate. video=%s range=%.2f-%.2f source=%s",
			     videoPath.toUtf8().constData(), range.startSec, range.endSec, source.toUtf8().constData());
			return -1;
		}
	}
	const int existingIndex = suggestedIndexForRange(range);
	if (existingIndex >= 0)
		return existingIndex;

	if (lastSemanticSuggestion.contentId.trimmed().isEmpty())
		lastSemanticSuggestion.contentId = Curation::Feedback::CurationFeedbackStore::fileContentId(videoPath);
	if (lastSemanticSuggestion.source.trimmed().isEmpty())
		lastSemanticSuggestion.source = source.trimmed().isEmpty()
							? QStringLiteral("manual_or_edited_review_marker")
							: source.trimmed();
	if (lastSemanticSuggestion.summary.trimmed().isEmpty())
		lastSemanticSuggestion.summary = QStringLiteral("review markers captured for explicit feedback");

	lastSemanticSuggestion.ranges.append(range);
	const int newIndex = lastSemanticSuggestion.ranges.size() - 1;

	QJsonObject diagnostic;
	diagnostic.insert(QStringLiteral("index"), newIndex);
	diagnostic.insert(QStringLiteral("start_sec"), range.startSec);
	diagnostic.insert(QStringLiteral("end_sec"), range.endSec);
	diagnostic.insert(QStringLiteral("duration_sec"), range.endSec - range.startSec);
	diagnostic.insert(QStringLiteral("source"), source.trimmed().isEmpty()
							    ? QStringLiteral("manual_or_edited_review_marker")
							    : source.trimmed());
	diagnostic.insert(QStringLiteral("final_score"), 0.0);
	diagnostic.insert(QStringLiteral("review_status"), QStringLiteral("explicit_user_marker"));
	QJsonArray evidence;
	evidence.append(QStringLiteral("explicit_user_marker"));
	evidence.append(QStringLiteral("manual_or_heavily_edited_marker_feedback"));
	diagnostic.insert(QStringLiteral("evidence"), evidence);
	lastSemanticSuggestion.candidateDiagnostics.append(diagnostic);

	boundaryFeedbackSaved = false;
	blog(LOG_INFO,
	     "Registered review marker as explicit feedback candidate. video=%s index=%d range=%.2f-%.2f source=%s",
	     videoPath.toUtf8().constData(), newIndex, range.startSec, range.endSec,
	     diagnostic.value(QStringLiteral("source")).toString().toUtf8().constData());
	return newIndex;
}

bool UploadReviewDialog::loadFeedbackTranscriptIfAvailable()
{
	if (feedbackTranscriptLoaded)
		return !feedbackTranscript.isEmpty();

	feedbackTranscriptLoaded = true;
	const CurationSettings settings = curationSettings();
	QString language = settings.transcriptionLanguage.trimmed();
	if (language.isEmpty() || language == QStringLiteral("auto"))
		language = settings.sourceLanguage.trimmed();
	if (language.isEmpty() || language == QStringLiteral("auto"))
		language = QStringLiteral("pt");

	feedbackTranscript = TranscriptStore::loadAlignedForVideoPath(videoPath, language);
	if (feedbackTranscript.isEmpty())
		feedbackTranscript = TranscriptStore::loadForVideoPath(videoPath, language);

	if (!feedbackTranscript.isEmpty()) {
		const QString transcriptId =
			Curation::Feedback::CurationFeedbackStore::transcriptContentId(feedbackTranscript);
		const QString fileId = Curation::Feedback::CurationFeedbackStore::fileContentId(videoPath);
		if (!transcriptId.isEmpty()) {
			lastSemanticSuggestion.contentId = transcriptId;
			if (!fileId.isEmpty() && fileId != transcriptId &&
			    !lastSemanticSuggestion.contentIdAliases.contains(fileId))
				lastSemanticSuggestion.contentIdAliases.append(fileId);
		}
		blog(LOG_INFO,
		     "Loaded transcript cache for explicit marker feedback re-evaluation. video=%s segments=%d wordAligned=%s contentId=%s",
		     videoPath.toUtf8().constData(), static_cast<int>(feedbackTranscript.segments.size()),
		     feedbackTranscript.hasWordTimings() ? "true" : "false",
		     lastSemanticSuggestion.contentId.toUtf8().constData());
	}

	return !feedbackTranscript.isEmpty();
}

void UploadReviewDialog::upsertFeedbackDiagnostic(int suggestedIndex, const QJsonObject &diagnostic)
{
	if (suggestedIndex < 0 || diagnostic.isEmpty())
		return;

	QJsonObject object = diagnostic;
	object.insert(QStringLiteral("index"), suggestedIndex);

	for (int i = 0; i < lastSemanticSuggestion.candidateDiagnostics.size(); ++i) {
		const QJsonValue value = lastSemanticSuggestion.candidateDiagnostics.at(i);
		if (!value.isObject())
			continue;
		if (value.toObject().value(QStringLiteral("index")).toInt(-1) == suggestedIndex) {
			lastSemanticSuggestion.candidateDiagnostics.replace(i, object);
			return;
		}
	}

	lastSemanticSuggestion.candidateDiagnostics.append(object);
}

QJsonObject UploadReviewDialog::reevaluateFeedbackRange(const ClipDuration &range, int suggestedIndex,
							const QString &source)
{
	QJsonObject diagnostic;
	diagnostic.insert(QStringLiteral("index"), suggestedIndex);
	diagnostic.insert(QStringLiteral("start_sec"), range.startSec);
	diagnostic.insert(QStringLiteral("end_sec"), range.endSec);
	diagnostic.insert(QStringLiteral("duration_sec"), range.endSec - range.startSec);
	diagnostic.insert(QStringLiteral("source"), source.trimmed().isEmpty()
							    ? QStringLiteral("manual_or_edited_review_marker")
							    : source.trimmed());
	diagnostic.insert(QStringLiteral("review_status"), QStringLiteral("explicit_user_marker_re_evaluated"));

	QJsonArray evidence;
	evidence.append(QStringLiteral("explicit_user_marker"));
	evidence.append(QStringLiteral("feedback_range_re_evaluated_on_review_decision"));

	if (!loadFeedbackTranscriptIfAvailable()) {
		evidence.append(QStringLiteral("feedback_re_evaluation_transcript_unavailable"));
		diagnostic.insert(QStringLiteral("evidence"), evidence);
		diagnostic.insert(QStringLiteral("final_score"), 0.0);
		return diagnostic;
	}

	Curation::Scoring::TranscriptIndex index(feedbackTranscript);
	Curation::Scoring::ClipCandidate candidate;
	candidate.range = range;
	candidate.source = source.trimmed().isEmpty() ? QStringLiteral("manual_or_edited_review_marker")
						      : source.trimmed();
	candidate.evidence = QStringList{QStringLiteral("explicit_user_marker"),
					 QStringLiteral("feedback_range_re_evaluated_on_review_decision")};

	const CurationSettings settings = curationSettings();
	Curation::Scoring::CheapScoringContext context;
	context.presetId = settings.curationPreset.trimmed().isEmpty() ? QStringLiteral("viewer_message_response")
								       : settings.curationPreset.trimmed();
	context.mainTarget = settings.topicKeywords.join(QStringLiteral(", ")).trimmed();
	context.transcriptionLanguage = settings.transcriptionLanguage;
	context.sourceLanguage = settings.sourceLanguage;
	context.reliableMainTarget = !context.mainTarget.isEmpty();

	Curation::Scoring::CheapClipScorer cheapScorer;
	Curation::Scoring::ClipCandidate scored = cheapScorer.score(index, candidate, context);

	Curation::Scoring::ExchangeArcBoundaryRefinementOptions refineOptions;
	refineOptions.scoring = context;
	refineOptions.generation.minDurationSec = 8.0;
	refineOptions.generation.maxDurationSec = 180.0;
	refineOptions.generation.boundaryMinDurationSec = 8.0;
	Curation::Scoring::ExchangeArcBoundaryRefiner refiner;
	Curation::Scoring::ClipCandidate refined = refiner.refine(index, scored, refineOptions);
	refined.range = range;
	if (refined.text.trimmed().isEmpty())
		refined.text = scored.text;
	if (refined.timedText.trimmed().isEmpty())
		refined.timedText = scored.timedText;

	evidence = reviewStringListToJson(refined.evidence);
	diagnostic.insert(QStringLiteral("final_score"), refined.scores.final);
	diagnostic.insert(QStringLiteral("scores"), reviewScoresToJson(refined.scores));
	QJsonObject labels;
	labels.insert(QStringLiteral("boundary_state"),
		      reviewEvidenceValue(refined.evidence, QStringLiteral("boundary_state:")));
	labels.insert(QStringLiteral("boundary_reasons"),
		      reviewEvidenceValue(refined.evidence, QStringLiteral("boundary_reasons:")));
	labels.insert(QStringLiteral("arc_dp"),
		      reviewEvidenceValue(refined.evidence, QStringLiteral("arc_dp_boundary_refined")));
	diagnostic.insert(QStringLiteral("classifier_labels"), labels);
	diagnostic.insert(QStringLiteral("evidence"), evidence);
	diagnostic.insert(QStringLiteral("text_preview"), refined.text.left(1200));
	if (!refined.timedText.trimmed().isEmpty())
		diagnostic.insert(QStringLiteral("timed_text_preview"), refined.timedText.left(1600));
	diagnostic.insert(QStringLiteral("review_status"), QStringLiteral("explicit_user_marker_re_evaluated"));
	diagnostic.insert(QStringLiteral("re_evaluation_backend"), QStringLiteral("cheap_scorer_exchange_arc"));
	return diagnostic;
}

static double diagnosticRangeSimilarityForReview(const ClipDuration &a, const ClipDuration &b)
{
	const double overlap = std::max(0.0, std::min(a.endSec, b.endSec) - std::max(a.startSec, b.startSec));
	const double uni = std::max(a.endSec, b.endSec) - std::min(a.startSec, b.startSec);
	if (uni <= 0.0)
		return 0.0;
	const double iou = overlap / uni;
	const double boundaryDistance = std::fabs(a.startSec - b.startSec) + std::fabs(a.endSec - b.endSec);
	const double boundaryScore = std::max(0.0, 1.0 - (boundaryDistance / 30.0));
	return (iou * 0.78) + (boundaryScore * 0.22);
}

QJsonObject UploadReviewDialog::diagnosticForRange(const ClipDuration &range, int suggestedIndex) const
{
	if (suggestedIndex >= 0) {
		for (const QJsonValue &value : lastSemanticSuggestion.candidateDiagnostics) {
			if (!value.isObject())
				continue;
			const QJsonObject object = value.toObject();
			if (object.value(QStringLiteral("index")).toInt(-1) != suggestedIndex)
				continue;
			ClipDuration candidateRange;
			if (diagnosticRangeFromObject(object, &candidateRange) &&
			    diagnosticRangeSimilarityForReview(range, candidateRange) >= 0.50)
				return object;
		}
	}

	QJsonObject best;
	double bestScore = 0.0;
	for (const QJsonValue &value : lastSemanticSuggestion.candidateDiagnostics) {
		if (!value.isObject())
			continue;
		const QJsonObject object = value.toObject();
		const ClipDuration candidateRange{object.value(QStringLiteral("start_sec")).toDouble(),
						  object.value(QStringLiteral("end_sec")).toDouble()};
		if (!std::isfinite(candidateRange.startSec) || !std::isfinite(candidateRange.endSec) ||
		    candidateRange.endSec <= candidateRange.startSec)
			continue;
		const double score = diagnosticRangeSimilarityForReview(range, candidateRange);
		if (score > bestScore) {
			bestScore = score;
			best = object;
		}
	}
	return bestScore >= 0.50 ? best : QJsonObject{};
}

void UploadReviewDialog::showMarkerDiagnostics(int row)
{
	if (!videoEditor || row < 0)
		return;

	if (isDiagnosticTableRow(row)) {
		ClipDuration range;
		QJsonObject diagnostic;
		if (!diagnosticRangeForTableRow(row, &range, &diagnostic))
			return;
		ClipCropper::Review::showMarkerDiagnosticsDialog(range, diagnostic,
								 videoEditor->formatTimecode(range.startSec),
								 videoEditor->formatTimecode(range.endSec), this);
		return;
	}

	const QVector<ClipDuration> ranges = videoEditor->clipRanges();
	if (row >= ranges.size())
		return;

	const ClipDuration range = ranges.at(row);
	const int suggestedIndex = suggestedIndexForRange(range);
	const QJsonObject diagnostic = diagnosticForRange(range, suggestedIndex);
	ClipCropper::Review::showMarkerDiagnosticsDialog(range, diagnostic, videoEditor->formatTimecode(range.startSec),
							 videoEditor->formatTimecode(range.endSec), this);
}

int UploadReviewDialog::ensureDiagnosticFeedbackSuggestion(int diagnosticIndex, const ClipDuration &range,
							   const QJsonObject &diagnostic)
{
	if (diagnosticIndex < 0 || !std::isfinite(range.startSec) || !std::isfinite(range.endSec) ||
	    range.endSec <= range.startSec)
		return -1;

	ClipDuration originalRange;
	const bool hasOriginalRange = diagnosticRangeFromObject(diagnostic, &originalRange);
	const ClipDuration reviewedRange = normalizedDiagnosticRange(range);

	if (lastSemanticSuggestion.contentId.trimmed().isEmpty())
		lastSemanticSuggestion.contentId = Curation::Feedback::CurationFeedbackStore::fileContentId(videoPath);
	if (lastSemanticSuggestion.source.trimmed().isEmpty())
		lastSemanticSuggestion.source = QStringLiteral("semantic_review_diagnostics_only");
	if (lastSemanticSuggestion.summary.trimmed().isEmpty())
		lastSemanticSuggestion.summary = QStringLiteral("diagnostic candidates captured for explicit feedback");

	const auto updateTaggedDiagnostic = [&](int suggestedIndex) {
		if (suggestedIndex < 0 || suggestedIndex >= lastSemanticSuggestion.ranges.size())
			return;
		lastSemanticSuggestion.ranges[suggestedIndex] = reviewedRange;

		QJsonObject taggedDiagnostic = diagnostic;
		taggedDiagnostic.insert(QStringLiteral("index"), suggestedIndex);
		taggedDiagnostic.insert(QStringLiteral("diagnostic_feedback_source_index"), diagnosticIndex);
		taggedDiagnostic.insert(QStringLiteral("start_sec"), reviewedRange.startSec);
		taggedDiagnostic.insert(QStringLiteral("end_sec"), reviewedRange.endSec);
		taggedDiagnostic.insert(QStringLiteral("duration_sec"), reviewedRange.endSec - reviewedRange.startSec);
		if (hasOriginalRange) {
			taggedDiagnostic.insert(QStringLiteral("diagnostic_original_start_sec"),
						originalRange.startSec);
			taggedDiagnostic.insert(QStringLiteral("diagnostic_original_end_sec"), originalRange.endSec);
			const bool edited = diagnosticRangeMeaningfullyEdited(originalRange, reviewedRange);
			taggedDiagnostic.insert(QStringLiteral("diagnostic_range_edited"), edited);
		}

		for (int i = 0; i < lastSemanticSuggestion.candidateDiagnostics.size(); ++i) {
			const QJsonValue value = lastSemanticSuggestion.candidateDiagnostics.at(i);
			if (!value.isObject())
				continue;
			const QJsonObject object = value.toObject();
			if (object.value(QStringLiteral("diagnostic_feedback_source_index")).toInt(-1) ==
			    diagnosticIndex) {
				lastSemanticSuggestion.candidateDiagnostics.replace(i, taggedDiagnostic);
				return;
			}
		}
		lastSemanticSuggestion.candidateDiagnostics.append(taggedDiagnostic);
	};

	const int rememberedIndex = diagnosticFeedbackSuggestedIndices.value(diagnosticIndex, -1);
	if (rememberedIndex >= 0 && rememberedIndex < lastSemanticSuggestion.ranges.size()) {
		updateTaggedDiagnostic(rememberedIndex);
		return rememberedIndex;
	}

	for (int i = 0; i < lastSemanticSuggestion.candidateDiagnostics.size(); ++i) {
		const QJsonValue value = lastSemanticSuggestion.candidateDiagnostics.at(i);
		if (!value.isObject())
			continue;
		const QJsonObject object = value.toObject();
		if (object.value(QStringLiteral("diagnostic_feedback_source_index")).toInt(-1) != diagnosticIndex)
			continue;
		const int existingIndex = object.value(QStringLiteral("index")).toInt(-1);
		if (existingIndex >= 0 && existingIndex < lastSemanticSuggestion.ranges.size()) {
			diagnosticFeedbackSuggestedIndices.insert(diagnosticIndex, existingIndex);
			updateTaggedDiagnostic(existingIndex);
			return existingIndex;
		}
	}

	lastSemanticSuggestion.ranges.append(reviewedRange);
	const int suggestedIndex = lastSemanticSuggestion.ranges.size() - 1;
	diagnosticFeedbackSuggestedIndices.insert(diagnosticIndex, suggestedIndex);
	updateTaggedDiagnostic(suggestedIndex);
	return suggestedIndex;
}

bool UploadReviewDialog::collectStructuredFeedback(int row, const QString &decision, QJsonObject *outFeedback)
{
	if (!videoEditor || row < 0 || !outFeedback)
		return false;
	const QVector<ClipDuration> ranges = videoEditor->clipRanges();
	if (row >= ranges.size())
		return false;

	const ClipDuration range = ranges.at(row);
	const int suggestedIndex = suggestedIndexForRange(range);
	const QJsonObject diagnostic = diagnosticForRange(range, suggestedIndex);
	auto feedback = ClipCropper::Review::collectStructuredFeedbackDialog(
		decision, range, diagnostic, videoEditor->formatTimecode(range.startSec),
		videoEditor->formatTimecode(range.endSec), this);
	if (!feedback)
		return false;

	*outFeedback = std::move(*feedback);
	outFeedback->insert(QStringLiteral("range_start_sec"), range.startSec);
	outFeedback->insert(QStringLiteral("range_end_sec"), range.endSec);
	if (suggestedIndex >= 0)
		outFeedback->insert(QStringLiteral("suggested_index"), suggestedIndex);
	const QString diagnosticKind = diagnostic.value(QStringLiteral("diagnostic_kind")).toString().trimmed();
	if (!diagnosticKind.isEmpty())
		outFeedback->insert(QStringLiteral("diagnostic_kind"), diagnosticKind);
	const QString rejectionReason = diagnostic.value(QStringLiteral("rejection_reason")).toString().trimmed();
	if (!rejectionReason.isEmpty())
		outFeedback->insert(QStringLiteral("diagnostic_rejection_reason"), rejectionReason);
	return true;
}

void UploadReviewDialog::setDiagnosticReviewDecision(int row, const QString &decision)
{
	if (!videoEditor || row < 0 || !isDiagnosticTableRow(row))
		return;

	const int diagnosticIndex = diagnosticIndexForTableRow(row);
	ClipDuration range;
	QJsonObject diagnostic;
	if (diagnosticIndex < 0 || !diagnosticRangeForTableRow(row, &range, &diagnostic))
		return;

	const QString normalizedDecision = decision.trimmed().toLower();
	const QString existingState = diagnosticReviewState(diagnosticIndex).trimmed();
	if (!existingState.isEmpty()) {
		blog(LOG_INFO,
		     "Ignored duplicate diagnostic feedback decision in current review session. video=%s row=%d diagnosticIndex=%d existing=%s requested=%s range=%.2f-%.2f",
		     videoPath.toUtf8().constData(), row, diagnosticIndex, existingState.toUtf8().constData(),
		     normalizedDecision.toUtf8().constData(), range.startSec, range.endSec);
		refreshDiagnosticCandidateTable();
		return;
	}
	{
		const CurationSettings settings = curationSettings();
		QString presetId = settings.curationPreset.trimmed();
		if (presetId.isEmpty())
			presetId = QStringLiteral("viewer_message_response");
		QStringList contentIds;
		const QString fileContentId = Curation::Feedback::CurationFeedbackStore::fileContentId(videoPath);
		if (!fileContentId.isEmpty())
			contentIds.append(fileContentId);
		const Curation::Feedback::FeedbackRangeMemory memory =
			Curation::Feedback::CurationFeedbackStore::loadRangeMemoryForVideo(videoPath, presetId,
											   contentIds);
		const QString persistedDecision = persistedFeedbackDecisionForRange(range, memory);
		if (!persistedDecision.trimmed().isEmpty()) {
			blog(LOG_INFO,
			     "Ignored duplicate diagnostic feedback decision for already-reviewed range. video=%s row=%d diagnosticIndex=%d decision=%s previous=%s range=%.2f-%.2f",
			     videoPath.toUtf8().constData(), row, diagnosticIndex,
			     normalizedDecision.toUtf8().constData(), persistedDecision.toUtf8().constData(),
			     range.startSec, range.endSec);
			refreshDiagnosticCandidateTable();
			return;
		}
	}
	ClipDuration originalRange;
	const bool hasOriginalRange = diagnosticRangeFromObject(diagnostic, &originalRange);
	const bool rangeEdited = hasOriginalRange && diagnosticRangeMeaningfullyEdited(originalRange, range);
	if (normalizedDecision == QStringLiteral("ignored_diagnostic")) {
		const int suggestedIndex = ensureDiagnosticFeedbackSuggestion(diagnosticIndex, range, diagnostic);
		if (suggestedIndex < 0)
			return;

		QJsonObject structuredFeedback;
		structuredFeedback.insert(QStringLiteral("schema_version"), 1);
		structuredFeedback.insert(QStringLiteral("decision"), normalizedDecision);
		structuredFeedback.insert(QStringLiteral("ignore_for_training"), true);
		structuredFeedback.insert(QStringLiteral("weak_negative"), true);
		structuredFeedback.insert(QStringLiteral("bad_topic"), false);
		structuredFeedback.insert(QStringLiteral("boundary_recoverable"), false);
		structuredFeedback.insert(QStringLiteral("range_start_sec"), range.startSec);
		structuredFeedback.insert(QStringLiteral("range_end_sec"), range.endSec);
		structuredFeedback.insert(QStringLiteral("suggested_index"), suggestedIndex);
		structuredFeedback.insert(QStringLiteral("diagnostic_source_index"), diagnosticIndex);
		if (hasOriginalRange) {
			structuredFeedback.insert(QStringLiteral("diagnostic_original_start_sec"),
						  originalRange.startSec);
			structuredFeedback.insert(QStringLiteral("diagnostic_original_end_sec"), originalRange.endSec);
			structuredFeedback.insert(QStringLiteral("diagnostic_range_edited"), rangeEdited);
		}
		const QString diagnosticKind = diagnostic.value(QStringLiteral("diagnostic_kind")).toString().trimmed();
		if (!diagnosticKind.isEmpty())
			structuredFeedback.insert(QStringLiteral("diagnostic_kind"), diagnosticKind);
		const QString rejectionReason =
			diagnostic.value(QStringLiteral("rejection_reason")).toString().trimmed();
		if (!rejectionReason.isEmpty())
			structuredFeedback.insert(QStringLiteral("diagnostic_rejection_reason"), rejectionReason);

		explicitReviewDecisions.insert(suggestedIndex, normalizedDecision);
		explicitReviewFeedbackDetails.insert(suggestedIndex, structuredFeedback);
		diagnosticReviewStates.insert(diagnosticIndex, normalizedDecision);

		boundaryFeedbackSaved = false;
		if (finishReviewButton) {
			finishReviewButton->setEnabled(true);
			finishReviewButton->setText(QStringLiteral("Finish review (save feedback)"));
		}
		refreshDiagnosticCandidateTable();
		if (clipTable && row >= 0 && row < clipTable->rowCount())
			clipTable->selectRow(row);
		blog(LOG_INFO,
		     "Explicit diagnostic candidate ignored for training. video=%s row=%d diagnosticIndex=%d suggestionIndex=%d range=%.2f-%.2f",
		     videoPath.toUtf8().constData(), row, diagnosticIndex, suggestedIndex, range.startSec,
		     range.endSec);
		return;
	}
	if ((normalizedDecision == QStringLiteral("adjusted") ||
	     normalizedDecision == QStringLiteral("approved_adjusted")) &&
	    !rangeEdited) {
		QMessageBox::information(
			this, QStringLiteral("Adjust range first"),
			normalizedDecision == QStringLiteral("approved_adjusted")
				? QStringLiteral(
					  "Para salvar como bom clip corrigido, edite o início ou o fim do range antes. Se o range original já está bom, use aprovar.")
				: QStringLiteral(
					  "Para marcar um diagnóstico como ajustado, edite o início ou o fim do range antes. Se o range já está bom, use aprovar; se continua ruim, use rejeitar."));
		refreshDiagnosticCandidateTable();
		return;
	}

	auto feedback = ClipCropper::Review::collectStructuredFeedbackDialog(
		normalizedDecision, range, diagnostic, videoEditor->formatTimecode(range.startSec),
		videoEditor->formatTimecode(range.endSec), this);
	if (!feedback) {
		refreshDiagnosticCandidateTable();
		return;
	}

	const int suggestedIndex = ensureDiagnosticFeedbackSuggestion(diagnosticIndex, range, diagnostic);
	if (suggestedIndex < 0)
		return;

	QJsonObject structuredFeedback = std::move(*feedback);
	structuredFeedback.insert(QStringLiteral("range_start_sec"), range.startSec);
	structuredFeedback.insert(QStringLiteral("range_end_sec"), range.endSec);
	structuredFeedback.insert(QStringLiteral("suggested_index"), suggestedIndex);
	structuredFeedback.insert(QStringLiteral("diagnostic_source_index"), diagnosticIndex);
	if (hasOriginalRange) {
		structuredFeedback.insert(QStringLiteral("diagnostic_original_start_sec"), originalRange.startSec);
		structuredFeedback.insert(QStringLiteral("diagnostic_original_end_sec"), originalRange.endSec);
		structuredFeedback.insert(QStringLiteral("diagnostic_range_edited"), rangeEdited);
	}
	if (normalizedDecision == QStringLiteral("approved_adjusted")) {
		structuredFeedback.insert(QStringLiteral("approved_corrected_range"), true);
		structuredFeedback.insert(QStringLiteral("semantic_positive_example"), true);
	}
	const QString diagnosticKind = diagnostic.value(QStringLiteral("diagnostic_kind")).toString().trimmed();
	if (!diagnosticKind.isEmpty())
		structuredFeedback.insert(QStringLiteral("diagnostic_kind"), diagnosticKind);
	const QString rejectionReason = diagnostic.value(QStringLiteral("rejection_reason")).toString().trimmed();
	if (!rejectionReason.isEmpty())
		structuredFeedback.insert(QStringLiteral("diagnostic_rejection_reason"), rejectionReason);

	explicitReviewDecisions.insert(suggestedIndex, normalizedDecision);
	explicitReviewFeedbackDetails.insert(suggestedIndex, structuredFeedback);
	diagnosticReviewStates.insert(diagnosticIndex, normalizedDecision);

	boundaryFeedbackSaved = false;
	if (finishReviewButton) {
		finishReviewButton->setEnabled(true);
		finishReviewButton->setText(QStringLiteral("Finish review (save feedback)"));
	}
	refreshDiagnosticCandidateTable();
	if (clipTable && row >= 0 && row < clipTable->rowCount())
		clipTable->selectRow(row);

	blog(LOG_INFO,
	     "Explicit diagnostic candidate decision captured. video=%s row=%d diagnosticIndex=%d suggestionIndex=%d decision=%s range=%.2f-%.2f",
	     videoPath.toUtf8().constData(), row, diagnosticIndex, suggestedIndex,
	     normalizedDecision.toUtf8().constData(), range.startSec, range.endSec);
}

void UploadReviewDialog::setClipReviewDecision(int row, const QString &decision)
{
	if (!videoEditor || row < 0)
		return;
	if (isDiagnosticTableRow(row)) {
		setDiagnosticReviewDecision(row, decision);
		return;
	}
	ensureReviewFeedbackSnapshot(QStringLiteral("existing_review_markers"));
	const QVector<ClipDuration> ranges = videoEditor->clipRanges();
	if (row >= ranges.size())
		return;

	const ClipDuration reviewedRange = ranges.at(row);
	const QString normalizedDecision = decision.trimmed().toLower();
	{
		const CurationSettings settings = curationSettings();
		QString presetId = settings.curationPreset.trimmed();
		if (presetId.isEmpty())
			presetId = QStringLiteral("viewer_message_response");
		QStringList contentIds;
		const QString fileContentId = Curation::Feedback::CurationFeedbackStore::fileContentId(videoPath);
		if (!fileContentId.isEmpty())
			contentIds.append(fileContentId);
		const Curation::Feedback::FeedbackRangeMemory memory =
			Curation::Feedback::CurationFeedbackStore::loadRangeMemoryForVideo(videoPath, presetId,
											   contentIds);
		const QString persistedDecision = persistedFeedbackDecisionForRange(reviewedRange, memory);
		if (!persistedDecision.trimmed().isEmpty()) {
			blog(LOG_INFO,
			     "Ignored duplicate marker feedback decision for already-reviewed range. video=%s row=%d decision=%s previous=%s range=%.2f-%.2f",
			     videoPath.toUtf8().constData(), row, normalizedDecision.toUtf8().constData(),
			     persistedDecision.toUtf8().constData(), reviewedRange.startSec, reviewedRange.endSec);
			refreshClipTable(ranges);
			return;
		}
	}
	const int suggestedIndex =
		ensureFeedbackSuggestionForRange(reviewedRange, QStringLiteral("manual_or_edited_review_marker"));
	if (suggestedIndex < 0) {
		blog(LOG_WARNING, "Could not register marker row for feedback. video=%s row=%d decision=%s",
		     videoPath.toUtf8().constData(), row, normalizedDecision.toUtf8().constData());
		refreshClipTable(ranges);
		return;
	}

	if (explicitReviewDecisions.contains(suggestedIndex)) {
		blog(LOG_INFO,
		     "Ignored duplicate marker feedback decision in current review session. video=%s row=%d suggestionIndex=%d existing=%s requested=%s",
		     videoPath.toUtf8().constData(), row, suggestedIndex,
		     explicitReviewDecisions.value(suggestedIndex).toUtf8().constData(),
		     normalizedDecision.toUtf8().constData());
		refreshClipTable(ranges);
		return;
	}

	QJsonObject structuredFeedback;
	if (normalizedDecision == QStringLiteral("ignored_diagnostic")) {
		structuredFeedback.insert(QStringLiteral("schema_version"), 1);
		structuredFeedback.insert(QStringLiteral("decision"), normalizedDecision);
		structuredFeedback.insert(QStringLiteral("ignore_for_training"), true);
		structuredFeedback.insert(QStringLiteral("weak_negative"), true);
		structuredFeedback.insert(QStringLiteral("bad_topic"), false);
		structuredFeedback.insert(QStringLiteral("boundary_recoverable"), false);
		structuredFeedback.insert(QStringLiteral("range_start_sec"), reviewedRange.startSec);
		structuredFeedback.insert(QStringLiteral("range_end_sec"), reviewedRange.endSec);
		structuredFeedback.insert(QStringLiteral("suggested_index"), suggestedIndex);
		const QJsonObject diagnostic = diagnosticForRange(reviewedRange, suggestedIndex);
		const QString diagnosticKind = diagnostic.value(QStringLiteral("diagnostic_kind")).toString().trimmed();
		if (!diagnosticKind.isEmpty())
			structuredFeedback.insert(QStringLiteral("diagnostic_kind"), diagnosticKind);
		const QString rejectionReason =
			diagnostic.value(QStringLiteral("rejection_reason")).toString().trimmed();
		if (!rejectionReason.isEmpty())
			structuredFeedback.insert(QStringLiteral("diagnostic_rejection_reason"), rejectionReason);
	} else {
		if (!collectStructuredFeedback(row, normalizedDecision, &structuredFeedback)) {
			refreshClipTable(ranges);
			return;
		}
		structuredFeedback.insert(QStringLiteral("suggested_index"), suggestedIndex);
		if (normalizedDecision == QStringLiteral("approved_adjusted")) {
			structuredFeedback.insert(QStringLiteral("approved_corrected_range"), true);
			structuredFeedback.insert(QStringLiteral("semantic_positive_example"), true);
		}
	}

	explicitReviewDecisions.insert(suggestedIndex, normalizedDecision);
	explicitReviewFeedbackDetails.insert(suggestedIndex, structuredFeedback);
	blog(LOG_INFO,
	     "Explicit review marker decision captured. video=%s row=%d suggestionIndex=%d decision=%s range=%.2f-%.2f",
	     videoPath.toUtf8().constData(), row, suggestedIndex, normalizedDecision.toUtf8().constData(),
	     reviewedRange.startSec, reviewedRange.endSec);

	boundaryFeedbackSaved = false;
	if (finishReviewButton) {
		finishReviewButton->setEnabled(true);
		finishReviewButton->setText(QStringLiteral("Finish review (save feedback)"));
	}
	if (normalizedDecision == QStringLiteral("disliked") ||
	    normalizedDecision == QStringLiteral("ignored_diagnostic")) {
		videoEditor->selectRange(row);
		videoEditor->removeSelectedRange();
		return;
	}
	refreshClipTable(videoEditor->clipRanges());
}

void UploadReviewDialog::finishReviewFeedback()
{
	saveCurationOptions();
	saveBoundaryFeedback(QStringLiteral("review_finished"));
	if (finishReviewButton) {
		finishReviewButton->setEnabled(false);
		finishReviewButton->setText(boundaryFeedbackSaved ? QStringLiteral("Feedback saved")
								  : QStringLiteral("No feedback to save"));
	}
}

void UploadReviewDialog::saveBoundaryFeedback(const QString &eventName)
{
	if (advancedSettingsOnly || boundaryFeedbackSaved || !videoEditor)
		return;
	if (explicitReviewDecisions.isEmpty()) {
		blog(LOG_INFO, "Skipping boundary feedback save without explicit review decisions. video=%s event=%s",
		     videoPath.toUtf8().constData(), eventName.toUtf8().constData());
		return;
	}
	ensureReviewFeedbackSnapshot(QStringLiteral("existing_review_markers"));
	if (!Curation::Feedback::CurationFeedbackStore::hasUsefulSuggestion(lastSemanticSuggestion))
		return;

	if (lastSemanticSuggestion.contentId.trimmed().isEmpty())
		lastSemanticSuggestion.contentId = Curation::Feedback::CurationFeedbackStore::fileContentId(videoPath);
	CurationSettings settings = curationSettings();
	settings.reviewSettingsKey = currentReviewSettingsKey();
	QVector<ClipDuration> userRanges = videoEditor->clipRanges();
	if (isDefaultNoMarkerPlaceholderRange(videoEditor, userRanges))
		userRanges.clear();
	for (auto it = explicitReviewDecisions.constBegin(); it != explicitReviewDecisions.constEnd(); ++it) {
		const int suggestedIndex = it.key();
		const QString decision = it.value().trimmed().toLower();
		if (decision != QStringLiteral("liked"))
			continue;
		if (suggestedIndex < 0 || suggestedIndex >= lastSemanticSuggestion.ranges.size())
			continue;
		const ClipDuration acceptedRange = lastSemanticSuggestion.ranges.at(suggestedIndex);
		bool alreadyPresent = false;
		for (const ClipDuration &range : std::as_const(userRanges)) {
			if (reviewRangeSimilarity(range, acceptedRange) >= 0.99) {
				alreadyPresent = true;
				break;
			}
		}
		if (!alreadyPresent)
			userRanges.append(acceptedRange);
	}
	if (Curation::Feedback::CurationFeedbackStore::appendReviewFeedback(
		    videoPath, settings, lastSemanticSuggestion, userRanges, eventName, {}, explicitReviewDecisions,
		    explicitReviewFeedbackDetails)) {
		boundaryFeedbackSaved = true;
	}
}

void UploadReviewDialog::updateCreditEstimate(const QVector<ClipDuration> &ranges)
{
	if (!creditEstimateLabel)
		return;

	const double totalSeconds = totalRangeDurationSeconds(ranges);
	const int estimatedCredits = estimate_opus_credits(totalSeconds);

	if (estimatedCredits <= 0) {
		creditEstimateLabel->setText(obsText("Label.OpusCreditsNoSelection"));
		return;
	}

	creditEstimateLabel->setText(obsText("Label.OpusCreditsSelectedEstimate")
					     .arg(estimatedCredits)
					     .arg(format_duration_seconds(totalSeconds)));
}

void UploadReviewDialog::loadSavedCurationOptions()
{
	const QString key = currentReviewSettingsKey();
	if (key.isEmpty())
		return;

	const QString raw = PluginConfig::getValue(key).trimmed();
	if (raw.isEmpty())
		return;

	const QJsonDocument document = QJsonDocument::fromJson(raw.toUtf8());
	if (!document.isObject())
		return;

	const QJsonObject root = document.object();

	if (topicKeywordsInput && root.contains(QStringLiteral("topicKeywords"))) {
		QStringList keywords;
		const QJsonValue value = root.value(QStringLiteral("topicKeywords"));
		if (value.isArray()) {
			const QJsonArray array = value.toArray();
			for (const QJsonValue &item : array) {
				const QString keyword = item.toString().trimmed();
				if (!keyword.isEmpty())
					keywords.append(keyword);
			}
		} else {
			const QString rawKeywords = value.toString().trimmed();
			if (!rawKeywords.isEmpty())
				keywords = rawKeywords.split(QStringLiteral(","), Qt::SkipEmptyParts);
		}

		for (QString &keyword : keywords)
			keyword = keyword.trimmed();

		topicKeywordsInput->setText(keywords.join(QStringLiteral(", ")));
	}

	setComboCurrentTextIfExists(genreInput, root.value(QStringLiteral("genre")).toString());
	setComboCurrentDataIfExists(
		curationPresetInput,
		CurationPreset::normalizeId(
			root.value(QStringLiteral("curationPreset")).toString(QStringLiteral("auto"))));
	setOpusModelOrDefault(modelInput, root.value(QStringLiteral("model")).toString());
	setComboCurrentDataIfExists(clipLengthInput,
				    root.value(QStringLiteral("clipLengthPreset")).toString(QStringLiteral("Medium")));
	setComboCurrentDataIfExists(
		sourceLanguageInput,
		normalizeLanguageSetting(
			root.value(QStringLiteral("sourceLanguage"))
				.toString(PluginConfig::getValue(QString::fromLatin1(CONFIG_OPUS_SOURCE_LANGUAGE),
								 QString::fromLatin1(LANGUAGE_AUTO)))));
	setComboCurrentDataIfExists(transcriptionLanguageInput,
				    normalizeLanguageSetting(root.value(QStringLiteral("transcriptionLanguage"))
								     .toString(QString::fromLatin1(LANGUAGE_AUTO))));

	if (skipCurateInput && root.contains(QStringLiteral("skipCurate")))
		skipCurateInput->setChecked(root.value(QStringLiteral("skipCurate")).toBool(false));

	if (opusPromptInput && root.contains(QStringLiteral("aiPrompt")))
		opusPromptInput->setPlainText(root.value(QStringLiteral("aiPrompt")).toString().trimmed());
}

void UploadReviewDialog::saveCurationOptions() const
{
	const QString key = currentReviewSettingsKey();
	if (key.isEmpty())
		return;

	QJsonObject root;
	root.insert(QStringLiteral("videoFileName"), QFileInfo(videoPath).fileName());
	root.insert(QStringLiteral("videoPath"), videoPath);
	root.insert(QStringLiteral("reviewSettingsKey"), key);
	root.insert(QStringLiteral("originalVideoDurationSec"),
		    advancedSettingsOnly ? initialSettings.originalVideoDurationSec
					 : (videoEditor ? videoEditor->durationMilliseconds() / 1000.0 : 0.0));

	QJsonArray ranges;
	const QVector<ClipDuration> currentRanges =
		advancedSettingsOnly ? initialSettings.clipDurations
				     : (videoEditor ? videoEditor->clipRanges() : QVector<ClipDuration>{});
	for (const ClipDuration &range : currentRanges) {
		QJsonObject item;
		item.insert(QStringLiteral("start"), range.startSec);
		item.insert(QStringLiteral("end"), range.endSec);
		ranges.append(item);
	}
	root.insert(QStringLiteral("clipDurations"), ranges);

	QJsonArray topicKeywords;
	if (topicKeywordsInput) {
		const QStringList keywords = topicKeywordsInput->text().split(QStringLiteral(","), Qt::SkipEmptyParts);
		for (QString keyword : keywords) {
			keyword = keyword.trimmed();
			if (!keyword.isEmpty())
				topicKeywords.append(keyword);
		}
	}
	root.insert(QStringLiteral("topicKeywords"), topicKeywords);
	root.insert(QStringLiteral("genre"), genreInput ? genreInput->currentText() : QStringLiteral("Auto"));
	root.insert(QStringLiteral("curationPreset"),
		    curationPresetInput ? curationPresetInput->currentData().toString() : QStringLiteral("auto"));
	root.insert(QStringLiteral("model"), modelInput ? modelInput->currentText() : QStringLiteral("ClipAnything"));
	root.insert(QStringLiteral("clipLengthPreset"),
		    clipLengthInput ? clipLengthInput->currentData().toString() : QStringLiteral("Medium"));
	root.insert(QStringLiteral("skipCurate"), skipCurateInput ? skipCurateInput->isChecked() : false);
	root.insert(QStringLiteral("sourceLanguage"),
		    sourceLanguageInput ? normalizeLanguageSetting(sourceLanguageInput->currentData().toString())
					: QString::fromLatin1(LANGUAGE_AUTO));
	root.insert(QStringLiteral("transcriptionLanguage"),
		    transcriptionLanguageInput
			    ? normalizeLanguageSetting(transcriptionLanguageInput->currentData().toString())
			    : QString::fromLatin1(LANGUAGE_AUTO));
	root.insert(QStringLiteral("aiPrompt"), opusPromptInput ? opusPromptInput->toPlainText().trimmed() : QString{});

	PluginConfig::setValue(key, QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

QString UploadReviewDialog::currentReviewSettingsKey() const
{
	if (advancedSettingsOnly && !initialSettings.reviewSettingsKey.trimmed().isEmpty())
		return initialSettings.reviewSettingsKey;

	const QString rangeKey =
		advancedSettingsOnly
			? reviewRangeKeyFromRanges(initialSettings.clipDurations)
			: reviewRangeKeyFromMarkers(videoEditor ? videoEditor->markerPositions() : QVector<double>{});
	return reviewSettingsConfigKey(videoPath, rangeKey);
}

CurationSettings UploadReviewDialog::curationSettings() const
{
	CurationSettings settings = advancedSettingsOnly ? initialSettings : CurationSettings{};

	const QVector<ClipDuration> ranges =
		advancedSettingsOnly ? initialSettings.clipDurations
				     : (videoEditor ? videoEditor->clipRanges() : QVector<ClipDuration>{});

	settings.reviewSettingsKey = currentReviewSettingsKey();
	settings.originalVideoDurationSec =
		advancedSettingsOnly ? initialSettings.originalVideoDurationSec
				     : (videoEditor ? videoEditor->durationMilliseconds() / 1000.0 : 0.0);
	settings.rangeStartSec = ranges.isEmpty() ? settings.rangeStartSec : ranges.first().startSec;
	settings.rangeEndSec = ranges.isEmpty() ? settings.rangeEndSec : ranges.last().endSec;
	settings.clipDurations.clear();
	for (const auto &range : ranges)
		settings.clipDurations.append(range);
	settings.uploadClipRangesIndependently = settings.clipDurations.size() > 1;

	settings.genre = genreInput->currentText();
	settings.curationPreset = curationPresetInput ? curationPresetInput->currentData().toString()
						      : QStringLiteral("auto");
	settings.skipCurate = skipCurateInput->isChecked();
	settings.model = modelInput->currentText();
	settings.clipLengthPreset = clipLengthInput ? clipLengthInput->currentData().toString()
						    : QStringLiteral("Medium");
	settings.sourceLanguage = sourceLanguageInput
					  ? normalizeLanguageSetting(sourceLanguageInput->currentData().toString())
					  : QString::fromLatin1(LANGUAGE_AUTO);
	settings.transcriptionLanguage =
		transcriptionLanguageInput
			? normalizeLanguageSetting(transcriptionLanguageInput->currentData().toString())
			: QString::fromLatin1(LANGUAGE_AUTO);
	settings.aiPrompt = opusPromptInput ? opusPromptInput->toPlainText().trimmed() : QString{};

	settings.topicKeywords.clear();
	const QStringList keywords = topicKeywordsInput->text().split(",", Qt::SkipEmptyParts);
	for (QString keyword : keywords)
		settings.topicKeywords.append(keyword.trimmed());

	return settings;
}
