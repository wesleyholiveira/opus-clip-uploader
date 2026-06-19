#include "curation/range/viewer-message-focus-range.hpp"

#include "curation/curation-preset.hpp"
#include "curation/curation-signals.hpp"

#include <obs-module.h>

#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace {

static constexpr double MIN_FOCUS_DURATION_SEC = 45.0;
static constexpr double MAX_FOCUS_DURATION_SEC = 150.0;
static constexpr double TARGET_ANCHOR_PADDING_BEFORE_SEC = 1.0;
static constexpr double TARGET_ANCHOR_AFTER_SEC = 120.0;
static constexpr double MIN_TARGET_ANCHOR_CONFIDENCE = 0.55;
static constexpr double PREAMBLE_ONLY_SEGMENT_OFFSET_SEC = 2.5;

static constexpr double VIEWER_CANDIDATE_PADDING_BEFORE_SEC = 0.35;
static constexpr double VIEWER_CANDIDATE_DEFAULT_AFTER_SEC = 28.0;
static constexpr double VIEWER_CANDIDATE_EMOTIONAL_AFTER_SEC = 34.0;
static constexpr double VIEWER_CANDIDATE_ADVICE_AFTER_SEC = 30.0;
static constexpr double VIEWER_CANDIDATE_MIN_DURATION_SEC = 18.0;
static constexpr double VIEWER_CANDIDATE_MAX_DURATION_SEC = 42.0;
static constexpr double VIEWER_CANDIDATE_MIN_SCORE = 0.55;
static constexpr double VIEWER_CANDIDATE_OVERLAP_TOLERANCE_SEC = 8.0;
static constexpr int MAX_LOGGED_CANDIDATES = 5;

QString stripDiacritics(const QString &value)
{
	const QString normalized = value.normalized(QString::NormalizationForm_D);
	QString output;
	output.reserve(normalized.size());

	for (const QChar ch : normalized) {
		if (ch.category() == QChar::Mark_NonSpacing)
			continue;
		output.append(ch);
	}

	return output;
}

QString normalizedText(QString value)
{
	value = stripDiacritics(value).toLower();
	value.replace(QRegularExpression(QStringLiteral("[^a-z0-9\\s]+")), QStringLiteral(" "));
	value.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
	return value.trimmed();
}

QString extractTargetFromPrompt(const QString &prompt)
{
	static const QRegularExpression specificPattern(
		QStringLiteral("specifically\\s+about\\s+([^\\.]+)"),
		QRegularExpression::CaseInsensitiveOption);
	QRegularExpressionMatch match = specificPattern.match(prompt);
	if (match.hasMatch())
		return match.captured(1).trimmed();

	static const QRegularExpression aboutPattern(QStringLiteral("single viewer message\\s+about\\s+([^\\.]+)"),
						       QRegularExpression::CaseInsensitiveOption);
	match = aboutPattern.match(prompt);
	if (match.hasMatch())
		return match.captured(1).trimmed();

	return {};
}

QVector<ClipDuration> validRanges(const CurationSettings &settings)
{
	QVector<ClipDuration> ranges;
	for (const ClipDuration &range : settings.clipDurations) {
		if (std::isfinite(range.startSec) && std::isfinite(range.endSec) && range.endSec > range.startSec)
			ranges.append(range);
	}

	if (ranges.isEmpty() && settings.rangeEndSec > settings.rangeStartSec)
		ranges.append({settings.rangeStartSec, settings.rangeEndSec});

	return ranges;
}

bool segmentOverlapsRange(const TranscriptSegment &segment, const ClipDuration &range)
{
	return std::min(segment.endSec, range.endSec) > std::max(segment.startSec, range.startSec);
}

QStringList wordsFromTarget(const QString &target)
{
	const QString normalized = normalizedText(target);
	QStringList words = normalized.split(' ', Qt::SkipEmptyParts);
	words.erase(std::remove_if(words.begin(), words.end(), [](const QString &word) {
		static const QSet<QString> stopWords{QStringLiteral("a"), QStringLiteral("an"), QStringLiteral("the"),
						       QStringLiteral("to"), QStringLiteral("for"), QStringLiteral("of"),
						       QStringLiteral("with"), QStringLiteral("and"), QStringLiteral("or"),
						       QStringLiteral("about"), QStringLiteral("how"), QStringLiteral("your"),
						       QStringLiteral("their"), QStringLiteral("his"), QStringLiteral("her"),
						       QStringLiteral("one"), QStringLiteral("single"), QStringLiteral("viewer"),
						       QStringLiteral("message"), QStringLiteral("response"), QStringLiteral("boss")};
		return word.size() < 3 || stopWords.contains(word);
	}),
		    words.end());
	words.removeDuplicates();
	return words;
}

