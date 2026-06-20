#pragma once

#include "curation/compiler/opus-prompt-compiler.hpp"
#include "models/curation-settings.hpp"
#include "models/transcript.hpp"

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace Curation {

const char *semanticGateFailurePrefixLiteral();
const char *promptGenerationBlockedPrefixLiteral();
const char *opusPromptPrefixLiteral();

bool containsAnyPhrase(const QString &text, const QStringList &phrases);
QString lineValueForPrefix(const QString &text, const char *prefix);
int transcriptTextCharCount(const RecordingTranscript &transcript);
bool allowNoStrongFailure(const RecordingTranscript &selectedRangeTranscript,
				  const CurationSettings &curationSettings);
QString renderFallbackRubricTemplate(QString templateText, const CurationSettings &curationSettings);
QString jsonStringValue(const QJsonObject &object, const QString &key);
QString cleanPlanPhrase(QString phrase);
QString compactPlanPhrase(QString phrase, int maxChars);
QString stripLeadingFindTarget(QString target);
QString arcVerbForType(QString arcType);
bool shouldRenderMultipleClips(const RecordingTranscript &selectedRangeTranscript,
			       const CurationSettings &curationSettings, const QString &hint = {});
QString scopeFindPhrase(const RecordingTranscript &selectedRangeTranscript, const CurationSettings &curationSettings,
			const QString &hint = {});
QString trimmedJoinNonEmpty(const QStringList &parts, const QString &separator);
bool containsPortuguesePromptMarkers(const QString &text);
bool hasEnglishOnlyPromptText(const QString &text);
QString englishOnlyPresetFallback(const RecordingTranscript &selectedRangeTranscript,
				  const CurationSettings &curationSettings, const QString &hint = {});

bool isGenericViewerContext(const QString &contextPhrase);
bool looksLikePortugueseViewerTarget(const QString &target);
bool isGenericViewerTarget(const QString &target);
QString cleanViewerTarget(QString target);
QString explicitViewerTargetFromSettings(const CurationSettings &curationSettings);
bool isStructuredViewerMessagePrompt(const QString &prompt);
QString viewerMessageTargetSuffix(QString target, QString contextPhrase);
QString viewerTargetFromRenderedPrompt(const QString &prompt);
bool isSafeViewerStopPhrase(const QString &phrase);
QString viewerStopPhrase(QString ending);
QString renderViewerMessagePlanPrompt(const QJsonObject &plan, const RecordingTranscript &selectedRangeTranscript,
				      const CurationSettings &curationSettings, const QString &planHint);

QJsonObject extractJsonObjectFromText(const QString &rawOutput);
QString firstCleanTopicKeyword(const CurationSettings &curationSettings);
QString deterministicPromptTarget(const QString &generatedPrompt, const RecordingTranscript &selectedRangeTranscript,
				  const CurationSettings &curationSettings);

} // namespace Curation
