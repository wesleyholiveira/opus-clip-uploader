#include "curation/scoring/semantic-clip-scorer.hpp"

#include "curation/scoring/semantic-prototypes.hpp"
#include "curation/scoring/text-analysis.hpp"

#include <algorithm>
#include <cmath>

using namespace Curation::Scoring;

namespace {

static double boundedScore(double value)
{
	return std::clamp(value, 0.0, 1.0);
}

static QString modelEvidence(const SemanticEmbeddingProvider &provider)
{
	const QString modelId = provider.modelId().trimmed();
	return modelId.isEmpty() ? QStringLiteral("semantic_model_available")
				       : QStringLiteral("semantic_model:%1").arg(modelId.left(80));
}

} // namespace

ClipCandidate SemanticClipScorer::score(const TranscriptIndex &index, const ClipCandidate &candidate,
					       const SemanticScoringContext &context,
					       const SemanticClipScoringOptions &options,
					       const SemanticEmbeddingProvider *provider) const
{
	ClipCandidate scored = candidate;
	if (!options.enabled) {
		scored.evidence.append(QStringLiteral("semantic_disabled"));
		scored.evidence.removeDuplicates();
		return scored;
	}

	if (!provider || !provider->isAvailable()) {
		scored.evidence.append(QStringLiteral("semantic_embedding_unavailable"));
		scored.evidence.removeDuplicates();
		return scored;
	}

	if (scored.text.trimmed().size() < options.minTextChars) {
		scored.evidence.append(QStringLiteral("semantic_skipped_short_text"));
		scored.evidence.removeDuplicates();
		return scored;
	}

	const SemanticEmbedding textEmbedding = provider->embed(scored.text);
	if (!textEmbedding.isValid()) {
		scored.evidence.append(QStringLiteral("semantic_embedding_failed"));
		scored.evidence.removeDuplicates();
		return scored;
	}

	scored.semanticScoringAvailable = true;
	scored.evidence.append(modelEvidence(*provider));

	const SemanticPrototypeSet &prototypes = defaultSemanticPrototypes();
	const QStringList targetPrototypes = targetPrototypesForPreset(context.presetId, context.mainTarget);

	scored.scores.embeddingTarget = maxPrototypeSimilarity(*provider, textEmbedding, targetPrototypes);
	scored.scores.semanticViewerMessage = maxPrototypeSimilarity(*provider, textEmbedding, prototypes.viewerMessage);
	scored.scores.semanticDirectAnswer = maxPrototypeSimilarity(*provider, textEmbedding, prototypes.directAnswer);
	const double greetingNoise = maxPrototypeSimilarity(*provider, textEmbedding, prototypes.greetingNoise);
	const double streamNoise = maxPrototypeSimilarity(*provider, textEmbedding, prototypes.streamManagement);
	scored.scores.semanticNoise = std::max(greetingNoise, streamNoise);
	scored.scores.semanticTopicShift = maxPrototypeSimilarity(*provider, textEmbedding, prototypes.topicShift);

	const double pairwiseTopicContinuity = topicContinuityByEmbedding(index, scored, *provider);
	if (pairwiseTopicContinuity > 0.0)
		scored.scores.topicContinuity = std::max(scored.scores.topicContinuity, pairwiseTopicContinuity);

	if (scored.scores.embeddingTarget > 0.0)
		scored.scores.semanticTarget = std::max(scored.scores.semanticTarget, scored.scores.embeddingTarget);

	const double viewerResponseSemantic = (scored.scores.semanticViewerMessage * 0.45) +
					    (scored.scores.semanticDirectAnswer * 0.55);
	if (viewerResponseSemantic > 0.0)
		scored.scores.viewerResponse = std::max(scored.scores.viewerResponse, viewerResponseSemantic);

	if (scored.scores.semanticNoise >= options.highConfidenceMatch) {
		scored.scores.noise = std::max(scored.scores.noise, scored.scores.semanticNoise);
		scored.evidence.append(QStringLiteral("semantic_noise_match"));
	}
	if (scored.scores.semanticTarget >= options.highConfidenceMatch)
		scored.evidence.append(QStringLiteral("semantic_target_match"));
	if (scored.scores.semanticViewerMessage >= options.highConfidenceMatch)
		scored.evidence.append(QStringLiteral("semantic_viewer_message_match"));
	if (scored.scores.semanticDirectAnswer >= options.highConfidenceMatch)
		scored.evidence.append(QStringLiteral("semantic_direct_answer_match"));
	if (scored.scores.topicContinuity >= options.highConfidenceMatch)
		scored.evidence.append(QStringLiteral("semantic_topic_continuity"));

	scored.scores.final = combineFinalScore(scored, context);
	scored.evidence.removeDuplicates();
	return scored;
}