bool containsPortugueseTargetMarkers(const QString &target)
{
	const QString normalized = normalizedText(target);
	const QString padded = QStringLiteral(" ") + normalized + QLatin1Char(' ');

	for (const QString &hardToken : {QStringLiteral("resposta"), QStringLiteral("pergunta"),
						 QStringLiteral("mensagem"), QStringLiteral("espectador"),
						 QStringLiteral("conversa"), QStringLiteral("aumento"),
						 QStringLiteral("chefe"), QStringLiteral("pedir"),
						 QStringLiteral("numa"), QStringLiteral("num")}) {
		if (padded.contains(QStringLiteral(" ") + hardToken + QLatin1Char(' ')))
			return true;
	}

	int softHits = 0;
	for (const QString &softToken : {QStringLiteral("sobre"), QStringLiteral("como"), QStringLiteral("para"),
						 QStringLiteral("com"), QStringLiteral("uma"), QStringLiteral("em"),
						 QStringLiteral("de"), QStringLiteral("da"), QStringLiteral("do"),
						 QStringLiteral("dos"), QStringLiteral("das")}) {
		if (padded.contains(QStringLiteral(" ") + softToken + QLatin1Char(' ')))
			++softHits;
	}

	return softHits >= 2;
}

bool isGenericFocusTarget(const QString &target)
{
	const QString normalized = normalizedText(target);
	if (normalized.isEmpty())
		return true;

	if (normalized.startsWith(QStringLiteral("one clear idea")) ||
	    normalized.startsWith(QStringLiteral("the selected idea")) ||
	    normalized.contains(QStringLiteral("selected range")) ||
	    normalized.contains(QStringLiteral("one exchange")) ||
	    normalized.contains(QStringLiteral("same message")))
		return true;

	static const QStringList genericPhrases{
		QStringLiteral("em uma conversa de live"),
		QStringLiteral("em uma live"),
		QStringLiteral("conversa de live"),
		QStringLiteral("live conversation"),
		QStringLiteral("conversation in a live"),
		QStringLiteral("live stream conversation"),
		QStringLiteral("stream conversation"),
		QStringLiteral("viewer interaction"),
		QStringLiteral("viewer message"),
		QStringLiteral("viewer question"),
		QStringLiteral("viewer comment"),
		QStringLiteral("chat message"),
		QStringLiteral("live chat"),
		QStringLiteral("general chat"),
		QStringLiteral("stream chat"),
	};

	for (const QString &phrase : genericPhrases) {
		if (normalized == phrase || normalized.contains(phrase))
			return true;
	}

	return false;
}

bool isReliableFocusTarget(const QString &target)
{
	const QString normalized = normalizedText(target);
	if (normalized.size() < 6)
		return false;
	if (containsPortugueseTargetMarkers(target) || isGenericFocusTarget(target))
		return false;

	const QStringList terms = wordsFromTarget(target);
	return terms.size() >= 1;
}

bool hasExplicitReliableFocusTarget(const CurationSettings &settings)
{
	for (const QString &keyword : settings.topicKeywords) {
		if (isReliableFocusTarget(keyword))
			return true;
	}
	return false;
}

