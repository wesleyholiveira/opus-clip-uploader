#include "curation/feedback/curation-feedback-store.hpp"
#include "transcription/transcript-store.hpp"

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#include <util/platform.h>
#ifdef __cplusplus
}
#endif

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSaveFile>
#include <QSet>
#include <QRegularExpression>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <cmath>
#include <limits>
#include <array>

namespace Curation::Feedback {

namespace {

static constexpr double SAME_RANGE_TOLERANCE_SEC = 1.25;
static constexpr double MEANINGFUL_ADJUSTMENT_SEC = 2.0;

static QString obsConfigPath(const char *relativePath)
{
	char *path = obs_module_config_path(relativePath);
	if (!path)
		return {};
	const QString result = QString::fromUtf8(path);
	bfree(path);
	return result;
}

static bool ensureDirectory(const QString &path)
{
	if (path.trimmed().isEmpty())
		return false;
	QDir dir(path);
	if (dir.exists())
		return true;
	return dir.mkpath(QStringLiteral("."));
}

static QString stableVideoId(const QString &videoPath)
{
	const QByteArray data = QFileInfo(videoPath).canonicalFilePath().isEmpty()
					? videoPath.toUtf8()
					: QFileInfo(videoPath).canonicalFilePath().toUtf8();
	return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex().left(16));
}

static QString normalizeContentId(QString value)
{
	value = value.trimmed();
	if (value.size() > 160)
		value = value.left(160);
	return value;
}

static QStringList normalizedContentIds(QStringList values)
{
	QStringList result;
	for (QString value : values) {
		value = normalizeContentId(value);
		if (!value.isEmpty() && !result.contains(value))
			result.append(value);
	}
	return result;
}

static QString fileContentIdUncached(const QString &videoPath)
{
	const QFileInfo info(videoPath);
	if (!info.exists() || !info.isFile() || info.size() <= 0)
		return {};

	QFile file(info.absoluteFilePath());
	if (!file.open(QIODevice::ReadOnly))
		return {};

	QCryptographicHash hash(QCryptographicHash::Sha256);
	const qint64 size = info.size();
	hash.addData(QByteArrayLiteral("clip-cropper-file-fingerprint-v1\n"));
	hash.addData(QByteArray::number(size));
	hash.addData(QByteArrayLiteral("\n"));

	constexpr qint64 chunkSize = 1024 * 1024;
	QVector<qint64> offsets;
	offsets << 0;
	if (size > chunkSize * 2)
		offsets << std::max<qint64>(0, (size / 2) - (chunkSize / 2));
	if (size > chunkSize)
		offsets << std::max<qint64>(0, size - chunkSize);

	QSet<qint64> seenOffsets;
	for (const qint64 offset : offsets) {
		if (seenOffsets.contains(offset))
			continue;
		seenOffsets.insert(offset);
		if (!file.seek(offset))
			continue;
		const QByteArray sample = file.read(std::min(chunkSize, size - offset));
		if (sample.isEmpty())
			continue;
		hash.addData(QByteArrayLiteral("offset:"));
		hash.addData(QByteArray::number(offset));
		hash.addData(QByteArrayLiteral("\n"));
		hash.addData(sample);
	}

	return QStringLiteral("file_sample_sha256:%1").arg(QString::fromLatin1(hash.result().toHex().left(32)));
}

static QString fileContentIdCached(const QString &videoPath)
{
	static QMap<QString, QString> cache;
	static QMutex cacheMutex;
	const QFileInfo info(videoPath);
	const QString key = info.exists() ? info.absoluteFilePath() : videoPath;
	if (key.trimmed().isEmpty())
		return {};
	{
		QMutexLocker locker(&cacheMutex);
		if (cache.contains(key))
			return cache.value(key);
	}
	const QString value = fileContentIdUncached(videoPath);
	{
		QMutexLocker locker(&cacheMutex);
		cache.insert(key, value);
	}
	return value;
}

static QStringList legacyContentIdsForRecord(const QJsonObject &record)
{
	const QString videoPath = record.value(QStringLiteral("video_path")).toString().trimmed();
	if (videoPath.isEmpty())
		return {};

	const QString language = record.value(QStringLiteral("transcription_language")).toString().trimmed();
	const QString key = videoPath + QChar('\n') + language;
	static QMap<QString, QStringList> cache;
	static QMutex cacheMutex;
	{
		QMutexLocker locker(&cacheMutex);
		if (cache.contains(key))
			return cache.value(key);
	}

	QStringList ids;
	const auto appendTranscriptId = [&ids](const RecordingTranscript &transcript) {
		const QString id = CurationFeedbackStore::transcriptContentId(transcript);
		if (!id.isEmpty())
			ids.append(id);
	};

	QStringList languages;
	if (!language.isEmpty())
		languages.append(language);
	languages.append(QStringLiteral("pt"));
	languages.append(QStringLiteral("auto"));
	languages.append(QStringLiteral("en"));
	languages.removeDuplicates();
	for (const QString &candidateLanguage : languages) {
		appendTranscriptId(TranscriptStore::loadForVideoPath(videoPath, candidateLanguage));
		appendTranscriptId(TranscriptStore::loadAlignedForVideoPath(videoPath, candidateLanguage));
	}

	const QString fileId = fileContentIdCached(videoPath);
	if (!fileId.isEmpty())
		ids.append(fileId);

	ids = normalizedContentIds(ids);
	{
		QMutexLocker locker(&cacheMutex);
		cache.insert(key, ids);
	}
	return ids;
}

static QString contentIdForSuggestion(const QString &videoPath, const FeedbackSuggestionSnapshot &suggestion)
{
	const QStringList ids = normalizedContentIds(QStringList{suggestion.contentId} + suggestion.contentIdAliases);
	if (!ids.isEmpty())
		return ids.first();
	return fileContentIdCached(videoPath);
}

static QJsonArray contentAliasesToJson(const QStringList &aliases, const QString &primary)
{
	QJsonArray array;
	for (const QString &alias : normalizedContentIds(aliases)) {
		if (!alias.isEmpty() && alias != primary)
			array.append(alias);
	}
	return array;
}

static void insertContentIdentity(QJsonObject &record, const QString &videoPath,
				  const FeedbackSuggestionSnapshot &suggestion)
{
	QStringList aliases = suggestion.contentIdAliases;
	const QString fileId = fileContentIdCached(videoPath);
	if (!fileId.isEmpty())
		aliases.append(fileId);
	const QString primary = contentIdForSuggestion(videoPath, suggestion);
	if (!primary.isEmpty()) {
		record.insert(QStringLiteral("content_id"), primary);
		record.insert(QStringLiteral("content_id_source"), primary.startsWith(QStringLiteral("transcript_"))
									   ? QStringLiteral("transcript_hash")
									   : QStringLiteral("file_fingerprint"));
		const QJsonArray aliasArray = contentAliasesToJson(aliases, primary);
		if (!aliasArray.isEmpty())
			record.insert(QStringLiteral("content_id_aliases"), aliasArray);
	}
}

static QString normalizedTranscriptText(QString value)
{
	value = value.toLower().normalized(QString::NormalizationForm_KC);
	value.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
	return value.trimmed();
}

static QJsonArray rangesToJson(const QVector<ClipDuration> &ranges)
{
	QJsonArray array;
	for (int i = 0; i < ranges.size(); ++i) {
		const ClipDuration &range = ranges.at(i);
		if (!std::isfinite(range.startSec) || !std::isfinite(range.endSec) || range.endSec <= range.startSec)
			continue;
		QJsonObject item;
		item.insert(QStringLiteral("index"), i);
		item.insert(QStringLiteral("start_sec"), range.startSec);
		item.insert(QStringLiteral("end_sec"), range.endSec);
		item.insert(QStringLiteral("duration_sec"), range.endSec - range.startSec);
		array.append(item);
	}
	return array;
}

static QJsonArray topicKeywordsToJson(const QStringList &keywords)
{
	QJsonArray array;
	for (QString keyword : keywords) {
		keyword = keyword.trimmed();
		if (!keyword.isEmpty())
			array.append(keyword);
	}
	return array;
}

static double overlapSec(const ClipDuration &a, const ClipDuration &b)
{
	return std::max(0.0, std::min(a.endSec, b.endSec) - std::max(a.startSec, b.startSec));
}

static double unionSec(const ClipDuration &a, const ClipDuration &b)
{
	return std::max(a.endSec, b.endSec) - std::min(a.startSec, b.startSec);
}

static double rangeSimilarity(const ClipDuration &suggested, const ClipDuration &user)
{
	const double overlap = overlapSec(suggested, user);
	const double uni = unionSec(suggested, user);
	if (uni <= 0.0)
		return 0.0;
	const double iou = overlap / uni;
	const double startDistance = std::fabs(suggested.startSec - user.startSec);
	const double endDistance = std::fabs(suggested.endSec - user.endSec);
	const double boundaryBonus = std::max(0.0, 1.0 - ((startDistance + endDistance) / 30.0));
	return (iou * 0.78) + (boundaryBonus * 0.22);
}

static int bestUserRangeIndexForSuggestion(const ClipDuration &suggested, const QVector<ClipDuration> &userRanges,
					   QSet<int> &usedUserRanges, double *outSimilarity)
{
	int bestIndex = -1;
	double bestScore = 0.0;
	for (int i = 0; i < userRanges.size(); ++i) {
		if (usedUserRanges.contains(i))
			continue;
		const ClipDuration &user = userRanges.at(i);
		if (!std::isfinite(user.startSec) || !std::isfinite(user.endSec) || user.endSec <= user.startSec)
			continue;
		const double score = rangeSimilarity(suggested, user);
		if (score > bestScore) {
			bestScore = score;
			bestIndex = i;
		}
	}
	if (outSimilarity)
		*outSimilarity = bestScore;
	return bestScore >= 0.18 ? bestIndex : -1;
}

static QString startErrorType(double generatedStartSec, double userStartSec)
{
	const double errorSec = generatedStartSec - userStartSec;
	if (errorSec > MEANINGFUL_ADJUSTMENT_SEC)
		return QStringLiteral("starts_too_late");
	if (errorSec < -MEANINGFUL_ADJUSTMENT_SEC)
		return QStringLiteral("starts_too_early");
	return QStringLiteral("good");
}

static QString endErrorType(double generatedEndSec, double userEndSec)
{
	const double errorSec = generatedEndSec - userEndSec;
	if (errorSec > MEANINGFUL_ADJUSTMENT_SEC)
		return QStringLiteral("overextended_after_resolution");
	if (errorSec < -MEANINGFUL_ADJUSTMENT_SEC)
		return QStringLiteral("ends_too_early");
	return QStringLiteral("good");
}

static QString matchedDecision(const ClipDuration &suggested, const ClipDuration &user)
{
	const bool sameStart = std::fabs(suggested.startSec - user.startSec) <= SAME_RANGE_TOLERANCE_SEC;
	const bool sameEnd = std::fabs(suggested.endSec - user.endSec) <= SAME_RANGE_TOLERANCE_SEC;
	return (sameStart && sameEnd) ? QStringLiteral("accepted") : QStringLiteral("adjusted");
}

static QJsonObject diagnosticsForSuggestedRange(const FeedbackSuggestionSnapshot &suggestion, int suggestedIndex,
						const ClipDuration &range)
{
	for (const QJsonValue &value : suggestion.candidateDiagnostics) {
		if (!value.isObject())
			continue;
		const QJsonObject candidate = value.toObject();
		if (candidate.value(QStringLiteral("index")).toInt(-1) == suggestedIndex)
			return candidate;
	}

	QJsonObject best;
	double bestScore = 0.0;
	for (const QJsonValue &value : suggestion.candidateDiagnostics) {
		if (!value.isObject())
			continue;
		const QJsonObject candidate = value.toObject();
		const ClipDuration candidateRange{candidate.value(QStringLiteral("start_sec")).toDouble(),
						  candidate.value(QStringLiteral("end_sec")).toDouble()};
		if (!std::isfinite(candidateRange.startSec) || !std::isfinite(candidateRange.endSec) ||
		    candidateRange.endSec <= candidateRange.startSec)
			continue;
		const double score = rangeSimilarity(range, candidateRange);
		if (score > bestScore) {
			bestScore = score;
			best = candidate;
		}
	}
	return bestScore >= 0.50 ? best : QJsonObject{};
}

static QJsonObject suggestionRecord(const QString &videoPath, const CurationSettings &settings,
				    const FeedbackSuggestionSnapshot &suggestion, int suggestedIndex,
				    const ClipDuration &suggestedRange, const QVector<ClipDuration> &userRanges,
				    QSet<int> &usedUserRanges, const QString &eventName, const QString &humanReason,
				    const QMap<int, QString> &explicitDecisionsBySuggestedIndex)
{
	double similarity = 0.0;
	const int matchedUserIndex =
		bestUserRangeIndexForSuggestion(suggestedRange, userRanges, usedUserRanges, &similarity);
	const bool matched = matchedUserIndex >= 0;
	if (matched)
		usedUserRanges.insert(matchedUserIndex);
	const ClipDuration userRange = matched ? userRanges.at(matchedUserIndex) : ClipDuration{};
	const QString explicitDecision = explicitDecisionsBySuggestedIndex.value(suggestedIndex).trimmed().toLower();
	QString decision = matched ? matchedDecision(suggestedRange, userRange) : QStringLiteral("removed_unrated");
	if (explicitDecision == QStringLiteral("disliked"))
		decision = QStringLiteral("rejected");
	else if (explicitDecision == QStringLiteral("liked") && matched)
		decision = matchedDecision(suggestedRange, userRange);

	QJsonObject record;
	record.insert(QStringLiteral("schema_version"), 1);
	record.insert(QStringLiteral("timestamp_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
	record.insert(QStringLiteral("event"),
		      eventName.trimmed().isEmpty() ? QStringLiteral("review_closed") : eventName.trimmed());
	record.insert(QStringLiteral("decision"), decision);
	if (!explicitDecision.isEmpty() &&
	    (explicitDecision == QStringLiteral("disliked") || decision != QStringLiteral("removed_unrated")))
		record.insert(QStringLiteral("explicit_review_decision"), explicitDecision);
	record.insert(QStringLiteral("video_id"), stableVideoId(videoPath));
	record.insert(QStringLiteral("video_file_name"), QFileInfo(videoPath).fileName());
	record.insert(QStringLiteral("video_path"), videoPath);
	insertContentIdentity(record, videoPath, suggestion);
	record.insert(QStringLiteral("preset"), settings.curationPreset.trimmed().isEmpty()
							? QStringLiteral("auto")
							: settings.curationPreset.trimmed());
	record.insert(QStringLiteral("model"), settings.model);
	record.insert(QStringLiteral("clip_length_preset"), settings.clipLengthPreset);
	record.insert(QStringLiteral("source_language"), settings.sourceLanguage);
	record.insert(QStringLiteral("transcription_language"), settings.transcriptionLanguage);
	record.insert(QStringLiteral("topic_keywords"), topicKeywordsToJson(settings.topicKeywords));
	record.insert(QStringLiteral("main_target"), settings.topicKeywords.join(QStringLiteral(", ")).left(240));
	record.insert(QStringLiteral("review_settings_key"), settings.reviewSettingsKey);
	record.insert(QStringLiteral("suggestion_source"), suggestion.source.trimmed().isEmpty()
								   ? QStringLiteral("semantic_review")
								   : suggestion.source.trimmed());
	record.insert(QStringLiteral("suggestion_summary"), suggestion.summary.left(1000));
	record.insert(QStringLiteral("suggested_index"), suggestedIndex);
	record.insert(QStringLiteral("generated_start_sec"), suggestedRange.startSec);
	record.insert(QStringLiteral("generated_end_sec"), suggestedRange.endSec);
	record.insert(QStringLiteral("generated_duration_sec"), suggestedRange.endSec - suggestedRange.startSec);
	record.insert(QStringLiteral("matched_user_index"), matchedUserIndex);
	record.insert(QStringLiteral("match_similarity"), similarity);

	if (matched) {
		record.insert(QStringLiteral("user_start_sec"), userRange.startSec);
		record.insert(QStringLiteral("user_end_sec"), userRange.endSec);
		record.insert(QStringLiteral("user_duration_sec"), userRange.endSec - userRange.startSec);
		record.insert(QStringLiteral("start_error_sec"), suggestedRange.startSec - userRange.startSec);
		record.insert(QStringLiteral("end_error_sec"), suggestedRange.endSec - userRange.endSec);
		record.insert(QStringLiteral("start_error_type"),
			      startErrorType(suggestedRange.startSec, userRange.startSec));
		record.insert(QStringLiteral("end_error_type"), endErrorType(suggestedRange.endSec, userRange.endSec));
	} else {
		if (explicitDecision == QStringLiteral("disliked")) {
			record.insert(QStringLiteral("reject_reason"), QStringLiteral("user_disliked_marker"));
			record.insert(QStringLiteral("start_error_type"), QStringLiteral("rejected"));
			record.insert(QStringLiteral("end_error_type"), QStringLiteral("rejected"));
		} else {
			record.insert(QStringLiteral("remove_reason"), QStringLiteral("user_removed_marker_not_rated"));
			record.insert(QStringLiteral("start_error_type"), QStringLiteral("removed_unrated"));
			record.insert(QStringLiteral("end_error_type"), QStringLiteral("removed_unrated"));
		}
	}
	if (explicitDecision == QStringLiteral("disliked"))
		record.insert(QStringLiteral("reject_reason"), QStringLiteral("user_disliked_marker"));

	const QJsonObject diagnostics = diagnosticsForSuggestedRange(suggestion, suggestedIndex, suggestedRange);
	if (!diagnostics.isEmpty()) {
		record.insert(QStringLiteral("scores"), diagnostics.value(QStringLiteral("scores")));
		record.insert(QStringLiteral("classifier_labels"),
			      diagnostics.value(QStringLiteral("classifier_labels")));
		record.insert(QStringLiteral("evidence"), diagnostics.value(QStringLiteral("evidence")));
		record.insert(QStringLiteral("candidate_source"), diagnostics.value(QStringLiteral("source")));
		record.insert(QStringLiteral("candidate_final_score"),
			      diagnostics.value(QStringLiteral("final_score")));
	}

	if (!humanReason.trimmed().isEmpty())
		record.insert(QStringLiteral("human_reason"), humanReason.trimmed().left(1000));
	return record;
}

static QJsonObject addedUserRangeRecord(const QString &videoPath, const CurationSettings &settings,
					const FeedbackSuggestionSnapshot &suggestion, int userIndex,
					const ClipDuration &userRange, const QString &eventName)
{
	QJsonObject record;
	record.insert(QStringLiteral("schema_version"), 1);
	record.insert(QStringLiteral("timestamp_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
	record.insert(QStringLiteral("event"),
		      eventName.trimmed().isEmpty() ? QStringLiteral("review_closed") : eventName.trimmed());
	record.insert(QStringLiteral("decision"), QStringLiteral("added_by_user"));
	record.insert(QStringLiteral("video_id"), stableVideoId(videoPath));
	record.insert(QStringLiteral("video_file_name"), QFileInfo(videoPath).fileName());
	record.insert(QStringLiteral("video_path"), videoPath);
	insertContentIdentity(record, videoPath, suggestion);
	record.insert(QStringLiteral("preset"), settings.curationPreset.trimmed().isEmpty()
							? QStringLiteral("auto")
							: settings.curationPreset.trimmed());
	record.insert(QStringLiteral("topic_keywords"), topicKeywordsToJson(settings.topicKeywords));
	record.insert(QStringLiteral("main_target"), settings.topicKeywords.join(QStringLiteral(", ")).left(240));
	record.insert(QStringLiteral("review_settings_key"), settings.reviewSettingsKey);
	record.insert(QStringLiteral("suggestion_source"), suggestion.source.trimmed().isEmpty()
								   ? QStringLiteral("semantic_review")
								   : suggestion.source.trimmed());
	record.insert(QStringLiteral("suggestion_summary"), suggestion.summary.left(1000));
	record.insert(QStringLiteral("matched_user_index"), userIndex);
	record.insert(QStringLiteral("user_start_sec"), userRange.startSec);
	record.insert(QStringLiteral("user_end_sec"), userRange.endSec);
	record.insert(QStringLiteral("user_duration_sec"), userRange.endSec - userRange.startSec);
	record.insert(QStringLiteral("start_error_type"), QStringLiteral("missed_candidate"));
	record.insert(QStringLiteral("end_error_type"), QStringLiteral("missed_candidate"));
	return record;
}

static bool appendJsonLines(const QString &path, const QVector<QJsonObject> &records)
{
	if (records.isEmpty())
		return true;
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
		return false;
	for (const QJsonObject &record : records) {
		file.write(QJsonDocument(record).toJson(QJsonDocument::Compact));
		file.write("\n");
	}
	return true;
}

} // namespace

QString CurationFeedbackStore::fileContentId(const QString &videoPath)
{
	return fileContentIdCached(videoPath);
}

QString CurationFeedbackStore::transcriptContentId(const RecordingTranscript &transcript)
{
	if (transcript.segments.isEmpty())
		return {};

	QCryptographicHash hash(QCryptographicHash::Sha256);
	hash.addData(QByteArrayLiteral("clip-cropper-transcript-fingerprint-v1\n"));
	int added = 0;
	for (const TranscriptSegment &segment : transcript.segments) {
		const QString text = normalizedTranscriptText(segment.text);
		if (text.isEmpty())
			continue;
		const int startTenths = static_cast<int>(std::llround(std::max(0.0, segment.startSec) * 10.0));
		const int endTenths = static_cast<int>(std::llround(std::max(segment.startSec, segment.endSec) * 10.0));
		hash.addData(QByteArray::number(startTenths));
		hash.addData(QByteArrayLiteral("-"));
		hash.addData(QByteArray::number(endTenths));
		hash.addData(QByteArrayLiteral("|"));
		hash.addData(text.toUtf8());
		hash.addData(QByteArrayLiteral("\n"));
		++added;
	}

	if (added <= 0)
		return {};
	return QStringLiteral("transcript_sha256:%1").arg(QString::fromLatin1(hash.result().toHex().left(32)));
}

bool CurationFeedbackStore::hasUsefulSuggestion(const FeedbackSuggestionSnapshot &snapshot)
{
	for (const ClipDuration &range : snapshot.ranges) {
		if (std::isfinite(range.startSec) && std::isfinite(range.endSec) && range.endSec > range.startSec)
			return true;
	}
	return false;
}

QString CurationFeedbackStore::feedbackDirectoryPath()
{
	const QString dir = obsConfigPath("feedback");
	if (!dir.isEmpty())
		return dir;
	return QDir::temp().filePath(QStringLiteral("clip-cropper-feedback"));
}

QString CurationFeedbackStore::feedbackJsonlPath()
{
	return QDir(feedbackDirectoryPath()).filePath(QStringLiteral("boundary-feedback.jsonl"));
}

QString CurationFeedbackStore::calibrationJsonPath()
{
	return QDir(feedbackDirectoryPath()).filePath(QStringLiteral("boundary-calibration.json"));
}

QJsonObject CurationFeedbackStore::loadCalibrationRoot()
{
	QFile file(calibrationJsonPath());
	if (!file.exists() || !file.open(QIODevice::ReadOnly))
		return {};
	QJsonParseError error;
	const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		blog(LOG_WARNING, "[clip-cropper] Invalid boundary calibration JSON: %s",
		     error.errorString().toUtf8().constData());
		return {};
	}
	static bool loggedCalibrationLoad = false;
	if (!loggedCalibrationLoad) {
		loggedCalibrationLoad = true;
		blog(LOG_INFO, "[clip-cropper] Loaded boundary calibration JSON. path=%s",
		     calibrationJsonPath().toUtf8().constData());
	}
	return document.object();
}

static bool validFeedbackRange(const ClipDuration &range)
{
	return std::isfinite(range.startSec) && std::isfinite(range.endSec) && range.endSec > range.startSec;
}

static ClipDuration recordRange(const QJsonObject &record, const QString &startKey, const QString &endKey)
{
	return ClipDuration{record.value(startKey).toDouble(std::numeric_limits<double>::quiet_NaN()),
			    record.value(endKey).toDouble(std::numeric_limits<double>::quiet_NaN())};
}

static bool presetMatchesFeedback(const QString &recordPreset, const QString &requestedPreset)
{
	const QString rowPreset = recordPreset.trimmed().isEmpty() ? QStringLiteral("auto") : recordPreset.trimmed();
	const QString preset = requestedPreset.trimmed().isEmpty() ? QStringLiteral("auto") : requestedPreset.trimmed();
	return rowPreset == preset || rowPreset == QStringLiteral("auto") || preset == QStringLiteral("auto");
}

static bool rangeLooksLikeColdStartPrelude(const ClipDuration &range)
{
	if (!validFeedbackRange(range))
		return false;
	// Cross-video temporal memory is intentionally narrow. It is safe for repeated live
	// preludes/setup/music at the start of long recordings, but it would be too risky
	// to apply arbitrary rejected timestamps globally across unrelated videos.
	return range.startSec >= 0.0 && range.startSec <= 210.0 && range.endSec <= 390.0;
}

static bool feedbackDecisionCanBeUsedAcrossVideos(const QString &decision, const QString &explicitDecision)
{
	return decision == QStringLiteral("rejected") || explicitDecision == QStringLiteral("disliked") ||
	       decision == QStringLiteral("adjusted") || decision == QStringLiteral("accepted") ||
	       explicitDecision == QStringLiteral("liked") || decision == QStringLiteral("added_by_user");
}

static QString crossVideoReason(const QString &reason)
{
	const QString clean = reason.trimmed();
	return clean.isEmpty() ? QStringLiteral("cross_video_cold_start_feedback")
			       : QStringLiteral("cross_video_cold_start_feedback:%1").arg(clean.left(96));
}

static void appendRangeSignal(QVector<FeedbackRangeSignal> &results, const ClipDuration &range, const QString &decision,
			      const QString &source, const QString &reason, double weight, int sequence)
{
	if (!validFeedbackRange(range))
		return;
	FeedbackRangeSignal signal;
	signal.range = range;
	signal.decision = decision;
	signal.source = source;
	signal.reason = reason;
	signal.weight = std::clamp(weight, 0.05, 4.0);
	signal.sequence = sequence;
	results.append(signal);
}

static double feedbackRangeDuration(const ClipDuration &range)
{
	return std::max(0.0, range.endSec - range.startSec);
}

static double feedbackRangeCenter(const ClipDuration &range)
{
	return range.startSec + ((range.endSec - range.startSec) * 0.5);
}

static double feedbackRangeOverlap(const ClipDuration &left, const ClipDuration &right)
{
	return std::max(0.0, std::min(left.endSec, right.endSec) - std::max(left.startSec, right.startSec));
}

static bool feedbackSignalsConflict(const FeedbackRangeSignal &left, const FeedbackRangeSignal &right)
{
	const double leftDuration = feedbackRangeDuration(left.range);
	const double rightDuration = feedbackRangeDuration(right.range);
	if (leftDuration <= 0.0 || rightDuration <= 0.0)
		return false;
	const double overlap = feedbackRangeOverlap(left.range, right.range);
	if (overlap <= 0.0)
		return false;
	const double shorterCoverage = overlap / std::min(leftDuration, rightDuration);
	const double longerCoverage = overlap / std::max(leftDuration, rightDuration);
	const double centerDistance = std::fabs(feedbackRangeCenter(left.range) - feedbackRangeCenter(right.range));
	const bool closeBoundaries = std::fabs(left.range.startSec - right.range.startSec) <= 5.0 &&
				     std::fabs(left.range.endSec - right.range.endSec) <= 7.0;
	return closeBoundaries || (shorterCoverage >= 0.72 && centerDistance <= 12.0) ||
	       (shorterCoverage >= 0.88 && longerCoverage >= 0.42);
}

static void resolvePositiveNegativeConflicts(FeedbackRangeMemory &memory)
{
	if (memory.positiveRanges.isEmpty() || memory.negativeRanges.isEmpty())
		return;

	QVector<FeedbackRangeSignal> positives;
	positives.reserve(memory.positiveRanges.size());
	for (const FeedbackRangeSignal &positive : memory.positiveRanges) {
		bool keep = true;
		for (const FeedbackRangeSignal &negative : memory.negativeRanges) {
			if (!feedbackSignalsConflict(positive, negative))
				continue;
			// When the same range was liked and later disliked, the newest explicit
			// feedback is the source of truth. If ordering is tied, keep the negative
			// because it is safer to avoid a known bad marker than to resurrect it.
			if (negative.sequence >= positive.sequence) {
				keep = false;
				break;
			}
		}
		if (keep)
			positives.append(positive);
	}

	QVector<FeedbackRangeSignal> negatives;
	negatives.reserve(memory.negativeRanges.size());
	for (const FeedbackRangeSignal &negative : memory.negativeRanges) {
		bool keep = true;
		for (const FeedbackRangeSignal &positive : memory.positiveRanges) {
			if (!feedbackSignalsConflict(negative, positive))
				continue;
			if (positive.sequence > negative.sequence) {
				keep = false;
				break;
			}
		}
		if (keep)
			negatives.append(negative);
	}

	memory.positiveRanges = positives;
	memory.negativeRanges = negatives;
}

FeedbackRangeMemory CurationFeedbackStore::loadRangeMemoryForVideo(const QString &videoPath, const QString &presetId,
								   const QStringList &contentIds)
{
	FeedbackRangeMemory memory;
	QFile file(feedbackJsonlPath());
	if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text))
		return memory;

	const QString videoId = stableVideoId(videoPath);
	QStringList currentContentIds = normalizedContentIds(contentIds);
	const QString currentFileContentId = fileContentIdCached(videoPath);
	if (!currentFileContentId.isEmpty())
		currentContentIds.append(currentFileContentId);
	currentContentIds = normalizedContentIds(currentContentIds);
	int rowSequence = 0;
	while (!file.atEnd()) {
		const QByteArray line = file.readLine().trimmed();
		if (line.isEmpty())
			continue;
		QJsonParseError error;
		const QJsonDocument document = QJsonDocument::fromJson(line, &error);
		if (error.error != QJsonParseError::NoError || !document.isObject())
			continue;
		const QJsonObject record = document.object();
		++rowSequence;
		if (!presetMatchesFeedback(record.value(QStringLiteral("preset")).toString(), presetId))
			continue;

		const QString decision = record.value(QStringLiteral("decision")).toString().trimmed().toLower();
		const QString explicitDecision =
			record.value(QStringLiteral("explicit_review_decision")).toString().trimmed().toLower();
		const QString event = record.value(QStringLiteral("event")).toString().trimmed().toLower();
		const QString source = record.value(QStringLiteral("suggestion_source")).toString();
		const ClipDuration generated =
			recordRange(record, QStringLiteral("generated_start_sec"), QStringLiteral("generated_end_sec"));
		const ClipDuration user =
			recordRange(record, QStringLiteral("user_start_sec"), QStringLiteral("user_end_sec"));

		// A plain dialog cancel used to write event=review_rejected for every visible marker.
		// Those rows are not a meaningful negative or positive signal unless the marker itself
		// was explicitly liked/disliked by the user.
		if (event == QStringLiteral("review_rejected") && explicitDecision.isEmpty())
			continue;
		if (decision == QStringLiteral("removed_unrated"))
			continue;

		const bool sameVideo = record.value(QStringLiteral("video_id")).toString() == videoId;
		QStringList recordContentIds;
		recordContentIds.append(record.value(QStringLiteral("content_id")).toString());
		const QJsonArray aliasArray = record.value(QStringLiteral("content_id_aliases")).toArray();
		for (const QJsonValue &alias : aliasArray)
			recordContentIds.append(alias.toString());
		bool usedLegacyContentLookup = false;
		if (recordContentIds.join(QString()).trimmed().isEmpty()) {
			recordContentIds.append(legacyContentIdsForRecord(record));
			usedLegacyContentLookup = !recordContentIds.isEmpty();
		}
		recordContentIds = normalizedContentIds(recordContentIds);
		bool sameContent = false;
		for (const QString &id : recordContentIds) {
			if (!id.isEmpty() && currentContentIds.contains(id)) {
				sameContent = true;
				break;
			}
		}
		const bool coldStartSignal = rangeLooksLikeColdStartPrelude(generated) ||
					     rangeLooksLikeColdStartPrelude(user);
		if (!sameVideo && !sameContent &&
		    (!coldStartSignal || !feedbackDecisionCanBeUsedAcrossVideos(decision, explicitDecision)))
			continue;

		++memory.recordsRead;
		if (sameVideo)
			++memory.exactRecordsRead;
		else if (sameContent) {
			++memory.contentRecordsRead;
			if (usedLegacyContentLookup)
				++memory.legacyContentRecordsRead;
		} else
			++memory.crossVideoRecordsRead;

		const double scopeWeight = (sameVideo || sameContent) ? 1.0 : 0.62;
		const auto reasonForScope = [sameVideo, sameContent](const QString &reason) {
			if (sameVideo)
				return reason;
			if (sameContent) {
				const QString clean = reason.trimmed();
				return clean.isEmpty() ? QStringLiteral("content_hash_feedback")
						       : QStringLiteral("content_hash_feedback:%1").arg(clean.left(96));
			}
			return crossVideoReason(reason);
		};

		if (decision == QStringLiteral("rejected") || explicitDecision == QStringLiteral("disliked")) {
			appendRangeSignal(memory.negativeRanges, generated, decision, source,
					  reasonForScope(record.value(QStringLiteral("reject_reason"))
								 .toString(QStringLiteral("user_rejected_range"))),
					  1.0 * scopeWeight, rowSequence);
			continue;
		}

		if (decision == QStringLiteral("adjusted")) {
			appendRangeSignal(memory.negativeRanges, generated, decision, source,
					  reasonForScope(QStringLiteral("user_adjusted_generated_boundaries")),
					  0.72 * scopeWeight, rowSequence);
			appendRangeSignal(memory.positiveRanges, user, decision, source,
					  reasonForScope(QStringLiteral("user_corrected_range")), 0.88 * scopeWeight,
					  rowSequence);
			continue;
		}

		if (decision == QStringLiteral("accepted") || explicitDecision == QStringLiteral("liked")) {
			appendRangeSignal(memory.positiveRanges, validFeedbackRange(user) ? user : generated, decision,
					  source, reasonForScope(QStringLiteral("user_accepted_range")),
					  1.0 * scopeWeight, rowSequence);
			continue;
		}

		if (decision == QStringLiteral("added_by_user")) {
			appendRangeSignal(memory.positiveRanges, user, decision, source,
					  reasonForScope(QStringLiteral("user_added_marker")), 0.90 * scopeWeight,
					  rowSequence);
		}
	}

	resolvePositiveNegativeConflicts(memory);
	memory.loaded = memory.recordsRead > 0;
	if (memory.loaded) {
		blog(LOG_INFO,
		     "[clip-cropper] Loaded boundary feedback range memory. video=%s preset=%s records=%d exact=%d sameContent=%d legacySameContent=%d crossVideoColdStart=%d negative=%d positive=%d",
		     videoPath.toUtf8().constData(), presetId.toUtf8().constData(), memory.recordsRead,
		     memory.exactRecordsRead, memory.contentRecordsRead, memory.legacyContentRecordsRead,
		     memory.crossVideoRecordsRead, static_cast<int>(memory.negativeRanges.size()),
		     static_cast<int>(memory.positiveRanges.size()));
	} else if (!currentContentIds.isEmpty()) {
		blog(LOG_INFO,
		     "[clip-cropper] No boundary feedback range memory matched. video=%s preset=%s contentIds=%d",
		     videoPath.toUtf8().constData(), presetId.toUtf8().constData(),
		     static_cast<int>(currentContentIds.size()));
	}
	return memory;
}

bool CurationFeedbackStore::appendReviewFeedback(const QString &videoPath, const CurationSettings &settings,
						 const FeedbackSuggestionSnapshot &suggestion,
						 const QVector<ClipDuration> &userRanges, const QString &eventName,
						 const QString &humanReason,
						 const QMap<int, QString> &explicitDecisionsBySuggestedIndex)
{
	if (!hasUsefulSuggestion(suggestion))
		return false;

	const QString dir = feedbackDirectoryPath();
	if (!ensureDirectory(dir)) {
		blog(LOG_ERROR, "[clip-cropper] Failed to create feedback directory: %s", dir.toUtf8().constData());
		return false;
	}

	QVector<QJsonObject> records;
	QSet<int> usedUserRanges;
	for (int i = 0; i < suggestion.ranges.size(); ++i) {
		const ClipDuration &suggested = suggestion.ranges.at(i);
		if (!std::isfinite(suggested.startSec) || !std::isfinite(suggested.endSec) ||
		    suggested.endSec <= suggested.startSec)
			continue;
		records.append(suggestionRecord(videoPath, settings, suggestion, i, suggested, userRanges,
						usedUserRanges, eventName, humanReason,
						explicitDecisionsBySuggestedIndex));
	}

	for (int i = 0; i < userRanges.size(); ++i) {
		if (usedUserRanges.contains(i))
			continue;
		const ClipDuration &range = userRanges.at(i);
		if (!std::isfinite(range.startSec) || !std::isfinite(range.endSec) || range.endSec <= range.startSec)
			continue;
		records.append(addedUserRangeRecord(videoPath, settings, suggestion, i, range, eventName));
	}

	const QString path = feedbackJsonlPath();
	const bool saved = appendJsonLines(path, records);
	blog(saved ? LOG_INFO : LOG_ERROR,
	     "[clip-cropper] Boundary feedback append %s. path=%s records=%d event=%s video=%s",
	     saved ? "succeeded" : "failed", path.toUtf8().constData(), static_cast<int>(records.size()),
	     eventName.toUtf8().constData(), videoPath.toUtf8().constData());
	return saved;
}

} // namespace Curation::Feedback
