#include "curation/range/viewer-message-focus-range.hpp"

#include "curation/curation-preset.hpp"
#include "curation/scoring/clip-scoring-pipeline.hpp"
#include "curation/scoring/text-analysis.hpp"

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
static constexpr int VIEWER_CANDIDATE_SHORT_MAX_PROJECTS = 3;
static constexpr int VIEWER_CANDIDATE_MEDIUM_MAX_PROJECTS = 6;
static constexpr int VIEWER_CANDIDATE_LARGE_MAX_PROJECTS = 12;
static constexpr int VIEWER_CANDIDATE_HUGE_MAX_PROJECTS = 16;

using Curation::Scoring::TextAnalysis::normalized;

struct ViewerFallbackProfile {
	QString name;
	int maxCandidates = VIEWER_CANDIDATE_SHORT_MAX_PROJECTS;
	double defaultAfterSec = 28.0;
	double emotionalAfterSec = 34.0;
	double adviceAfterSec = 30.0;
	double minDurationSec = 18.0;
	double boundaryMinDurationSec = 7.0;
	double maxDurationSec = 42.0;
	double minFinalScore = 0.50;
	double slidingWindowStepSec = 15.0;
	int maxRawCandidates = 120;
};

double rangeDurationSec(const ClipDuration &range)
{
	return std::max(0.0, range.endSec - range.startSec);
}

ViewerFallbackProfile fallbackProfileForRange(const ClipDuration &range)
{
	const double durationSec = rangeDurationSec(range);
	if (durationSec >= 7200.0) {
		return {QStringLiteral("huge_discovery"), VIEWER_CANDIDATE_HUGE_MAX_PROJECTS, 42.0, 48.0, 50.0,
			22.0, 9.0, 64.0, 0.34, 24.0, 480};
	}
	if (durationSec >= 1800.0) {
		return {QStringLiteral("large_discovery"), VIEWER_CANDIDATE_LARGE_MAX_PROJECTS, 40.0, 46.0, 48.0,
			22.0, 9.0, 60.0, 0.34, 20.0, 360};
	}
	if (durationSec >= 600.0) {
		return {QStringLiteral("medium_discovery"), VIEWER_CANDIDATE_MEDIUM_MAX_PROJECTS, 34.0, 42.0, 42.0,
			20.0, 8.0, 52.0, 0.38, 18.0, 220};
	}

	return {QStringLiteral("precision"), VIEWER_CANDIDATE_SHORT_MAX_PROJECTS, 28.0, 34.0, 30.0, 18.0,
		7.0, 42.0, 0.45, 15.0, 120};
}

