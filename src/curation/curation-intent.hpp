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
	StopAfterEmotionalPayoff,
	StopAfterStoryPayoff,
	StopAfterClaimResolution,
	StopAfterStepCompletion,
};

struct Intent {
	QString requestedPresetId;
	QString resolvedPresetId;
	QString scope;
	double selectedDurationSec = 0.0;
	double viewerExchangeScore = 0.0;
	double adviceScore = 0.0;
	double emotionalScore = 0.0;
	double explanationScore = 0.0;
	double storyScore = 0.0;
	double opinionScore = 0.0;
	double tutorialScore = 0.0;
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
