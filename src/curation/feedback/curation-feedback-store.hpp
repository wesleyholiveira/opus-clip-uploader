#pragma once

#include "models/curation-settings.hpp"
#include "models/transcript.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace Curation::Feedback {

struct FeedbackSuggestionSnapshot {
	QVector<ClipDuration> ranges;
	QString contentId;
	QStringList contentIdAliases;
	QString summary;
	QString source;
	QJsonArray candidateDiagnostics;
};

struct FeedbackRangeSignal {
	ClipDuration range;
	QString decision;
	QString source;
	QString reason;
	double weight = 1.0;
	int sequence = 0;
	bool semanticPrototypeEligible = false;
	bool weakNegative = false;
	bool ignoreForTraining = false;
};


struct FeedbackPurgeResult {
	bool attempted = false;
	bool succeeded = false;
	int recordsRead = 0;
	int recordsRemoved = 0;
	QString backupPath;
	QString error;
};

struct FeedbackRangeMemory {
	QVector<FeedbackRangeSignal> negativeRanges;
	QVector<FeedbackRangeSignal> positiveRanges;
	int recordsRead = 0;
	int exactRecordsRead = 0;
	int contentRecordsRead = 0;
	int crossVideoRecordsRead = 0;
	int legacyContentRecordsRead = 0;
	int rejectedNegativeSignals = 0;
	int weakNegativeSignals = 0;
	int ignoredDiagnosticSignals = 0;
	int adjustedNegativeSignals = 0;
	int adjustedPositiveSignals = 0;
	int adjustedWithoutEditedRangeSignals = 0;
	int acceptedPositiveSignals = 0;
	int approvedAdjustedPositiveSignals = 0;
	int addedByUserPositiveSignals = 0;
	int semanticPrototypePositiveSignals = 0;
	int boundaryOnlyPositiveSignals = 0;
	int negativeSignalsBeforeConflictResolution = 0;
	int positiveSignalsBeforeConflictResolution = 0;
	int prunedNegativeSignals = 0;
	int prunedPositiveSignals = 0;
	bool loaded = false;
};

class CurationFeedbackStore {
public:
	static bool hasUsefulSuggestion(const FeedbackSuggestionSnapshot &snapshot);

	static bool appendReviewFeedback(const QString &videoPath, const CurationSettings &settings,
					 const FeedbackSuggestionSnapshot &suggestion,
					 const QVector<ClipDuration> &userRanges, const QString &eventName,
					 const QString &humanReason = {},
					 const QMap<int, QString> &explicitDecisionsBySuggestedIndex = {},
					 const QMap<int, QJsonObject> &explicitFeedbackBySuggestedIndex = {});

	static QString feedbackDirectoryPath();
	static QString feedbackProfileDirectoryPath(const QString &profileId);
	static QString feedbackJsonlPath();
	static QString candidateSnapshotsJsonlPath();
	static QString calibrationJsonPath();
	static QString calibrationJsonPathForProfile(const QString &profileId);
	static QString feedbackRankerModelPathForProfile(const QString &profileId, const QString &fileName = {});
	static QJsonObject loadCalibrationRoot();
	static QJsonObject loadCalibrationRootForProfile(const QString &profileId);
	static QString transcriptContentId(const RecordingTranscript &transcript);
	static QString fileContentId(const QString &videoPath);
	static FeedbackRangeMemory loadRangeMemoryForVideo(const QString &videoPath, const QString &presetId,
							   const QStringList &contentIds = QStringList{});

	static FeedbackPurgeResult removeFeedbackForRanges(const QString &videoPath, const CurationSettings &settings,
							     const QVector<ClipDuration> &ranges,
							     const QString &reason = {});
};

} // namespace Curation::Feedback