QStringList anchorTermsForTarget(const QString &target, const CurationSettings &settings)
{
	QStringList terms = wordsFromTarget(target);
	const QString normalizedTarget = normalizedText(target);

	if (normalizedTarget.contains(QStringLiteral("raise")) ||
	    normalizedTarget.contains(QStringLiteral("salary increase"))) {
		terms << QStringLiteral("raise") << QStringLiteral("salary") << QStringLiteral("increase")
		      << QStringLiteral("aumento") << QStringLiteral("chefe") << QStringLiteral("pedir")
		      << QStringLiteral("salario") << QStringLiteral("aumentar");
	}

	if (normalizedTarget.contains(QStringLiteral("gambl")) || normalizedTarget.contains(QStringLiteral("bet")) ||
	    normalizedTarget.contains(QStringLiteral("father"))) {
		terms << QStringLiteral("pai") << QStringLiteral("aposta") << QStringLiteral("apostas")
		      << QStringLiteral("perdi") << QStringLiteral("perdeu") << QStringLiteral("father")
		      << QStringLiteral("gambling") << QStringLiteral("betting");
	}

	if (normalizedTarget.contains(QStringLiteral("relationship")) || normalizedTarget.contains(QStringLiteral("ex"))) {
		terms << QStringLiteral("relacionamento") << QStringLiteral("pessoa") << QStringLiteral("ex")
		      << QStringLiteral("passado") << QStringLiteral("afastei") << QStringLiteral("relationship");
	}

	if (normalizedTarget.contains(QStringLiteral("sobriety")) || normalizedTarget.contains(QStringLiteral("consent"))) {
		terms << QStringLiteral("sobriety") << QStringLiteral("sober") << QStringLiteral("consent")
		      << QStringLiteral("alcool") << QStringLiteral("alcoolico") << QStringLiteral("sobrio")
		      << QStringLiteral("consentimento");
	}

	for (const QString &keyword : settings.topicKeywords) {
		if (containsPortugueseTargetMarkers(keyword) || isGenericFocusTarget(keyword))
			continue;
		const QStringList keywordWords = wordsFromTarget(keyword);
		for (const QString &word : keywordWords)
			terms << word;
	}

	QStringList normalizedTerms;
	for (const QString &term : terms) {
		const QString normalized = normalizedText(term);
		if (normalized.size() >= 3)
			normalizedTerms << normalized;
	}

	normalizedTerms.removeDuplicates();
	return normalizedTerms;
}

int targetTermHitCount(const QString &normalizedSegment, const QStringList &terms)
{
	int hits = 0;
	for (const QString &term : terms) {
		if (normalizedSegment.contains(term))
			++hits;
	}
	return hits;
}

double targetAnchorConfidence(int hits, const QStringList &terms)
{
	const long long expectedTermCount = std::min(4LL, std::max(2LL, static_cast<long long>(terms.size())));
	return std::clamp(static_cast<double>(hits) / static_cast<double>(expectedTermCount), 0.0, 1.0);
}

int earliestTermPosition(const QString &normalizedSegment, const QStringList &terms)
{
	int position = -1;
	for (const QString &term : terms) {
		const int index = normalizedSegment.indexOf(term);
		if (index < 0)
			continue;
		if (position < 0 || index < position)
			position = index;
	}
	return position;
}

struct AnchorStartEstimate {
	double startSec = 0.0;
	QString anchorText;
	bool adjusted = false;
};

QString trimmedAnchorTextFromPosition(const TranscriptSegment &segment, int normalizedPosition)
{
	const QString original = segment.text.trimmed();
	if (original.isEmpty() || normalizedPosition <= 0)
		return original;

	const QString normalized = normalizedText(original);
	if (normalized.isEmpty())
		return original;

	const double ratio = std::clamp(static_cast<double>(normalizedPosition) / static_cast<double>(normalized.size()), 0.0, 1.0);
	const int originalIndex = std::clamp(static_cast<int>(std::floor(ratio * static_cast<double>(original.size()))), 0,
					      std::max(0, static_cast<int>(original.size()) - 1));

	QString trimmed = original.mid(originalIndex).trimmed();
	trimmed.replace(QRegularExpression(QStringLiteral("^[\\s\\-:;,.!?\"]+")), QString());
	return trimmed.isEmpty() ? original : trimmed;
}

int firstPositionForPhrases(const QString &normalizedSegment, const QStringList &phrases)
{
	int position = -1;
	for (const QString &phrase : phrases) {
		const int index = normalizedSegment.indexOf(phrase);
		if (index < 0)
			continue;
		if (position < 0 || index < position)
			position = index;
	}
	return position;
}

