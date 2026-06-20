#include "curation/scoring/clip-scoring-pipeline.hpp"

#include <QStringList>

#include <algorithm>
#include <utility>

namespace Curation::Scoring {

ClipScoringResult ClipScoringPipeline::score(const RecordingTranscript &transcript,
						     const ClipScoringPipelineOptions &options) const
{
	TranscriptIndex index(transcript);
	ClipScoringResult result;
	if (index.isEmpty()) {
		result.summary = QStringLiteral("no_candidates: empty_transcript");
		return result;
	}

	CandidateGenerator generator;
	QVector<ClipCandidate> candidates = generator.generate(index, options.generation);

	CheapClipScorer scorer;
	for (ClipCandidate &candidate : candidates)
		candidate = scorer.score(index, candidate, options.scoring);

	ClipRanker ranker;
	result.candidates = ranker.rank(std::move(candidates), options.ranking);
	result.summary = buildSummary(result.candidates);
	return result;
}

QString ClipScoringPipeline::buildSummary(const QVector<ClipCandidate> &candidates) const
{
	if (candidates.isEmpty())
		return QStringLiteral("no_candidates: cheap_scorer_found_no_viable_ranges");

	QStringList parts;
	const int limit = static_cast<int>(std::min(static_cast<long long>(5), static_cast<long long>(candidates.size())));
	for (int i = 0; i < limit; ++i) {
		const ClipCandidate &candidate = candidates.at(i);
		parts.append(QStringLiteral("#%1 %2-%3s score=%4 source=%5 evidence=%6")
			     .arg(i + 1)
			     .arg(QString::number(candidate.range.startSec, 'f', 2))
			     .arg(QString::number(candidate.range.endSec, 'f', 2))
			     .arg(QString::number(candidate.scores.final, 'f', 2))
			     .arg(candidate.source)
			     .arg(candidate.evidence.join(QLatin1Char('|'))));
	}
	return parts.join(QStringLiteral("; "));
}

} // namespace Curation::Scoring
