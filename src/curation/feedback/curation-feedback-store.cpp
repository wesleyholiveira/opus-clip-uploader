#include "curation/feedback/curation-feedback-store.hpp"
#include "curation/feedback/curation-feedback-detail.hpp"
#include "curation/curation-preset-profile.hpp"

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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Curation::Feedback {

using namespace Curation::Feedback::Detail;

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

QString CurationFeedbackStore::feedbackProfileDirectoryPath(const QString &profileId)
{
	const QString profile = Curation::normalizePresetProfileId(profileId);
	return QDir(feedbackDirectoryPath()).filePath(QStringLiteral("profiles/%1").arg(profile));
}

QString CurationFeedbackStore::feedbackJsonlPath()
{
	return QDir(feedbackDirectoryPath()).filePath(QStringLiteral("boundary-feedback.jsonl"));
}

QString CurationFeedbackStore::candidateSnapshotsJsonlPath()
{
	return QDir(feedbackDirectoryPath()).filePath(QStringLiteral("candidate-snapshots.jsonl"));
}

QString CurationFeedbackStore::calibrationJsonPath()
{
	return QDir(feedbackDirectoryPath()).filePath(QStringLiteral("boundary-calibration.json"));
}

QString CurationFeedbackStore::calibrationJsonPathForProfile(const QString &profileId)
{
	return QDir(feedbackProfileDirectoryPath(profileId)).filePath(QStringLiteral("boundary-calibration.json"));
}

QString CurationFeedbackStore::feedbackRankerModelPathForProfile(const QString &profileId, const QString &fileName)
{
	const QString cleanFileName = fileName.trimmed().isEmpty() ? QStringLiteral("feedback-ranker.json") : fileName.trimmed();
	return QDir(feedbackProfileDirectoryPath(profileId)).filePath(cleanFileName);
}

static QJsonObject loadCalibrationFile(const QString &path, const char *scope)
{
	QFile file(path);
	if (!file.exists() || !file.open(QIODevice::ReadOnly))
		return {};
	QJsonParseError error;
	const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		blog(LOG_WARNING, "[clip-cropper] Invalid %s boundary calibration JSON: %s", scope,
		     error.errorString().toUtf8().constData());
		return {};
	}
	blog(LOG_INFO, "[clip-cropper] Loaded %s boundary calibration JSON. path=%s", scope,
	     path.toUtf8().constData());
	return document.object();
}

QJsonObject CurationFeedbackStore::loadCalibrationRoot()
{
	return loadCalibrationFile(calibrationJsonPath(), "global");
}

QJsonObject CurationFeedbackStore::loadCalibrationRootForProfile(const QString &profileId)
{
	const QJsonObject profileRoot = loadCalibrationFile(calibrationJsonPathForProfile(profileId), "profile");
	if (!profileRoot.isEmpty())
		return profileRoot;
	return loadCalibrationRoot();
}


static bool purgeRangeIsValid(const ClipDuration &range)
{
	return std::isfinite(range.startSec) && std::isfinite(range.endSec) && range.endSec > range.startSec;
}

static bool purgeRangesAreSameMarker(const ClipDuration &recordRange, const ClipDuration &removedRange)
{
	if (!purgeRangeIsValid(recordRange) || !purgeRangeIsValid(removedRange))
		return false;

	const double overlap = Detail::overlapSec(recordRange, removedRange);
	if (overlap <= 0.0)
		return false;

	const double recordDuration = recordRange.endSec - recordRange.startSec;
	const double removedDuration = removedRange.endSec - removedRange.startSec;
	const double minDuration = std::max(0.001, std::min(recordDuration, removedDuration));
	const double unionDuration = std::max(recordRange.endSec, removedRange.endSec) -
			       std::min(recordRange.startSec, removedRange.startSec);
	const double iou = unionDuration > 0.0 ? overlap / unionDuration : 0.0;
	const double containment = overlap / minDuration;
	const double startDistance = std::fabs(recordRange.startSec - removedRange.startSec);
	const double endDistance = std::fabs(recordRange.endSec - removedRange.endSec);
	const double centerDistance = std::fabs(((recordRange.startSec + recordRange.endSec) * 0.5) -
					       ((removedRange.startSec + removedRange.endSec) * 0.5));

	return iou >= 0.72 ||
	       (startDistance <= 1.50 && endDistance <= 1.50) ||
	       (containment >= 0.92 && centerDistance <= 2.50);
}