int meaningfulStartPosition(const QString &normalizedSegment, const QStringList &terms)
{
	static const QStringList strongestMessageStarts{
		QStringLiteral("eu perdi meu pai"),
		QStringLiteral("perdi meu pai"),
		QStringLiteral("eu perdi minha mae"),
		QStringLiteral("perdi minha mae"),
		QStringLiteral("meu pai morreu"),
		QStringLiteral("minha mae morreu"),
		QStringLiteral("sinto muita falta"),
		QStringLiteral("eu tava passando"),
		QStringLiteral("tava passando"),
	};
	const int strongestPosition = firstPositionForPhrases(normalizedSegment, strongestMessageStarts);
	if (strongestPosition >= 0)
		return strongestPosition;

	int position = earliestTermPosition(normalizedSegment, terms);

	static const QStringList usefulMessageStarts{
		QStringLiteral("mano como"),
		QStringLiteral("como eu posso"),
		QStringLiteral("como posso"),
		QStringLiteral("eu nunca pedi"),
		QStringLiteral("eu gostava"),
		QStringLiteral("por que"),
		QStringLiteral("lembra"),
	};

	const int usefulPosition = firstPositionForPhrases(normalizedSegment, usefulMessageStarts);
	if (usefulPosition >= 0 && (position < 0 || usefulPosition < position))
		position = usefulPosition;

	return position;
}

AnchorStartEstimate estimatedAnchorStart(const TranscriptSegment &segment, const QStringList &terms)
{
	AnchorStartEstimate estimate;
	estimate.startSec = segment.startSec;
	estimate.anchorText = segment.text.trimmed();

	const QString normalized = normalizedText(segment.text);
	const int position = meaningfulStartPosition(normalized, terms);
	if (position <= 0 || normalized.isEmpty())
		return estimate;

	estimate.anchorText = trimmedAnchorTextFromPosition(segment, position);
	estimate.adjusted = true;

	const double duration = std::max(0.0, segment.endSec - segment.startSec);
	if (duration <= 0.0) {
		estimate.startSec = segment.startSec + PREAMBLE_ONLY_SEGMENT_OFFSET_SEC;
		return estimate;
	}

	const double ratio = std::clamp(static_cast<double>(position) / static_cast<double>(normalized.size()), 0.0, 1.0);
	estimate.startSec = segment.startSec + (duration * ratio);
	return estimate;
}

ClipDuration clampedFocusRange(const ClipDuration &manualRange, double anchorStartSec)
{
	const double manualStart = std::max(0.0, manualRange.startSec);
	const double manualEnd = std::max(manualStart, manualRange.endSec);
	const double startSec = std::clamp(anchorStartSec - TARGET_ANCHOR_PADDING_BEFORE_SEC, manualStart, manualEnd);
	const double preferredEnd = std::min(manualEnd, anchorStartSec + TARGET_ANCHOR_AFTER_SEC);
	const double minEnd = std::min(manualEnd, startSec + MIN_FOCUS_DURATION_SEC);
	const double endSec = std::min(manualEnd, std::max(preferredEnd, minEnd));

	if (endSec <= startSec)
		return manualRange;

	const double duration = endSec - startSec;
	if (duration > MAX_FOCUS_DURATION_SEC)
		return {startSec, std::min(manualEnd, startSec + MAX_FOCUS_DURATION_SEC)};

	return {startSec, endSec};
}

bool looksLikeQuestionOrViewerMessage(const QString &text)
{
	if (text.contains(QLatin1Char('?')))
		return true;

	const QString normalized = normalizedText(text);

	static const QStringList markers{QStringLiteral("mano"), QStringLiteral("cara"), QStringLiteral("perdi"),
					       QStringLiteral("eu "), QStringLiteral("como "), QStringLiteral("por que"),
					       QStringLiteral("lembra"), QStringLiteral("acho que"), QStringLiteral("meu "),
					       QStringLiteral("sinto "), QStringLiteral("tenho ")};
	for (const QString &marker : markers) {
		if (normalized.startsWith(marker) || normalized.contains(QStringLiteral(" ") + marker))
			return true;
	}

	return false;
}

QString textForRange(const RecordingTranscript &transcript, const ClipDuration &range)
{
	QStringList parts;
	for (const TranscriptSegment &segment : transcript.segments) {
		if (!segmentOverlapsRange(segment, range))
			continue;
		const QString text = segment.text.simplified();
		if (!text.isEmpty())
			parts << text;
	}
	return parts.join(QStringLiteral(" ")).simplified();
}

QString sampleForLog(QString text)
{
	text = text.simplified();
	if (text.size() <= 100)
		return text;
	return text.left(97).trimmed() + QStringLiteral("...");
}

