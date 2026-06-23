#include "curation/scoring/boundary-refinement-stage.hpp"

#include "curation/scoring/candidate-range-utils.hpp"

#include <algorithm>
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

bool hasDetailedArcEvidence(const ClipCandidate &candidate)
{
	return std::any_of(candidate.evidence.constBegin(), candidate.evidence.constEnd(), [](const QString &evidence) {
		return evidence.startsWith(QStringLiteral("exchange_arc_state_machine:")) ||
			evidence.startsWith(QStringLiteral("exchange_arc_window_dfs_graph:")) ||
			evidence.startsWith(QStringLiteral("exchange_arc_roles:")) ||
			evidence.startsWith(QStringLiteral("exchange_arc_contextual_role_scores:")) ||
			evidence.startsWith(QStringLiteral("exchange_arc opening:")) ||
			evidence.startsWith(QStringLiteral("exchange_arc_boundary")) ||
			evidence == QStringLiteral("exchange_arc_no_valid_subspan") ||
			evidence == QStringLiteral("exchange_arc_contextual_dp_subspan_recovered") ||
			evidence.startsWith(QStringLiteral("exchange_arc_refiner_skipped:"));
	});
}

} // namespace

QVector<ClipCandidate> BoundaryRefinementStage::apply(const TranscriptIndex &index,
	QVector<ClipCandidate> candidates,
	const BoundaryRefinementStageOptions &options) const
{
	if (!options.embeddingProvider || !options.embeddingProvider->isAvailable() || candidates.isEmpty())
		return candidates;

	struct IndexedCandidate {
		int index = 0;
		ClipCandidate candidate;
	};

	auto refineChunk = [this, &index, &options](int first, int last, const QVector<ClipCandidate> &source) {
		QVector<IndexedCandidate> chunk;
		chunk.reserve(std::max(0, last - first));
		for (int i = first; i < last; ++i) {
			ClipCandidate topicCandidate = refineOne(index, source.at(i), options);
			chunk.append(IndexedCandidate{i, topicCandidate});
		}
		return chunk;
	};

	const int workerCount = boundedWorkerCount(static_cast<int>(candidates.size()), options.viewerMessagePreset ? 4 : 2);
	QVector<IndexedCandidate> indexed;
	indexed.reserve(candidates.size());
	if (workerCount <= 1) {
		indexed = refineChunk(0, static_cast<int>(candidates.size()), candidates);
	} else {
		std::vector<std::future<QVector<IndexedCandidate>>> futures;
		futures.reserve(workerCount);
		const int chunkSize = static_cast<int>(std::ceil(static_cast<double>(candidates.size()) /
			static_cast<double>(workerCount)));
		for (int first = 0; first < static_cast<int>(candidates.size()); first += chunkSize) {
			const int last = std::min(static_cast<int>(candidates.size()), first + chunkSize);
			futures.emplace_back(std::async(std::launch::async, refineChunk, first, last, std::cref(candidates)));
		}
		for (std::future<QVector<IndexedCandidate>> &future : futures) {
			const QVector<IndexedCandidate> chunk = future.get();
			for (const IndexedCandidate &candidate : chunk)
				indexed.append(candidate);
		}
		std::sort(indexed.begin(), indexed.end(), [](const IndexedCandidate &left, const IndexedCandidate &right) {
			return left.index < right.index;
		});
	}

	QVector<ClipCandidate> refined;
	refined.reserve(indexed.size());
	QStringList seen;
	for (const IndexedCandidate &entry : indexed) {
		const QString key = rangeKey(entry.candidate.range);
		if (seen.contains(key))
			continue;
		seen.append(key);
		refined.append(entry.candidate);
	}
	return refined.isEmpty() ? candidates : refined;
}

ClipCandidate BoundaryRefinementStage::refineOne(const TranscriptIndex &index,
	const ClipCandidate &candidate,
	const BoundaryRefinementStageOptions &options) const
{
	ExchangeArcBoundaryRefinementOptions refinerOptions;
	refinerOptions.generation = options.generation;
	refinerOptions.scoring = options.scoring;
	refinerOptions.qualityGate = options.qualityGate;
	refinerOptions.embeddingProvider = options.embeddingProvider;

	ClipCandidate refinerInput = candidate;
	if (options.viewerMessagePreset) {
		refinerInput.evidence.append(QStringLiteral("exchange_arc_role_classifier:v20_pipeline_entered_refiner"));
		refinerInput.evidence.removeDuplicates();
	}

	ExchangeArcBoundaryRefiner refiner;
	ClipCandidate refined = refiner.refine(index, refinerInput, refinerOptions);

	if (!CandidateBuilder::isStructurallyViable(refined, options.generation, options.qualityGate)) {
		refined.evidence.append(QStringLiteral("exchange_arc_refiner_output_not_structurally_viable"));
		refined.evidence.removeDuplicates();
		return refined;
	}

	const bool materiallyChanged = std::fabs(refined.range.startSec - candidate.range.startSec) > 1.0 ||
		std::fabs(refined.range.endSec - candidate.range.endSec) > 1.0;
	if (materiallyChanged)
		return CandidateBuilder::scoreStructurally(index, refined);

	if (!hasDetailedArcEvidence(refined) && options.viewerMessagePreset) {
		refined.evidence.append(QStringLiteral("exchange_arc_refiner_returned_without_detailed_arc_evidence_v20"));
		refined.evidence.removeDuplicates();
	}
	return refined;
}

} // namespace Curation::Scoring
