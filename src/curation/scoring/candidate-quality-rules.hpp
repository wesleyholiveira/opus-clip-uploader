#pragma once

#include "curation/scoring/candidate-quality-gate.hpp"

#include <QString>
#include <QVector>

#include <initializer_list>

namespace Curation::Scoring::CandidateQualityRules {

bool isNeutralRerankerFailureReason(const QString &failureReason);
double boundedScore(double value);
double durationSec(const ClipCandidate &candidate);
bool hasEvidence(const ClipCandidate &candidate, const QString &value);
bool hasEvidenceContaining(const ClipCandidate &candidate, const QString &fragment);
bool isFeedbackPositiveGroundTruth(const ClipCandidate &candidate);
bool isFeedbackNegativeBlocked(const ClipCandidate &candidate);
QString foldedQualityText(QString value);
bool qualityTextContainsAny(const QString &text, std::initializer_list<const char *> needles);
bool hasHardLearnedContextBlocker(const ClipCandidate &candidate);
bool evidenceContainsFragment(const ClipCandidate &candidate, const QString &fragment);
bool hasArcRole(const ClipCandidate &candidate, const QString &role);
bool arcRolesStartWith(const ClipCandidate &candidate, const QString &role);
bool hasResolutionOnlyArcRoles(const ClipCandidate &candidate);
bool hasDevelopmentOnlyArcRoles(const ClipCandidate &candidate);
bool hasInvalidContextualStateMachine(const ClipCandidate &candidate);
bool hasValidContextualStateMachine(const ClipCandidate &candidate);
bool requiresStrictContextualArc(const CandidateQualityGateOptions &options);
bool hasCompleteContextualArc(const ClipCandidate &candidate);
bool hasImplicitContextualOpening(const ClipCandidate &candidate);
QString arcRolesEvidence(const ClipCandidate &candidate);
int arcRoleCount(const ClipCandidate &candidate, const QString &role);
bool hasRecoveredContextualDpSubspan(const ClipCandidate &candidate);
bool hasStrongRecoveredContextualDpArc(const ClipCandidate &candidate);
bool hasResolutionPlateauWithoutOpening(const ClipCandidate &candidate);
bool hasSpecificMainTarget(const QString &target);
bool hasStrictReliableTargetSupport(const ClipCandidate &candidate, const CandidateQualityGateOptions &options);
bool hasStrongHumanContext(const ClipCandidate &candidate);
bool hasStrongRepairedResolutionOnlyArc(const ClipCandidate &candidate);
bool semanticSupportsCollapsedArcRoles(const ClipCandidate &candidate);
bool isWeakConclusionReason(const QString &reason);

} // namespace Curation::Scoring::CandidateQualityRules
