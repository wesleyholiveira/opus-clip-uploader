#include "curation/feedback/curation-feedback-detail.hpp"

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
#include <QJsonValue>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace Curation::Feedback::Detail {

static constexpr double SAME_RANGE_TOLERANCE_SEC = 1.25;
static constexpr double MEANINGFUL_ADJUSTMENT_SEC = 2.0;

QString obsConfigPath(const char *relativePath)
{
	char *path = obs_module_config_path(relativePath);
	if (!path)
		return {};
	const QString result = QString::fromUtf8(path);
	bfree(path);
	return result;
}

bool ensureDirectory(const QString &path)
{
	if (path.trimmed().isEmpty())
		return false;
	QDir dir(path);
	if (dir.exists())
		return true;
	return dir.mkpath(QStringLiteral("."));
}

QString stableVideoId(const QString &videoPath)
{
	const QByteArray data = QFileInfo(videoPath).canonicalFilePath().isEmpty()
					? videoPath.toUtf8()
					: QFileInfo(videoPath).canonicalFilePath().toUtf8();
	return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex().left(16));
}

QString normalizeContentId(QString value)
{
	value = value.trimmed();
	if (value.size() > 160)
		value = value.left(160);
	return value;
}

QStringList normalizedContentIds(QStringList values)
{
	QStringList result;
	QSet<QString> seen;
	result.reserve(values.size());
	seen.reserve(values.size());
	for (QString value : values) {
		value = normalizeContentId(std::move(value));
		if (value.isEmpty() || seen.contains(value))
			continue;
		seen.insert(value);
		result.append(std::move(value));
	}
	return result;
}

QString fileContentIdUncached(const QString &videoPath)
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

QString fileContentIdCached(const QString &videoPath)
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

QStringList legacyContentIdsForRecord(const QJsonObject &record)
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

QString contentIdForSuggestion(const QString &videoPath, const FeedbackSuggestionSnapshot &suggestion)
{
	const QStringList ids = normalizedContentIds(QStringList{suggestion.contentId} + suggestion.contentIdAliases);
	if (!ids.isEmpty())
		return ids.first();
	return fileContentIdCached(videoPath);
}

QJsonArray contentAliasesToJson(const QStringList &aliases, const QString &primary)
{
	QJsonArray array;
	for (const QString &alias : normalizedContentIds(aliases)) {
		if (!alias.isEmpty() && alias != primary)
			array.append(alias);
	}
	return array;
}

void insertContentIdentity(QJsonObject &record, const QString &videoPath, const FeedbackSuggestionSnapshot &suggestion)
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

QString normalizedTranscriptText(QString value)
{
	value = value.toLower().normalized(QString::NormalizationForm_KC);
	value.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
	return value.trimmed();
}

QJsonArray rangesToJson(const QVector<ClipDuration> &ranges)
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

QJsonArray topicKeywordsToJson(const QStringList &keywords)
{
	QJsonArray array;
	for (QString keyword : keywords) {
		keyword = keyword.trimmed();
		if (!keyword.isEmpty())
			array.append(keyword);
	}
	return array;
}

double overlapSec(const ClipDuration &a, const ClipDuration &b)
{
	return std::max(0.0, std::min(a.endSec, b.endSec) - std::max(a.startSec, b.startSec));
}

double unionSec(const ClipDuration &a, const ClipDuration &b)
{
	return std::max(a.endSec, b.endSec) - std::min(a.startSec, b.startSec);
}

double rangeSimilarity(const ClipDuration &suggested, const ClipDuration &user)
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

