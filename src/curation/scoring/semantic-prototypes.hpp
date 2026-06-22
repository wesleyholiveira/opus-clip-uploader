#pragma once

#include <QString>
#include <QStringList>

namespace Curation::Scoring {

struct SemanticPrototypeSet {
	QStringList viewerMessage;
	QStringList directAnswer;
	QStringList greetingNoise;
	QStringList streamManagement;
	QStringList topicShift;
	QStringList clipValue;
	QStringList empathy;
	QStringList hook;
	QStringList resolution;
	QStringList metaNoise;
};

QString normalizedSemanticLanguageCode(const QString &transcriptionLanguage, const QString &sourceLanguage = {});
QString semanticLanguageDisplayName(const QString &languageCode);
bool isPortugueseSemanticLanguage(const QString &languageCode);
QString semanticLanguageInstruction(const QString &languageCode);
QString semanticDocumentPrefix(const QString &languageCode);

const SemanticPrototypeSet &defaultSemanticPrototypes();
const SemanticPrototypeSet &semanticPrototypesForLanguage(const QString &languageCode);
QStringList targetPrototypesForPreset(const QString &presetId, const QString &mainTarget);
QStringList targetPrototypesForPreset(const QString &presetId, const QString &mainTarget, const QString &languageCode);

} // namespace Curation::Scoring
