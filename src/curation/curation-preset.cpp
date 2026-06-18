#include "curation/curation-preset.hpp"

#include "curation/opus-prompt-renderer.hpp"

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

	if (presetId == viewerMessageResponsePresetId() || presetId == QStringLiteral("viewer") ||
	    presetId == QStringLiteral("chat") || presetId == QStringLiteral("q&a") ||
	    presetId == QStringLiteral("qa") || presetId == QStringLiteral("viewer_response") ||
	    presetId == QStringLiteral("viewer_message") || presetId == QStringLiteral("viewer_message_response"))
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

	const bool liveOrViewerMetadata =
		metadata.contains(QStringLiteral("chat")) || metadata.contains(QStringLiteral("q&a")) ||
		metadata.contains(QStringLiteral("viewer")) || metadata.contains(QStringLiteral("comment")) ||
		metadata.contains(QStringLiteral("message")) || metadata.contains(QStringLiteral("live")) ||
		metadata.contains(QStringLiteral("stream")) || metadata.contains(QStringLiteral("espectador")) ||
		metadata.contains(QStringLiteral("comentário")) || metadata.contains(QStringLiteral("comentario")) ||
		metadata.contains(QStringLiteral("mensagem")) || metadata.contains(QStringLiteral("pergunta"));

	if (liveOrViewerMetadata)
		return viewerMessageResponsePresetId();

	if (metadata.contains(QStringLiteral("advice")) || metadata.contains(QStringLiteral("conselho")) ||
	    metadata.contains(QStringLiteral("relationship")) || metadata.contains(QStringLiteral("relacionamento")))
		return QStringLiteral("advice_answer");

	return autoPresetId();
}

QString gptContextForId(const QString &presetId)
{
	const QString id = normalizeId(presetId);
	if (id == viewerMessageResponsePresetId())
		return QStringLiteral(
			"Use the viewer-message-response preset: find one viewer message plus the speaker's direct response to that same message. Prefer one complete answer to that single message, and stop before another viewer message, stream management, donor thanks, or a different topic.");
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
	return OpusPromptRenderer::renderPresetPrompt(presetId, multipleClips);
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
