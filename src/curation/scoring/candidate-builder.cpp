#include "curation/scoring/candidate-builder.hpp"

#include <algorithm>
#include <cmath>

namespace Curation::Scoring {
namespace {

double boundedScore(double value)
{
	return std::clamp(value, 0.0, 1.0);
}

} // namespace

ClipCandidate CandidateBuilder::buildForRange(const TranscriptIndex &index,
	const CandidateGenerationOptions &generation,
	const CheapScoringContext &scoring,
	const CandidateQualityGateOptions &qualityGate,
	const ClipDuration &range,
	const SemanticCoarseRegion &region)
{
	ClipCandidate candidate;
	candidate.range = index.clampRange(range, generation.searchRange);
	candidate.firstSegmentIndex = index.firstSegmentIndexOverlapping(candidate.range);
	candidate.lastSegmentIndex = index.lastSegmentIndexOverlapping(candidate.range);
	candidate.text = index.textForRange(candidate.range).simplified();
	candidate.timedText = index.timedTextForRange(candidate.range);
	candidate.anchorText = candidate.text.left(220);
	candidate.source = QStringLiteral("semantic_coarse_region");
	candidate.hasReliableMainTarget = scoring.reliableMainTarget;
	candidate.scores.coarseSemantic = region.score;
	candidate.evidence.append(region.evidence);
	candidate.evidence.append(QStringLiteral("structural_candidate_from_coarse_region"));
	candidate.evidence.removeDuplicates();
	return scoreStructurally(index, candidate);
}

ClipCandidate CandidateBuilder::buildForSegmentWindow(const TranscriptIndex &index,
	const CandidateGenerationOptions &generation,
	const CheapScoringContext &scoring,
	const CandidateQualityGateOptions &qualityGate,
	int firstIndex,
	int lastIndex,
	const SemanticCoarseRegion &region)
{
	const TranscriptSegment *firstSegment = index.segmentAt(firstIndex);
	const TranscriptSegment *lastSegment = index.segmentAt(lastIndex);
	if (!firstSegment || !lastSegment || lastIndex < firstIndex)
		return {};
	return buildForRange(index, generation, scoring, qualityGate, {firstSegment->startSec, lastSegment->endSec}, region);
}

ClipCandidate CandidateBuilder::buildVariantFromSeed(const TranscriptIndex &index,
	const CandidateGenerationOptions &generation,
	const CheapScoringContext &scoring,
	const CandidateQualityGateOptions &qualityGate,
	const ClipCandidate &seed,
	const ClipDuration &range,
	const QString &variantEvidence)
{
	ClipCandidate candidate;
	candidate.range = index.clampRange(range, generation.searchRange);
	candidate.firstSegmentIndex = index.firstSegmentIndexOverlapping(candidate.range);
	candidate.lastSegmentIndex = index.lastSegmentIndexOverlapping(candidate.range);
	candidate.text = index.textForRange(candidate.range).simplified();
	candidate.timedText = index.timedTextForRange(candidate.range);
	candidate.anchorText = candidate.text.left(220);
	candidate.source = seed.source.trimmed().isEmpty() ? QStringLiteral("semantic_beam_variant")
		: seed.source + QStringLiteral("_beam");
	candidate.startsNearViewerCue = seed.startsNearViewerCue;
	candidate.endsBeforeNextCue = seed.endsBeforeNextCue;
	candidate.hasReliableMainTarget = seed.hasReliableMainTarget;
	candidate.scores.coarseSemantic = seed.scores.coarseSemantic;
	candidate.evidence = seed.evidence;
	candidate.evidence.append(QStringLiteral("candidate_beam_variant"));
	candidate.evidence.append(variantEvidence);
	const bool seedWasPositiveExact = seed.source == QStringLiteral("feedback_positive_exact_seed") ||
		seed.evidence.contains(QStringLiteral("feedback_positive_exact_seed"));
	if (seedWasPositiveExact) {
		const bool sameRange = std::fabs(seed.range.startSec - candidate.range.startSec) <= 0.75 &&
			std::fabs(seed.range.endSec - candidate.range.endSec) <= 0.75;
		if (sameRange) {
			candidate.source = QStringLiteral("feedback_positive_exact_seed");
			candidate.evidence.append(QStringLiteral("feedback_positive_exact_seed_preserved"));
		} else {
			candidate.source = QStringLiteral("feedback_positive_boundary_variant");
			candidate.evidence.removeAll(QStringLiteral("feedback_positive_exact_seed"));
			candidate.evidence.removeAll(QStringLiteral("feedback_positive_exact_seed_preserved"));
			candidate.evidence.append(QStringLiteral("feedback_positive_boundary_variant_from_user_seed"));
		}
	}
	candidate.evidence.removeDuplicates();
	return scoreStructurally(index, candidate);
}

ClipCandidate CandidateBuilder::scoreStructurally(const TranscriptIndex &index, const ClipCandidate &candidate)
{
	ClipCandidate scored = candidate;
	if (scored.timedText.trimmed().isEmpty())
		scored.timedText = index.timedTextForRange(scored.range);
	const double durationSec = scored.range.endSec - scored.range.startSec;
	scored.scores.duration = durationScore(durationSec);
	scored.scores.pauseBeforeSec = index.silenceBeforeRange(scored.range);
	scored.scores.pauseAfterSec = index.silenceAfterRange(scored.range);
	scored.scores.maxInternalPauseSec = index.maxInternalSilenceInRange(scored.range);
	scored.scores.pauseBoundary = boundedScore((std::min(scored.scores.pauseBeforeSec, 4.0) * 0.12) +
		(std::min(scored.scores.pauseAfterSec, 4.0) * 0.18));
	scored.scores.boundary = boundedScore(boundaryScore(index, scored) + (scored.scores.pauseBoundary * 0.18));
	const double coarseScore = scored.scores.coarseSemantic;
	const double charsPerSecond = durationSec > 0.0 ? static_cast<double>(scored.text.trimmed().size()) / durationSec : 0.0;
	const double textDensityScore = boundedScore(charsPerSecond / 8.0);
	scored.scores.final = boundedScore((coarseScore * 0.34) + (scored.scores.boundary * 0.26) +
		(scored.scores.duration * 0.22) + (textDensityScore * 0.18));
	if (textDensityScore >= 0.55)
		scored.evidence.append(QStringLiteral("speech_density_ok"));
	if (scored.scores.boundary >= 0.7)
		scored.evidence.append(QStringLiteral("clean_boundary"));
	if (scored.scores.pauseAfterSec >= 3.0)
		scored.evidence.append(QStringLiteral("pause_after:%1").arg(QString::number(scored.scores.pauseAfterSec, 'f', 1)));
	if (scored.scores.maxInternalPauseSec >= 2.0)
		scored.evidence.append(QStringLiteral("internal_pause:%1").arg(QString::number(scored.scores.maxInternalPauseSec, 'f', 1)));
	scored.evidence.append(QStringLiteral("structural_score_only"));
	scored.evidence.removeDuplicates();
	return scored;
}

bool CandidateBuilder::isStructurallyViable(const ClipCandidate &candidate,
	const CandidateGenerationOptions &generation,
	const CandidateQualityGateOptions &qualityGate)
{
	const double durationSec = candidate.range.endSec - candidate.range.startSec;
	if (durationSec < generation.minDurationSec || durationSec > generation.maxDurationSec + 3.0)
		return false;
	if (candidate.firstSegmentIndex < 0 || candidate.lastSegmentIndex < candidate.firstSegmentIndex)
		return false;

	const QString text = candidate.text.trimmed();
	if (text.size() < std::max(50, qualityGate.minTextChars))
		return false;

	const double charsPerSecond = durationSec > 0.0 ? static_cast<double>(text.size()) / durationSec : 0.0;
	return charsPerSecond >= 1.2;
}

QVector<ClipCandidate> CandidateBuilder::enforceSemanticAvailability(QVector<ClipCandidate> candidates,
	bool requireSemanticScoring,
	bool embeddingProviderConfigured)
{
	if (!requireSemanticScoring || !embeddingProviderConfigured)
		return candidates;

	for (ClipCandidate &candidate : candidates) {
		if (candidate.semanticScoringAvailable)
			continue;
		candidate.rejectedByQualityGate = true;
		candidate.qualityGateChecked = true;
		if (candidate.evidence.contains(QStringLiteral("semantic_embedding_failed")))
			candidate.rejectionReason = QStringLiteral("semantic_embedding_failed");
		else
			candidate.rejectionReason = QStringLiteral("semantic_embedding_unavailable");
		candidate.evidence.append(QStringLiteral("quality_rejected:%1").arg(candidate.rejectionReason));
		candidate.evidence.removeDuplicates();
	}
	return candidates;
}

double CandidateBuilder::durationScore(double durationSec)
{
	if (durationSec <= 0.0)
		return 0.0;
	if (durationSec < 12.0)
		return 0.15;
	if (durationSec < 18.0)
		return 0.45;
	if (durationSec <= 120.0)
		return 1.0;
	if (durationSec <= 180.0)
		return 0.90;
	return 0.30;
}

double CandidateBuilder::boundaryScore(const TranscriptIndex &index, const ClipCandidate &candidate)
{
	double score = 0.0;
	const double silenceBefore = index.silenceBeforeRange(candidate.range);
	const double silenceAfter = index.silenceAfterRange(candidate.range);

	if (silenceBefore >= 0.35)
		score += 0.25;
	if (silenceBefore >= 0.75)
		score += 0.10;
	if (silenceAfter >= 0.45)
		score += 0.30;
	if (silenceAfter >= 0.85)
		score += 0.10;

	const QString trimmed = candidate.text.trimmed();
	if (trimmed.endsWith(QLatin1Char('.')) || trimmed.endsWith(QLatin1Char('!')) ||
	    trimmed.endsWith(QLatin1Char('?')))
		score += 0.25;

	return boundedScore(score);
}

} // namespace Curation::Scoring