QString extractTargetFromPrompt(const QString &prompt)
{
	static const QRegularExpression specificPattern(QStringLiteral("specifically\\s+about\\s+([^\\.]+)"),
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

bool containsPortugueseTargetMarkers(const QString &target)
{
	const QString value = normalized(target);
	const QString padded = QStringLiteral(" ") + value + QLatin1Char(' ');

	for (const QString &hardToken : {QStringLiteral("resposta"), QStringLiteral("pergunta"),
		     QStringLiteral("mensagem"), QStringLiteral("espectador"), QStringLiteral("conversa"),
		     QStringLiteral("aumento"), QStringLiteral("chefe"), QStringLiteral("pedir"),
		     QStringLiteral("numa"), QStringLiteral("num")}) {
		if (padded.contains(QStringLiteral(" ") + hardToken + QLatin1Char(' ')))
			return true;
	}

	int softHits = 0;
	for (const QString &softToken : {QStringLiteral("sobre"), QStringLiteral("como"), QStringLiteral("para"),
		     QStringLiteral("com"), QStringLiteral("uma"), QStringLiteral("em"), QStringLiteral("de"),
		     QStringLiteral("da"), QStringLiteral("do"), QStringLiteral("dos"), QStringLiteral("das")}) {
		if (padded.contains(QStringLiteral(" ") + softToken + QLatin1Char(' ')))
			++softHits;
	}

	return softHits >= 2;
}

QStringList wordsFromTarget(const QString &target)
{
	QSet<QString> extraStopWords;
	extraStopWords.insert(QStringLiteral("boss"));
	return Curation::Scoring::TextAnalysis::meaningfulTerms(target, extraStopWords);
}

bool isReliableFocusTarget(const QString &target)
{
	const QString value = normalized(target);
	if (value.size() < 6)
		return false;
	if (containsPortugueseTargetMarkers(target) || Curation::Scoring::TextAnalysis::isGenericFocusTarget(target))
		return false;

	return !wordsFromTarget(target).isEmpty();
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
	const QString normalizedTarget = normalized(target);

	if (normalizedTarget.contains(QStringLiteral("raise")) || normalizedTarget.contains(QStringLiteral("salary increase"))) {
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
		if (containsPortugueseTargetMarkers(keyword) || Curation::Scoring::TextAnalysis::isGenericFocusTarget(keyword))
			continue;
		for (const QString &word : wordsFromTarget(keyword))
			terms << word;
	}

	QStringList normalizedTerms;
	for (const QString &term : terms) {
		const QString value = normalized(term);
		if (value.size() >= 3)
			normalizedTerms << value;
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

int earliestTermPosition(const QString &normalizedSegment, const QStringList &terms)
{
	return firstPositionForPhrases(normalizedSegment, terms);
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

	const QString normalizedOriginal = normalized(original);
	if (normalizedOriginal.isEmpty())
		return original;

	const double ratio = std::clamp(static_cast<double>(normalizedPosition) /
					      static_cast<double>(normalizedOriginal.size()),
				      0.0, 1.0);
	const int originalIndex = std::clamp(static_cast<int>(std::floor(ratio * static_cast<double>(original.size()))), 0,
					      std::max(0, static_cast<int>(original.size()) - 1));

	QString trimmed = original.mid(originalIndex).trimmed();
	trimmed.replace(QRegularExpression(QStringLiteral("^[\\s\\-:;,.!?\"]+")), QString());
	return trimmed.isEmpty() ? original : trimmed;
}

int meaningfulStartPosition(const QString &normalizedSegment, const QStringList &terms)
{
	static const QStringList strongestMessageStarts{
		QStringLiteral("so aposta"), QStringLiteral("so apostam"), QStringLiteral("aposta quem quer"),
		QStringLiteral("apostam a quem quer"), QStringLiteral("a pessoa aposta"),
		QStringLiteral("eu perdi meu pai"), QStringLiteral("perdi meu pai"),
		QStringLiteral("eu perdi minha mae"), QStringLiteral("perdi minha mae"),
		QStringLiteral("meu pai morreu"), QStringLiteral("minha mae morreu"),
		QStringLiteral("sinto muita falta"), QStringLiteral("eu tava passando"), QStringLiteral("tava passando"),
	};
	const int strongestPosition = firstPositionForPhrases(normalizedSegment, strongestMessageStarts);
	if (strongestPosition >= 0)
		return strongestPosition;

	int position = earliestTermPosition(normalizedSegment, terms);
	static const QStringList usefulMessageStarts{
		QStringLiteral("nao sei se eu tenho"), QStringLiteral("nao sei se tenho"),
		QStringLiteral("nao sei se e"), QStringLiteral("depressao ou ansiedade"),
		QStringLiteral("depressao e ansiedade"), QStringLiteral("mano como"),
		QStringLiteral("como eu posso"), QStringLiteral("como posso"), QStringLiteral("eu nunca pedi"),
		QStringLiteral("eu gostava"), QStringLiteral("por que"), QStringLiteral("lembra"),
		QStringLiteral("nao sei"), QStringLiteral("na ansiedade"),
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

	const QString normalizedSegment = normalized(segment.text);
	const int position = meaningfulStartPosition(normalizedSegment, terms);
	if (position <= 0 || normalizedSegment.isEmpty())
		return estimate;

	estimate.anchorText = trimmedAnchorTextFromPosition(segment, position);
	estimate.adjusted = true;

	const double duration = std::max(0.0, segment.endSec - segment.startSec);
	if (duration <= 0.0) {
		estimate.startSec = segment.startSec + PREAMBLE_ONLY_SEGMENT_OFFSET_SEC;
		return estimate;
	}

	const double ratio = std::clamp(static_cast<double>(position) / static_cast<double>(normalizedSegment.size()), 0.0, 1.0);
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

QVector<ClipDuration> genericCandidateRanges(const RecordingTranscript &transcript, const ClipDuration &manualRange,
					      QString *summary)
{
	const ViewerFallbackProfile profile = fallbackProfileForRange(manualRange);

	Curation::Scoring::ClipScoringPipelineOptions options;
	options.generation.searchRange = manualRange;
	options.generation.presetId = QStringLiteral("viewer_message_response");
	options.generation.minDurationSec = profile.minDurationSec;
	options.generation.maxDurationSec = profile.maxDurationSec;
	options.generation.defaultAfterSec = profile.defaultAfterSec;
	options.generation.emotionalAfterSec = profile.emotionalAfterSec;
	options.generation.adviceAfterSec = profile.adviceAfterSec;
	options.generation.boundaryMinDurationSec = profile.boundaryMinDurationSec;
	options.generation.slidingWindowStepSec = profile.slidingWindowStepSec;
	options.generation.maxRawCandidates = profile.maxRawCandidates;
	options.scoring.presetId = QStringLiteral("viewer_message_response");
	options.ranking.maxCandidates = profile.maxCandidates;
	options.ranking.minFinalScore = profile.minFinalScore;
	options.ranking.overlapToleranceSec = 8.0;

	const Curation::Scoring::ClipScoringPipeline pipeline;
	const Curation::Scoring::ClipScoringResult scoring = pipeline.score(transcript, options);
	if (summary) {
		*summary = QStringLiteral("profile=%1 maxProjects=%2 selected=%3; %4")
			   .arg(profile.name)
			   .arg(profile.maxCandidates)
			   .arg(scoring.candidates.size())
			   .arg(scoring.summary);
	}
	return scoring.ranges();
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

		const QString normalizedSegment = normalized(segment.text);
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
