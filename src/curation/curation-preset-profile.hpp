#pragma once

#include "models/curation-settings.hpp"

#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

namespace Curation {

enum class PresetArchetype {
	Auto,
	ViewerMessageResponse,
	AdviceAnswer,
	EmotionalReaction,
	Explanation,
	StoryArc,
	Opinion,
	TutorialStep,
};

enum class PresetContentKind {
	Unknown,
	LiveChat,
	Advice,
	EmotionalReaction,
	Explanation,
	Story,
	Opinion,
	Tutorial,
};

enum class PresetClipLengthPolicy {
	None,
	Always,
	AutoOnly,
	UnlessLong,
};

struct ExchangeArcPolicy {
	bool preferSingleExchange = false;
	bool requireCleanOpening = true;
	bool requireDevelopment = true;
	bool requireLocalResolution = true;
	bool stopBeforeNextViewerTurn = true;
	bool stopBeforeTopicShift = true;
	bool stopBeforeStreamMeta = true;
	double minOpening = 0.30;
	double minDevelopment = 0.38;
	double minConclusion = 0.30;
	double minCompleteness = 0.40;
	double minBoundaryCleanliness = 0.34;
	double maxTailRisk = 0.72;
};

struct CurationPresetProfile {
	QString id;
	QString label;
	PresetArchetype archetype = PresetArchetype::Auto;
	PresetContentKind contentKind = PresetContentKind::Unknown;
	PresetClipLengthPolicy clipLengthPolicy = PresetClipLengthPolicy::None;
	double minDurationSec = 0.0;
	double maxDurationSec = 0.0;
	QString clipLengthSource = QStringLiteral("auto");
	bool allowMultipleClips = false;
	ExchangeArcPolicy arcPolicy;
	QStringList semanticFocusHints;
};

QString autoPresetProfileId();
QString viewerMessageResponsePresetProfileId();
QString normalizePresetProfileId(QString presetId);
QString resolvePresetProfileId(const CurationSettings &settings, const QString &prompt = QString());
QVector<CurationPresetProfile> presetProfiles();
QVector<QPair<QString, QString>> presetProfileOptions();
CurationPresetProfile presetProfileForId(const QString &presetId);
CurationPresetProfile presetProfileForSettings(const CurationSettings &settings, const QString &prompt = QString());
QString presetProfileLabelForId(const QString &presetId);
bool isViewerMessageResponsePrompt(const QString &prompt);

} // namespace Curation
