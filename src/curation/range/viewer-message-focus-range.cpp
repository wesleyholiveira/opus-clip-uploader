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
static constexpr double VIEWER_CANDIDATE_OVERLAP_TOLERANCE_SEC = 8.0;
static constexpr double VIEWER_CANDIDATE_SAME_CUE_DEDUPE_WINDOW_SEC = 45.0;
static constexpr double VIEWER_CANDIDATE_NEXT_CUE_PADDING_SEC = 0.35;
static constexpr double VIEWER_CANDIDATE_NEXT_CUE_MIN_GAP_SEC = 10.0;
static constexpr double VIEWER_CANDIDATE_ANCHOR_CUE_MISMATCH_PENALTY = 0.45;
static constexpr double VIEWER_CANDIDATE_QUOTA_ABSOLUTE_MIN_DURATION_SEC = 9.0;
static constexpr double VIEWER_CANDIDATE_CONTEXT_PRELUDE_LOOKBACK_SEC = 36.0;
static constexpr double VIEWER_CANDIDATE_QUOTA_EMOTIONAL_MIN_SCORE = 0.20;
static constexpr double VIEWER_CANDIDATE_LARGE_DISCOVERY_SEC = 1800.0;
static constexpr double VIEWER_CANDIDATE_MEDIUM_DISCOVERY_SEC = 600.0;
static constexpr int VIEWER_CANDIDATE_SHORT_MAX_PROJECTS = 3;
static constexpr int VIEWER_CANDIDATE_MEDIUM_MAX_PROJECTS = 6;
static constexpr int VIEWER_CANDIDATE_LARGE_MAX_PROJECTS = 12;
static constexpr int VIEWER_CANDIDATE_HUGE_MAX_PROJECTS = 16;

struct CandidateDiscoveryProfile {
	QString name;
	int maxCandidates = VIEWER_CANDIDATE_SHORT_MAX_PROJECTS;
	double defaultAfterSec = 28.0;
	double emotionalAfterSec = 34.0;
	double adviceAfterSec = 30.0;
	double minDurationSec = 18.0;
	double boundaryMinDurationSec = 7.0;
	double maxDurationSec = 42.0;
	double minScore = 0.55;
	double poolMinScore = 0.50;
	double quotaMinScore = 0.50;
	double secondaryMinScore = 0.55;
	double secondaryRelativeToBest = 0.25;
	double temporalQuotaWindowSec = 0.0;
	int temporalQuotaMinCandidates = 0;
};

double rangeDurationSec(const ClipDuration &range)
{
	return std::max(0.0, range.endSec - range.startSec);
}

CandidateDiscoveryProfile discoveryProfileForRange(const ClipDuration &range)
{
	const double durationSec = rangeDurationSec(range);
	if (durationSec >= 7200.0) {
		return {QStringLiteral("huge_discovery"), VIEWER_CANDIDATE_HUGE_MAX_PROJECTS, 42.0, 48.0, 50.0, 22.0,
			9.0, 64.0, 0.42, 0.28, 0.30, 0.42, 0.12, 600.0, 1};
	}
	if (durationSec >= VIEWER_CANDIDATE_LARGE_DISCOVERY_SEC) {
		return {QStringLiteral("large_discovery"), VIEWER_CANDIDATE_LARGE_MAX_PROJECTS, 40.0, 46.0, 48.0, 22.0,
			9.0, 60.0, 0.42, 0.28, 0.30, 0.42, 0.14, 300.0, 1};
	}
	if (durationSec >= VIEWER_CANDIDATE_MEDIUM_DISCOVERY_SEC) {
		return {QStringLiteral("medium_discovery"), VIEWER_CANDIDATE_MEDIUM_MAX_PROJECTS, 34.0, 42.0, 42.0,
			20.0, 8.0, 52.0, 0.46, 0.30, 0.32, 0.46, 0.18, 300.0, 1};
	}

	return {QStringLiteral("precision"), VIEWER_CANDIDATE_SHORT_MAX_PROJECTS, 28.0, 34.0, 30.0, 18.0, 7.0,
		42.0, 0.55, 0.50, 0.50, 0.55, 0.25, 0.0, 0};
}

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

int priorityPositionForPhrases(const QString &normalizedSegment, const QStringList &phrases)
{
	for (const QString &phrase : phrases) {
		const int index = normalizedSegment.indexOf(phrase);
		if (index >= 0)
			return index;
	}
	return -1;
}