struct CandidateRangeScore {
	ClipDuration range;
	double score = 0.0;
	double emotionalScore = 0.0;
	double adviceScore = 0.0;
	double questionScore = 0.0;
	QStringList emotionalCues;
	QString anchorText;
	QString sampleText;
	bool startAdjusted = false;
};

bool hasStrongLocalCue(const QString &text)
{
	QStringList cues;
	const double emotionalScore = Curation::emotionalScoreForText(text, &cues);
	const double adviceScore = Curation::adviceScoreForText(text);
	return emotionalScore >= 0.40 || adviceScore >= 0.25 || looksLikeQuestionOrViewerMessage(text);
}

QStringList genericAnchorTermsForSegment(const QString &text)
{
	const QString normalized = normalizedText(text);
	QStringList terms;

	if (normalized.contains(QStringLiteral("perdi")) || normalized.contains(QStringLiteral("pai")) ||
	    normalized.contains(QStringLiteral("aposta"))) {
		terms << QStringLiteral("perdi") << QStringLiteral("pai") << QStringLiteral("aposta")
		      << QStringLiteral("apostas");
	}
	if (normalized.contains(QStringLiteral("como")) || normalized.contains(QStringLiteral("aumento")) ||
	    normalized.contains(QStringLiteral("chefe"))) {
		terms << QStringLiteral("como") << QStringLiteral("aumento") << QStringLiteral("chefe")
		      << QStringLiteral("pedir");
	}
	if (normalized.contains(QStringLiteral("sinto")) || normalized.contains(QStringLiteral("falta"))) {
		terms << QStringLiteral("sinto") << QStringLiteral("falta") << QStringLiteral("amigo");
	}
	if (normalized.contains(QStringLiteral("ansiedade")) || normalized.contains(QStringLiteral("sentindo"))) {
		terms << QStringLiteral("ansiedade") << QStringLiteral("sentindo") << QStringLiteral("bem");
	}

	if (terms.isEmpty())
		terms = normalized.split(' ', Qt::SkipEmptyParts).mid(0, 4);

	terms.removeDuplicates();
	return terms;
}

double candidateAfterSeconds(double emotionalScore, double adviceScore)
{
	if (emotionalScore >= 0.55)
		return VIEWER_CANDIDATE_EMOTIONAL_AFTER_SEC;
	if (adviceScore >= 0.35)
		return VIEWER_CANDIDATE_ADVICE_AFTER_SEC;
	return VIEWER_CANDIDATE_DEFAULT_AFTER_SEC;
}

CandidateRangeScore scoreCandidateRange(const RecordingTranscript &transcript, const ClipDuration &range,
						 const AnchorStartEstimate &anchor)
{
	CandidateRangeScore score;
	score.range = range;
	score.anchorText = sampleForLog(anchor.anchorText);
	score.startAdjusted = anchor.adjusted;

	const QString candidateText = textForRange(transcript, range);
	score.sampleText = sampleForLog(candidateText);
	score.emotionalScore = Curation::emotionalScoreForText(candidateText, &score.emotionalCues);
	score.adviceScore = Curation::adviceScoreForText(candidateText);
	score.questionScore = looksLikeQuestionOrViewerMessage(anchor.anchorText) ? 0.25 : 0.0;

	const double compactnessBonus = std::max(0.0, 1.0 - ((range.endSec - range.startSec) / VIEWER_CANDIDATE_MAX_DURATION_SEC)) * 0.08;
	score.score = std::clamp((score.emotionalScore * 2.4) + (score.adviceScore * 0.8) + score.questionScore + compactnessBonus,
				       0.0, 3.6);
	return score;
}

bool rangesOverlapTooMuch(const ClipDuration &left, const ClipDuration &right)
{
	const double intersection = std::max(0.0, std::min(left.endSec, right.endSec) - std::max(left.startSec, right.startSec));
	if (intersection <= 0.0)
		return false;

	const double leftDuration = std::max(0.001, left.endSec - left.startSec);
	const double rightDuration = std::max(0.001, right.endSec - right.startSec);
	return intersection >= std::min(leftDuration, rightDuration) * 0.35 ||
	       std::fabs(left.startSec - right.startSec) < VIEWER_CANDIDATE_OVERLAP_TOLERANCE_SEC;
}

