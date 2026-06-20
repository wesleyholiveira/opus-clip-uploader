#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

namespace Curation::Scoring::TextAnalysis {

QString stripDiacritics(const QString &value);
QString normalized(QString value);
QString sampleForLog(QString text, int maxChars = 100);
QStringList meaningfulTerms(const QString &text, const QSet<QString> &extraStopWords = QSet<QString>());
QSet<QString> contentTokens(const QString &text);
QSet<QString> semanticCategories(const QString &text);
QSet<QString> contentCategories(const QString &text);

bool containsAny(const QString &text, const QStringList &phrases);
bool normalizedContainsAny(const QString &normalizedText, const QStringList &markers);
bool isGenericFocusTarget(const QString &target);
bool looksLikeQuestionOrViewerMessage(const QString &text);
bool looksLikeSameExchangeContinuation(const QString &text);
bool looksLikeMentalHealthContext(const QString &text);
bool looksLikeGamblingContext(const QString &text);
bool looksLikeViewerContextPrelude(const QString &previousText, const QString &anchorText);
bool looksLikeHardTopicShift(const QString &text, const QString &contextText);
bool looksLikeSameTopicContinuation(const QString &text, const QString &contextText);
bool hasSharedSemanticTopic(const QString &text, const QString &contextText);
bool hasNoiseOnlySemanticTopic(const QString &text);
bool isBacklogOrGreetingText(const QString &text);
bool hasConcreteViewerQuestion(const QString &text);

double lexicalTopicOverlap(const QString &text, const QString &contextText);

} // namespace Curation::Scoring::TextAnalysis
