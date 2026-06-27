#include "curation/scoring/clip-scoring-pipeline-detail.hpp"

#include "curation/scoring/boundary-refinement-stage.hpp"
#include "curation/scoring/candidate-beam-expander.hpp"
#include "curation/scoring/candidate-source-builder.hpp"
#include "curation/scoring/cheap-clip-scorer.hpp"
#include "curation/scoring/feedback-guided-candidate-stage.hpp"
#include "curation/scoring/viewer-arc-gate.hpp"

namespace Curation::Scoring::ClipScoringPipelineDetail {

ViewerArcGateOptions viewerArcGateOptionsFromOptions(const ClipScoringPipelineOptions &options)
{
	ViewerArcGateOptions gateOptions;
	gateOptions.enabled = isViewerMessagePreset(options);
	gateOptions.presetId = options.scoring.presetId;
	gateOptions.searchRange = options.generation.searchRange;
	gateOptions.minDurationSec = options.generation.minDurationSec;
	gateOptions.maxDurationSec = options.generation.maxDurationSec;
	gateOptions.mainTarget = options.scoring.mainTarget;
	gateOptions.reliableMainTarget = options.scoring.reliableMainTarget;
	return gateOptions;
}

CandidateSourceBuilderOptions candidateSourceOptionsFromOptions(const ClipScoringPipelineOptions &options)
{
	CandidateSourceBuilderOptions sourceOptions;
	sourceOptions.generation = options.generation;
	sourceOptions.scoring = options.scoring;
	sourceOptions.qualityGate = options.qualityGate;
	sourceOptions.maxRawCandidates = options.generation.maxRawCandidates;
	sourceOptions.viewerMessagePreset = isViewerMessagePreset(options);
	return sourceOptions;
}

CandidateBeamExpansionOptions beamExpansionOptionsFromOptions(const ClipScoringPipelineOptions &options)
{
	CandidateBeamExpansionOptions beamOptions;
	beamOptions.generation = options.generation;
	beamOptions.scoring = options.scoring;
	beamOptions.qualityGate = options.qualityGate;
	beamOptions.finalBudget = CandidateSourceBuilder::semanticCandidateBudget(
		candidateSourceOptionsFromOptions(options), options.budget.maxCandidatesBeforeEmbedding);
	beamOptions.viewerMessagePreset = isViewerMessagePreset(options);
	return beamOptions;
}

BoundaryRefinementStageOptions boundaryRefinementOptionsFromOptions(const ClipScoringPipelineOptions &options)
{
	BoundaryRefinementStageOptions refinementOptions;
	refinementOptions.generation = options.generation;
	refinementOptions.scoring = options.scoring;
	refinementOptions.qualityGate = options.qualityGate;
	refinementOptions.embeddingProvider = options.embeddingProvider;
	refinementOptions.viewerMessagePreset = isViewerMessagePreset(options);
	return refinementOptions;
}

FeedbackGuidedCandidateStageOptions feedbackGuidedStageOptionsFromOptions(const ClipScoringPipelineOptions &options)
{
	FeedbackGuidedCandidateStageOptions stageOptions;
	stageOptions.generation = options.generation;
	stageOptions.scoring = options.scoring;
	stageOptions.qualityGate = options.qualityGate;
	stageOptions.ranking = options.ranking;
	stageOptions.videoPath = options.videoPath;
	return stageOptions;
}

bool isViewerMessagePreset(const ClipScoringPipelineOptions &options)
{
	return options.scoring.presetId == QStringLiteral("viewer_message_response");
}
} // namespace Curation::Scoring::ClipScoringPipelineDetail