static QVector<ClipDuration> purgeCandidateRangesFromRecord(const QJsonObject &record)
{
	QVector<ClipDuration> ranges;
	const auto appendRange = [&ranges, &record](const QString &startKey, const QString &endKey) {
		const ClipDuration range = Detail::recordRange(record, startKey, endKey);
		if (purgeRangeIsValid(range))
			ranges.append(range);
	};

	appendRange(QStringLiteral("user_start_sec"), QStringLiteral("user_end_sec"));
	appendRange(QStringLiteral("generated_start_sec"), QStringLiteral("generated_end_sec"));
	appendRange(QStringLiteral("diagnostic_original_start_sec"), QStringLiteral("diagnostic_original_end_sec"));
	appendRange(QStringLiteral("start_sec"), QStringLiteral("end_sec"));

	const QJsonObject structured = record.value(QStringLiteral("explicit_structured_feedback")).toObject();
	if (!structured.isEmpty()) {
		const auto appendStructuredRange = [&ranges, &structured](const QString &startKey, const QString &endKey) {
			const double startSec = structured.value(startKey).toDouble(std::numeric_limits<double>::quiet_NaN());
			const double endSec = structured.value(endKey).toDouble(std::numeric_limits<double>::quiet_NaN());
			const ClipDuration range{startSec, endSec};
			if (purgeRangeIsValid(range))
				ranges.append(range);
		};
		appendStructuredRange(QStringLiteral("range_start_sec"), QStringLiteral("range_end_sec"));
		appendStructuredRange(QStringLiteral("diagnostic_original_start_sec"), QStringLiteral("diagnostic_original_end_sec"));
	}

	return ranges;
}

