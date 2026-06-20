#include "curation/scoring/semantic-reranker.hpp"

#include <algorithm>
#include <cmath>

using namespace Curation::Scoring;

namespace {

static double boundedScore(double value)
{
	return std::clamp(value, 0.0, 1.0);
}

static double absoluteRerankerScore(double rawScore)
{
	if (rawScore >= 0.0 && rawScore <= 1.0)
		return boundedScore(rawScore);
	return boundedScore(1.0 / (1.0 + std::exp(-rawScore)));
}

static QVector<double> normalizedScores(const QVector<double> &rawScores)
{
	if (rawScores.isEmpty())
		return {};

	double minScore = rawScores.first();
	double maxScore = rawScores.first();
	for (const double score : rawScores) {
		minScore = std::min(minScore, score);
		maxScore = std::max(maxScore, score);
	}

	QVector<double> result;
	result.reserve(static_cast<long long>(rawScores.size()));
	if (std::fabs(maxScore - minScore) < 0.000001) {
		for (const double score : rawScores)
			result.append(boundedScore(score));
		return result;
	}

	for (const double score : rawScores)
		result.append(boundedScore((score - minScore) / (maxScore - minScore)));
	return result;
}

} // namespace


QVector<double> SemanticReranker::scoreBatch(const QString &query, const QVector<QString> &candidateTexts) const
{
	QVector<double> scores;
	scores.reserve(static_cast<long long>(candidateTexts.size()));
	for (const QString &candidateText : candidateTexts)
		scores.append(score(query, candidateText));
	return scores;
}

QVector<ClipCandidate> SemanticRerankerStage::apply(QVector<ClipCandidate> candidates,
						    const SemanticRerankerContext &context,
						    const SemanticRerankerOptions &options,
						    const SemanticReranker *reranker) const
{
	if (!options.enabled) {
		for (ClipCandidate &candidate : candidates)
			candidate.evidence.append(QStringLiteral("reranker_disabled"));
		return candidates;
	}

	if (!reranker || !reranker->isAvailable()) {
		for (ClipCandidate &candidate : candidates)
			candidate.evidence.append(QStringLiteral("reranker_unavailable"));
		return candidates;
	}

	const bool hasValidSemanticEmbeddings = std::any_of(candidates.constBegin(), candidates.constEnd(),
								     [](const ClipCandidate &candidate) {
									     return candidate.semanticScoringAvailable;
								     });
	if (!hasValidSemanticEmbeddings) {
		for (ClipCandidate &candidate : candidates)
			candidate.evidence.append(QStringLiteral("reranker_skipped_no_valid_embeddings"));
		return candidates;
	}

	const QString query = queryForContext(context);
	const QString badClipQuery = badClipQueryForContext(context);
	QVector<QString> candidateTexts;
	candidateTexts.reserve(static_cast<long long>(candidates.size()));
	for (const ClipCandidate &candidate : candidates)
		candidateTexts.append(documentForCandidate(candidate));

	for (ClipCandidate &candidate : candidates)
		candidate.rerankerAttempted = true;

	const QVector<double> rawRerankerScores = reranker->scoreBatch(query, candidateTexts);
	if (rawRerankerScores.size() != candidates.size()) {
		QString failureReason = reranker->lastError().trimmed();
		if (failureReason.isEmpty())
			failureReason = QStringLiteral("invalid_batch_response");
		for (ClipCandidate &candidate : candidates) {
			candidate.rerankerFailed = true;
			candidate.rerankerFailureReason = failureReason;
			candidate.evidence.append(QStringLiteral("reranker_invalid_batch_response"));
			if (!failureReason.isEmpty())
				candidate.evidence.append(QStringLiteral("reranker_failure:%1").arg(failureReason.left(120)));
		}
		return candidates;
	}

	const QVector<double> rawBadClipScores = reranker->scoreBatch(badClipQuery, candidateTexts);
	const bool hasBadClipScores = rawBadClipScores.size() == candidates.size();
	if (!hasBadClipScores) {
		for (ClipCandidate &candidate : candidates)
			candidate.evidence.append(QStringLiteral("reranker_bad_clip_score_unavailable"));
	}

	const QVector<double> rerankerScores = normalizedScores(rawRerankerScores);
	for (qsizetype i = 0; i < candidates.size(); ++i) {
		ClipCandidate &candidate = candidates[i];
		const double rawScore = absoluteRerankerScore(rawRerankerScores.at(i));
		const double badClipScore = hasBadClipScores ? absoluteRerankerScore(rawBadClipScores.at(i)) : 0.0;
		const double margin = rawScore - badClipScore;
		const double rerankerScore = rerankerScoreFromRaw(rawScore, boundedScore(rerankerScores.at(i)), badClipScore);
		candidate.scores.reranker = rerankerScore;
		candidate.scores.rerankerRaw = rawScore;
		candidate.scores.rerankerBadClip = badClipScore;
		candidate.scores.rerankerClipQualityMargin = margin;
		candidate.rerankerAvailable = true;
		candidate.rerankerFailed = false;
		candidate.rerankerFailureReason.clear();
		candidate.scores.final = combineFinalScore(candidate, rerankerScore, options.contributionWeight);
		candidate.evidence.append(QStringLiteral("reranker_model:%1").arg(reranker->modelId().left(80)));
		candidate.evidence.append(QStringLiteral("reranker_raw:%1").arg(QString::number(rawScore, 'f', 2)));
		if (hasBadClipScores) {
			candidate.evidence.append(QStringLiteral("reranker_bad:%1").arg(QString::number(badClipScore, 'f', 2)));
			candidate.evidence.append(QStringLiteral("reranker_margin:%1").arg(QString::number(margin, 'f', 2)));
		}
		if ((rerankerScore >= 0.72 || rawScore >= 0.82) && margin >= 0.12)
			candidate.evidence.append(QStringLiteral("reranker_strong_match"));
		candidate.evidence.removeDuplicates();
	}

	return candidates;
}