QString candidateSummary(const QVector<CandidateRangeScore> &candidates)
{
	QStringList parts;
	for (const CandidateRangeScore &candidate : candidates) {
		const QString cues = candidate.emotionalCues.isEmpty() ? QStringLiteral("-") : candidate.emotionalCues.join(QStringLiteral("|"));
		parts << QStringLiteral("%1-%2 score=%3 emotional=%4 advice=%5 question=%6 adjusted=%7 cues=%8 anchor=\"%9\"")
				 .arg(candidate.range.startSec, 0, 'f', 2)
				 .arg(candidate.range.endSec, 0, 'f', 2)
				 .arg(candidate.score, 0, 'f', 2)
				 .arg(candidate.emotionalScore, 0, 'f', 2)
				 .arg(candidate.adviceScore, 0, 'f', 2)
				 .arg(candidate.questionScore, 0, 'f', 2)
				 .arg(candidate.startAdjusted ? QStringLiteral("true") : QStringLiteral("false"))
				 .arg(cues, candidate.anchorText);
	}
	return parts.join(QStringLiteral("; "));
}

QVector<ClipDuration> genericCandidateRanges(const RecordingTranscript &transcript, const ClipDuration &manualRange,
					      QString *summary)
{
	QVector<CandidateRangeScore> allCandidates;

	for (const TranscriptSegment &segment : transcript.segments) {
		if (!segmentOverlapsRange(segment, manualRange) || segment.text.trimmed().isEmpty())
			continue;
		if (!hasStrongLocalCue(segment.text))
			continue;

		const QStringList terms = genericAnchorTermsForSegment(segment.text);
		const AnchorStartEstimate anchor = estimatedAnchorStart(segment, terms);
		const double segmentEmotional = Curation::emotionalScoreForText(segment.text);
		const double segmentAdvice = Curation::adviceScoreForText(segment.text);
		const double afterSec = candidateAfterSeconds(segmentEmotional, segmentAdvice);

		const double startSec = std::clamp(anchor.startSec - VIEWER_CANDIDATE_PADDING_BEFORE_SEC,
						       manualRange.startSec, manualRange.endSec);
		const double desiredEndSec = std::min(manualRange.endSec, anchor.startSec + afterSec);
		const double minEndSec = std::min(manualRange.endSec, startSec + VIEWER_CANDIDATE_MIN_DURATION_SEC);
		const double endSec = std::min(manualRange.endSec, std::max(desiredEndSec, minEndSec));
		if (endSec <= startSec)
			continue;

		ClipDuration range{startSec, endSec};
		if ((range.endSec - range.startSec) > VIEWER_CANDIDATE_MAX_DURATION_SEC)
			range.endSec = std::min(manualRange.endSec, range.startSec + VIEWER_CANDIDATE_MAX_DURATION_SEC);

		CandidateRangeScore scored = scoreCandidateRange(transcript, range, anchor);
		if (scored.score < VIEWER_CANDIDATE_MIN_SCORE)
			continue;

		allCandidates.append(scored);
	}

	std::sort(allCandidates.begin(), allCandidates.end(), [](const CandidateRangeScore &left, const CandidateRangeScore &right) {
		if (std::fabs(left.score - right.score) > 0.0001)
			return left.score > right.score;
		return left.range.startSec < right.range.startSec;
	});

	QVector<CandidateRangeScore> selected;
	selected.reserve(std::min(static_cast<long long>(MAX_LOGGED_CANDIDATES), static_cast<long long>(allCandidates.size())));
	for (const CandidateRangeScore &candidate : allCandidates) {
		bool overlapsSelected = false;
		for (const CandidateRangeScore &selectedCandidate : selected) {
			if (rangesOverlapTooMuch(candidate.range, selectedCandidate.range)) {
				overlapsSelected = true;
				break;
			}
		}
		if (overlapsSelected)
			continue;

		selected.append(candidate);
		if (selected.size() >= MAX_LOGGED_CANDIDATES)
			break;
	}

	if (summary)
		*summary = candidateSummary(selected);

	QVector<ClipDuration> ranges;
	ranges.reserve(selected.size());
	for (const CandidateRangeScore &candidate : selected)
		ranges.append(candidate.range);
	return ranges;
}

} // namespace