static bool feedbackRecordMatchesRemovedMarker(const QJsonObject &record, const QString &videoId,
					       const QString &profileId, const QVector<ClipDuration> &removedRanges)
{
	if (removedRanges.isEmpty())
		return false;
	if (record.value(QStringLiteral("video_id")).toString() != videoId)
		return false;

	const QString recordPreset = record.value(QStringLiteral("training_profile")).toString(
		record.value(QStringLiteral("preset")).toString());
	if (!Detail::presetMatchesFeedback(recordPreset, profileId))
		return false;

	const QString decision = record.value(QStringLiteral("decision")).toString().trimmed().toLower();
	if (decision == QStringLiteral("removed_unrated"))
		return false;

	const QVector<ClipDuration> recordRanges = purgeCandidateRangesFromRecord(record);
	for (const ClipDuration &recordRange : recordRanges) {
		for (const ClipDuration &removedRange : removedRanges) {
			if (purgeRangesAreSameMarker(recordRange, removedRange))
				return true;
		}
	}
	return false;
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
		const QString recordPreset = record.value(QStringLiteral("training_profile")).toString(
			record.value(QStringLiteral("preset")).toString());
		if (!presetMatchesFeedback(recordPreset, presetId))
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
		if (isDefaultNoMarkerPlaceholderFeedback(record))
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

		if (decision == QStringLiteral("ignored_diagnostic") ||
		    explicitDecision == QStringLiteral("ignored_diagnostic") ||
		    structuredFeedbackIgnoresTraining(record)) {
			const QString ignoreReason =
				record.value(QStringLiteral("ignore_reason"))
					.toString(QStringLiteral("user_ignored_diagnostic_not_training_signal"));
			if (appendRangeSignal(memory.negativeRanges, generated, QStringLiteral("ignored_diagnostic"),
					      source, reasonForScope(ignoreReason), 0.08 * scopeWeight, rowSequence,
					      false, true, true))
				++memory.ignoredDiagnosticSignals;
			continue;
		}

		const bool recoverableBoundaryFeedback =
			structuredFeedbackMarksRecoverableBoundary(record) ||
			((decision == QStringLiteral("adjusted") || decision == QStringLiteral("approved_adjusted") ||
			  explicitDecision == QStringLiteral("approved_adjusted")) &&
			 diagnosticReasonIsIncompleteViewerArc(record) &&
			 feedbackRangeMeaningfullyEdited(generated, user));

		if (decision == QStringLiteral("rejected") || explicitDecision == QStringLiteral("disliked")) {
			const bool weakNegativeFeedback = !structuredFeedbackMarksBadTopic(record) &&
							  (structuredFeedbackMarksWeakNegative(record) ||
							   diagnosticReasonIsExploratoryOrLowSignal(record));
			const QString defaultReason =
				recoverableBoundaryFeedback
					? QStringLiteral("user_rejected_recoverable_boundary_not_bad_topic")
				: weakNegativeFeedback ? QStringLiteral("user_rejected_diagnostic_weak_negative")
						       : QStringLiteral("user_rejected_range");
			const double negativeWeight = recoverableBoundaryFeedback ? 0.36
						      : weakNegativeFeedback      ? 0.18
										  : 1.0;
			if (appendRangeSignal(
				    memory.negativeRanges, generated, decision, source,
				    reasonForScope(
					    record.value(QStringLiteral("reject_reason")).toString(defaultReason)),
				    negativeWeight * scopeWeight, rowSequence, false, weakNegativeFeedback, false)) {
				++memory.rejectedNegativeSignals;
				if (weakNegativeFeedback)
					++memory.weakNegativeSignals;
			}
			continue;
		}

		if (decision == QStringLiteral("approved_adjusted") ||
		    explicitDecision == QStringLiteral("approved_adjusted")) {
			const QString originalReason =
				recoverableBoundaryFeedback
					? QStringLiteral("user_approved_recoverable_boundary_original")
					: QStringLiteral("user_approved_corrected_original_boundaries");
			const double originalWeight = recoverableBoundaryFeedback ? 0.34 : 0.58;
			if (appendRangeSignal(memory.negativeRanges, generated, QStringLiteral("approved_adjusted"),
					      source, reasonForScope(originalReason), originalWeight * scopeWeight,
					      rowSequence))
				++memory.adjustedNegativeSignals;
			const ClipDuration positiveRange = validFeedbackRange(user) ? user : generated;
			if (appendRangeSignal(
				    memory.positiveRanges, positiveRange, QStringLiteral("approved_adjusted"), source,
				    reasonForScope(QStringLiteral("user_approved_corrected_range_complete_clip")),
				    1.12 * scopeWeight, rowSequence, true)) {
				++memory.approvedAdjustedPositiveSignals;
				countPositiveSemanticEligibility(memory, true);
			}
			continue;
		}

		if (decision == QStringLiteral("adjusted")) {
			const QString originalReason =
				recoverableBoundaryFeedback
					? QStringLiteral("user_adjusted_recoverable_boundary_original")
					: QStringLiteral("user_adjusted_generated_boundaries");
			const double originalWeight = recoverableBoundaryFeedback ? 0.38 : 0.72;
			if (appendRangeSignal(memory.negativeRanges, generated, decision, source,
					      reasonForScope(originalReason), originalWeight * scopeWeight,
					      rowSequence))
				++memory.adjustedNegativeSignals;
			if (feedbackRangeMeaningfullyEdited(generated, user)) {
				const bool semanticPrototypeEligible =
					structuredFeedbackForcesSemanticPositive(record) ||
					structuredFeedbackDescribesCompleteClip(record);
				if (appendRangeSignal(
					    memory.positiveRanges, user, decision, source,
					    reasonForScope(
						    semanticPrototypeEligible
							    ? QStringLiteral("user_corrected_range_complete_clip")
							    : QStringLiteral("user_corrected_range_boundary_only")),
					    0.88 * scopeWeight, rowSequence, semanticPrototypeEligible)) {
					++memory.adjustedPositiveSignals;
					countPositiveSemanticEligibility(memory, semanticPrototypeEligible);
				}
			} else
				++memory.adjustedWithoutEditedRangeSignals;
			continue;
		}

		if (decision == QStringLiteral("accepted") || explicitDecision == QStringLiteral("liked")) {
			if (appendRangeSignal(memory.positiveRanges, validFeedbackRange(user) ? user : generated,
					      decision, source, reasonForScope(QStringLiteral("user_accepted_range")),
					      1.0 * scopeWeight, rowSequence, true)) {
				++memory.acceptedPositiveSignals;
				countPositiveSemanticEligibility(memory, true);
			}
			continue;
		}

		if (decision == QStringLiteral("added_by_user")) {
			if (appendRangeSignal(memory.positiveRanges, user, decision, source,
					      reasonForScope(QStringLiteral("user_added_marker")), 0.90 * scopeWeight,
					      rowSequence, true)) {
				++memory.addedByUserPositiveSignals;
				countPositiveSemanticEligibility(memory, true);
			}
		}
	}

	memory.negativeSignalsBeforeConflictResolution = static_cast<int>(memory.negativeRanges.size());
	memory.positiveSignalsBeforeConflictResolution = static_cast<int>(memory.positiveRanges.size());
	resolvePositiveNegativeConflicts(memory);
	memory.prunedNegativeSignals =
		memory.negativeSignalsBeforeConflictResolution - static_cast<int>(memory.negativeRanges.size());
	memory.prunedPositiveSignals =
		memory.positiveSignalsBeforeConflictResolution - static_cast<int>(memory.positiveRanges.size());
	memory.loaded = memory.recordsRead > 0;
	if (memory.loaded) {
		blog(LOG_INFO,
		     "[clip-cropper] Loaded boundary feedback range memory. video=%s preset=%s records=%d exact=%d sameContent=%d legacySameContent=%d crossVideoColdStart=%d negative=%d positive=%d rawNegative=%d rawPositive=%d rejectedNegative=%d weakNegative=%d ignoredDiagnostic=%d adjustedNegative=%d adjustedPositive=%d adjustedWithoutEditedRange=%d acceptedPositive=%d approvedAdjustedPositive=%d addedByUserPositive=%d semanticPrototypePositive=%d boundaryOnlyPositive=%d prunedNegative=%d prunedPositive=%d",
		     videoPath.toUtf8().constData(), presetId.toUtf8().constData(), memory.recordsRead,
		     memory.exactRecordsRead, memory.contentRecordsRead, memory.legacyContentRecordsRead,
		     memory.crossVideoRecordsRead, static_cast<int>(memory.negativeRanges.size()),
		     static_cast<int>(memory.positiveRanges.size()), memory.negativeSignalsBeforeConflictResolution,
		     memory.positiveSignalsBeforeConflictResolution, memory.rejectedNegativeSignals,
		     memory.weakNegativeSignals, memory.ignoredDiagnosticSignals, memory.adjustedNegativeSignals,
		     memory.adjustedPositiveSignals, memory.adjustedWithoutEditedRangeSignals,
		     memory.acceptedPositiveSignals, memory.approvedAdjustedPositiveSignals,
		     memory.addedByUserPositiveSignals, memory.semanticPrototypePositiveSignals,
		     memory.boundaryOnlyPositiveSignals, memory.prunedNegativeSignals, memory.prunedPositiveSignals);

	} else if (!currentContentIds.isEmpty()) {
		blog(LOG_INFO,
		     "[clip-cropper] No boundary feedback range memory matched. video=%s preset=%s contentIds=%d",
		     videoPath.toUtf8().constData(), presetId.toUtf8().constData(),
		     static_cast<int>(currentContentIds.size()));
	}
	return memory;
}


