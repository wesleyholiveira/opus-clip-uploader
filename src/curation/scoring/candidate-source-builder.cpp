#include "curation/scoring/candidate-source-builder.hpp"

#include "curation/scoring/candidate-range-utils.hpp"

#include <algorithm>
#include <QStringList>
#include <cmath>
#include <future>
#include <thread>
#include <vector>

using namespace Curation::Scoring::CandidateRangeUtils;

namespace Curation::Scoring {
namespace {

int boundedWorkerCount(int taskCount, int preferredMaxWorkers)
{
	if (taskCount <= 1)
		return 1;
	const unsigned int hardware = std::max(1u, std::thread::hardware_concurrency());
	return std::clamp(taskCount, 1, std::max(1, std::min(preferredMaxWorkers, static_cast<int>(hardware))));
}

QVector<double> targetDurationsForOptions(const CandidateSourceBuilderOptions &options)
{
	const CandidateGenerationOptions &generation = options.generation;
	QVector<double> targetDurations;
	if (generation.maxDurationSec <= 40.0) {
		targetDurations = {
			std::clamp(10.0, generation.minDurationSec, generation.maxDurationSec),
			std::clamp(14.0, generation.minDurationSec, generation.maxDurationSec),
			std::clamp(18.0, generation.minDurationSec, generation.maxDurationSec),
			std::clamp(24.0, generation.minDurationSec, generation.maxDurationSec),
			std::clamp(32.0, generation.minDurationSec, generation.maxDurationSec),
			generation.maxDurationSec,
		};
	} else if (options.viewerMessagePreset) {
		targetDurations = {
			std::clamp(14.0, generation.minDurationSec, generation.maxDurationSec),
			std::clamp(18.0, generation.minDurationSec, generation.maxDurationSec),
			std::clamp(24.0, generation.minDurationSec, generation.maxDurationSec),
			std::clamp(32.0, generation.minDurationSec, generation.maxDurationSec),
			std::clamp(45.0, generation.minDurationSec, generation.maxDurationSec),
			std::clamp(60.0, generation.minDurationSec, generation.maxDurationSec),
		};
	} else {
		targetDurations = {
			std::clamp(24.0, generation.minDurationSec, generation.maxDurationSec),
			std::clamp(45.0, generation.minDurationSec, generation.maxDurationSec),
			std::clamp(60.0, generation.minDurationSec, generation.maxDurationSec),
			std::clamp(90.0, generation.minDurationSec, generation.maxDurationSec),
			std::clamp(120.0, generation.minDurationSec, generation.maxDurationSec),
			std::clamp(150.0, generation.minDurationSec, generation.maxDurationSec),
			generation.maxDurationSec,
		};
	}
	targetDurations.erase(std::unique(targetDurations.begin(), targetDurations.end(), [](double left, double right) {
		return std::fabs(left - right) < 0.25;
	}), targetDurations.end());
	return targetDurations;
}

QVector<ClipCandidate> buildRegionCandidates(const TranscriptIndex &index,
	const SemanticCoarseRegion &region,
	const CandidateSourceBuilderOptions &options,
	const QVector<double> &targetDurations,
	int maxCandidatesPerRegion)
{
	QVector<ClipCandidate> regionCandidates;
	const ClipDuration searchRange = index.clampRange(region.range, options.generation.searchRange);
	ClipDuration focusRange = region.focusRange;
	if (focusRange.endSec <= focusRange.startSec)
		focusRange = searchRange;
	focusRange = index.clampRange(focusRange, searchRange);
	if ((searchRange.endSec - searchRange.startSec) < options.generation.minDurationSec)
		return regionCandidates;

	const QVector<int> focusSegmentIndices = index.segmentIndicesForRange(focusRange);
	const QVector<int> searchSegmentIndices = index.segmentIndicesForRange(searchRange);
	if (focusSegmentIndices.isEmpty() || searchSegmentIndices.isEmpty())
		return regionCandidates;

	QVector<int> focusAnchorIndices;
	const int desiredAnchors = std::max(5, maxCandidatesPerRegion * 2);
	const int anchorStep = std::max(1, static_cast<int>(std::ceil(static_cast<double>(focusSegmentIndices.size()) /
		static_cast<double>(desiredAnchors))));
	for (int position = 0; position < static_cast<int>(focusSegmentIndices.size()); position += anchorStep)
		focusAnchorIndices.append(focusSegmentIndices.at(position));
	if (!focusAnchorIndices.contains(focusSegmentIndices.first()))
		focusAnchorIndices.prepend(focusSegmentIndices.first());
	if (!focusAnchorIndices.contains(focusSegmentIndices.last()))
		focusAnchorIndices.append(focusSegmentIndices.last());

	regionCandidates.reserve(maxCandidatesPerRegion * 6);
	QStringList regionSeen;
	for (const int anchorIndex : focusAnchorIndices) {
		const TranscriptSegment *anchorSegment = index.segmentAt(anchorIndex);
		if (!anchorSegment || anchorSegment->text.trimmed().isEmpty())
			continue;

		for (const double requestedDurationSec : targetDurations) {
			QVector<double> startTimes;
			appendUniqueStart(startTimes, anchorSegment->startSec - 2.0);
			appendUniqueStart(startTimes, anchorSegment->startSec - 8.0);
			if (options.viewerMessagePreset) {
				appendUniqueStart(startTimes, anchorSegment->startSec - 13.0);
				appendUniqueStart(startTimes, anchorSegment->startSec - 16.0);
				appendUniqueStart(startTimes, anchorSegment->startSec - 24.0);
				appendUniqueStart(startTimes, anchorSegment->startSec - 32.0);
				appendUniqueStart(startTimes, anchorSegment->startSec - 44.0);
			}
			appendUniqueStart(startTimes, anchorSegment->startSec - (requestedDurationSec * 0.25));
			appendUniqueStart(startTimes, anchorSegment->startSec - (requestedDurationSec * 0.50));
			appendUniqueStart(startTimes, rangeCenterSec(focusRange) - (requestedDurationSec * 0.50));
			appendUniqueStart(startTimes, focusRange.startSec - 6.0);
			if (options.viewerMessagePreset) {
				appendUniqueStart(startTimes, focusRange.startSec - 16.0);
				appendUniqueStart(startTimes, focusRange.startSec - 32.0);
				appendUniqueStart(startTimes, searchRange.startSec + 4.0);
				appendUniqueStart(startTimes, searchRange.startSec + 12.0);
			}
			appendUniqueStart(startTimes, focusRange.endSec - requestedDurationSec + 6.0);

			for (double requestedStartSec : startTimes) {
				ClipDuration range = normalizedVariantRange(index, options.generation,
					{requestedStartSec, requestedStartSec + requestedDurationSec}, searchRange);
				if (range.endSec <= range.startSec)
					continue;
				if (!substantiallyOverlapsFocus(range, focusRange))
					continue;

				ClipCandidate candidate = CandidateBuilder::buildForRange(index, options.generation,
					options.scoring, options.qualityGate, range, region);
				candidate.evidence.append(QStringLiteral("coarse_focus_seed_only"));
				candidate.evidence.append(QStringLiteral("candidate_generated_around_coarse_focus"));
				candidate.evidence.removeDuplicates();
				const QString key = rangeKey(candidate.range);
				if (regionSeen.contains(key) || !CandidateBuilder::isStructurallyViable(candidate,
					    options.generation, options.qualityGate))
					continue;

				regionSeen.append(key);
				regionCandidates.append(candidate);
				if (static_cast<int>(regionCandidates.size()) >= maxCandidatesPerRegion * 6)
					break;
			}
			if (static_cast<int>(regionCandidates.size()) >= maxCandidatesPerRegion * 6)
				break;
		}
		if (static_cast<int>(regionCandidates.size()) >= maxCandidatesPerRegion * 6)
			break;
	}

	std::sort(regionCandidates.begin(), regionCandidates.end(), [](const ClipCandidate &left,
		const ClipCandidate &right) {
		if (std::fabs(left.scores.final - right.scores.final) > 0.0001)
			return left.scores.final > right.scores.final;
		if (std::fabs(left.scores.coarseSemantic - right.scores.coarseSemantic) > 0.0001)
			return left.scores.coarseSemantic > right.scores.coarseSemantic;
		return left.range.startSec < right.range.startSec;
	});
	return regionCandidates;
}

} // namespace

QVector<ClipCandidate> CandidateSourceBuilder::fromSemanticCoarseRegions(const TranscriptIndex &index,
	const QVector<SemanticCoarseRegion> &regions,
	const CandidateSourceBuilderOptions &options) const
{
	QVector<ClipCandidate> candidates;
	QStringList seen;
	if (regions.isEmpty())
		return candidates;

	const int maxRawCandidates = std::max(1, options.maxRawCandidates);
	const int maxCandidatesPerRegion = std::max(4, maxRawCandidates / std::max(1, static_cast<int>(regions.size())));
	const QVector<double> targetDurations = targetDurationsForOptions(options);

	const int workerCount = boundedWorkerCount(static_cast<int>(regions.size()), 4);
	QVector<QVector<ClipCandidate>> perRegion;
	perRegion.reserve(regions.size());
	if (workerCount <= 1) {
		for (const SemanticCoarseRegion &region : regions)
			perRegion.append(buildRegionCandidates(index, region, options, targetDurations, maxCandidatesPerRegion));
	} else {
		std::vector<std::future<QVector<QVector<ClipCandidate>>>> futures;
		futures.reserve(workerCount);
		const int chunkSize = static_cast<int>(std::ceil(static_cast<double>(regions.size()) /
			static_cast<double>(workerCount)));
		for (int first = 0; first < static_cast<int>(regions.size()); first += chunkSize) {
			const int last = std::min(static_cast<int>(regions.size()), first + chunkSize);
			futures.emplace_back(std::async(std::launch::async, [&index, &regions, &options, &targetDurations,
				maxCandidatesPerRegion, first, last]() {
				QVector<QVector<ClipCandidate>> chunk;
				chunk.reserve(std::max(0, last - first));
				for (int i = first; i < last; ++i)
					chunk.append(buildRegionCandidates(index, regions.at(i), options, targetDurations, maxCandidatesPerRegion));
				return chunk;
			}));
		}
		for (std::future<QVector<QVector<ClipCandidate>>> &future : futures) {
			const QVector<QVector<ClipCandidate>> chunk = future.get();
			for (const QVector<ClipCandidate> &regionCandidates : chunk)
				perRegion.append(regionCandidates);
		}
	}

	for (const QVector<ClipCandidate> &regionCandidates : perRegion) {
		int acceptedFromRegion = 0;
		for (const ClipCandidate &candidate : regionCandidates) {
			const QString key = rangeKey(candidate.range);
			if (seen.contains(key))
				continue;
			seen.append(key);
			candidates.append(candidate);
			++acceptedFromRegion;
			if (acceptedFromRegion >= maxCandidatesPerRegion || static_cast<int>(candidates.size()) >= maxRawCandidates)
				break;
		}
		if (static_cast<int>(candidates.size()) >= maxRawCandidates)
			break;
	}

	return candidates;
}

QVector<ClipCandidate> CandidateSourceBuilder::fromLocalHeuristics(const TranscriptIndex &index,
	const CandidateSourceBuilderOptions &options) const
{
	CandidateGenerator generator;
	QVector<ClipCandidate> candidates = generator.generate(index, options.generation);
	CheapClipScorer cheapScorer;
	for (ClipCandidate &candidate : candidates)
		candidate = cheapScorer.score(index, candidate, options.scoring);
	return candidates;
}

int CandidateSourceBuilder::semanticCandidateBudget(const CandidateSourceBuilderOptions &options, int requestedBeforeEmbedding)
{
	const int requested = std::max(1, requestedBeforeEmbedding);
	const int rawBudget = std::max(1, options.maxRawCandidates);
	if (options.viewerMessagePreset) {
		const int expanded = std::max(requested * 2, requested + 24);
		return std::min(rawBudget, std::min(expanded, 128));
	}
	return std::min(rawBudget, std::max(requested, 32));
}

} // namespace Curation::Scoring
