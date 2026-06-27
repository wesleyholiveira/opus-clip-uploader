#include "curation/scoring/feedback-similarity-scorer.hpp"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace Curation::Scoring {

namespace {

static double boundedScore(double value)
{
	return std::clamp(value, 0.0, 1.0);
}

static bool isWeakNegativeSignal(const Curation::Feedback::FeedbackRangeSignal &signal)
{
	return signal.weakNegative || signal.ignoreForTraining ||
	       signal.decision == QStringLiteral("ignored_diagnostic") ||
	       signal.reason.contains(QStringLiteral("weak_negative"), Qt::CaseInsensitive) ||
	       signal.reason.contains(QStringLiteral("ignored_diagnostic"), Qt::CaseInsensitive) ||
	       signal.reason.contains(QStringLiteral("not_training_signal"), Qt::CaseInsensitive);
}

static bool isIgnoredDiagnosticSignal(const Curation::Feedback::FeedbackRangeSignal &signal)
{
	return signal.ignoreForTraining || signal.decision == QStringLiteral("ignored_diagnostic") ||
	       signal.reason.contains(QStringLiteral("ignored_diagnostic"), Qt::CaseInsensitive) ||
	       signal.reason.contains(QStringLiteral("not_training_signal"), Qt::CaseInsensitive);
}

static QVector<Curation::Feedback::FeedbackRangeSignal>
negativeSignalsForScoring(const QVector<Curation::Feedback::FeedbackRangeSignal> &results)
{
	QVector<Curation::Feedback::FeedbackRangeSignal> filtered;
	filtered.reserve(results.size());
	for (const Curation::Feedback::FeedbackRangeSignal &signal : results) {
		if (isIgnoredDiagnosticSignal(signal))
			continue;
		filtered.append(signal);
	}
	return filtered;
}

static QStringList tokenize(QString value)
{
	value = value.toLower().normalized(QString::NormalizationForm_KC);
	value.replace(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}]+")), QStringLiteral(" "));
	const QStringList raw = value.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
	QStringList tokens;
	tokens.reserve(raw.size());
	static const QSet<QString> stopwords = {
		QStringLiteral("que"),    QStringLiteral("pra"),    QStringLiteral("para"),  QStringLiteral("com"),
		QStringLiteral("uma"),    QStringLiteral("uns"),    QStringLiteral("das"),   QStringLiteral("dos"),
		QStringLiteral("por"),    QStringLiteral("isso"),   QStringLiteral("aqui"),  QStringLiteral("ali"),
		QStringLiteral("tipo"),   QStringLiteral("cara"),   QStringLiteral("mano"),  QStringLiteral("então"),
		QStringLiteral("porque"), QStringLiteral("você"),   QStringLiteral("voce"),  QStringLiteral("ele"),
		QStringLiteral("ela"),    QStringLiteral("eles"),   QStringLiteral("elas"),  QStringLiteral("meu"),
		QStringLiteral("minha"),  QStringLiteral("seu"),    QStringLiteral("sua"),   QStringLiteral("não"),
		QStringLiteral("sim"),    QStringLiteral("bom"),    QStringLiteral("boa"),   QStringLiteral("né"),
		QStringLiteral("assim"),  QStringLiteral("também"), QStringLiteral("tambem")};
	for (const QString &token : raw) {
		if (token.size() < 3 || stopwords.contains(token))
			continue;
		tokens.append(token.left(32));
	}
	tokens.removeDuplicates();
	return tokens;
}

} // namespace

double FeedbackSimilarityScorer::rangeDurationSec(const ClipDuration &range)
{
	return std::max(0.0, range.endSec - range.startSec);
}

double FeedbackSimilarityScorer::rangeCenterSec(const ClipDuration &range)
{
	return range.startSec + ((range.endSec - range.startSec) * 0.5);
}

double FeedbackSimilarityScorer::overlapSec(const ClipDuration &left, const ClipDuration &right)
{
	return std::max(0.0, std::min(left.endSec, right.endSec) - std::max(left.startSec, right.startSec));
}

double FeedbackSimilarityScorer::rangeSimilarity(const ClipDuration &candidate, const ClipDuration &feedback)
{
	const double candidateDuration = rangeDurationSec(candidate);
	const double feedbackDuration = rangeDurationSec(feedback);
	if (candidateDuration <= 0.0 || feedbackDuration <= 0.0)
		return 0.0;
	const double overlap = overlapSec(candidate, feedback);
	const double uni =
		std::max(candidate.endSec, feedback.endSec) - std::min(candidate.startSec, feedback.startSec);
	const double iou = uni > 0.0 ? overlap / uni : 0.0;
	const double coverage = overlap / std::min(candidateDuration, feedbackDuration);
	const double startScore = std::max(0.0, 1.0 - (std::fabs(candidate.startSec - feedback.startSec) / 18.0));
	const double endScore = std::max(0.0, 1.0 - (std::fabs(candidate.endSec - feedback.endSec) / 24.0));
	return boundedScore((iou * 0.48) + (coverage * 0.26) + (startScore * 0.16) + (endScore * 0.10));
}