int bestUserRangeIndexForSuggestion(const ClipDuration &suggested, const QVector<ClipDuration> &userRanges,
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

QString startErrorType(double generatedStartSec, double userStartSec)
{
	const double errorSec = generatedStartSec - userStartSec;
	if (errorSec > MEANINGFUL_ADJUSTMENT_SEC)
		return QStringLiteral("starts_too_late");
	if (errorSec < -MEANINGFUL_ADJUSTMENT_SEC)
		return QStringLiteral("starts_too_early");
	return QStringLiteral("good");
}

QString endErrorType(double generatedEndSec, double userEndSec)
{
	const double errorSec = generatedEndSec - userEndSec;
	if (errorSec > MEANINGFUL_ADJUSTMENT_SEC)
		return QStringLiteral("overextended_after_resolution");
	if (errorSec < -MEANINGFUL_ADJUSTMENT_SEC)
		return QStringLiteral("ends_too_early");
	return QStringLiteral("good");
}

QString matchedDecision(const ClipDuration &suggested, const ClipDuration &user)
{
	const bool sameStart = std::fabs(suggested.startSec - user.startSec) <= SAME_RANGE_TOLERANCE_SEC;
	const bool sameEnd = std::fabs(suggested.endSec - user.endSec) <= SAME_RANGE_TOLERANCE_SEC;
	return (sameStart && sameEnd) ? QStringLiteral("accepted") : QStringLiteral("adjusted");
}

QJsonObject diagnosticsForSuggestedRange(const FeedbackSuggestionSnapshot &suggestion, int suggestedIndex,
					 const ClipDuration &range)
{
	for (const QJsonValue &value : suggestion.candidateDiagnostics) {
		if (!value.isObject())
			continue;
		const QJsonObject candidate = value.toObject();
		if (candidate.value(QStringLiteral("index")).toInt(-1) != suggestedIndex)
			continue;
		const ClipDuration candidateRange{candidate.value(QStringLiteral("start_sec")).toDouble(),
						  candidate.value(QStringLiteral("end_sec")).toDouble()};
		if (std::isfinite(candidateRange.startSec) && std::isfinite(candidateRange.endSec) &&
		    candidateRange.endSec > candidateRange.startSec && rangeSimilarity(range, candidateRange) >= 0.50)
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

QJsonObject suggestionRecord(const QString &videoPath, const CurationSettings &settings,
			     const FeedbackSuggestionSnapshot &suggestion, int suggestedIndex,
			     const ClipDuration &suggestedRange, const QVector<ClipDuration> &userRanges,
			     QSet<int> &usedUserRanges, const QString &eventName, const QString &humanReason,
			     const QMap<int, QString> &explicitDecisionsBySuggestedIndex,
			     const QMap<int, QJsonObject> &explicitFeedbackBySuggestedIndex)
{
	const QString explicitDecision = explicitDecisionsBySuggestedIndex.value(suggestedIndex).trimmed().toLower();
	const QJsonObject structuredFeedback = explicitFeedbackBySuggestedIndex.value(suggestedIndex);

	const auto structuredRange = [](const QJsonObject &object, const QString &startKey, const QString &endKey,
					ClipDuration *outRange) {
		if (!outRange)
			return false;
		const double startSec = object.value(startKey).toDouble(std::numeric_limits<double>::quiet_NaN());
		const double endSec = object.value(endKey).toDouble(std::numeric_limits<double>::quiet_NaN());
		if (!std::isfinite(startSec) || !std::isfinite(endSec) || endSec <= startSec)
			return false;
		outRange->startSec = startSec;
		outRange->endSec = endSec;
		return true;
	};

	ClipDuration generatedRange = suggestedRange;
	ClipDuration diagnosticOriginalRange{};
	const bool hasDiagnosticOriginalRange =
		structuredRange(structuredFeedback, QStringLiteral("diagnostic_original_start_sec"),
				QStringLiteral("diagnostic_original_end_sec"), &diagnosticOriginalRange);
	if (hasDiagnosticOriginalRange)
		generatedRange = diagnosticOriginalRange;

	ClipDuration explicitReviewedRange{};
	const bool hasExplicitReviewedRange = structuredRange(structuredFeedback, QStringLiteral("range_start_sec"),
							      QStringLiteral("range_end_sec"), &explicitReviewedRange);
	const bool explicitDecisionUsesReviewedRange = hasExplicitReviewedRange &&
						       (explicitDecision == QStringLiteral("adjusted") ||
							explicitDecision == QStringLiteral("liked") ||
							explicitDecision == QStringLiteral("approved_adjusted"));

	double similarity = 0.0;
	const int matchedUserIndex =
		bestUserRangeIndexForSuggestion(generatedRange, userRanges, usedUserRanges, &similarity);
	const bool matched = matchedUserIndex >= 0;
	if (matched)
		usedUserRanges.insert(matchedUserIndex);
	const ClipDuration matchedUserRange = matched ? userRanges.at(matchedUserIndex) : ClipDuration{};
	const ClipDuration feedbackUserRange = explicitDecisionUsesReviewedRange ? explicitReviewedRange
										 : matchedUserRange;
	const bool hasFeedbackUserRange = matched || explicitDecisionUsesReviewedRange;

	QString decision = matched ? matchedDecision(generatedRange, matchedUserRange)
				   : QStringLiteral("removed_unrated");
	if (explicitDecision == QStringLiteral("disliked"))
		decision = QStringLiteral("rejected");
	else if (explicitDecision == QStringLiteral("adjusted"))
		decision = QStringLiteral("adjusted");
	else if (explicitDecision == QStringLiteral("approved_adjusted"))
		decision = hasFeedbackUserRange ? QStringLiteral("approved_adjusted") : QStringLiteral("accepted");
	else if (explicitDecision == QStringLiteral("liked"))
		decision = hasFeedbackUserRange ? matchedDecision(generatedRange, feedbackUserRange)
						: QStringLiteral("accepted");
	else if (explicitDecision == QStringLiteral("ignored_diagnostic"))
		decision = QStringLiteral("ignored_diagnostic");

	QJsonObject record;
	record.insert(QStringLiteral("schema_version"), 1);
	record.insert(QStringLiteral("timestamp_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
	record.insert(QStringLiteral("event"),
		      eventName.trimmed().isEmpty() ? QStringLiteral("review_closed") : eventName.trimmed());
	record.insert(QStringLiteral("decision"), decision);
	if (!explicitDecision.isEmpty() &&
	    (explicitDecision == QStringLiteral("disliked") || explicitDecision == QStringLiteral("adjusted") ||
	     explicitDecision == QStringLiteral("approved_adjusted") ||
	     explicitDecision == QStringLiteral("ignored_diagnostic") || decision != QStringLiteral("removed_unrated")))
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
	record.insert(QStringLiteral("generated_start_sec"), generatedRange.startSec);
	record.insert(QStringLiteral("generated_end_sec"), generatedRange.endSec);
	record.insert(QStringLiteral("generated_duration_sec"), generatedRange.endSec - generatedRange.startSec);
	if (hasDiagnosticOriginalRange) {
		record.insert(QStringLiteral("diagnostic_original_start_sec"), diagnosticOriginalRange.startSec);
		record.insert(QStringLiteral("diagnostic_original_end_sec"), diagnosticOriginalRange.endSec);
		const bool edited =
			hasExplicitReviewedRange &&
			(std::fabs(explicitReviewedRange.startSec - diagnosticOriginalRange.startSec) > 0.05 ||
			 std::fabs(explicitReviewedRange.endSec - diagnosticOriginalRange.endSec) > 0.05);
		record.insert(QStringLiteral("diagnostic_range_edited"), edited);
	}
	record.insert(QStringLiteral("matched_user_index"), matchedUserIndex);
	record.insert(QStringLiteral("match_similarity"), similarity);

	if (hasFeedbackUserRange) {
		record.insert(QStringLiteral("user_start_sec"), feedbackUserRange.startSec);
		record.insert(QStringLiteral("user_end_sec"), feedbackUserRange.endSec);
		record.insert(QStringLiteral("user_duration_sec"),
			      feedbackUserRange.endSec - feedbackUserRange.startSec);
		record.insert(QStringLiteral("start_error_sec"), generatedRange.startSec - feedbackUserRange.startSec);
		record.insert(QStringLiteral("end_error_sec"), generatedRange.endSec - feedbackUserRange.endSec);
		record.insert(QStringLiteral("start_error_type"),
			      startErrorType(generatedRange.startSec, feedbackUserRange.startSec));
		record.insert(QStringLiteral("end_error_type"),
			      endErrorType(generatedRange.endSec, feedbackUserRange.endSec));
	} else {
		if (explicitDecision == QStringLiteral("ignored_diagnostic")) {
			record.insert(QStringLiteral("ignore_reason"),
				      QStringLiteral("user_marked_diagnostic_not_training_signal"));
			record.insert(QStringLiteral("start_error_type"), QStringLiteral("ignored_diagnostic"));
			record.insert(QStringLiteral("end_error_type"), QStringLiteral("ignored_diagnostic"));
		} else if (explicitDecision == QStringLiteral("disliked")) {
			record.insert(QStringLiteral("reject_reason"), QStringLiteral("user_disliked_marker"));
			record.insert(QStringLiteral("start_error_type"), QStringLiteral("rejected"));
			record.insert(QStringLiteral("end_error_type"), QStringLiteral("rejected"));
		} else if (explicitDecision == QStringLiteral("adjusted") ||
			   explicitDecision == QStringLiteral("approved_adjusted")) {
			record.insert(QStringLiteral("adjustment_without_corrected_range"), true);
			record.insert(QStringLiteral("start_error_type"), QStringLiteral("needs_adjustment"));
			record.insert(QStringLiteral("end_error_type"), QStringLiteral("needs_adjustment"));
		} else if (explicitDecision == QStringLiteral("liked")) {
			record.insert(QStringLiteral("user_start_sec"), generatedRange.startSec);
			record.insert(QStringLiteral("user_end_sec"), generatedRange.endSec);
			record.insert(QStringLiteral("user_duration_sec"),
				      generatedRange.endSec - generatedRange.startSec);
			record.insert(QStringLiteral("start_error_sec"), 0.0);
			record.insert(QStringLiteral("end_error_sec"), 0.0);
			record.insert(QStringLiteral("start_error_type"), QStringLiteral("good"));
			record.insert(QStringLiteral("end_error_type"), QStringLiteral("good"));
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

	if (!structuredFeedback.isEmpty()) {
		record.insert(QStringLiteral("explicit_structured_feedback"), structuredFeedback);
		const QStringList boolKeys{QStringLiteral("has_beginning"),
					   QStringLiteral("has_development"),
					   QStringLiteral("has_conclusion"),
					   QStringLiteral("has_single_topic"),
					   QStringLiteral("has_smooth_ending"),
					   QStringLiteral("has_good_hook"),
					   QStringLiteral("has_viewer_cue"),
					   QStringLiteral("has_topic_shift"),
					   QStringLiteral("starts_too_late"),
					   QStringLiteral("starts_too_early"),
					   QStringLiteral("ends_too_early"),
					   QStringLiteral("overextended_after_resolution"),
					   QStringLiteral("has_meta_noise"),
					   QStringLiteral("good_topic_bad_boundary"),
					   QStringLiteral("bad_topic"),
					   QStringLiteral("incomplete_but_recoverable"),
					   QStringLiteral("boundary_recoverable"),
					   QStringLiteral("approved_corrected_range"),
					   QStringLiteral("semantic_positive_example"),
					   QStringLiteral("ignore_for_training"),
					   QStringLiteral("weak_negative")};
		for (const QString &key : boolKeys) {
			if (structuredFeedback.contains(key))
				record.insert(QStringLiteral("feedback_%1").arg(key),
					      structuredFeedback.value(key).toBool());
		}
	}

	if (!structuredFeedback.isEmpty()) {
		const bool ignoreForTraining =
			explicitDecision == QStringLiteral("ignored_diagnostic") ||
			structuredFeedback.value(QStringLiteral("ignore_for_training")).toBool(false);
		const bool weakNegative = structuredFeedback.value(QStringLiteral("weak_negative")).toBool(false);
		const bool badTopic = !ignoreForTraining &&
				      structuredFeedback.value(QStringLiteral("bad_topic")).toBool(false);
		const bool boundaryRecoverable =
			!badTopic && !ignoreForTraining &&
			(structuredFeedback.value(QStringLiteral("boundary_recoverable")).toBool(false) ||
			 structuredFeedback.value(QStringLiteral("good_topic_bad_boundary")).toBool(false) ||
			 structuredFeedback.value(QStringLiteral("incomplete_but_recoverable")).toBool(false));
		const QString diagnosticRejection =
			structuredFeedback.value(QStringLiteral("diagnostic_rejection_reason")).toString();
		if (ignoreForTraining)
			record.insert(QStringLiteral("generated_feedback_class"), QStringLiteral("ignored_diagnostic"));
		else if (weakNegative)
			record.insert(QStringLiteral("generated_feedback_class"), QStringLiteral("weak_negative"));
		else if (badTopic)
			record.insert(QStringLiteral("generated_feedback_class"), QStringLiteral("bad_topic"));
		else if (boundaryRecoverable)
			record.insert(QStringLiteral("generated_feedback_class"),
				      QStringLiteral("good_topic_bad_boundary"));
		else if (diagnosticRejection.contains(QStringLiteral("incomplete_viewer_arc"), Qt::CaseInsensitive))
			record.insert(QStringLiteral("generated_feedback_class"),
				      QStringLiteral("incomplete_viewer_arc_unclassified"));
	}

	if (!humanReason.trimmed().isEmpty())
		record.insert(QStringLiteral("human_reason"), humanReason.trimmed().left(1000));
	return record;
}

QJsonObject addedUserRangeRecord(const QString &videoPath, const CurationSettings &settings,
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

bool appendJsonLines(const QString &path, const QVector<QJsonObject> &records)
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
} // namespace Curation::Feedback::Detail
