#pragma once

#include "curation/feedback/curation-feedback-store.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QVector>

namespace Curation::Feedback::Detail {

QString obsConfigPath(const char *relativePath);
bool ensureDirectory(const QString &path);
QString stableVideoId(const QString &videoPath);
QString normalizeContentId(QString value);
QStringList normalizedContentIds(QStringList values);
QString fileContentIdUncached(const QString &videoPath);
QString fileContentIdCached(const QString &videoPath);
QStringList legacyContentIdsForRecord(const QJsonObject &record);
QString contentIdForSuggestion(const QString &videoPath, const FeedbackSuggestionSnapshot &suggestion);
QJsonArray contentAliasesToJson(const QStringList &aliases, const QString &primary);
void insertContentIdentity(QJsonObject &record, const QString &videoPath, const FeedbackSuggestionSnapshot &suggestion);
QString normalizedTranscriptText(QString value);
QJsonArray rangesToJson(const QVector<ClipDuration> &ranges);
QJsonArray topicKeywordsToJson(const QStringList &keywords);
double overlapSec(const ClipDuration &a, const ClipDuration &b);
double unionSec(const ClipDuration &a, const ClipDuration &b);
double rangeSimilarity(const ClipDuration &suggested, const ClipDuration &user);
int bestUserRangeIndexForSuggestion(const ClipDuration &suggested, const QVector<ClipDuration> &userRanges,
				    QSet<int> &usedUserRanges, double *outSimilarity);
QString startErrorType(double generatedStartSec, double userStartSec);
QString endErrorType(double generatedEndSec, double userEndSec);
QString matchedDecision(const ClipDuration &suggested, const ClipDuration &user);
QJsonObject diagnosticsForSuggestedRange(const FeedbackSuggestionSnapshot &suggestion, int suggestedIndex,
					 const ClipDuration &range);
QJsonObject suggestionRecord(const QString &videoPath, const CurationSettings &settings,
			     const FeedbackSuggestionSnapshot &suggestion, int suggestedIndex,
			     const ClipDuration &suggestedRange, const QVector<ClipDuration> &userRanges,
			     QSet<int> &usedUserRanges, const QString &eventName, const QString &humanReason,
			     const QMap<int, QString> &explicitDecisionsBySuggestedIndex,
			     const QMap<int, QJsonObject> &explicitFeedbackBySuggestedIndex);
QJsonObject addedUserRangeRecord(const QString &videoPath, const CurationSettings &settings,
				 const FeedbackSuggestionSnapshot &suggestion, int userIndex,
				 const ClipDuration &userRange, const QString &eventName);
bool appendJsonLines(const QString &path, const QVector<QJsonObject> &records);
bool validFeedbackRange(const ClipDuration &range);
bool feedbackRangeMeaningfullyEdited(const ClipDuration &generated, const ClipDuration &user);
ClipDuration recordRange(const QJsonObject &record, const QString &startKey, const QString &endKey);
bool structuredFeedbackBool(const QJsonObject &record, const QString &key, bool fallback = false);
QString structuredFeedbackString(const QJsonObject &record, const QString &key);
bool diagnosticReasonIsIncompleteViewerArc(const QJsonObject &record);
bool structuredFeedbackMarksBadTopic(const QJsonObject &record);
bool structuredFeedbackMarksRecoverableBoundary(const QJsonObject &record);
bool structuredFeedbackIgnoresTraining(const QJsonObject &record);
bool structuredFeedbackMarksWeakNegative(const QJsonObject &record);
bool diagnosticReasonIsExploratoryOrLowSignal(const QJsonObject &record);
bool structuredFeedbackDescribesCompleteClip(const QJsonObject &record);
bool structuredFeedbackForcesSemanticPositive(const QJsonObject &record);
void countPositiveSemanticEligibility(FeedbackRangeMemory &memory, bool semanticPrototypeEligible);
bool isDefaultNoMarkerPlaceholderFeedback(const QJsonObject &record);
bool presetMatchesFeedback(const QString &recordPreset, const QString &requestedPreset);
bool rangeLooksLikeColdStartPrelude(const ClipDuration &range);
bool feedbackDecisionCanBeUsedAcrossVideos(const QString &decision, const QString &explicitDecision);
QString crossVideoReason(const QString &reason);
bool appendRangeSignal(QVector<FeedbackRangeSignal> &results, const ClipDuration &range, const QString &decision,
		       const QString &source, const QString &reason, double weight, int sequence,
		       bool semanticPrototypeEligible = false, bool weakNegative = false,
		       bool ignoreForTraining = false);
double feedbackRangeDuration(const ClipDuration &range);
double feedbackRangeCenter(const ClipDuration &range);
double feedbackRangeOverlap(const ClipDuration &left, const ClipDuration &right);
bool feedbackSignalsConflict(const FeedbackRangeSignal &left, const FeedbackRangeSignal &right);
bool feedbackSignalReasonContains(const FeedbackRangeSignal &signal, const QString &needle);
bool adjustedGeneratedNegativeSignal(const FeedbackRangeSignal &signal);
bool adjustedCorrectedPositiveSignal(const FeedbackRangeSignal &signal);
bool approvedAdjustedOriginalNegativeSignal(const FeedbackRangeSignal &signal);
bool approvedAdjustedCorrectedPositiveSignal(const FeedbackRangeSignal &signal);
bool sameCorrectedFeedbackPair(const FeedbackRangeSignal &negative, const FeedbackRangeSignal &positive);
void resolvePositiveNegativeConflicts(FeedbackRangeMemory &memory);

} // namespace Curation::Feedback::Detail
