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
};

struct FeedbackRangeMemory {
	QVector<FeedbackRangeSignal> negativeRanges;
	QVector<FeedbackRangeSignal> positiveRanges;
	int recordsRead = 0;
	int exactRecordsRead = 0;
	int contentRecordsRead = 0;
	int crossVideoRecordsRead = 0;
	int legacyContentRecordsRead = 0;
	bool loaded = false;
};

class CurationFeedbackStore {
public:
	static bool hasUsefulSuggestion(const FeedbackSuggestionSnapshot &snapshot);

	static bool appendReviewFeedback(const QString &videoPath, const CurationSettings &settings,
		const FeedbackSuggestionSnapshot &suggestion, const QVector<ClipDuration> &userRanges,
		const QString &eventName, const QString &humanReason = {},
		const QMap<int, QString> &explicitDecisionsBySuggestedIndex = {});

	static QString feedbackDirectoryPath();
	static QString feedbackJsonlPath();
	static QString calibrationJsonPath();
	static QJsonObject loadCalibrationRoot();
	static QString transcriptContentId(const RecordingTranscript &transcript);
	static QString fileContentId(const QString &videoPath);
	static FeedbackRangeMemory loadRangeMemoryForVideo(const QString &videoPath, const QString &presetId,
		const QStringList &contentIds = QStringList{});
};

} // namespace Curation::Feedback
