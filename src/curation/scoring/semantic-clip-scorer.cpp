#include "curation/scoring/semantic-clip-scorer.hpp"

#include "curation/scoring/semantic-prototypes.hpp"
#include <algorithm>
#include <cmath>
#include <initializer_list>

using namespace Curation::Scoring;

namespace {

static double boundedScore(double value)
{
	return std::clamp(value, 0.0, 1.0);
}

static void appendUniquePreparedText(QStringList &texts, const QString &text)
{
	const QString value = text.simplified();
	if (value.isEmpty() || texts.contains(value))
		return;
	texts.append(value);
}

static QString modelEvidence(const SemanticEmbeddingProvider &provider)
{
	const QString modelId = provider.modelId().trimmed();
	return modelId.isEmpty() ? QStringLiteral("semantic_model_available")
				 : QStringLiteral("semantic_model:%1").arg(modelId.left(80));
}

struct SemanticPrototypeBudget {
	QStringList target;
	QStringList viewerMessage;
	QStringList directAnswer;
	QStringList noise;
	QStringList topicShift;
	QStringList clipValue;
	QStringList empathy;
	QStringList hook;
	QStringList resolution;
	QStringList metaNoise;

	int total() const
	{
		return target.size() + viewerMessage.size() + directAnswer.size() + noise.size() + topicShift.size() +
		       clipValue.size() + empathy.size() + hook.size() + resolution.size() + metaNoise.size();
	}
};

static void appendLimited(QStringList &destination, const QStringList &source, int &remaining, int maxItems)
{
	if (remaining <= 0 || maxItems <= 0)
		return;

	for (const QString &item : source) {
		const QString value = item.simplified();
		if (value.isEmpty() || destination.contains(value))
			continue;
		destination.append(value);
		--remaining;
		--maxItems;
		if (remaining <= 0 || maxItems <= 0)
			return;
	}
}

static SemanticPrototypeBudget prototypeBudgetFor(const QStringList &targetPrototypes,
						  const SemanticPrototypeSet &defaultPrototypes, int maxPrototypeTexts)
{
	SemanticPrototypeBudget budget;
	int remaining = std::clamp(maxPrototypeTexts, 0, 24);
	if (remaining <= 0)
		return budget;

	appendLimited(budget.target, targetPrototypes, remaining, 2);
	appendLimited(budget.clipValue, defaultPrototypes.clipValue, remaining, 3);
	appendLimited(budget.empathy, defaultPrototypes.empathy, remaining, 2);
	appendLimited(budget.metaNoise, defaultPrototypes.metaNoise, remaining, 3);
	appendLimited(budget.hook, defaultPrototypes.hook, remaining, 2);
	appendLimited(budget.resolution, defaultPrototypes.resolution, remaining, 2);
	appendLimited(budget.viewerMessage, defaultPrototypes.viewerMessage, remaining, 1);
	appendLimited(budget.directAnswer, defaultPrototypes.directAnswer, remaining, 1);

	QStringList noisePrototypes = defaultPrototypes.greetingNoise;
	noisePrototypes.append(defaultPrototypes.streamManagement);
	noisePrototypes.append(defaultPrototypes.metaNoise);
	appendLimited(budget.noise, noisePrototypes, remaining, budget.target.isEmpty() ? 2 : 1);
	appendLimited(budget.topicShift, defaultPrototypes.topicShift, remaining, remaining);
	return budget;
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

	const QString languageCode =
		normalizedSemanticLanguageCode(context.transcriptionLanguage, context.sourceLanguage);
	const SemanticPrototypeSet &prototypes = semanticPrototypesForLanguage(languageCode);
	const QStringList targetPrototypes =
		targetPrototypesForPreset(context.presetId, context.mainTarget, languageCode);
	const SemanticPrototypeBudget prototypeBudget =
		prototypeBudgetFor(targetPrototypes, prototypes, options.maxPrototypeTexts);
	scored.evidence.append(QStringLiteral("semantic_language:%1").arg(languageCode));
	scored.evidence.append(QStringLiteral("semantic_prototype_budget:%1").arg(prototypeBudget.total()));

	scored.scores.embeddingTarget = maxPrototypeSimilarity(*provider, textEmbedding, prototypeBudget.target);
	scored.scores.semanticViewerMessage =
		maxPrototypeSimilarity(*provider, textEmbedding, prototypeBudget.viewerMessage);
	scored.scores.semanticDirectAnswer =
		maxPrototypeSimilarity(*provider, textEmbedding, prototypeBudget.directAnswer);
	scored.scores.semanticNoise = maxPrototypeSimilarity(*provider, textEmbedding, prototypeBudget.noise);
	scored.scores.semanticTopicShift = maxPrototypeSimilarity(*provider, textEmbedding, prototypeBudget.topicShift);
	scored.scores.semanticClipValue = maxPrototypeSimilarity(*provider, textEmbedding, prototypeBudget.clipValue);
	scored.scores.semanticEmpathy = maxPrototypeSimilarity(*provider, textEmbedding, prototypeBudget.empathy);
	scored.scores.semanticClipValue =
		std::max(scored.scores.semanticClipValue, scored.scores.semanticEmpathy * 0.92);
	scored.scores.semanticHook = maxPrototypeSimilarity(*provider, textEmbedding, prototypeBudget.hook);
	scored.scores.semanticResolution = maxPrototypeSimilarity(*provider, textEmbedding, prototypeBudget.resolution);
	scored.scores.semanticMetaNoise = maxPrototypeSimilarity(*provider, textEmbedding, prototypeBudget.metaNoise);

	const QStringList openingHookPrototypes = prototypeBudget.hook + prototypeBudget.viewerMessage;
	const QStringList endingResolutionPrototypes = prototypeBudget.resolution + prototypeBudget.directAnswer;
	scoreOpeningAndEnding(index, scored, *provider, openingHookPrototypes, endingResolutionPrototypes,
			      prototypeBudget.metaNoise, prototypeBudget.topicShift);

	if (options.enablePairwiseTopicContinuity) {
		const double pairwiseTopicContinuity = topicContinuityByEmbedding(index, scored, *provider);
		if (pairwiseTopicContinuity > 0.0)
			scored.scores.topicContinuity =
				std::max(scored.scores.topicContinuity, pairwiseTopicContinuity);
	}
	if (scored.scores.semanticFocusContinuity > 0.0)
		scored.scores.topicContinuity =
			std::max(scored.scores.topicContinuity, scored.scores.semanticFocusContinuity);

	if (scored.scores.embeddingTarget > 0.0)
		scored.scores.semanticTarget = std::max(scored.scores.semanticTarget, scored.scores.embeddingTarget);

	const double viewerResponseSemantic =
		(scored.scores.semanticViewerMessage * 0.26) + (scored.scores.semanticDirectAnswer * 0.34) +
		(scored.scores.semanticClipValue * 0.24) + (scored.scores.semanticEmpathy * 0.16);
	if (viewerResponseSemantic > 0.0)
		scored.scores.viewerResponse = std::max(scored.scores.viewerResponse, viewerResponseSemantic);

	const double semanticNoise = std::max(scored.scores.semanticNoise, scored.scores.semanticMetaNoise);
	if (semanticNoise >= options.highConfidenceMatch) {
		scored.scores.noise = std::max(scored.scores.noise, semanticNoise);
		scored.evidence.append(QStringLiteral("semantic_noise_match"));
	}
	if (scored.scores.semanticMetaNoise >= options.highConfidenceMatch)
		scored.evidence.append(QStringLiteral("semantic_meta_noise_match"));
	if (scored.scores.semanticTarget >= options.highConfidenceMatch)
		scored.evidence.append(QStringLiteral("semantic_target_match"));
	if (scored.scores.semanticViewerMessage >= options.highConfidenceMatch)
		scored.evidence.append(QStringLiteral("semantic_viewer_message_match"));
	if (scored.scores.semanticDirectAnswer >= options.highConfidenceMatch)
		scored.evidence.append(QStringLiteral("semantic_direct_answer_match"));
	if (scored.scores.semanticClipValue >= options.highConfidenceMatch)
		scored.evidence.append(QStringLiteral("semantic_clip_value"));
	if (scored.scores.semanticEmpathy >= 0.55)
		scored.evidence.append(QStringLiteral("semantic_empathy_score:%1")
					       .arg(QString::number(scored.scores.semanticEmpathy, 'f', 2)));
	if (scored.scores.semanticEmpathy >= options.highConfidenceMatch)
		scored.evidence.append(QStringLiteral("semantic_empathy"));
	if (scored.scores.semanticHook >= options.highConfidenceMatch)
		scored.evidence.append(QStringLiteral("semantic_hook"));
	if (scored.scores.semanticOpeningHook >= options.highConfidenceMatch)
		scored.evidence.append(QStringLiteral("semantic_opening_hook"));
	if (scored.scores.semanticOpeningMetaNoise >= options.highConfidenceMatch)
		scored.evidence.append(QStringLiteral("semantic_opening_meta_noise"));
	if (scored.scores.semanticResolution >= options.highConfidenceMatch)
		scored.evidence.append(QStringLiteral("semantic_resolution"));
	if (scored.scores.semanticEndingResolution >= options.highConfidenceMatch)
		scored.evidence.append(QStringLiteral("semantic_ending_resolution"));
	if (scored.scores.semanticEndingMetaNoise >= options.highConfidenceMatch)
		scored.evidence.append(QStringLiteral("semantic_ending_meta_noise"));
	if (scored.scores.semanticEndingTopicShift >= options.highConfidenceMatch)
		scored.evidence.append(QStringLiteral("semantic_ending_topic_shift"));
	if (scored.scores.topicContinuity >= options.highConfidenceMatch)
		scored.evidence.append(QStringLiteral("semantic_topic_continuity"));

	scored.scores.final = combineFinalScore(scored, context);
	scored.evidence.removeDuplicates();
	return scored;
}

QVector<ClipCandidate> SemanticClipScorer::scoreBatch(const TranscriptIndex &index,
						      const QVector<ClipCandidate> &candidates,
						      const SemanticScoringContext &context,
						      const SemanticClipScoringOptions &options,
						      const SemanticEmbeddingProvider *provider) const
{
	if (!provider || !provider->isAvailable() || candidates.isEmpty()) {
		QVector<ClipCandidate> result;
		result.reserve(static_cast<long long>(candidates.size()));
		for (const ClipCandidate &candidate : candidates)
			result.append(score(index, candidate, context, options, provider));
		return result;
	}

	QStringList textsToWarm;
	const QString languageCode =
		normalizedSemanticLanguageCode(context.transcriptionLanguage, context.sourceLanguage);
	const SemanticPrototypeSet &prototypes = semanticPrototypesForLanguage(languageCode);
	const QStringList targetPrototypes =
		targetPrototypesForPreset(context.presetId, context.mainTarget, languageCode);
	const SemanticPrototypeBudget prototypeBudget =
		prototypeBudgetFor(targetPrototypes, prototypes, options.maxPrototypeTexts);
	for (const QStringList &group :
	     {prototypeBudget.target, prototypeBudget.viewerMessage, prototypeBudget.directAnswer,
	      prototypeBudget.noise, prototypeBudget.topicShift, prototypeBudget.clipValue, prototypeBudget.empathy,
	      prototypeBudget.hook, prototypeBudget.resolution, prototypeBudget.metaNoise}) {
		for (const QString &prototype : group)
			appendUniquePreparedText(textsToWarm, prototype);
	}

	for (const ClipCandidate &candidate : candidates) {
		if (candidate.text.trimmed().size() >= options.minTextChars)
			appendUniquePreparedText(textsToWarm, candidate.text);
		appendUniquePreparedText(textsToWarm, openingText(index, candidate));
		appendUniquePreparedText(textsToWarm, endingText(index, candidate));
		if (options.enablePairwiseTopicContinuity) {
			appendUniquePreparedText(textsToWarm, firstHalfText(index, candidate));
			appendUniquePreparedText(textsToWarm, secondHalfText(index, candidate));
		}
	}

	if (!textsToWarm.isEmpty())
		provider->embedBatch(textsToWarm);

	QVector<ClipCandidate> result;
	result.reserve(static_cast<long long>(candidates.size()));
	for (const ClipCandidate &candidate : candidates)
		result.append(score(index, candidate, context, options, provider));
	return result;
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

void SemanticClipScorer::scoreOpeningAndEnding(const TranscriptIndex &index, ClipCandidate &candidate,
					       const SemanticEmbeddingProvider &provider,
					       const QStringList &hookPrototypes,
					       const QStringList &resolutionPrototypes,
					       const QStringList &metaNoisePrototypes,
					       const QStringList &topicShiftPrototypes) const
{
	const QString opening = openingText(index, candidate);
	const QString ending = endingText(index, candidate);
	if (opening.trimmed().size() >= 24) {
		const SemanticEmbedding openingEmbedding = provider.embed(opening);
		if (openingEmbedding.isValid()) {
			candidate.scores.semanticOpeningHook =
				maxPrototypeSimilarity(provider, openingEmbedding, hookPrototypes);
			candidate.scores.semanticOpeningMetaNoise =
				maxPrototypeSimilarity(provider, openingEmbedding, metaNoisePrototypes);
			candidate.scores.semanticHook =
				std::max(candidate.scores.semanticHook, candidate.scores.semanticOpeningHook);
		}
	}
	if (ending.trimmed().size() >= 24) {
		const SemanticEmbedding endingEmbedding = provider.embed(ending);
		if (endingEmbedding.isValid()) {
			candidate.scores.semanticEndingResolution =
				maxPrototypeSimilarity(provider, endingEmbedding, resolutionPrototypes);
			candidate.scores.semanticEndingMetaNoise =
				maxPrototypeSimilarity(provider, endingEmbedding, metaNoisePrototypes);
			candidate.scores.semanticEndingTopicShift =
				maxPrototypeSimilarity(provider, endingEmbedding, topicShiftPrototypes);
			candidate.scores.semanticResolution = std::max(candidate.scores.semanticResolution,
								       candidate.scores.semanticEndingResolution);
		}
	}
	if (opening.trimmed().size() >= 24 && ending.trimmed().size() >= 24) {
		const SemanticEmbedding openingEmbedding = provider.embed(opening);
		const SemanticEmbedding endingEmbedding = provider.embed(ending);
		if (openingEmbedding.isValid() && endingEmbedding.isValid())
			candidate.scores.semanticFocusContinuity =
				boundedScore(cosineSimilarity(openingEmbedding, endingEmbedding));
	}
}

double SemanticClipScorer::combineFinalScore(const ClipCandidate &candidate,
					     const SemanticScoringContext &context) const
{
	const ClipCandidateScores &s = candidate.scores;
	const double semanticNoisePenalty =
		std::max({s.noise, s.semanticNoise, s.semanticMetaNoise, s.semanticOpeningMetaNoise,
			  s.semanticEndingMetaNoise, s.semanticEndingTopicShift}) *
		(s.semanticEmpathy >= 0.64 ? 0.42 : 0.48);
	const bool pauseWasTrimmed =
		candidate.evidence.contains(QStringLiteral("semantic_boundary_pause_tail_trimmed")) ||
		candidate.evidence.contains(QStringLiteral("semantic_boundary_pause_tail_blocked")) ||
		candidate.evidence.contains(QStringLiteral("exchange_arc_pause_tail_trimmed")) ||
		candidate.evidence.contains(QStringLiteral("exchange_arc_pause_tail_blocked"));
	const double unresolvedLongPausePenalty = (!pauseWasTrimmed && s.maxInternalPauseSec >= 4.0)
							  ? boundedScore((s.maxInternalPauseSec - 3.0) / 4.0) * 0.08
							  : 0.0;
	const bool empathyClearlyUseful =
		s.semanticEmpathy >= 0.60 &&
		(s.semanticEmpathy >=
			 std::max({s.semanticMetaNoise, s.semanticOpeningMetaNoise, s.semanticEndingMetaNoise}) +
				 0.02 ||
		 s.semanticDirectAnswer >= 0.62 || s.semanticViewerMessage >= 0.62 || s.semanticTarget >= 0.62);
	const double empathySignal = empathyClearlyUseful ? s.semanticEmpathy : s.semanticEmpathy * 0.55;
	const double valueSignal =
		std::max({s.semanticClipValue, empathySignal, s.advice, s.explanation, s.story, s.opinion, s.tutorial});
	const double hookSignal = std::max({s.hook, s.semanticHook, s.semanticOpeningHook, s.arcOpening});
	const double resolutionSignal =
		std::max({s.semanticResolution, s.semanticEndingResolution, s.topicContinuity, s.arcConclusion});
	const double arcSignal =
		s.arcCompleteness > 0.0
			? boundedScore((s.arcCompleteness * 0.46) + (s.arcBoundaryCleanliness * 0.24) +
				       (s.arcDevelopment * 0.18) + (s.arcConclusion * 0.12) - (s.arcTailRisk * 0.10))
			: 0.0;

	if (context.reliableMainTarget && !context.mainTarget.trimmed().isEmpty()) {
		return boundedScore((s.semanticTarget * 0.28) + (s.viewerResponse * 0.18) + (valueSignal * 0.24) +
				    (hookSignal * 0.08) + (resolutionSignal * 0.12) + (s.boundary * 0.10) -
				    semanticNoisePenalty - unresolvedLongPausePenalty);
	}

	if (context.presetId == QStringLiteral("viewer_message_response")) {
		return boundedScore((s.viewerResponse * 0.18) + (valueSignal * 0.20) + (resolutionSignal * 0.14) +
				    (hookSignal * 0.10) + (arcSignal * 0.18) + (s.semanticTarget * 0.08) +
				    (s.boundary * 0.08) + (s.duration * 0.04) - semanticNoisePenalty -
				    unresolvedLongPausePenalty);
	}

	return boundedScore((s.boundary * 0.16) + (s.duration * 0.08) + (hookSignal * 0.12) +
			    (s.semanticTarget * 0.18) + (resolutionSignal * 0.16) + (valueSignal * 0.30) -
			    semanticNoisePenalty - unresolvedLongPausePenalty);
}

QString SemanticClipScorer::firstHalfText(const TranscriptIndex &index, const ClipCandidate &candidate) const
{
	if (candidate.firstSegmentIndex < 0 || candidate.lastSegmentIndex < candidate.firstSegmentIndex)
		return {};

	const int midpoint =
		candidate.firstSegmentIndex + ((candidate.lastSegmentIndex - candidate.firstSegmentIndex) / 2);
	return index.textForSegmentWindow(candidate.firstSegmentIndex, midpoint);
}

QString SemanticClipScorer::secondHalfText(const TranscriptIndex &index, const ClipCandidate &candidate) const
{
	if (candidate.firstSegmentIndex < 0 || candidate.lastSegmentIndex < candidate.firstSegmentIndex)
		return {};

	const int midpoint =
		candidate.firstSegmentIndex + ((candidate.lastSegmentIndex - candidate.firstSegmentIndex) / 2) + 1;
	return index.textForSegmentWindow(midpoint, candidate.lastSegmentIndex);
}

QString SemanticClipScorer::openingText(const TranscriptIndex &index, const ClipCandidate &candidate) const
{
	if (candidate.firstSegmentIndex < 0 || candidate.lastSegmentIndex < candidate.firstSegmentIndex)
		return {};

	const double durationSec = candidate.range.endSec - candidate.range.startSec;
	const double maxOpeningSec = std::clamp(durationSec * 0.35, 6.0, 14.0);
	const double openingEndSec = std::min(candidate.range.endSec, candidate.range.startSec + maxOpeningSec);
	return index.textForRange({candidate.range.startSec, openingEndSec});
}

QString SemanticClipScorer::endingText(const TranscriptIndex &index, const ClipCandidate &candidate) const
{
	if (candidate.firstSegmentIndex < 0 || candidate.lastSegmentIndex < candidate.firstSegmentIndex)
		return {};

	const double durationSec = candidate.range.endSec - candidate.range.startSec;
	const double maxEndingSec = std::clamp(durationSec * 0.35, 6.0, 14.0);
	const double endingStartSec = std::max(candidate.range.startSec, candidate.range.endSec - maxEndingSec);
	return index.textForRange({endingStartSec, candidate.range.endSec});
}