int meaningfulStartPosition(const QString &normalizedSegment, const QStringList &terms)
{
	static const QStringList strongestMessageStarts{
		QStringLiteral("so aposta"),
		QStringLiteral("so apostam"),
		QStringLiteral("aposta quem quer"),
		QStringLiteral("apostam a quem quer"),
		QStringLiteral("a pessoa aposta"),
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
	const int strongestPosition = priorityPositionForPhrases(normalizedSegment, strongestMessageStarts);
	if (strongestPosition >= 0)
		return strongestPosition;

	int position = earliestTermPosition(normalizedSegment, terms);

	static const QStringList usefulMessageStarts{
		QStringLiteral("nao sei se eu tenho"),
		QStringLiteral("nao sei se tenho"),
		QStringLiteral("nao sei se e"),
		QStringLiteral("depressao ou ansiedade"),
		QStringLiteral("depressao e ansiedade"),
		QStringLiteral("mano como"),
		QStringLiteral("como eu posso"),
		QStringLiteral("como posso"),
		QStringLiteral("eu nunca pedi"),
		QStringLiteral("eu gostava"),
		QStringLiteral("por que"),
		QStringLiteral("lembra"),
		QStringLiteral("nao sei"),
		QStringLiteral("na ansiedade"),
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
					       QStringLiteral("tenho ")};
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
	QString fullText;
	QString cueSignature;
	bool startAdjusted = false;
	bool anchorCueAligned = true;
	bool boundaryTrimmed = false;
};

bool hasStrongLocalCue(const QString &text)
{
	QStringList cues;
	const double emotionalScore = Curation::emotionalScoreForText(text, &cues);
	const double adviceScore = Curation::adviceScoreForText(text);
	return emotionalScore >= 0.40 || adviceScore >= 0.25 || looksLikeQuestionOrViewerMessage(text);
}

QStringList normalizedCueList(const QStringList &cues)
{
	QStringList normalized;
	for (const QString &cue : cues) {
		const QString value = normalizedText(cue);
		if (!value.isEmpty())
			normalized << value;
	}
	normalized.removeDuplicates();
	normalized.sort();
	return normalized;
}

bool textContainsCue(const QString &text, const QStringList &cues)
{
	const QString normalized = normalizedText(text);
	for (const QString &cue : normalizedCueList(cues)) {
		if (normalized.contains(cue))
			return true;
	}
	return false;
}

QString cueSignatureForCues(const QStringList &cues)
{
	return normalizedCueList(cues).join(QLatin1Char('|'));
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
	if (normalized.contains(QStringLiteral("sinto muita falta")) || normalized.contains(QStringLiteral("sinto falta")) ||
	    normalized.contains(QStringLiteral("melhor amigo"))) {
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

double candidateAfterSeconds(const CandidateDiscoveryProfile &profile, double emotionalScore, double adviceScore)
{
	if (emotionalScore >= 0.55)
		return profile.emotionalAfterSec;
	if (adviceScore >= 0.35)
		return profile.adviceAfterSec;
	return profile.defaultAfterSec;
}

CandidateRangeScore scoreCandidateRange(const RecordingTranscript &transcript, const ClipDuration &range,
						 const AnchorStartEstimate &anchor, const CandidateDiscoveryProfile &profile)
{
	CandidateRangeScore score;
	score.range = range;
	score.anchorText = sampleForLog(anchor.anchorText);
	score.startAdjusted = anchor.adjusted;

	const QString candidateText = textForRange(transcript, range);
	score.fullText = candidateText;
	score.sampleText = sampleForLog(candidateText);
	score.emotionalScore = Curation::emotionalScoreForText(candidateText, &score.emotionalCues);
	score.adviceScore = Curation::adviceScoreForText(candidateText);
	score.questionScore = looksLikeQuestionOrViewerMessage(anchor.anchorText) ? 0.25 : 0.0;
	score.cueSignature = cueSignatureForCues(score.emotionalCues);
	score.anchorCueAligned = score.emotionalCues.isEmpty() || textContainsCue(anchor.anchorText, score.emotionalCues);

	const double compactnessBonus = std::max(0.0, 1.0 - ((range.endSec - range.startSec) / profile.maxDurationSec)) * 0.08;
	const double mismatchPenalty = score.anchorCueAligned ? 0.0 : VIEWER_CANDIDATE_ANCHOR_CUE_MISMATCH_PENALTY;
	score.score = std::clamp((score.emotionalScore * 2.4) + (score.adviceScore * 0.8) + score.questionScore + compactnessBonus - mismatchPenalty,
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

bool hasStrongViewerIssueSignal(const CandidateRangeScore &candidate)
{
	if (candidate.adviceScore >= 0.25)
		return true;
	if (candidate.emotionalScore >= 0.40)
		return true;
	if (candidate.score >= 0.95)
		return true;
	return false;
}

bool shouldKeepSecondaryCandidate(const CandidateRangeScore &candidate, const CandidateRangeScore &bestCandidate,
					  const CandidateDiscoveryProfile &profile)
{
	if (!candidate.anchorCueAligned)
		return false;

	const double relativeMinimum = bestCandidate.score >= 1.80
		? bestCandidate.score * profile.secondaryRelativeToBest
		: profile.secondaryMinScore;
	const double minimumScore = std::max(profile.secondaryMinScore, relativeMinimum);

	if (candidate.score < minimumScore && candidate.adviceScore < 0.25)
		return false;

	return hasStrongViewerIssueSignal(candidate);
}

QString candidateSummary(const QVector<CandidateRangeScore> &candidates)
{
	QStringList parts;
	for (const CandidateRangeScore &candidate : candidates) {
		const QString cues = candidate.emotionalCues.isEmpty() ? QStringLiteral("-") : candidate.emotionalCues.join(QStringLiteral("|"));
		parts << QStringLiteral("%1-%2 score=%3 emotional=%4 advice=%5 question=%6 adjusted=%7 cueAligned=%8 boundaryTrimmed=%9 cues=%10 anchor=\"%11\"")
					 .arg(candidate.range.startSec, 0, 'f', 2)
					 .arg(candidate.range.endSec, 0, 'f', 2)
					 .arg(candidate.score, 0, 'f', 2)
					 .arg(candidate.emotionalScore, 0, 'f', 2)
					 .arg(candidate.adviceScore, 0, 'f', 2)
					 .arg(candidate.questionScore, 0, 'f', 2)
					 .arg(candidate.startAdjusted ? QStringLiteral("true") : QStringLiteral("false"))
					 .arg(candidate.anchorCueAligned ? QStringLiteral("true") : QStringLiteral("false"))
					 .arg(candidate.boundaryTrimmed ? QStringLiteral("true") : QStringLiteral("false"))
					 .arg(cues, candidate.anchorText);
	}
	return parts.join(QStringLiteral("; "));
}


struct CandidateAnchorChoice {
	AnchorStartEstimate anchor;
	double emotionalScore = 0.0;
	double adviceScore = 0.0;
	bool realigned = false;
};

CandidateAnchorChoice bestAnchorInsideRange(const RecordingTranscript &transcript, const ClipDuration &range,
						     const AnchorStartEstimate &fallbackAnchor)
{
	CandidateAnchorChoice best;
	best.anchor = fallbackAnchor;
	best.emotionalScore = Curation::emotionalScoreForText(fallbackAnchor.anchorText);
	best.adviceScore = Curation::adviceScoreForText(fallbackAnchor.anchorText);

	double bestScore = (best.emotionalScore * 2.6) + (best.adviceScore * 0.8) +
			   (looksLikeQuestionOrViewerMessage(fallbackAnchor.anchorText) ? 0.15 : 0.0);

	for (const TranscriptSegment &segment : transcript.segments) {
		if (!segmentOverlapsRange(segment, range) || segment.text.trimmed().isEmpty())
			continue;

		QStringList cues;
		const double emotionalScore = Curation::emotionalScoreForText(segment.text, &cues);
		const double adviceScore = Curation::adviceScoreForText(segment.text);
		const double questionScore = looksLikeQuestionOrViewerMessage(segment.text) ? 0.15 : 0.0;
		const double localScore = (emotionalScore * 2.6) + (adviceScore * 0.8) + questionScore;

		if (localScore <= bestScore + 0.0001)
			continue;

		const QStringList terms = genericAnchorTermsForSegment(segment.text);
		best.anchor = estimatedAnchorStart(segment, terms);
		best.emotionalScore = emotionalScore;
		best.adviceScore = adviceScore;
		best.realigned = std::fabs(best.anchor.startSec - fallbackAnchor.startSec) > 0.5;
		bestScore = localScore;
	}

	return best;
}

ClipDuration buildCandidateRange(const ClipDuration &manualRange, const AnchorStartEstimate &anchor,
					  double emotionalScore, double adviceScore, const CandidateDiscoveryProfile &profile)
{
	const double afterSec = candidateAfterSeconds(profile, emotionalScore, adviceScore);
	const double beforePaddingSec = anchor.adjusted ? 0.08 : VIEWER_CANDIDATE_PADDING_BEFORE_SEC;
	const double startSec = std::clamp(anchor.startSec - beforePaddingSec, manualRange.startSec, manualRange.endSec);
	const double desiredEndSec = std::min(manualRange.endSec, anchor.startSec + afterSec);
	const double minEndSec = std::min(manualRange.endSec, startSec + profile.minDurationSec);
	double endSec = std::min(manualRange.endSec, std::max(desiredEndSec, minEndSec));

	if ((endSec - startSec) > profile.maxDurationSec)
		endSec = std::min(manualRange.endSec, startSec + profile.maxDurationSec);

	return {startSec, endSec};
}

bool looksLikeSameExchangeContinuation(const QString &text)
{
	const QString normalized = normalizedText(text);
	if (normalized.isEmpty())
		return false;

	static const QStringList continuationStarts{
		QStringLiteral("mas "),
		QStringLiteral("porque "),
		QStringLiteral("imagina"),
		QStringLiteral("uma coisa"),
		QStringLiteral("outra coisa"),
		QStringLiteral("por exemplo"),
		QStringLiteral("entao assim"),
		QStringLiteral("e se"),
		QStringLiteral("so que"),
		QStringLiteral("olha so"),
		QStringLiteral("tipo assim"),
	};

	for (const QString &marker : continuationStarts) {
		if (normalized.startsWith(marker))
			return true;
	}

	static const QStringList continuationContains{
		QStringLiteral("ja parou pra pensar"),
		QStringLiteral("voce ja parou pra pensar"),
		QStringLiteral("ce ja parou pra pensar"),
		QStringLiteral("ja parou para pensar"),
		QStringLiteral("parou pra pensar"),
		QStringLiteral("parou para pensar"),
		QStringLiteral("voce ja parou"),
		QStringLiteral("ce ja parou"),
		QStringLiteral("na minha opiniao"),
		QStringLiteral("moralmente incorreto"),
		QStringLiteral("legalmente correto"),
		QStringLiteral("felipe"),
		QStringLiteral("entendendo"),
		QStringLiteral("entendeu"),
	};

	for (const QString &marker : continuationContains) {
		if (normalized.contains(marker))
			return true;
	}

	return false;
}

bool looksLikeMentalHealthContext(const QString &text)
{
	const QString normalized = normalizedText(text);
	if (normalized.isEmpty())
		return false;

	static const QStringList strongMarkers{
		QStringLiteral("depress"),
		QStringLiteral("ansiedade"),
		QStringLiteral("ansioso"),
		QStringLiteral("saude mental"),
		QStringLiteral("psicolog"),
		QStringLiteral("terapia"),
		QStringLiteral("crise"),
		QStringLiteral("fobia social"),
		QStringLiteral("tdah"),
		QStringLiteral("transtorno"),
		QStringLiteral("diagnostic"),
	};

	for (const QString &marker : strongMarkers) {
		if (normalized.contains(marker))
			return true;
	}

	static const QStringList uncertainFirstPersonMarkers{
		QStringLiteral("nao sei se"),
		QStringLiteral("eu nao sei se"),
		QStringLiteral("nao sei se eu"),
		QStringLiteral("acho que tenho"),
		QStringLiteral("acho que eu tenho"),
		QStringLiteral("sera que eu tenho"),
		QStringLiteral("eu acho que"),
		QStringLiteral("eu tenho medo"),
		QStringLiteral("eu sinto que"),
		QStringLiteral("sinto que"),
		QStringLiteral("me sinto"),
		QStringLiteral("eu to com"),
		QStringLiteral("to com"),
		QStringLiteral("estou com"),
	};

	for (const QString &marker : uncertainFirstPersonMarkers) {
		if (normalized.contains(marker))
			return true;
	}

	return false;
}

bool looksLikeGamblingContext(const QString &text)
{
	const QString normalized = normalizedText(text);
	if (normalized.isEmpty())
		return false;

	static const QStringList markers{
		QStringLiteral("aposta"),
		QStringLiteral("apostam"),
		QStringLiteral("apostar"),
		QStringLiteral("vicio"),
		QStringLiteral("cassino"),
		QStringLiteral("jogo de azar"),
		QStringLiteral("bet"),
		QStringLiteral("publicidade"),
		QStringLiteral("influencia"),
		QStringLiteral("influenciador"),
		QStringLiteral("receber um valor"),
		QStringLiteral("moralmente"),
		QStringLiteral("legalmente"),
		QStringLiteral("felipe"),
	};

	for (const QString &marker : markers) {
		if (normalized.contains(marker))
			return true;
	}

	return false;
}

bool normalizedContainsAny(const QString &normalized, const QStringList &markers)
{
	for (const QString &marker : markers) {
		if (normalized.contains(marker))
			return true;
	}

	return false;
}

QSet<QString> semanticCategoriesForText(const QString &text)
{
	const QString normalized = normalizedText(text);
	QSet<QString> categories;
	if (normalized.isEmpty())
		return categories;

	if (looksLikeMentalHealthContext(normalized))
		categories.insert(QStringLiteral("mental_health"));
	if (looksLikeGamblingContext(normalized))
		categories.insert(QStringLiteral("gambling"));

	if (normalizedContainsAny(normalized, {QStringLiteral("esquecer"), QStringLiteral("garota"),
						     QStringLiteral("relacionamento"), QStringLiteral("namor"),
						     QStringLiteral("gostei"), QStringLiteral("termin"),
						     QStringLiteral("ficante")}))
		categories.insert(QStringLiteral("relationship"));

	if (normalizedContainsAny(normalized, {QStringLiteral("aumento"), QStringLiteral("chefe"),
						     QStringLiteral("empresa"), QStringLiteral("trabalho"),
						     QStringLiteral("salario"), QStringLiteral("merecedor")}))
		categories.insert(QStringLiteral("career"));

	if (normalizedContainsAny(normalized, {QStringLiteral("usado"), QStringLiteral("me sinto"),
						     QStringLiteral("sinto que"), QStringLiteral("valor"),
						     QStringLiteral("amizade"), QStringLiteral("amigo")}))
		categories.insert(QStringLiteral("self_worth"));

	if (normalizedContainsAny(normalized, {QStringLiteral("perdi meu pai"), QStringLiteral("pai pra aposta"),
						     QStringLiteral("minha mae"), QStringLiteral("padrasto"),
						     QStringLiteral("familia")}))
		categories.insert(QStringLiteral("family"));

	if (normalizedContainsAny(normalized, {QStringLiteral("convite"), QStringLiteral("lobby"),
						     QStringLiteral("alice"), QStringLiteral("aceita esse"),
						     QStringLiteral("cancela"), QStringLiteral("entrar na sala")}))
		categories.insert(QStringLiteral("stream_operation"));

	if (normalizedContainsAny(normalized, {QStringLiteral("boa noite"), QStringLiteral("bom dia"),
						     QStringLiteral("boa tarde"), QStringLiteral("seja bem vindo"),
						     QStringLiteral("bem vindo"), QStringLiteral("salve"),
						     QStringLiteral("e ai "), QStringLiteral("oi ")}))
		categories.insert(QStringLiteral("greeting"));

	if (normalizedContainsAny(normalized, {QStringLiteral("block blast"), QStringLiteral("sand bricks"),
						     QStringLiteral("jogo parecido"), QStringLiteral("partida"),
						     QStringLiteral("personagem"), QStringLiteral("tela de jogo")}))
		categories.insert(QStringLiteral("gameplay"));

	return categories;
}

bool isNoiseCategory(const QString &category)
{
	return category == QStringLiteral("stream_operation") || category == QStringLiteral("greeting") ||
	       category == QStringLiteral("gameplay");
}

QSet<QString> contentCategoriesForText(const QString &text)
{
	QSet<QString> categories = semanticCategoriesForText(text);
	for (auto it = categories.begin(); it != categories.end();) {
		if (isNoiseCategory(*it))
			it = categories.erase(it);
		else
			++it;
	}

	return categories;
}

QSet<QString> contentTokensForText(const QString &text)
{
	static const QSet<QString> stopWords{
		QStringLiteral("acho"), QStringLiteral("assim"), QStringLiteral("aqui"), QStringLiteral("aquela"),
		QStringLiteral("aquele"), QStringLiteral("cara"), QStringLiteral("mano"), QStringLiteral("tipo"),
		QStringLiteral("voce"), QStringLiteral("voces"), QStringLiteral("porque"), QStringLiteral("entao"),
		QStringLiteral("tambem"), QStringLiteral("como"), QStringLiteral("quando"), QStringLiteral("onde"),
		QStringLiteral("isso"), QStringLiteral("esse"), QStringLiteral("essa"), QStringLiteral("esses"),
		QStringLiteral("essas"), QStringLiteral("para"), QStringLiteral("pela"), QStringLiteral("pelo"),
		QStringLiteral("tava"), QStringLiteral("estava"), QStringLiteral("agora"), QStringLiteral("muito"),
		QStringLiteral("mesmo"), QStringLiteral("meio"), QStringLiteral("ne"), QStringLiteral("ta"),
	};

	QSet<QString> tokens;
	for (const QString &token : normalizedText(text).split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
		if (token.size() < 4 || stopWords.contains(token))
			continue;
		tokens.insert(token);
	}

	return tokens;
}

double lexicalTopicOverlap(const QString &text, const QString &contextText)
{
	const QSet<QString> left = contentTokensForText(text);
	const QSet<QString> right = contentTokensForText(contextText);
	if (left.isEmpty() || right.isEmpty())
		return 0.0;

	int intersection = 0;
	for (const QString &token : left) {
		if (right.contains(token))
			++intersection;
	}

	const int denominator = std::max(1, std::min(static_cast<int>(left.size()), static_cast<int>(right.size())));
	return static_cast<double>(intersection) / static_cast<double>(denominator);
}

bool hasSharedSemanticTopic(const QString &text, const QString &contextText)
{
	const QSet<QString> textCategories = contentCategoriesForText(text);
	const QSet<QString> contextCategories = contentCategoriesForText(contextText);
	for (const QString &category : textCategories) {
		if (contextCategories.contains(category))
			return true;
	}

	return lexicalTopicOverlap(text, contextText) >= 0.25;
}

bool hasNoiseOnlySemanticTopic(const QString &text)
{
	const QSet<QString> categories = semanticCategoriesForText(text);
	if (categories.isEmpty())
		return false;

	for (const QString &category : categories) {
		if (!isNoiseCategory(category))
			return false;
	}

	return true;
}

bool looksLikeHardTopicShift(const QString &text, const QString &contextText)
{
	const QString normalized = normalizedText(text);
	if (normalized.isEmpty())
		return false;

	if (hasNoiseOnlySemanticTopic(text) && !hasSharedSemanticTopic(text, contextText))
		return true;

	const QSet<QString> textCategories = contentCategoriesForText(text);
	const QSet<QString> contextCategories = contentCategoriesForText(contextText);
	if (!textCategories.isEmpty() && !contextCategories.isEmpty() && !hasSharedSemanticTopic(text, contextText))
		return true;

	if (looksLikeQuestionOrViewerMessage(text) && !hasSharedSemanticTopic(text, contextText) &&
	    !looksLikeSameExchangeContinuation(text))
		return true;

	return false;
}

bool looksLikeSameTopicContinuation(const QString &text, const QString &contextText)
{
	const QString normalized = normalizedText(text);
	const QString context = normalizedText(contextText);
	if (normalized.isEmpty() || context.isEmpty())
		return false;

	if (hasSharedSemanticTopic(normalized, context))
		return true;
	if (looksLikeGamblingContext(context) && looksLikeGamblingContext(normalized))
		return true;
	if (looksLikeMentalHealthContext(context) && looksLikeMentalHealthContext(normalized))
		return true;

	return false;
}

bool anchorLooksLikeMentalHealth(const QString &anchorText)
{
	const QString anchor = normalizedText(anchorText);
	return anchor.contains(QStringLiteral("ansiedade")) || anchor.contains(QStringLiteral("depress")) ||
	       anchor.contains(QStringLiteral("tdah")) || anchor.contains(QStringLiteral("fobia social"));
}

bool looksLikeViewerContextPrelude(const QString &previousText, const QString &anchorText)
{
	const QString previous = normalizedText(previousText);
	const QString anchor = normalizedText(anchorText);
	if (previous.isEmpty() || anchor.isEmpty())
		return false;

	if (previous.contains(QStringLiteral("seja bem vindo")) || previous.contains(QStringLiteral("bem vindo")) ||
	    previous.contains(QStringLiteral("salve")))
		return false;

	if (anchorLooksLikeMentalHealth(anchorText) && looksLikeMentalHealthContext(previousText))
		return true;

	const bool anchorIsRelationship = anchor.contains(QStringLiteral("esquecer")) || anchor.contains(QStringLiteral("garota")) ||
					  anchor.contains(QStringLiteral("relacionamento"));
	if (anchorIsRelationship && (previous.contains(QStringLiteral("gostei")) || previous.contains(QStringLiteral("relacionamento")) ||
				       previous.contains(QStringLiteral("garota"))))
		return true;

	return false;
}

ClipDuration expandCandidateStartForViewerContext(const RecordingTranscript &transcript, const ClipDuration &range,
							  const AnchorStartEstimate &anchor, const ClipDuration &manualRange)
{
	double startSec = range.startSec;
	const bool mentalHealthAnchor = anchorLooksLikeMentalHealth(anchor.anchorText);
	const double lookbackSec = mentalHealthAnchor ? VIEWER_CANDIDATE_CONTEXT_PRELUDE_LOOKBACK_SEC : 18.0;

	// Whisper can split the viewer setup and the detected anchor across adjacent
	// segments, or keep both inside one segment. Search the whole local prelude
	// window and choose the earliest segment that still looks like the same
	// viewer issue instead of returning on the first matching segment.
	for (const TranscriptSegment &segment : transcript.segments) {
		if (segment.endSec < range.startSec - lookbackSec)
			continue;
		if (segment.startSec > range.startSec + 0.75)
			break;
		if (segment.endSec < range.startSec - lookbackSec)
			continue;
		if (!looksLikeViewerContextPrelude(segment.text, anchor.anchorText))
			continue;

		startSec = std::min(startSec, std::max(manualRange.startSec, segment.startSec - 0.10));
	}

	return {startSec, range.endSec};
}

ClipDuration trimCandidateBeforeNextCue(const RecordingTranscript &transcript, const ClipDuration &range, double anchorStartSec,
						 const CandidateDiscoveryProfile &profile)
{
	QString rangeContext = textForRange(transcript, {range.startSec, std::min(range.endSec, anchorStartSec + 6.0)});
	if (rangeContext.trimmed().isEmpty())
		rangeContext = textForRange(transcript, range);

	for (const TranscriptSegment &segment : transcript.segments) {
		if (!segmentOverlapsRange(segment, range) || segment.text.trimmed().isEmpty())
			continue;
		if (segment.startSec <= anchorStartSec + 1.75)
			continue;

		const bool hardTopicShift = looksLikeHardTopicShift(segment.text, rangeContext);
		if (!hardTopicShift && segment.startSec <= anchorStartSec + VIEWER_CANDIDATE_NEXT_CUE_MIN_GAP_SEC)
			continue;

		const bool newStrongCue = hasStrongLocalCue(segment.text);
		if (!hardTopicShift && !newStrongCue)
			continue;
		if (!hardTopicShift &&
		    (looksLikeSameExchangeContinuation(segment.text) || looksLikeSameTopicContinuation(segment.text, rangeContext))) {
			rangeContext.append(QLatin1Char(' '));
			rangeContext.append(segment.text);
			continue;
		}

		const double trimmedEndSec = segment.startSec - VIEWER_CANDIDATE_NEXT_CUE_PADDING_SEC;
		if (trimmedEndSec > range.startSec + profile.boundaryMinDurationSec)
			return {range.startSec, std::min(range.endSec, trimmedEndSec)};
	}

	return range;
}

bool looksLikeExplanatoryGamblingOpinion(const QString &text)
{
	const QString normalized = normalizedText(text);
	if (!looksLikeGamblingContext(normalized))
		return false;
	if (normalized.contains(QStringLiteral("perdi meu pai")) || normalized.contains(QStringLiteral("pai pra aposta")))
		return false;

	static const QStringList opinionMarkers{
		QStringLiteral("na minha opiniao"),
		QStringLiteral("moralmente"),
		QStringLiteral("legalmente"),
		QStringLiteral("vicio"),
		QStringLiteral("felipe"),
		QStringLiteral("publicidade"),
		QStringLiteral("influencia"),
	};

	for (const QString &marker : opinionMarkers) {
		if (normalized.contains(marker))
			return true;
	}

	return false;
}

ClipDuration extendCandidateForOpenExplanation(const RecordingTranscript &transcript, const ClipDuration &range,
					       const ClipDuration &manualRange, const CandidateDiscoveryProfile &profile)
{
	QString extensionContext = textForRange(transcript, range);
	const QString candidateText = normalizedText(extensionContext);
	const bool explanatoryGamblingOpinion = looksLikeExplanatoryGamblingOpinion(candidateText);
	const bool shouldExtend = explanatoryGamblingOpinion || looksLikeSameExchangeContinuation(candidateText) ||
				  looksLikeGamblingContext(candidateText) || looksLikeMentalHealthContext(candidateText);
	if (!shouldExtend)
		return range;

	double endSec = range.endSec;
	const double maxEndSec = std::min(manualRange.endSec, range.startSec + profile.maxDurationSec);
	const double minimumExplanatoryEndSec = explanatoryGamblingOpinion ? std::min(maxEndSec, range.startSec + 24.0) : range.endSec;
	for (const TranscriptSegment &segment : transcript.segments) {
		if (segment.endSec <= endSec + 0.05)
			continue;
		if (segment.startSec > maxEndSec)
			break;
		if (segment.endSec <= range.startSec + 0.05)
			continue;

		const bool overlapsCurrentEnd = segment.startSec < endSec + 1.25;
		const bool closeContinuationGap = explanatoryGamblingOpinion && segment.startSec < endSec + 3.0 &&
						  endSec < minimumExplanatoryEndSec;
		const bool hardTopicShift = looksLikeHardTopicShift(segment.text, extensionContext);
		const bool isContinuation = !hardTopicShift &&
			(looksLikeSameExchangeContinuation(segment.text) ||
			 looksLikeSameTopicContinuation(segment.text, extensionContext) ||
			 (explanatoryGamblingOpinion && looksLikeGamblingContext(segment.text)));
		if (hardTopicShift)
			break;
		if (!overlapsCurrentEnd && !isContinuation && !closeContinuationGap)
			break;
		if (hasStrongLocalCue(segment.text) && !isContinuation && !closeContinuationGap)
			break;

		endSec = std::min(maxEndSec, std::max(endSec, segment.endSec));
		extensionContext.append(QLatin1Char(' '));
		extensionContext.append(segment.text);

		const QString normalizedSegment = normalizedText(segment.text);
		const bool reachedMinimumExplanation = !explanatoryGamblingOpinion || endSec >= minimumExplanatoryEndSec - 0.01;
		if (reachedMinimumExplanation &&
		    (normalizedSegment.contains(QStringLiteral("entendeu")) || normalizedSegment.contains(QStringLiteral("sacou")) ||
		     normalizedSegment.contains(QStringLiteral("por isso"))))
			break;
	}

	return {range.startSec, std::max(range.endSec, endSec)};
}

bool sameSemanticCue(const CandidateRangeScore &left, const CandidateRangeScore &right)
{
	const QStringList leftCues = normalizedCueList(left.emotionalCues);
	const QStringList rightCues = normalizedCueList(right.emotionalCues);
	if (leftCues.isEmpty() || rightCues.isEmpty())
		return false;

	for (const QString &cue : leftCues) {
		if (rightCues.contains(cue))
			return true;
	}
	return false;
}

bool temporalQuotaEnabled(const CandidateDiscoveryProfile &profile)
{
	return profile.temporalQuotaWindowSec > 0.0 && profile.temporalQuotaMinCandidates > 0 &&
	       profile.maxCandidates > VIEWER_CANDIDATE_SHORT_MAX_PROJECTS;
}

int temporalBucketIndex(const ClipDuration &manualRange, const CandidateDiscoveryProfile &profile, double startSec)
{
	if (profile.temporalQuotaWindowSec <= 0.0)
		return 0;

	const double offsetSec = std::max(0.0, startSec - manualRange.startSec);
	return std::max(0, static_cast<int>(std::floor(offsetSec / profile.temporalQuotaWindowSec)));
}

bool hasBacklogOrGreetingAnchor(const CandidateRangeScore &candidate)
{
	const QString normalizedAnchor = normalizedText(candidate.anchorText);
	const QString normalizedSample = normalizedText(candidate.fullText.isEmpty() ? candidate.sampleText : candidate.fullText);
	const QString combined = QStringLiteral("%1 %2").arg(normalizedAnchor, normalizedSample).trimmed();

	if (combined.isEmpty())
		return false;

	static const QStringList backlogMarkers{
		QStringLiteral("nao sei como e que ele ta"),
		QStringLiteral("nao sei como ele ta"),
		QStringLiteral("j2 tambem"),
		QStringLiteral("o j2"),
		QStringLiteral("ta indo ai"),
		QStringLiteral("nao sei nem se ele"),
		QStringLiteral("seja bem vindo"),
		QStringLiteral("bem vindo"),
		QStringLiteral("pessoa sem nick"),
		QStringLiteral("sem nick"),
		QStringLiteral("estamos entendidos"),
		QStringLiteral("e tambem seguir"),
		QStringLiteral("tambem seguir"),
		QStringLiteral("ok estamos"),
		QStringLiteral("salve"),
		QStringLiteral("boa noite"),
		QStringLiteral("bom dia"),
		QStringLiteral("boa tarde"),
		QStringLiteral("convite errado"),
		QStringLiteral("mandei o convite"),
		QStringLiteral("nao aceita esse"),
		QStringLiteral("nao aceita esse nao"),
		QStringLiteral("cancela cancela"),
		QStringLiteral("alice"),
		QStringLiteral("block blast"),
		QStringLiteral("sand bricks"),
		QStringLiteral("jogo parecido"),
	};

	for (const QString &marker : backlogMarkers) {
		if (combined.contains(marker))
			return true;
	}

	return normalizedAnchor.startsWith(QStringLiteral("e ai ")) || normalizedAnchor == QStringLiteral("e ai") ||
	       normalizedAnchor.startsWith(QStringLiteral("ok "));
}

bool hasConcreteViewerQuestion(const CandidateRangeScore &candidate)
{
	const QString normalizedAnchor = normalizedText(candidate.anchorText);
	const QString normalizedSample = normalizedText(candidate.fullText.isEmpty() ? candidate.sampleText : candidate.fullText);
	const QString combined = QStringLiteral("%1 %2").arg(normalizedAnchor, normalizedSample).trimmed();

	static const QStringList concreteQuestionMarkers{
		QStringLiteral("como posso"),
		QStringLiteral("como eu posso"),
		QStringLiteral("o que eu faco"),
		QStringLiteral("o que faco"),
		QStringLiteral("por que"),
		QStringLiteral("ansiedade"),
		QStringLiteral("depress"),
		QStringLiteral("aumento"),
		QStringLiteral("chefe"),
		QStringLiteral("esquecer"),
		QStringLiteral("relacionamento"),
		QStringLiteral("conselho"),
		QStringLiteral("me sinto"),
		QStringLiteral("tenho"),
		QStringLiteral("perdi"),
		QStringLiteral("lembra"),
		QStringLiteral("nao sei se"),
		QStringLiteral("nao sei o que"),
		QStringLiteral("nao sei como fazer"),
		QStringLiteral("na ansiedade"),
	};

	for (const QString &marker : concreteQuestionMarkers) {
		if (combined.contains(marker))
			return true;
	}

	return false;
}

bool hasUsableDiscoverySignal(const CandidateRangeScore &candidate)
{
	if (candidate.adviceScore >= 0.25 && (candidate.questionScore > 0.0 || hasConcreteViewerQuestion(candidate)))
		return true;
	if (candidate.emotionalScore >= VIEWER_CANDIDATE_QUOTA_EMOTIONAL_MIN_SCORE)
		return true;
	if (candidate.emotionalScore >= 0.08 && candidate.questionScore > 0.0 && hasConcreteViewerQuestion(candidate))
		return true;
	if (candidate.questionScore > 0.0 && hasConcreteViewerQuestion(candidate))
		return true;
	return false;
}

bool shouldKeepTemporalQuotaCandidate(const CandidateRangeScore &candidate, const CandidateDiscoveryProfile &profile)
{
	if (!candidate.anchorCueAligned)
		return false;
	if (candidate.score < profile.quotaMinScore)
		return false;

	const double durationSec = rangeDurationSec(candidate.range);
	const double minDurationSec = std::max(VIEWER_CANDIDATE_QUOTA_ABSOLUTE_MIN_DURATION_SEC, profile.boundaryMinDurationSec);
	if (durationSec < minDurationSec)
		return false;

	if (hasBacklogOrGreetingAnchor(candidate))
		return false;

	const bool weakQuestionOnly = candidate.questionScore > 0.0 && candidate.emotionalScore < 0.10 &&
				      candidate.adviceScore < 0.25;
	if (weakQuestionOnly && !hasConcreteViewerQuestion(candidate))
		return false;

	return hasUsableDiscoverySignal(candidate);
}

bool candidateAlreadyCovered(const CandidateRangeScore &candidate, const QVector<CandidateRangeScore> &selected)
{
	for (const CandidateRangeScore &selectedCandidate : selected) {
		const bool duplicateCue = sameSemanticCue(candidate, selectedCandidate) &&
			(std::fabs(candidate.range.startSec - selectedCandidate.range.startSec) <
				 VIEWER_CANDIDATE_SAME_CUE_DEDUPE_WINDOW_SEC ||
			 rangesOverlapTooMuch(candidate.range, selectedCandidate.range));
		if (duplicateCue || rangesOverlapTooMuch(candidate.range, selectedCandidate.range))
			return true;
	}

	return false;
}

bool appendDistinctCandidate(QVector<CandidateRangeScore> &selected, const CandidateRangeScore &candidate, int maxCandidates)
{
	if (selected.size() >= maxCandidates)
		return false;
	if (candidateAlreadyCovered(candidate, selected))
		return false;

	selected.append(candidate);
	return true;
}

int appendTemporalQuotaCandidates(const QVector<CandidateRangeScore> &allCandidates, const ClipDuration &manualRange,
					 const CandidateDiscoveryProfile &profile, QVector<CandidateRangeScore> &selected, int *qualityRejected)
{
	if (qualityRejected)
		*qualityRejected = 0;

	if (!temporalQuotaEnabled(profile) || allCandidates.isEmpty())
		return 0;

	const double durationSec = rangeDurationSec(manualRange);
	const int bucketCount = std::max(1, static_cast<int>(std::ceil(durationSec / profile.temporalQuotaWindowSec)));
	int appended = 0;

	for (int bucket = 0; bucket < bucketCount && selected.size() < profile.maxCandidates; ++bucket) {
		int bucketSelected = 0;
		for (const CandidateRangeScore &candidate : allCandidates) {
			if (selected.size() >= profile.maxCandidates)
				break;
			if (temporalBucketIndex(manualRange, profile, candidate.range.startSec) != bucket)
				continue;
			if (!shouldKeepTemporalQuotaCandidate(candidate, profile)) {
				if (qualityRejected)
					++(*qualityRejected);
				continue;
			}
			if (!appendDistinctCandidate(selected, candidate, profile.maxCandidates))
				continue;

			++bucketSelected;
			++appended;
			if (bucketSelected >= profile.temporalQuotaMinCandidates)
				break;
		}
	}

	return appended;
}

QVector<ClipDuration> genericCandidateRanges(const RecordingTranscript &transcript, const ClipDuration &manualRange,
					      QString *summary)
{
	const CandidateDiscoveryProfile profile = discoveryProfileForRange(manualRange);
	QVector<CandidateRangeScore> allCandidates;

	for (const TranscriptSegment &segment : transcript.segments) {
		if (!segmentOverlapsRange(segment, manualRange) || segment.text.trimmed().isEmpty())
			continue;
		if (!hasStrongLocalCue(segment.text))
			continue;

		const QStringList terms = genericAnchorTermsForSegment(segment.text);
		const AnchorStartEstimate initialAnchor = estimatedAnchorStart(segment, terms);
		const double segmentEmotional = Curation::emotionalScoreForText(segment.text);
		const double segmentAdvice = Curation::adviceScoreForText(segment.text);
		ClipDuration range = buildCandidateRange(manualRange, initialAnchor, segmentEmotional, segmentAdvice, profile);
		if (range.endSec <= range.startSec)
			continue;

		const CandidateAnchorChoice aligned = bestAnchorInsideRange(transcript, range, initialAnchor);
		range = buildCandidateRange(manualRange, aligned.anchor, aligned.emotionalScore, aligned.adviceScore, profile);
		range = expandCandidateStartForViewerContext(transcript, range, aligned.anchor, manualRange);
		range = extendCandidateForOpenExplanation(transcript, range, manualRange, profile);
		const double endSecBeforeBoundaryTrim = range.endSec;
		range = trimCandidateBeforeNextCue(transcript, range, aligned.anchor.startSec, profile);
		range = extendCandidateForOpenExplanation(transcript, range, manualRange, profile);
		const bool boundaryTrimmed = range.endSec < endSecBeforeBoundaryTrim - 0.01;
		if (range.endSec <= range.startSec)
			continue;

		CandidateRangeScore scored = scoreCandidateRange(transcript, range, aligned.anchor, profile);
		scored.boundaryTrimmed = boundaryTrimmed;
		scored.startAdjusted = scored.startAdjusted || aligned.realigned;
		if (scored.score < profile.poolMinScore)
			continue;

		allCandidates.append(scored);
	}

	std::sort(allCandidates.begin(), allCandidates.end(), [](const CandidateRangeScore &left, const CandidateRangeScore &right) {
		if (std::fabs(left.score - right.score) > 0.0001)
			return left.score > right.score;
		return left.range.startSec < right.range.startSec;
	});

	QVector<CandidateRangeScore> selected;
	selected.reserve(std::min(static_cast<long long>(profile.maxCandidates), static_cast<long long>(allCandidates.size())));
	int quotaRejected = 0;
	const int quotaSelected = appendTemporalQuotaCandidates(allCandidates, manualRange, profile, selected, &quotaRejected);

	if (!allCandidates.isEmpty()) {
		const CandidateRangeScore bestCandidate = allCandidates.first();
		for (const CandidateRangeScore &candidate : allCandidates) {
			if (selected.size() >= profile.maxCandidates)
				break;
			if (candidate.score < profile.minScore)
				continue;
			if (temporalQuotaEnabled(profile) && !shouldKeepTemporalQuotaCandidate(candidate, profile))
				continue;
			if (!selected.isEmpty() && !shouldKeepSecondaryCandidate(candidate, bestCandidate, profile))
				continue;
			appendDistinctCandidate(selected, candidate, profile.maxCandidates);
		}
	}

	std::sort(selected.begin(), selected.end(), [](const CandidateRangeScore &left, const CandidateRangeScore &right) {
		if (std::fabs(left.score - right.score) > 0.0001)
			return left.score > right.score;
		return left.range.startSec < right.range.startSec;
	});

	if (summary) {
		const QString selectedSummary = candidateSummary(selected);
		*summary = QStringLiteral("profile=%1 maxProjects=%2 rawCandidates=%3 quotaWindowSec=%4 quotaSelected=%5 quotaRejected=%6 selected=%7; %8")
				   .arg(profile.name)
				   .arg(profile.maxCandidates)
				   .arg(allCandidates.size())
				   .arg(profile.temporalQuotaWindowSec, 0, 'f', 0)
				   .arg(quotaSelected)
				   .arg(quotaRejected)
				   .arg(selected.size())
				   .arg(selectedSummary);
	}

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
