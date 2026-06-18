#include "curation/curation-preset.hpp"

#include <QStringList>

namespace {

static QString normalizedClipLengthPreset(QString preset)
{
	preset = preset.trimmed().toLower();

	if (preset == QStringLiteral("short"))
		return QStringLiteral("Short");

	if (preset == QStringLiteral("long"))
		return QStringLiteral("Long");

	if (preset == QStringLiteral("medium") || preset == QStringLiteral("recommended") ||
	    preset == QStringLiteral("complete"))
		return QStringLiteral("Medium");

	return QStringLiteral("Auto");
}

static bool clipLengthBoundsForPreset(const QString &preset, double &minSec, double &maxSec)
{
	const QString normalizedPreset = normalizedClipLengthPreset(preset);

	if (normalizedPreset == QStringLiteral("Short")) {
		minSec = 30.0;
		maxSec = 60.0;
		return true;
	}

	if (normalizedPreset == QStringLiteral("Medium")) {
		minSec = 60.0;
		maxSec = 120.0;
		return true;
	}

	if (normalizedPreset == QStringLiteral("Long")) {
		minSec = 120.0;
		maxSec = 180.0;
		return true;
	}

	minSec = 0.0;
	maxSec = 0.0;
	return false;
}

static bool promptContainsAll(const QString &prompt, const QStringList &tokens)
{
	const QString lower = prompt.toLower();
	for (const QString &token : tokens) {
		if (!lower.contains(token))
			return false;
	}
	return true;
}

} // namespace