bool FeedbackSimilarityScorer::negativeRangeContaminatesCandidate(
	const ClipDuration &candidate, const Curation::Feedback::FeedbackRangeSignal &negative)
{
	const double overlap = overlapSec(candidate, negative.range);
	const double negativeDuration = rangeDurationSec(negative.range);
	const double candidateDuration = rangeDurationSec(candidate);
	if (overlap <= 0.0 || negativeDuration <= 0.0 || candidateDuration <= 0.0)
		return false;
	const bool coversRejectedCore = overlap >= std::min(10.0, negativeDuration * 0.55);
	const bool containsRejectedCenter = candidate.startSec <= rangeCenterSec(negative.range) &&
					    candidate.endSec >= rangeCenterSec(negative.range) &&
					    overlap >= std::min(6.0, negativeDuration * 0.35);
	const bool boundaryAlmostSame = std::fabs(candidate.startSec - negative.range.startSec) <= 4.0 ||
					std::fabs(candidate.endSec - negative.range.endSec) <= 5.0;
	return coversRejectedCore || containsRejectedCenter || (boundaryAlmostSame && overlap >= 5.0);
}

bool FeedbackSimilarityScorer::positiveRangeExplainsCandidate(const ClipDuration &candidate,
							      const Curation::Feedback::FeedbackRangeSignal &positive)
{
	const double overlap = overlapSec(candidate, positive.range);
	const double positiveDuration = rangeDurationSec(positive.range);
	const double candidateDuration = rangeDurationSec(candidate);
	if (overlap <= 0.0 || positiveDuration <= 0.0 || candidateDuration <= 0.0)
		return false;
	const double positiveCoverage = overlap / positiveDuration;
	const double candidateCoverage = overlap / candidateDuration;
	const bool closeStart = std::fabs(candidate.startSec - positive.range.startSec) <= 5.0;
	const bool closeEnd = std::fabs(candidate.endSec - positive.range.endSec) <= 6.0;
	return positiveCoverage >= 0.72 && candidateCoverage >= 0.58 &&
	       (closeStart || closeEnd || candidateDuration <= positiveDuration * 1.35);
}

double FeedbackSimilarityScorer::lexicalSimilarity(const QString &left, const QString &right)
{
	const QStringList leftTokens = tokenize(left);
	const QStringList rightTokens = tokenize(right);
	if (leftTokens.isEmpty() || rightTokens.isEmpty())
		return 0.0;
	QSet<QString> leftSet;
	for (const QString &token : leftTokens)
		leftSet.insert(token);
	QSet<QString> rightSet;
	for (const QString &token : rightTokens)
		rightSet.insert(token);
	int intersection = 0;
	for (const QString &token : leftSet) {
		if (rightSet.contains(token))
			++intersection;
	}
	const int uni = leftSet.size() + rightSet.size() - intersection;
	if (uni <= 0)
		return 0.0;
	const double jaccard = static_cast<double>(intersection) / static_cast<double>(uni);
	const double containment =
		static_cast<double>(intersection) / static_cast<double>(std::min(leftSet.size(), rightSet.size()));
	return boundedScore((jaccard * 0.55) + (containment * 0.45));
}

double FeedbackSimilarityScorer::bestRangeSimilarity(const ClipDuration &range,
						     const QVector<Curation::Feedback::FeedbackRangeSignal> &results,
						     Curation::Feedback::FeedbackRangeSignal *bestSignal) const
{
	double bestScore = 0.0;
	Curation::Feedback::FeedbackRangeSignal best;
	for (const Curation::Feedback::FeedbackRangeSignal &signal : results) {
		const double score = rangeSimilarity(range, signal.range) * std::max(0.05, signal.weight);
		if (score > bestScore) {
			bestScore = score;
			best = signal;
		}
	}
	if (bestSignal)
		*bestSignal = best;
	return boundedScore(bestScore);
}

double FeedbackSimilarityScorer::bestOverlapSeconds(const ClipDuration &range,
						    const QVector<Curation::Feedback::FeedbackRangeSignal> &results,
						    Curation::Feedback::FeedbackRangeSignal *bestSignal) const
{
	double bestOverlap = 0.0;
	Curation::Feedback::FeedbackRangeSignal best;
	for (const Curation::Feedback::FeedbackRangeSignal &signal : results) {
		const double overlap = overlapSec(range, signal.range) * std::max(0.05, signal.weight);
		if (overlap > bestOverlap) {
			bestOverlap = overlap;
			best = signal;
		}
	}
	if (bestSignal)
		*bestSignal = best;
	return bestOverlap;
}

