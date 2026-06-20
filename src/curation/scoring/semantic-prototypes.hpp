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
	QStringList hook;
	QStringList resolution;
	QStringList metaNoise;
};

const SemanticPrototypeSet &defaultSemanticPrototypes();
QStringList targetPrototypesForPreset(const QString &presetId, const QString &mainTarget);

} // namespace Curation::Scoring