namespace CurationPreset {

QString autoPresetId()
{
	return QStringLiteral("auto");
}

QString viewerMessageResponsePresetId()
{
	return QStringLiteral("viewer_message_response");
}

QString normalizeId(QString presetId)
{
	presetId = presetId.trimmed().toLower();
	presetId.replace(QLatin1Char('-'), QLatin1Char('_'));
	presetId.replace(QLatin1Char(' '), QLatin1Char('_'));

	if (presetId.isEmpty() || presetId == QStringLiteral("auto"))
		return autoPresetId();

	if (presetId == QStringLiteral("viewer") || presetId == QStringLiteral("chat") ||
	    presetId == QStringLiteral("q&a") || presetId == QStringLiteral("qa") ||
	    presetId == QStringLiteral("viewer_response") || presetId == QStringLiteral("viewer_message"))
		return viewerMessageResponsePresetId();

	if (presetId == QStringLiteral("advice") || presetId == QStringLiteral("advice_answer"))
		return QStringLiteral("advice_answer");

	if (presetId == QStringLiteral("emotional") || presetId == QStringLiteral("emotional_reaction"))
		return QStringLiteral("emotional_reaction");

	if (presetId == QStringLiteral("explanation"))
		return QStringLiteral("explanation");

	if (presetId == QStringLiteral("story") || presetId == QStringLiteral("story_arc"))
		return QStringLiteral("story_arc");

	if (presetId == QStringLiteral("opinion") || presetId == QStringLiteral("hot_take"))
		return QStringLiteral("opinion");

	if (presetId == QStringLiteral("tutorial") || presetId == QStringLiteral("tutorial_step"))
		return QStringLiteral("tutorial_step");

	return autoPresetId();
}

QVector<QPair<QString, QString>> options()
{
	return {
		{autoPresetId(), QStringLiteral("Auto")},
		{viewerMessageResponsePresetId(), QStringLiteral("Viewer message response")},
		{QStringLiteral("advice_answer"), QStringLiteral("Advice answer")},
		{QStringLiteral("emotional_reaction"), QStringLiteral("Emotional reaction")},
		{QStringLiteral("explanation"), QStringLiteral("Explanation")},
		{QStringLiteral("story_arc"), QStringLiteral("Story arc")},
		{QStringLiteral("opinion"), QStringLiteral("Opinion / hot take")},
		{QStringLiteral("tutorial_step"), QStringLiteral("Tutorial step")},
	};
}

QString labelForId(const QString &presetId)
{
	const QString normalized = normalizeId(presetId);
	for (const auto &option : options()) {
		if (option.first == normalized)
			return option.second;
	}
	return QStringLiteral("Auto");
}

bool isViewerMessageResponsePrompt(const QString &prompt)
{
	return promptContainsAll(prompt, {QStringLiteral("viewer message"), QStringLiteral("same message")}) ||
	       promptContainsAll(prompt,
				 {QStringLiteral("viewer message"), QStringLiteral("first resolved response")}) ||
	       promptContainsAll(prompt, {QStringLiteral("viewer message"), QStringLiteral("next viewer message")});
}

QString resolveId(const CurationSettings &settings, const QString &prompt)
{
	const QString explicitPreset = normalizeId(settings.curationPreset);
	if (explicitPreset != autoPresetId())
		return explicitPreset;

	if (isViewerMessageResponsePrompt(prompt) || isViewerMessageResponsePrompt(settings.aiPrompt))
		return viewerMessageResponsePresetId();

	const QString metadata = (settings.genre + QLatin1Char(' ') + settings.topicKeywords.join(QLatin1Char(' ')) +
				  QLatin1Char(' ') + prompt + QLatin1Char(' ') + settings.aiPrompt)
					 .toLower();

	if (metadata.contains(QStringLiteral("advice")) || metadata.contains(QStringLiteral("conselho")) ||
	    metadata.contains(QStringLiteral("relationship")) || metadata.contains(QStringLiteral("relacionamento")))
		return QStringLiteral("advice_answer");

	if (metadata.contains(QStringLiteral("chat")) || metadata.contains(QStringLiteral("q&a")) ||
	    metadata.contains(QStringLiteral("viewer")) || metadata.contains(QStringLiteral("comment")) ||
	    metadata.contains(QStringLiteral("espectador")) || metadata.contains(QStringLiteral("comentário")) ||
	    metadata.contains(QStringLiteral("comentario")))
		return viewerMessageResponsePresetId();

	return autoPresetId();
}

QString gptContextForId(const QString &presetId)
{
	const QString id = normalizeId(presetId);
	if (id == viewerMessageResponsePresetId())
		return QStringLiteral(
			"Use the viewer-message-response preset: find one viewer message plus the speaker's direct response to that same message. Prefer the longest coherent response while it stays on the same message, but choose a shorter complete reaction instead of continuing into another viewer message, housekeeping, or a different topic.");
	if (id == QStringLiteral("advice_answer"))
		return QStringLiteral(
			"Use the advice-answer preset: find one concrete question or problem and the speaker's useful answer. Prefer the full coherent advice, but stop before adjacent chat, examples, or a new problem if they start a separate arc.");
	if (id == QStringLiteral("emotional_reaction"))
		return QStringLiteral(
			"Use the emotional-reaction preset: find one emotionally consequential message and the speaker's immediate resolved reaction. A short complete reaction is better than extending into unrelated chat.");
	if (id == QStringLiteral("explanation"))
		return QStringLiteral(
			"Use the explanation preset: find one clear idea explained from setup through conclusion with continuous spoken development.");
	if (id == QStringLiteral("story_arc"))
		return QStringLiteral(
			"Use the story-arc preset: find one story with setup, development, and payoff without merging adjacent stories.");
	if (id == QStringLiteral("opinion"))
		return QStringLiteral(
			"Use the opinion preset: find one focused claim or take and the reasoning that resolves it.");
	if (id == QStringLiteral("tutorial_step"))
		return QStringLiteral(
			"Use the tutorial-step preset: find one actionable step or walkthrough segment that can stand alone.");
	return QStringLiteral(
		"Use Auto preset selection based on the transcript: choose the strongest self-contained local arc without mixing adjacent topics.");
}

QString opusPromptForId(const QString &presetId, bool multipleClips)
{
	const QString id = normalizeId(presetId);
	const QString clipNoun = multipleClips ? QStringLiteral("clips") : QStringLiteral("clip");
	const QString findPrefix = multipleClips ? QStringLiteral("Find self-contained clips")
						 : QStringLiteral("Find the strongest self-contained clip");

	if (id == viewerMessageResponsePresetId()) {
		return QStringLiteral(
			       "%1 built from the longest coherent response to a single viewer message. Prioritize emotionally consequential and clearly useful messages over casual banter; each %2 should include only enough of that one message for context, then follow the speaker's direct response while it stays on the same message. Choose %2 that stop at the first natural resolution; if no clean longer response exists, choose a shorter complete reaction instead of continuing into another viewer message, stream housekeeping, or a different topic.")
			.arg(findPrefix, clipNoun)
			.simplified();
	}

	if (id == QStringLiteral("advice_answer")) {
		return QStringLiteral(
			       "%1 where the speaker answers one concrete question or problem with useful advice. Prioritize moments that include enough setup for that problem, then follow one coherent answer while it stays on the same issue. Choose %2 that end at the first resolved advice, caveat, or takeaway instead of continuing into adjacent chat or a new problem.")
			.arg(findPrefix, clipNoun)
			.simplified();
	}

	if (id == QStringLiteral("emotional_reaction")) {
		return QStringLiteral(
			       "%1 where the speaker reacts to one emotionally consequential message or moment. Prioritize clips that include enough setup for that moment, then follow the immediate reaction until its first natural emotional payoff. Choose shorter complete reactions instead of clips that continue into another message or topic.")
			.arg(findPrefix)
			.simplified();
	}

	if (id == QStringLiteral("story_arc")) {
		return QStringLiteral(
			       "%1 where the speaker tells one self-contained story. Prioritize moments with clear setup, development, and payoff, keeping the story arc continuous. Choose %2 that end after the payoff or lesson resolves without merging into another story or topic.")
			.arg(findPrefix, clipNoun)
			.simplified();
	}

	if (id == QStringLiteral("opinion")) {
		return QStringLiteral(
			       "%1 where the speaker gives one focused opinion or take. Prioritize moments that state the claim, develop the reasoning, and reach a clear conclusion. Choose %2 that stop after that take resolves instead of drifting into adjacent topics.")
			.arg(findPrefix, clipNoun)
			.simplified();
	}

	if (id == QStringLiteral("tutorial_step")) {
		return QStringLiteral(
			       "%1 where the speaker walks through one actionable step or demo segment. Prioritize moments that introduce the step, show or explain it clearly, and reach a useful stopping point. Choose %2 that do not stop mid-step or merge into the next step.")
			.arg(findPrefix, clipNoun)
			.simplified();
	}

	return QStringLiteral(
		       "%1 where the speaker develops one clear local idea from setup to conclusion. Prioritize continuous spoken development with minimal dead air and enough local context to stand alone. Choose %2 with clean natural boundaries that stop after the local point resolves instead of continuing into another topic.")
		.arg(findPrefix, clipNoun)
		.simplified();
}

QString fallbackOpusPrompt(const CurationSettings &settings, bool multipleClips)
{
	return opusPromptForId(resolveId(settings, settings.aiPrompt), multipleClips);
}

ClipLengthBounds clipLengthBoundsForSettings(const CurationSettings &settings)
{
	ClipLengthBounds result;
	if (settings.skipCurate)
		return result;

	const QString normalizedPreset = normalizedClipLengthPreset(settings.clipLengthPreset);
	const QString presetId = resolveId(settings, settings.aiPrompt);

	if (presetId == viewerMessageResponsePresetId() && normalizedPreset != QStringLiteral("Long")) {
		result.enabled = true;
		result.minSec = 8.0;
		result.maxSec = 35.0;
		result.source = QStringLiteral("preset:viewer-message-response");
		return result;
	}

	if (presetId == QStringLiteral("emotional_reaction") && normalizedPreset != QStringLiteral("Long")) {
		result.enabled = true;
		result.minSec = 6.0;
		result.maxSec = 30.0;
		result.source = QStringLiteral("preset:emotional-reaction");
		return result;
	}

	if (presetId == QStringLiteral("advice_answer") && normalizedPreset == QStringLiteral("Auto")) {
		result.enabled = true;
		result.minSec = 12.0;
		result.maxSec = 45.0;
		result.source = QStringLiteral("preset:advice-answer");
		return result;
	}

	double minSec = 0.0;
	double maxSec = 0.0;
	if (clipLengthBoundsForPreset(settings.clipLengthPreset, minSec, maxSec)) {
		result.enabled = true;
		result.minSec = minSec;
		result.maxSec = maxSec;
		result.source = QStringLiteral("clip-length-preset");
		return result;
	}

	return result;
}

} // namespace CurationPreset
