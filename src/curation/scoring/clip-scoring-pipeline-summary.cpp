#include "curation/scoring/clip-scoring-pipeline-summary.hpp"

#include <QMap>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Curation::Scoring::ClipScoringPipelineSummary {

QString build(const QVector<ClipCandidate> &candidates)
{
	if (candidates.isEmpty())
		return QStringLiteral("no_candidates: scoring_pipeline_found_no_viable_ranges");

	QVector<ClipCandidate> summaryCandidates = candidates;
	std::sort(summaryCandidates.begin(), summaryCandidates.end(),
		  [](const ClipCandidate &left, const ClipCandidate &right) {
			  if (left.selectedRank > 0 || right.selectedRank > 0) {
				  const int leftRank = left.selectedRank > 0 ? left.selectedRank
									     : std::numeric_limits<int>::max();
				  const int rightRank = right.selectedRank > 0 ? right.selectedRank
									       : std::numeric_limits<int>::max();
				  if (leftRank != rightRank)
					  return leftRank < rightRank;
			  }
			  if (std::fabs(left.scores.final - right.scores.final) > 0.0001)
				  return left.scores.final > right.scores.final;
			  return left.range.startSec < right.range.startSec;
		  });

	QStringList parts;
	const int limit =
		static_cast<int>(std::min(static_cast<long long>(5), static_cast<long long>(summaryCandidates.size())));
	for (int i = 0; i < limit; ++i) {
		const ClipCandidate &candidate = summaryCandidates.at(i);
		parts.append(
			QStringLiteral(
				"#%1 selectedRank=%2 %3-%4s score=%5 value=%6 hook=%7 openingHook=%8 resolution=%9 endingResolution=%10 metaNoise=%11 openingMeta=%12 endingMeta=%13 endingShift=%14 continuity=%15 arc=%16 arcOpen=%17 arcDev=%18 arcEnd=%19 arcClean=%20 arcTail=%21 semantic=%22 viewer=%23 boundary=%24 pauseBefore=%25 pauseAfter=%26 internalPause=%27 coarse=%28 reranker=%29 raw=%30 defect=%31 openDef=%32 endDef=%33 structDef=%34 margin=%35 source=%36 evidence=%37")
				.arg(i + 1)
				.arg(candidate.selectedRank > 0 ? candidate.selectedRank : i + 1)
				.arg(QString::number(candidate.range.startSec, 'f', 2))
				.arg(QString::number(candidate.range.endSec, 'f', 2))
				.arg(QString::number(candidate.scores.final, 'f', 2))
				.arg(QString::number(candidate.scores.semanticClipValue, 'f', 2))
				.arg(QString::number(candidate.scores.semanticHook, 'f', 2))
				.arg(QString::number(candidate.scores.semanticOpeningHook, 'f', 2))
				.arg(QString::number(candidate.scores.semanticResolution, 'f', 2))
				.arg(QString::number(candidate.scores.semanticEndingResolution, 'f', 2))
				.arg(QString::number(candidate.scores.semanticMetaNoise, 'f', 2))
				.arg(QString::number(candidate.scores.semanticOpeningMetaNoise, 'f', 2))
				.arg(QString::number(candidate.scores.semanticEndingMetaNoise, 'f', 2))
				.arg(QString::number(candidate.scores.semanticEndingTopicShift, 'f', 2))
				.arg(QString::number(candidate.scores.topicContinuity, 'f', 2))
				.arg(QString::number(candidate.scores.arcCompleteness, 'f', 2))
				.arg(QString::number(candidate.scores.arcOpening, 'f', 2))
				.arg(QString::number(candidate.scores.arcDevelopment, 'f', 2))
				.arg(QString::number(candidate.scores.arcConclusion, 'f', 2))
				.arg(QString::number(candidate.scores.arcBoundaryCleanliness, 'f', 2))
				.arg(QString::number(candidate.scores.arcTailRisk, 'f', 2))
				.arg(QString::number(candidate.scores.semanticTarget, 'f', 2))
				.arg(QString::number(candidate.scores.viewerResponse, 'f', 2))
				.arg(QString::number(candidate.scores.boundary, 'f', 2))
				.arg(QString::number(candidate.scores.pauseBeforeSec, 'f', 1))
				.arg(QString::number(candidate.scores.pauseAfterSec, 'f', 1))
				.arg(QString::number(candidate.scores.maxInternalPauseSec, 'f', 1))
				.arg(QString::number(candidate.scores.coarseSemantic, 'f', 2))
				.arg(QString::number(candidate.scores.reranker, 'f', 2))
				.arg(QString::number(candidate.scores.rerankerRaw, 'f', 2))
				.arg(QString::number(candidate.scores.rerankerBadClip, 'f', 2))
				.arg(QString::number(candidate.scores.rerankerOpeningDefect, 'f', 2))
				.arg(QString::number(candidate.scores.rerankerEndingDefect, 'f', 2))
				.arg(QString::number(candidate.scores.rerankerStructureDefect, 'f', 2))
				.arg(QString::number(candidate.scores.rerankerClipQualityMargin, 'f', 2))
				.arg(candidate.source)
				.arg(candidate.evidence.join(QLatin1Char('|'))));
	}
	return parts.join(QStringLiteral("; "));
}

