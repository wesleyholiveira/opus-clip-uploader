#include "curation/opus-prompt-renderer.hpp"

#include "curation/curation-preset.hpp"
#include "curation/curation-rules.hpp"

namespace OpusPromptRenderer {

QString renderPresetPrompt(const QString &presetId, bool multipleClips)
{
	const QString id = CurationPreset::normalizeId(presetId);
	const QString clipNoun = multipleClips ? QStringLiteral("clips") : QStringLiteral("clip");
	const QString findPrefix = multipleClips ? QStringLiteral("Find self-contained clips")
						 : QStringLiteral("Find the strongest self-contained clip");

	if (id == CurationPreset::viewerMessageResponsePresetId()) {
		const QString eachClip = multipleClips ? QStringLiteral("each clip") : QStringLiteral("the clip");
		const QString chooseClips = multipleClips ? QStringLiteral("Choose clips that stop")
							  : QStringLiteral("Choose a clip that stops");

		return QStringLiteral(
			       "%1 built from the longest coherent response to a single viewer message. Prioritize emotionally consequential and clearly useful viewer messages over casual banter; %2 should include only enough of that one message for context, then follow the speaker's direct response while it stays focused on that same message. %3 at the first natural resolution; if no clean longer response exists, choose a shorter complete reaction instead of continuing into another viewer message, stream housekeeping, or a different topic.")
			.arg(findPrefix, eachClip, chooseClips)
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

QString renderIntentPrompt(const Curation::Intent &intent)
{
	return renderPresetPrompt(intent.resolvedPresetId, Curation::shouldUseMultipleClips(intent));
}

} // namespace OpusPromptRenderer