double SemanticClipScorer::maxPrototypeSimilarity(const SemanticEmbeddingProvider &provider,
						 const SemanticEmbedding &textEmbedding,
						 const QStringList &prototypes) const
{
	double best = 0.0;
	for (const QString &prototype : prototypes) {
		if (prototype.trimmed().isEmpty())
			continue;
		best = std::max(best, cosineSimilarity(textEmbedding, provider.embed(prototype)));
	}
	return boundedScore(best);
}

double SemanticClipScorer::topicContinuityByEmbedding(const TranscriptIndex &index, const ClipCandidate &candidate,
						      const SemanticEmbeddingProvider &provider) const
{
	const QString left = firstHalfText(index, candidate);
	const QString right = secondHalfText(index, candidate);
	if (left.trimmed().size() < 30 || right.trimmed().size() < 30)
		return 0.0;

	return cosineSimilarity(provider.embed(left), provider.embed(right));
}

double SemanticClipScorer::combineFinalScore(const ClipCandidate &candidate,
						    const SemanticScoringContext &context) const
{
	const ClipCandidateScores &s = candidate.scores;
	const double semanticNoisePenalty = std::max(s.noise, s.semanticNoise) * 0.35;

	if (context.reliableMainTarget && !context.mainTarget.trimmed().isEmpty()) {
		return boundedScore((s.semanticTarget * 0.45) + (s.viewerResponse * 0.22) + (s.boundary * 0.16) +
				    (s.topicContinuity * 0.10) + (s.emotional * 0.07) - semanticNoisePenalty);
	}

	if (context.presetId == QStringLiteral("viewer_message_response")) {
		return boundedScore((s.viewerResponse * 0.33) + (s.semanticTarget * 0.18) + (s.boundary * 0.18) +
				    (s.topicContinuity * 0.12) + (s.emotional * 0.09) + (s.hook * 0.05) +
				    (s.duration * 0.05) - semanticNoisePenalty);
	}

	return boundedScore((s.boundary * 0.18) + (s.duration * 0.10) + (s.hook * 0.10) +
			    (s.semanticTarget * 0.22) + (s.topicContinuity * 0.15) +
			    (std::max({s.emotional, s.advice, s.explanation, s.story, s.opinion, s.tutorial}) * 0.25) -
			    semanticNoisePenalty);
}

QString SemanticClipScorer::firstHalfText(const TranscriptIndex &index, const ClipCandidate &candidate) const
{
	if (candidate.firstSegmentIndex < 0 || candidate.lastSegmentIndex < candidate.firstSegmentIndex)
		return {};

	const int midpoint = candidate.firstSegmentIndex + ((candidate.lastSegmentIndex - candidate.firstSegmentIndex) / 2);
	return index.textForSegmentWindow(candidate.firstSegmentIndex, midpoint);
}

QString SemanticClipScorer::secondHalfText(const TranscriptIndex &index, const ClipCandidate &candidate) const
{
	if (candidate.firstSegmentIndex < 0 || candidate.lastSegmentIndex < candidate.firstSegmentIndex)
		return {};

	const int midpoint = candidate.firstSegmentIndex + ((candidate.lastSegmentIndex - candidate.firstSegmentIndex) / 2) + 1;
	return index.textForSegmentWindow(midpoint, candidate.lastSegmentIndex);
}