QString SemanticRerankerStage::queryForContext(const SemanticRerankerContext &context) const
{
	const QString target = context.mainTarget.trimmed();
	QString task = QStringLiteral(
		"Rank transcript excerpts by actual short-form clip quality. A good clip must start with a strong hook, stay on one subject, "
		"contain useful advice, emotional reflection, confession, or philosophical payoff, and end with a smooth resolution. "
		"Do not reward general politeness, greetings, donations, follower goals, live management, moderation, casual chat, "
		"or multiple unrelated viewer interactions. A clip with no clear takeaway or no conclusion is bad even if the answer is respectful.");

	if (!target.isEmpty()) {
		task += QStringLiteral(" The best excerpt must be specifically about: %1.").arg(target);
	} else if (context.presetId == QStringLiteral("viewer_message_response")) {
		task += QStringLiteral(" The best excerpt is one complete valuable response to one viewer message, with advice, insight, or emotional payoff.");
	} else if (context.presetId == QStringLiteral("advice_answer")) {
		task += QStringLiteral(" The best excerpt gives practical advice with a clear recommendation and resolution.");
	} else if (context.presetId == QStringLiteral("emotional_reaction")) {
		task += QStringLiteral(" The best excerpt has a strong emotional reaction with a clear payoff and complete moment.");
	} else {
		task += QStringLiteral(" The best excerpt is a self-contained high-quality clip with a clear beginning and resolution.");
	}

	return task;
}

QString SemanticRerankerStage::documentForCandidate(const ClipCandidate &candidate) const
{
	const double durationSec = candidate.range.endSec - candidate.range.startSec;
	QString document = QStringLiteral("Duration: %1s. Transcript: %2")
		.arg(QString::number(durationSec, 'f', 1), candidate.text.simplified());
	return document.simplified();
}

QString SemanticRerankerStage::badClipQueryForContext(const SemanticRerankerContext &context) const
{
	QString task = QStringLiteral(
		"Rank transcript excerpts by how bad they are as short-form clips. Highest score means the excerpt is mostly greetings, "
		"donation/sub/follower talk, stream management, moderation or spam rules, casual banter, fragmented setup, "
		"multiple unrelated viewer topics, topic changes, or has no hook, no useful takeaway, and no smooth ending.");
	if (context.presetId == QStringLiteral("viewer_message_response"))
		task += QStringLiteral(" For viewer-message clips, bad examples include reading several chat messages or answering without a valuable payoff.");
	return task;
}

double SemanticRerankerStage::rerankerScoreFromRaw(double rawScore, double normalizedScore, double badClipScore) const
{
	const double absolute = boundedScore(rawScore);
	const double relative = boundedScore(normalizedScore);
	const double bad = boundedScore(badClipScore);
	const double marginBonus = std::max(0.0, absolute - bad) * 0.24;
	return boundedScore((absolute * 0.72) + (relative * 0.18) + marginBonus - (bad * 0.58));
}

double SemanticRerankerStage::combineFinalScore(const ClipCandidate &candidate, double rerankerScore,
						       double contributionWeight) const
{
	const double weight = std::clamp(contributionWeight, 0.0, 0.65);
	return boundedScore((candidate.scores.final * (1.0 - weight)) + (rerankerScore * weight));
}
