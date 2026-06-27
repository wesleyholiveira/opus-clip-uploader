#pragma once

#include "curation/scoring/clip-scoring-pipeline.hpp"
#include "curation/feedback/curation-feedback-store.hpp"
#include "curation/scoring/boundary-refinement-stage.hpp"
#include "curation/scoring/candidate-beam-expander.hpp"
#include "curation/scoring/candidate-source-builder.hpp"
#include "curation/scoring/feedback-guided-candidate-stage.hpp"
#include "curation/scoring/feedback-similarity-scorer.hpp"
#include "curation/scoring/viewer-arc-gate.hpp"

#include <QString>
#include <QVector>

namespace Curation::Scoring::ClipScoringPipelineDetail {

struct NoveltyExplorationBuildStats {
	int rawCandidates = 0;
	int localAccepted = 0;
	int selectedForExpensiveScoring = 0;
	int workerCount = 1;
	long long generationMs = 0;
	long long localScoreMs = 0;
	long long diversityMs = 0;
	int reviewedSuppressed = 0;
};

enum class DiagnosticReviewedRangeMode {
	Strict,
	RelaxedExploration,
};
bool isViewerMessagePreset(const ClipScoringPipelineOptions &options);
CandidateSourceBuilderOptions candidateSourceOptionsFromOptions(const ClipScoringPipelineOptions &options);
bool isSelectionEvidence(const QString &evidence);
void clearSelectionMetadata(QVector<ClipCandidate> &candidates);
int usableCandidateCount(const QVector<ClipCandidate> &candidates);
int rejectedCandidateCount(const QVector<ClipCandidate> &candidates);
int positiveFeedbackCandidateCount(const QVector<ClipCandidate> &candidates);
int learnedFeedbackAcceptedCandidateCount(const QVector<ClipCandidate> &candidates);
bool isRejectedCandidate(const ClipCandidate &candidate);
bool candidateHasEvidence(const ClipCandidate &candidate, const QString &needle);
bool isExactPositiveFeedbackSeed(const ClipCandidate &candidate);
bool isUnsafeArcBypass(const ClipCandidate &candidate);
ClipCandidate demoteUnsafeArcBypass(ClipCandidate candidate);
bool hasSimilarDiagnosticRange(const QVector<ClipCandidate> &diagnostics, const ClipDuration &range);
double candidateEvidenceScoreValue(const ClipCandidate &candidate, const QString &prefix);
bool hasHardArcBlockerEvidence(const ClipCandidate &candidate);
bool isRecoverableIncompleteArcDiagnostic(const ClipCandidate &candidate);
QVector<ClipCandidate> tentativeRecoverableReviewMarkersFromDiagnostics(const QVector<ClipCandidate> &diagnostics,
									int limit);
bool isFeedbackSuppressedRejectedCandidate(const ClipCandidate &candidate);
QString diagnosticEvidenceSummary(const ClipCandidate &candidate);
double diagnosticRangeCenter(const ClipDuration &range);
bool hasNearbyDiagnosticReviewCluster(const QVector<ClipCandidate> &diagnostics, const ClipDuration &range);
void appendUniqueDiagnosticCandidates(QVector<ClipCandidate> &target, const QVector<ClipCandidate> &candidates,
				      int limit);
void logRejectedCandidateDiagnostics(const QString &videoPath, const QString &stage,
				     const QVector<ClipCandidate> &diagnostics);
bool validMemoryRange(const ClipDuration &range);
double rangeOverlapSec(const ClipDuration &left, const ClipDuration &right);
bool rangeMatchesReviewedSignal(const ClipDuration &range, const Curation::Feedback::FeedbackRangeSignal &signal);
bool rangeWasAlreadyReviewedByFeedback(const ClipDuration &range,
				       const Curation::Feedback::FeedbackRangeMemory &memory);
bool rangeMatchesExactReviewedSignal(const ClipDuration &range, const Curation::Feedback::FeedbackRangeSignal &signal);
bool rangeWasExactlyReviewedByFeedback(const ClipDuration &range,
				       const Curation::Feedback::FeedbackRangeMemory &memory);
bool isIgnoredDiagnosticFeedbackSignal(const Curation::Feedback::FeedbackRangeSignal &signal);
bool rangeMatchesIgnoredDiagnosticNeighborhood(const ClipDuration &range,
					       const Curation::Feedback::FeedbackRangeSignal &signal);
bool rangeMatchesReviewedSignalForExploration(const ClipDuration &range,
					      const Curation::Feedback::FeedbackRangeSignal &signal);
bool rangeWasAlreadyReviewedByFeedbackForExploration(const ClipDuration &range,
						     const Curation::Feedback::FeedbackRangeMemory &memory);
bool diagnosticRangeWasAlreadyReviewed(const ClipDuration &range,
				       const Curation::Feedback::FeedbackRangeMemory *feedbackMemory,
				       DiagnosticReviewedRangeMode reviewedRangeMode);
QVector<ClipCandidate> topRejectedCandidatesForDiagnostics(
	const QVector<ClipCandidate> &candidates, int limit = 12,
	const Curation::Feedback::FeedbackRangeMemory *feedbackMemory = nullptr,
	DiagnosticReviewedRangeMode reviewedRangeMode = DiagnosticReviewedRangeMode::Strict);
QString feedbackGateRejectionDiagnostics(const QVector<ClipCandidate> &candidates);
int feedbackSuppressedCandidateCount(const QVector<ClipCandidate> &candidates);
int semanticPrototypePositiveRangeCount(const Curation::Feedback::FeedbackRangeMemory &memory);
bool positiveSignalCanReplayAsTimelineMarker(const Curation::Feedback::FeedbackRangeSignal &signal);
bool hasSimilarTimelineCandidateRange(const QVector<ClipCandidate> &candidates, const ClipDuration &range);
bool rangeMatchesExistingReviewRange(const ClipDuration &range, const QVector<ClipDuration> &existingRanges);
bool isStrictExactPositiveFeedbackSeedCandidate(const ClipCandidate &candidate);
bool isExactUserFeedbackSeedCandidate(const ClipCandidate &candidate);
bool exactSeedMatchesPositiveMemory(const ClipCandidate &candidate,
				    const Curation::Feedback::FeedbackRangeMemory &memory);
int suppressAlreadyReviewedExactPositiveSeeds(QVector<ClipCandidate> &candidates,
					      const Curation::Feedback::FeedbackRangeMemory &memory);
int suppressAlreadyReviewedFeedbackCandidates(QVector<ClipCandidate> &candidates,
					      const Curation::Feedback::FeedbackRangeMemory &memory);
int suppressExistingReviewRangeExactSeeds(QVector<ClipCandidate> &candidates,
					  const QVector<ClipDuration> &existingRanges);
QVector<ClipCandidate> replayPositiveFeedbackTimelineCandidates(const TranscriptIndex &index,
								const ClipScoringPipelineOptions &options,
								const Curation::Feedback::FeedbackRangeMemory &memory,
								int limit);
double localExplorationScore(const ClipCandidate &candidate, const FeedbackSimilarityFeatures &feedback);
bool hasSimilarExplorationRange(const QVector<ClipCandidate> &candidates, const ClipDuration &range);
QString noveltyRangeKey(const ClipDuration &range);
QVector<double> fastNoveltyDurations(const CandidateGenerationOptions &generation);
QVector<ClipCandidate> buildFastNoveltyRawCandidates(const TranscriptIndex &index,
						     const ClipScoringPipelineOptions &options, int maxRawCandidates);
QVector<ClipCandidate> buildNoveltyExplorationCandidates(const TranscriptIndex &index,
							 const ClipScoringPipelineOptions &options,
							 const Curation::Feedback::FeedbackRangeMemory &memory,
							 NoveltyExplorationBuildStats *stats = nullptr);
QVector<ClipCandidate> buildExhaustionCoverageDiagnostics(const TranscriptIndex &index,
							  const ClipScoringPipelineOptions &options,
							  const Curation::Feedback::FeedbackRangeMemory &memory,
							  int limit = 6);
QVector<ClipCandidate> explorationDiagnosticsFromCandidates(const QVector<ClipCandidate> &candidates,
							    const Curation::Feedback::FeedbackRangeMemory &memory,
							    int limit = 12);
QVector<ClipCandidate> noveltyTimelineCandidatesFromCandidates(const QVector<ClipCandidate> &candidates,
							       const Curation::Feedback::FeedbackRangeMemory &memory,
							       int limit = 4);
ViewerArcGateOptions viewerArcGateOptionsFromOptions(const ClipScoringPipelineOptions &options);
CandidateBeamExpansionOptions beamExpansionOptionsFromOptions(const ClipScoringPipelineOptions &options);
BoundaryRefinementStageOptions boundaryRefinementOptionsFromOptions(const ClipScoringPipelineOptions &options);
FeedbackGuidedCandidateStageOptions feedbackGuidedStageOptionsFromOptions(const ClipScoringPipelineOptions &options);

} // namespace Curation::Scoring::ClipScoringPipelineDetail
