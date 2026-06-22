#pragma once

#include <QString>
#include <QStringList>

namespace Curation {

struct CurationPlan {
	QString presetId;
	QString arcType;
	QString mainTarget;
	QString contextPhrase;
	QString openingCriteria;
	QString developmentCriteria;
	QString continuityCriteria;
	QString endingCriteria;
	QString boundaryCriteria;
	bool noStrongClipFound = false;
	QString reason;
};

struct CurationDecision {
	CurationPlan plan;
	QString opusPrompt;
	QStringList validationIssues;
	bool usedFallback = false;
	bool blockedBySemanticGate = false;
};

} // namespace Curation