FeedbackPurgeResult CurationFeedbackStore::removeFeedbackForRanges(const QString &videoPath,
								   const CurationSettings &settings,
								   const QVector<ClipDuration> &ranges,
								   const QString &reason)
{
	FeedbackPurgeResult result;
	QVector<ClipDuration> removedRanges;
	removedRanges.reserve(ranges.size());
	for (const ClipDuration &range : ranges) {
		if (purgeRangeIsValid(range))
			removedRanges.append(range);
	}
	if (removedRanges.isEmpty())
		return result;

	result.attempted = true;
	const QString path = feedbackJsonlPath();
	QFile file(path);
	if (!file.exists()) {
		result.succeeded = true;
		return result;
	}
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		result.error = QStringLiteral("failed_to_open_feedback_for_read");
		return result;
	}

	const QString videoId = stableVideoId(videoPath);
	const QString profileId = Curation::resolvePresetProfileId(settings, settings.aiPrompt);
	QVector<QByteArray> keptLines;
	while (!file.atEnd()) {
		const QByteArray rawLine = file.readLine();
		const QByteArray trimmed = rawLine.trimmed();
		if (trimmed.isEmpty()) {
			keptLines.append(rawLine);
			continue;
		}
		QJsonParseError error;
		const QJsonDocument document = QJsonDocument::fromJson(trimmed, &error);
		if (error.error != QJsonParseError::NoError || !document.isObject()) {
			keptLines.append(rawLine);
			continue;
		}
		++result.recordsRead;
		if (feedbackRecordMatchesRemovedMarker(document.object(), videoId, profileId, removedRanges)) {
			++result.recordsRemoved;
			continue;
		}
		keptLines.append(rawLine);
	}
	file.close();

	if (result.recordsRemoved <= 0) {
		result.succeeded = true;
		return result;
	}

	const QString timestamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
	const QString backupPath = QDir(feedbackDirectoryPath()).filePath(
		QStringLiteral("boundary-feedback.before-marker-delete-%1.jsonl").arg(timestamp));
	if (!QFile::copy(path, backupPath)) {
		result.error = QStringLiteral("failed_to_backup_feedback_before_marker_delete");
		return result;
	}
	result.backupPath = backupPath;

	QSaveFile output(path);
	if (!output.open(QIODevice::WriteOnly | QIODevice::Text)) {
		result.error = QStringLiteral("failed_to_open_feedback_for_rewrite");
		return result;
	}
	for (QByteArray line : keptLines) {
		if (!line.endsWith('\n'))
			line.append('\n');
		if (output.write(line) != line.size()) {
			result.error = QStringLiteral("failed_to_write_rewritten_feedback");
			return result;
		}
	}
	if (!output.commit()) {
		result.error = QStringLiteral("failed_to_commit_rewritten_feedback");
		return result;
	}

	result.succeeded = true;
	blog(LOG_INFO,
	     "[clip-cropper] Removed persisted feedback for deleted review marker. video=%s profile=%s ranges=%d recordsRemoved=%d backup=%s reason=%s",
	     videoPath.toUtf8().constData(), profileId.toUtf8().constData(), static_cast<int>(removedRanges.size()),
	     result.recordsRemoved, backupPath.toUtf8().constData(), reason.toUtf8().constData());
	return result;
}

