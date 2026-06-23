#include "curation/scoring/candidate-beam-expander.hpp"

#include "curation/scoring/candidate-range-utils.hpp"

#include <algorithm>
#include <QPair>
#include <cmath>

using namespace Curation::Scoring::CandidateRangeUtils;

namespace Curation::Scoring {

QVector<ClipCandidate> CandidateBeamExpander::expand(const TranscriptIndex &index,
	QVector<ClipCandidate> candidates,
	const CandidateBeamExpansionOptions &options) const
{
	if (candidates.isEmpty())
		return candidates;

	const int finalBudget = std::max(1, options.finalBudget);
	const int variantsPerSeed = options.viewerMessagePreset ? 12 : 5;
	const int protectedVariantsPerSeed = options.viewerMessagePreset ? 6 : 1;
	QVector<ClipCandidate> expanded;
	QStringList seen;
	expanded.reserve(std::min(finalBudget, static_cast<int>(candidates.size()) * variantsPerSeed));

	for (const ClipCandidate &candidate : candidates) {
		QVector<ClipCandidate> variants = expandVariants(index, candidate, options);
		if (variants.isEmpty())
			variants.append(candidate);

		int acceptedFromSeed = 0;
		auto appendVariant = [&expanded, &seen, &acceptedFromSeed, variantsPerSeed](ClipCandidate variant) {
			if (acceptedFromSeed >= variantsPerSeed)
				return;
			const QString key = rangeKey(variant.range);
			if (seen.contains(key))
				return;
			variant.evidence.append(QStringLiteral("candidate_beam_selected"));
			variant.evidence.removeDuplicates();
			seen.append(key);
			expanded.append(std::move(variant));
			++acceptedFromSeed;
		};

		for (int i = 0; i < std::min(protectedVariantsPerSeed, static_cast<int>(variants.size())); ++i)
			appendVariant(variants.at(i));

		std::sort(variants.begin(), variants.end(), [](const ClipCandidate &left, const ClipCandidate &right) {
			if (std::fabs(left.scores.final - right.scores.final) > 0.0001)
				return left.scores.final > right.scores.final;
			if (std::fabs(left.scores.boundary - right.scores.boundary) > 0.0001)
				return left.scores.boundary > right.scores.boundary;
			return left.range.startSec < right.range.startSec;
		});
		for (const ClipCandidate &variant : variants)
			appendVariant(variant);

		if (static_cast<int>(expanded.size()) >= finalBudget)
			break;
	}

	if (expanded.isEmpty())
		return candidates;
	if (static_cast<int>(expanded.size()) > finalBudget)
		expanded.resize(finalBudget);
	for (ClipCandidate &candidate : expanded)
		candidate.evidence.append(QStringLiteral("candidate_beam_budget:%1").arg(finalBudget));
	return expanded;
}

QVector<ClipCandidate> CandidateBeamExpander::expandVariants(const TranscriptIndex &index,
	const ClipCandidate &candidate,
	const CandidateBeamExpansionOptions &options) const
{
	QVector<ClipCandidate> variants;
	QVector<ClipDuration> ranges;
	const double localLookbackSec = options.viewerMessagePreset ? 64.0 : 24.0;
	const double localLookaheadSec = options.viewerMessagePreset ? 44.0 : 24.0;
	const ClipDuration localSearchRange = index.clampRange({candidate.range.startSec - localLookbackSec,
		candidate.range.endSec + localLookaheadSec}, options.generation.searchRange);
	if (localSearchRange.endSec <= localSearchRange.startSec)
		return variants;

	const double seedDurationSec = candidate.range.endSec - candidate.range.startSec;
	const double seedCenterSec = rangeCenterSec(candidate.range);
	const TranscriptSegment *firstSegment = index.segmentAt(candidate.firstSegmentIndex);
	const TranscriptSegment *lastSegment = index.segmentAt(candidate.lastSegmentIndex);
	const double firstSpeechSec = firstSegment ? firstSegment->startSec : candidate.range.startSec;
	const double lastSpeechEndSec = lastSegment ? lastSegment->endSec : candidate.range.endSec;

	const QVector<QPair<double, double>> primaryShifts = options.viewerMessagePreset
		? QVector<QPair<double, double>>{
			{0.0, 0.0}, {-13.0, 4.0}, {-16.0, 4.0}, {-16.0, 0.0}, {-16.0, -24.0},
			{-24.0, -24.0}, {-32.0, -24.0}, {-44.0, -24.0}, {-44.0, 0.0}, {-56.0, 0.0},
			{-8.0, 8.0}, {-13.0, 8.0}, {-16.0, 8.0}, {-24.0, 8.0}}
		: QVector<QPair<double, double>>{{0.0, 0.0}, {-8.0, 0.0}, {0.0, 8.0}, {-8.0, 8.0}};
	for (const QPair<double, double> &shift : primaryShifts) {
		ClipDuration range = normalizedVariantRange(index, options.generation,
			{candidate.range.startSec + shift.first, candidate.range.endSec + shift.second}, localSearchRange);
		appendUniqueRange(ranges, range);
	}

	QVector<double> durations;
	const QVector<double> candidateDurations = options.viewerMessagePreset
		? QVector<double>{14.0, 18.0, 24.0, 32.0, 45.0, 60.0, seedDurationSec}
		: QVector<double>{18.0, 24.0, 32.0, 45.0, 60.0, 90.0, 120.0, seedDurationSec,
			options.generation.maxDurationSec};
	for (const double duration : candidateDurations) {
		if (duration >= options.generation.minDurationSec && duration <= options.generation.maxDurationSec &&
		    (!options.viewerMessagePreset || duration <= 68.0))
			appendUniqueValue(durations, duration);
	}

	for (const double duration : durations) {
		QVector<double> starts;
		appendUniqueValue(starts, candidate.range.startSec);
		appendUniqueValue(starts, candidate.range.endSec - duration);
		appendUniqueValue(starts, seedCenterSec - (duration * 0.50));
		appendUniqueValue(starts, firstSpeechSec - 0.35);
		appendUniqueValue(starts, lastSpeechEndSec - duration + 0.35);
		if (options.viewerMessagePreset) {
			appendUniqueValue(starts, firstSpeechSec - 13.0);
			appendUniqueValue(starts, firstSpeechSec - 16.0);
			appendUniqueValue(starts, firstSpeechSec - 24.0);
			appendUniqueValue(starts, firstSpeechSec - 32.0);
			appendUniqueValue(starts, firstSpeechSec - 44.0);
		}
		for (const double startSec : starts) {
			ClipDuration range = normalizedVariantRange(index, options.generation,
				{startSec, startSec + duration}, localSearchRange);
			appendUniqueRange(ranges, range);
		}
	}

	variants.reserve(ranges.size());
	for (int i = 0; i < static_cast<int>(ranges.size()); ++i) {
		const ClipDuration &range = ranges.at(i);
		if (!rangeIsWithinDurationLimits(range, options.generation))
			continue;
		ClipCandidate variant = CandidateBuilder::buildVariantFromSeed(index, options.generation, options.scoring,
			options.qualityGate, candidate, range, QStringLiteral("candidate_beam_variant_index:%1").arg(i));
		if (!CandidateBuilder::isStructurallyViable(variant, options.generation, options.qualityGate))
			continue;
		variants.append(variant);
	}
	return variants;
}

} // namespace Curation::Scoring
