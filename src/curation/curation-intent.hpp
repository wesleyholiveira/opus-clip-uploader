#pragma once

#include <QString>
#include <QVector>

namespace Curation {

enum class ContentKind {
	Unknown,
	LiveChat,
	Advice,
	EmotionalReaction,
	Explanation,
	Story,
	Opinion,
	Tutorial,
};

enum class ClipArchetype {
	Auto,
	ViewerMessageResponse,
	AdviceAnswer,
	EmotionalReaction,
	Explanation,
	StoryArc,
	Opinion,
	TutorialStep,
};

enum class ClipStrategy {
	BestMoment,
	MultipleIndependentClips,
};

enum class BoundaryPolicy {
	NaturalResolution,
	StopBeforeNextViewerMessage,
	StopBeforeHousekeeping,
	StopBeforeTopicShift,
};

struct Intent {
	QString requestedPresetId;
	QString resolvedPresetId;
	QString scope;
	double selectedDurationSec = 0.0;
	bool viewerSignals = false;
	ContentKind contentKind = ContentKind::Unknown;
	ClipArchetype archetype = ClipArchetype::Auto;
	ClipStrategy strategy = ClipStrategy::BestMoment;
	QVector<BoundaryPolicy> boundaryPolicies;
};

ClipArchetype archetypeFromPresetId(const QString &presetId);
ContentKind contentKindFromArchetype(ClipArchetype archetype, bool viewerSignals);
bool isMultipleClipStrategy(ClipStrategy strategy);

} // namespace Curation