bool CurationFeedbackStore::appendReviewFeedback(const QString &videoPath, const CurationSettings &settings,
						 const FeedbackSuggestionSnapshot &suggestion,
						 const QVector<ClipDuration> &userRanges, const QString &eventName,
						 const QString &humanReason,
						 const QMap<int, QString> &explicitDecisionsBySuggestedIndex,
						 const QMap<int, QJsonObject> &explicitFeedbackBySuggestedIndex)
{
	if (!hasUsefulSuggestion(suggestion))
		return false;

	const QString dir = feedbackDirectoryPath();
	if (!ensureDirectory(dir)) {
		blog(LOG_ERROR, "[clip-cropper] Failed to create feedback directory: %s", dir.toUtf8().constData());
		return false;
	}

	QVector<QJsonObject> snapshotRecords;
	QVector<QJsonObject> records;
	QSet<int> usedUserRanges;
	for (int i = 0; i < suggestion.ranges.size(); ++i) {
		const ClipDuration &suggested = suggestion.ranges.at(i);
		if (!std::isfinite(suggested.startSec) || !std::isfinite(suggested.endSec) ||
		    suggested.endSec <= suggested.startSec)
			continue;
		snapshotRecords.append(candidateSnapshotRecord(videoPath, settings, suggestion, i, suggested,
								     QStringLiteral("generated_candidate")));
		records.append(suggestionRecord(videoPath, settings, suggestion, i, suggested, userRanges,
						usedUserRanges, eventName, humanReason,
						explicitDecisionsBySuggestedIndex, explicitFeedbackBySuggestedIndex));
	}

	for (int i = 0; i < userRanges.size(); ++i) {
		if (usedUserRanges.contains(i))
			continue;
		const ClipDuration &range = userRanges.at(i);
		if (!std::isfinite(range.startSec) || !std::isfinite(range.endSec) || range.endSec <= range.startSec)
			continue;
		snapshotRecords.append(userAddedSnapshotRecord(videoPath, settings, suggestion, i, range, eventName));
		records.append(addedUserRangeRecord(videoPath, settings, suggestion, i, range, eventName));
	}

	const QString snapshotPath = candidateSnapshotsJsonlPath();
	const bool snapshotsSaved = appendJsonLines(snapshotPath, snapshotRecords);
	if (!snapshotsSaved) {
		blog(LOG_ERROR,
		     "[clip-cropper] Candidate snapshot append failed. path=%s records=%d event=%s video=%s",
		     snapshotPath.toUtf8().constData(), static_cast<int>(snapshotRecords.size()),
		     eventName.toUtf8().constData(), videoPath.toUtf8().constData());
		return false;
	}

	const QString path = feedbackJsonlPath();
	const bool saved = appendJsonLines(path, records);
	blog(saved ? LOG_INFO : LOG_ERROR,
	     "[clip-cropper] Boundary feedback append %s. path=%s records=%d snapshots=%d event=%s video=%s",
	     saved ? "succeeded" : "failed", path.toUtf8().constData(), static_cast<int>(records.size()),
	     static_cast<int>(snapshotRecords.size()), eventName.toUtf8().constData(), videoPath.toUtf8().constData());
	return saved;
}

} // namespace Curation::Feedback
