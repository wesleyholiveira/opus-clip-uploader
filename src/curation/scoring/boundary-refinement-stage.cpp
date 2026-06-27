#include "curation/scoring/boundary-refinement-stage.hpp"

#include "curation/scoring/candidate-dedupe.hpp"
#include "curation/scoring/candidate-range-utils.hpp"
#include "curation/scoring/evidence-view.hpp"
#include "curation/scoring/parallel-chunk-map.hpp"

#include <QSet>

#include <algorithm>
#include <cmath>
#include <utility>

using namespace Curation::Scoring::CandidateRangeUtils;

namespace Curation::Scoring {
namespace {

bool hasDetailedArcEvidence(const ClipCandidate &candidate)
{
	return EvidenceView::anyStartsWith(candidate.evidence, QStringLiteral("exchange_arc_state_machine:")) ||
	       EvidenceView::anyStartsWith(candidate.evidence, QStringLiteral("exchange_arc_window_dfs_graph:")) ||
	       EvidenceView::anyStartsWith(candidate.evidence, QStringLiteral("exchange_arc_roles:")) ||
	       EvidenceView::anyStartsWith(candidate.evidence,
					   QStringLiteral("exchange_arc_contextual_role_scores:")) ||
	       EvidenceView::anyStartsWith(candidate.evidence, QStringLiteral("exchange_arc opening:")) ||
	       EvidenceView::anyStartsWith(candidate.evidence, QStringLiteral("exchange_arc_boundary")) ||
	       EvidenceView::anyEquals(candidate.evidence, QStringLiteral("exchange_arc_no_valid_subspan")) ||
	       EvidenceView::anyEquals(candidate.evidence,
				       QStringLiteral("exchange_arc_contextual_dp_subspan_recovered")) ||
	       EvidenceView::anyStartsWith(candidate.evidence, QStringLiteral("exchange_arc_refiner_skipped:"));
}

static bool hasEvidenceEqualOrContaining(const ClipCandidate &candidate, const QString &needle)
{
	return EvidenceView::anyEquals(candidate.evidence, needle) ||
	       EvidenceView::anyContains(candidate.evidence, needle);
}

static bool isExactPositiveFeedbackSeed(const ClipCandidate &candidate)
{
	return candidate.source == QStringLiteral("feedback_positive_exact_seed") ||
	       hasEvidenceEqualOrContaining(candidate, QStringLiteral("feedback_positive_exact_seed_preserved")) ||
	       hasEvidenceEqualOrContaining(candidate,
					    QStringLiteral("complete_viewer_arc_gate_passed_by_user_feedback"));
}

} // namespace

QVector<ClipCandidate> BoundaryRefinementStage::apply(const TranscriptIndex &index, QVector<ClipCandidate> candidates,
						      const BoundaryRefinementStageOptions &options) const
{
	if (!options.embeddingProvider || !options.embeddingProvider->isAvailable() || candidates.isEmpty())
		return candidates;

	struct IndexedCandidate {
		int index = 0;
		ClipCandidate candidate;
	};

	QVector<IndexedCandidate> indexed = ParallelChunkMap::transformIndexed(
		candidates, options.viewerMessagePreset ? 4 : 2,
		[this, &index, &options](const ClipCandidate &candidate, int originalIndex) {
			return IndexedCandidate{originalIndex, refineOne(index, candidate, options)};
		});
	std::sort(indexed.begin(), indexed.end(),
		  [](const IndexedCandidate &left, const IndexedCandidate &right) { return left.index < right.index; });

	QVector<ClipCandidate> refined;
	refined.reserve(indexed.size());
	QSet<QString> seen;
	seen.reserve(indexed.size());
	for (IndexedCandidate &entry : indexed) {
		if (!CandidateDedupe::markSeen(seen, entry.candidate.range))
			continue;
		refined.append(std::move(entry.candidate));
	}
	return refined.isEmpty() ? candidates : refined;
}

ClipCandidate BoundaryRefinementStage::refineOne(const TranscriptIndex &index, const ClipCandidate &candidate,
						 const BoundaryRefinementStageOptions &options) const
{
	if (options.viewerMessagePreset && isExactPositiveFeedbackSeed(candidate)) {
		ClipCandidate preserved = candidate;
		preserved.evidence.append(
			QStringLiteral("boundary_refinement_skipped_for_exact_positive_feedback_seed"));
		preserved.evidence.removeDuplicates();
		return preserved;
	}

	ExchangeArcBoundaryRefinementOptions refinerOptions;
	refinerOptions.generation = options.generation;
	refinerOptions.scoring = options.scoring;
	refinerOptions.qualityGate = options.qualityGate;
	refinerOptions.embeddingProvider = options.embeddingProvider;

	ClipCandidate refinerInput = candidate;
	if (options.viewerMessagePreset) {
		refinerInput.evidence.append(
			QStringLiteral("exchange_arc_role_classifier:v20_pipeline_entered_refiner"));
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
		refined.evidence.append(
			QStringLiteral("exchange_arc_refiner_returned_without_detailed_arc_evidence_v20"));
		refined.evidence.removeDuplicates();
	}
	return refined;
}

} // namespace Curation::Scoring