double FeedbackSimilarityScorer::bestTextSimilarity(const QString &text, const TranscriptIndex &index,
						    const QVector<Curation::Feedback::FeedbackRangeSignal> &results,
						    Curation::Feedback::FeedbackRangeSignal *bestSignal) const
{
	const QString clean = text.trimmed();
	if (clean.isEmpty())
		return 0.0;
	double bestScore = 0.0;
	Curation::Feedback::FeedbackRangeSignal best;
	for (const Curation::Feedback::FeedbackRangeSignal &signal : results) {
		const QString signalText = index.textForRange(signal.range).trimmed();
		if (signalText.isEmpty())
			continue;
		const double score = lexicalSimilarity(clean, signalText) * std::max(0.05, signal.weight);
		if (score > bestScore) {
			bestScore = score;
			best = signal;
		}
	}
	if (bestSignal)
		*bestSignal = best;
	return boundedScore(bestScore);
}

FeedbackSimilarityFeatures FeedbackSimilarityScorer::score(const ClipCandidate &candidate, const TranscriptIndex &index,
							   const Curation::Feedback::FeedbackRangeMemory &memory) const
{
	const QString text = candidate.text.trimmed().isEmpty() ? index.textForRange(candidate.range) : candidate.text;
	return scoreRange(candidate.range, text, index, memory);
}

FeedbackSimilarityFeatures
FeedbackSimilarityScorer::scoreRange(const ClipDuration &range, const QString &text, const TranscriptIndex &index,
				     const Curation::Feedback::FeedbackRangeMemory &memory) const
{
	FeedbackSimilarityFeatures features;
	Curation::Feedback::FeedbackRangeSignal positiveRange;
	Curation::Feedback::FeedbackRangeSignal negativeRange;
	Curation::Feedback::FeedbackRangeSignal positiveText;
	Curation::Feedback::FeedbackRangeSignal negativeText;

	const QVector<Curation::Feedback::FeedbackRangeSignal> negativeScoringRanges =
		negativeSignalsForScoring(memory.negativeRanges);

	features.positiveRangeSimilarity = bestRangeSimilarity(range, memory.positiveRanges, &positiveRange);
	features.negativeRangeSimilarity = bestRangeSimilarity(range, negativeScoringRanges, &negativeRange);
	features.positiveOverlapSec = bestOverlapSeconds(range, memory.positiveRanges, &positiveRange);
	features.negativeOverlapSec = bestOverlapSeconds(range, negativeScoringRanges, &negativeRange);
	features.positiveTextSimilarity = bestTextSimilarity(text, index, memory.positiveRanges, &positiveText);
	features.negativeTextSimilarity = bestTextSimilarity(text, index, negativeScoringRanges, &negativeText);
	features.positiveScore =
		boundedScore(std::max(features.positiveRangeSimilarity, features.positiveTextSimilarity * 0.92));
	features.negativeScore =
		boundedScore(std::max(features.negativeRangeSimilarity, features.negativeTextSimilarity * 0.96));
	features.margin = features.positiveScore - features.negativeScore;
	const bool weakNegativeRange = isWeakNegativeSignal(negativeRange);
	features.negativeRangeContamination = !negativeScoringRanges.isEmpty() && !weakNegativeRange &&
					      negativeRangeContaminatesCandidate(range, negativeRange);
	if (weakNegativeRange && features.negativeScore > 0.0)
		features.evidence.append(QStringLiteral("feedback_weak_negative_match"));
	features.explainedByPositiveRange = !memory.positiveRanges.isEmpty() &&
					    positiveRangeExplainsCandidate(range, positiveRange);
	features.positiveReason = positiveRange.reason.isEmpty() ? positiveText.reason : positiveRange.reason;
	features.negativeReason = negativeRange.reason.isEmpty() ? negativeText.reason : negativeRange.reason;
	features.positiveDecision = positiveRange.decision.isEmpty() ? positiveText.decision : positiveRange.decision;
	features.negativeDecision = negativeRange.decision.isEmpty() ? negativeText.decision : negativeRange.decision;
	features.evidence.append(QStringLiteral("feedback_positive_range:%1")
					 .arg(QString::number(features.positiveRangeSimilarity, 'f', 2)));
	features.evidence.append(QStringLiteral("feedback_negative_range:%1")
					 .arg(QString::number(features.negativeRangeSimilarity, 'f', 2)));
	features.evidence.append(QStringLiteral("feedback_positive_text:%1")
					 .arg(QString::number(features.positiveTextSimilarity, 'f', 2)));
	features.evidence.append(QStringLiteral("feedback_negative_text:%1")
					 .arg(QString::number(features.negativeTextSimilarity, 'f', 2)));
	features.evidence.append(QStringLiteral("feedback_margin:%1").arg(QString::number(features.margin, 'f', 2)));
	if (features.negativeRangeContamination)
		features.evidence.append(QStringLiteral("feedback_negative_contamination"));
	if (features.explainedByPositiveRange)
		features.evidence.append(QStringLiteral("feedback_positive_range_explains_candidate"));
	return features;
}

} // namespace Curation::Scoring