QString rejection(const QVector<ClipCandidate> &candidates)
{
	if (candidates.isEmpty())
		return QStringLiteral("candidates=0");

	QMap<QString, int> rejectedReasons;
	int rejected = 0;
	int passed = 0;
	double bestFinal = 0.0;
	double bestSemantic = 0.0;
	double bestReranker = 0.0;
	double bestRaw = 0.0;
	double bestValue = 0.0;
	double bestHook = 0.0;
	double bestResolution = 0.0;
	double bestMetaNoise = 0.0;
	double bestOpeningHook = 0.0;
	double bestEndingResolution = 0.0;
	double bestBadClip = 0.0;
	double bestMargin = -1.0;
	QString firstFailure;
	QVector<ClipCandidate> topRejected;

	for (const ClipCandidate &candidate : candidates) {
		bestFinal = std::max(bestFinal, candidate.scores.final);
		bestSemantic = std::max(bestSemantic, candidate.scores.semanticTarget);
		bestReranker = std::max(bestReranker, candidate.scores.reranker);
		bestRaw = std::max(bestRaw, candidate.scores.rerankerRaw);
		bestValue = std::max(bestValue, candidate.scores.semanticClipValue);
		bestHook = std::max(bestHook, candidate.scores.semanticHook);
		bestResolution = std::max(bestResolution, candidate.scores.semanticResolution);
		bestMetaNoise = std::max(bestMetaNoise, candidate.scores.semanticMetaNoise);
		bestOpeningHook = std::max(bestOpeningHook, candidate.scores.semanticOpeningHook);
		bestEndingResolution = std::max(bestEndingResolution, candidate.scores.semanticEndingResolution);
		bestBadClip = std::max(bestBadClip, candidate.scores.rerankerBadClip);
		bestMargin = std::max(bestMargin, candidate.scores.rerankerClipQualityMargin);
		if (candidate.rejectedByQualityGate || candidate.rejectedAsNoise) {
			++rejected;
			const QString reason = candidate.rejectionReason.trimmed().isEmpty()
						       ? QStringLiteral("unknown")
						       : candidate.rejectionReason.trimmed().left(96);
			rejectedReasons[reason] = rejectedReasons.value(reason) + 1;
			topRejected.append(candidate);
			if (firstFailure.isEmpty() && !candidate.rerankerFailureReason.trimmed().isEmpty())
				firstFailure = candidate.rerankerFailureReason.trimmed().left(180);
		} else {
			++passed;
		}
	}

	QStringList reasonParts;
	for (auto it = rejectedReasons.constBegin(); it != rejectedReasons.constEnd(); ++it)
		reasonParts.append(QStringLiteral("%1=%2").arg(it.key()).arg(it.value()));

	std::sort(topRejected.begin(), topRejected.end(), [](const ClipCandidate &left, const ClipCandidate &right) {
		if (std::fabs(left.scores.final - right.scores.final) > 0.0001)
			return left.scores.final > right.scores.final;
		if (std::fabs(left.scores.rerankerRaw - right.scores.rerankerRaw) > 0.0001)
			return left.scores.rerankerRaw > right.scores.rerankerRaw;
		return left.range.startSec < right.range.startSec;
	});

	QStringList rejectedParts;
	const int rejectedLimit =
		static_cast<int>(std::min(static_cast<long long>(5), static_cast<long long>(topRejected.size())));
	for (int i = 0; i < rejectedLimit; ++i) {
		const ClipCandidate &candidate = topRejected.at(i);
		QStringList arcEvidence;
		for (const QString &evidence : candidate.evidence) {
			if (evidence.startsWith(QStringLiteral("exchange_arc_role_classifier:")) ||
			    evidence.startsWith(QStringLiteral("exchange_arc_state_machine:")) ||
			    evidence.startsWith(QStringLiteral("exchange_arc_roles:")) ||
			    evidence.startsWith(QStringLiteral("exchange_arc_contextual_role_scores:")) ||
			    evidence.startsWith(QStringLiteral("exchange_arc_refiner_skipped:")) ||
			    evidence == QStringLiteral("exchange_arc_refiner_returned_without_arc_evidence") ||
			    evidence ==
				    QStringLiteral("exchange_arc_refiner_returned_without_detailed_arc_evidence_v20") ||
			    evidence == QStringLiteral("exchange_arc_refiner_output_not_structurally_viable") ||
			    evidence == QStringLiteral("exchange_arc_no_valid_subspan") ||
			    evidence == QStringLiteral("exchange_arc_contextual_dp_subspan_recovered")) {
				arcEvidence.append(evidence.left(180));
			}
			if (arcEvidence.size() >= 6)
				break;
		}
		rejectedParts.append(
			QStringLiteral(
				"%1-%2:%3 final=%4 value=%5 hook=%6 res=%7 meta=%8 raw=%9 defect=%10 openDef=%11 endDef=%12 structDef=%13 margin=%14 arcEvidence=%15")
				.arg(QString::number(candidate.range.startSec, 'f', 1))
				.arg(QString::number(candidate.range.endSec, 'f', 1))
				.arg(candidate.rejectionReason.left(40))
				.arg(QString::number(candidate.scores.final, 'f', 2))
				.arg(QString::number(candidate.scores.semanticClipValue, 'f', 2))
				.arg(QString::number(candidate.scores.semanticHook, 'f', 2))
				.arg(QString::number(candidate.scores.semanticResolution, 'f', 2))
				.arg(QString::number(candidate.scores.semanticMetaNoise, 'f', 2))
				.arg(QString::number(candidate.scores.rerankerRaw, 'f', 2))
				.arg(QString::number(candidate.scores.rerankerBadClip, 'f', 2))
				.arg(QString::number(candidate.scores.rerankerOpeningDefect, 'f', 2))
				.arg(QString::number(candidate.scores.rerankerEndingDefect, 'f', 2))
				.arg(QString::number(candidate.scores.rerankerStructureDefect, 'f', 2))
				.arg(QString::number(candidate.scores.rerankerClipQualityMargin, 'f', 2))
				.arg(arcEvidence.isEmpty() ? QStringLiteral("none")
							   : arcEvidence.join(QStringLiteral("|"))));
	}

	QString summary =
		QStringLiteral(
			"candidates=%1 passed=%2 rejected=%3 bestFinal=%4 bestSemantic=%5 bestReranker=%6 bestRaw=%7 bestValue=%8 bestHook=%9 bestOpeningHook=%10 bestResolution=%11 bestEndingResolution=%12 bestMetaNoise=%13 bestBad=%14 bestMargin=%15 reasons=[%16]")
			.arg(static_cast<long long>(candidates.size()))
			.arg(passed)
			.arg(rejected)
			.arg(QString::number(bestFinal, 'f', 2))
			.arg(QString::number(bestSemantic, 'f', 2))
			.arg(QString::number(bestReranker, 'f', 2))
			.arg(QString::number(bestRaw, 'f', 2))
			.arg(QString::number(bestValue, 'f', 2))
			.arg(QString::number(bestHook, 'f', 2))
			.arg(QString::number(bestOpeningHook, 'f', 2))
			.arg(QString::number(bestResolution, 'f', 2))
			.arg(QString::number(bestEndingResolution, 'f', 2))
			.arg(QString::number(bestMetaNoise, 'f', 2))
			.arg(QString::number(bestBadClip, 'f', 2))
			.arg(QString::number(bestMargin, 'f', 2))
			.arg(reasonParts.join(QLatin1Char(',')));
	if (!rejectedParts.isEmpty())
		summary += QStringLiteral(" topRejected=[%1]").arg(rejectedParts.join(QStringLiteral("; ")));
	if (!firstFailure.isEmpty())
		summary += QStringLiteral(" firstRerankerFailure=%1").arg(firstFailure);
	return summary;
}

QString noCandidate(const QString &reason, const QVector<ClipCandidate> &candidates)
{
	return QStringLiteral("no_candidates: %1; %2").arg(reason, rejection(candidates));
}

} // namespace Curation::Scoring::ClipScoringPipelineSummary