ViewerMessageFocusRangeResult ViewerMessageFocusRangeResolver::resolve(const RecordingTranscript &transcript,
									 const CurationSettings &settings,
									 const QString &opusPrompt) const
{
	ViewerMessageFocusRangeResult result;
	const QString resolvedPreset = CurationPreset::resolveId(settings, opusPrompt);
	if (resolvedPreset != QStringLiteral("viewer_message_response")) {
		result.reason = QStringLiteral("preset_is_not_viewer_message_response");
		return result;
	}

	const QVector<ClipDuration> ranges = validRanges(settings);
	if (ranges.size() != 1) {
		result.reason = QStringLiteral("requires_exactly_one_manual_range");
		return result;
	}

	result.manualRange = ranges.first();
	if (transcript.segments.isEmpty()) {
		result.reason = QStringLiteral("empty_transcript");
		return result;
	}

	result.target = extractTargetFromPrompt(opusPrompt);
	if (result.target.trimmed().isEmpty()) {
		result.candidateRanges = genericCandidateRanges(transcript, result.manualRange, &result.candidateSummary);
		result.reason = QStringLiteral("no_reliable_target");
		return result;
	}

	if (!hasExplicitReliableFocusTarget(settings)) {
		result.candidateRanges = genericCandidateRanges(transcript, result.manualRange, &result.candidateSummary);
		result.reason = QStringLiteral("target_not_explicitly_requested");
		return result;
	}

	if (!isReliableFocusTarget(result.target)) {
		result.candidateRanges = genericCandidateRanges(transcript, result.manualRange, &result.candidateSummary);
		result.reason = QStringLiteral("unreliable_target");
		return result;
	}

	const QStringList terms = anchorTermsForTarget(result.target, settings);
	if (terms.size() < 2) {
		result.reason = QStringLiteral("not_enough_anchor_terms");
		return result;
	}

	int bestHits = 0;
	TranscriptSegment bestSegment;
	bool found = false;

	for (const TranscriptSegment &segment : transcript.segments) {
		if (!segmentOverlapsRange(segment, result.manualRange) || segment.text.trimmed().isEmpty())
			continue;

		const QString normalizedSegment = normalizedText(segment.text);
		const int hits = targetTermHitCount(normalizedSegment, terms);
		if (hits < 2)
			continue;

		if (!found || hits > bestHits || segment.startSec < bestSegment.startSec) {
			found = true;
			bestHits = hits;
			bestSegment = segment;
		}
	}

	if (!found) {
		result.candidateRanges = genericCandidateRanges(transcript, result.manualRange, &result.candidateSummary);
		result.reason = QStringLiteral("target_anchor_not_found");
		return result;
	}

	result.confidence = targetAnchorConfidence(bestHits, terms);
	if (result.confidence < MIN_TARGET_ANCHOR_CONFIDENCE) {
		result.candidateRanges = genericCandidateRanges(transcript, result.manualRange, &result.candidateSummary);
		result.reason = QStringLiteral("target_anchor_confidence_too_low");
		return result;
	}

	const AnchorStartEstimate anchorStart = estimatedAnchorStart(bestSegment, terms);
	result.focusRange = clampedFocusRange(result.manualRange, anchorStart.startSec);
	const double focusDuration = std::max(0.0, result.focusRange.endSec - result.focusRange.startSec);
	const double manualDuration = std::max(0.0, result.manualRange.endSec - result.manualRange.startSec);
	if (focusDuration <= 0.0 || manualDuration <= 0.0 || focusDuration >= manualDuration * 0.95) {
		result.reason = QStringLiteral("focus_range_not_narrower_than_manual_range");
		return result;
	}

	result.applied = true;
	result.reason = QStringLiteral("target_anchor_found");
	result.anchorText = anchorStart.anchorText.trimmed();
	result.startAdjusted = anchorStart.adjusted;
	result.candidateRanges.append(result.focusRange);
	return result;
}

CurationSettings ViewerMessageFocusRangeResolver::applyFocusRange(const CurationSettings &settings,
								  const ViewerMessageFocusRangeResult &result) const
{
	if (!result.applied)
		return settings;

	CurationSettings adjusted = settings;
	adjusted.clipDurations.clear();
	adjusted.clipDurations.append(result.focusRange);
	adjusted.rangeStartSec = result.focusRange.startSec;
	adjusted.rangeEndSec = result.focusRange.endSec;
	return adjusted;
}
