#include "curation/curation-preset-profile.hpp"

#include <QStringList>

#include <utility>

namespace {

static bool promptContainsAll(const QString &prompt, const QStringList &tokens)
{
	const QString lower = prompt.toLower();
	for (const QString &token : tokens) {
		if (!lower.contains(token))
			return false;
	}
	return true;
}

static Curation::ExchangeArcPolicy defaultArcPolicy()
{
	return {};
}

static Curation::ExchangeArcPolicy singleViewerExchangeArcPolicy()
{
	Curation::ExchangeArcPolicy policy;
	policy.preferSingleExchange = true;
	policy.requireCleanOpening = true;
	policy.requireDevelopment = true;
	policy.requireLocalResolution = true;
	policy.stopBeforeNextViewerTurn = true;
	policy.stopBeforeTopicShift = true;
	policy.stopBeforeStreamMeta = true;
	policy.minOpening = 0.30;
	policy.minDevelopment = 0.38;
	policy.minConclusion = 0.30;
	policy.minCompleteness = 0.40;
	policy.minBoundaryCleanliness = 0.34;
	policy.maxTailRisk = 0.72;
	return policy;
}

static Curation::ExchangeArcPolicy emotionalArcPolicy()
{
	Curation::ExchangeArcPolicy policy = defaultArcPolicy();
	policy.requireDevelopment = false;
	policy.requireLocalResolution = true;
	policy.minOpening = 0.26;
	policy.minDevelopment = 0.24;
	policy.minConclusion = 0.26;
	policy.minCompleteness = 0.32;
	policy.minBoundaryCleanliness = 0.30;
	policy.maxTailRisk = 0.78;
	return policy;
}

static Curation::CurationPresetProfile makeProfile(QString id, QString label, Curation::PresetArchetype archetype,
						   Curation::PresetContentKind contentKind,
						   Curation::PresetClipLengthPolicy clipPolicy, double minSec,
						   double maxSec, QString clipLengthSource,
						   Curation::ExchangeArcPolicy arcPolicy, QStringList focusHints = {})
{
	Curation::CurationPresetProfile profile;
	profile.id = std::move(id);
	profile.label = std::move(label);
	profile.archetype = archetype;
	profile.contentKind = contentKind;
	profile.clipLengthPolicy = clipPolicy;
	profile.minDurationSec = minSec;
	profile.maxDurationSec = maxSec;
	profile.clipLengthSource = std::move(clipLengthSource);
	profile.arcPolicy = std::move(arcPolicy);
	profile.semanticFocusHints = std::move(focusHints);
	return profile;
}

} // namespace

namespace Curation {

QString autoPresetProfileId()
{
	return QStringLiteral("auto");
}

QString viewerMessageResponsePresetProfileId()
{
	return QStringLiteral("viewer_message_response");
}

QString normalizePresetProfileId(QString presetId)
{
	presetId = presetId.trimmed().toLower();
	presetId.replace(QLatin1Char('-'), QLatin1Char('_'));
	presetId.replace(QLatin1Char(' '), QLatin1Char('_'));

	if (presetId.isEmpty() || presetId == QStringLiteral("auto"))
		return autoPresetProfileId();

	if (presetId == viewerMessageResponsePresetProfileId() || presetId == QStringLiteral("viewer") ||
	    presetId == QStringLiteral("chat") || presetId == QStringLiteral("q&a") ||
	    presetId == QStringLiteral("qa") || presetId == QStringLiteral("viewer_response") ||
	    presetId == QStringLiteral("viewer_message"))
		return viewerMessageResponsePresetProfileId();

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

	return autoPresetProfileId();
}

QVector<CurationPresetProfile> presetProfiles()
{
	const ExchangeArcPolicy generic = defaultArcPolicy();
	const ExchangeArcPolicy viewer = singleViewerExchangeArcPolicy();
	return {
		makeProfile(autoPresetProfileId(), QStringLiteral("Auto"), PresetArchetype::Auto,
			    PresetContentKind::Unknown, PresetClipLengthPolicy::None, 0.0, 0.0, QStringLiteral("auto"),
			    generic),
		makeProfile(viewerMessageResponsePresetProfileId(), QStringLiteral("Viewer message response"),
			    PresetArchetype::ViewerMessageResponse, PresetContentKind::LiveChat,
			    PresetClipLengthPolicy::Always, 12.0, 180.0,
			    QStringLiteral("preset:viewer-message-response-complete-arc"), viewer,
			    {QStringLiteral("single viewer exchange"),
			     QStringLiteral("direct answer to one chat message"),
			     QStringLiteral("self-contained curiosity story or opinion arc"),
			     QStringLiteral("clean opening and first local resolution")}),
		makeProfile(QStringLiteral("advice_answer"), QStringLiteral("Advice answer"),
			    PresetArchetype::AdviceAnswer, PresetContentKind::Advice, PresetClipLengthPolicy::AutoOnly,
			    12.0, 45.0, QStringLiteral("preset:advice-answer"), viewer,
			    {QStringLiteral("practical advice"),
			     QStringLiteral("recommendation with reason and conclusion")}),
		makeProfile(QStringLiteral("emotional_reaction"), QStringLiteral("Emotional reaction"),
			    PresetArchetype::EmotionalReaction, PresetContentKind::EmotionalReaction,
			    PresetClipLengthPolicy::UnlessLong, 6.0, 30.0, QStringLiteral("preset:emotional-reaction"),
			    emotionalArcPolicy(),
			    {QStringLiteral("emotional reaction"), QStringLiteral("strong payoff")}),
		makeProfile(QStringLiteral("explanation"), QStringLiteral("Explanation"), PresetArchetype::Explanation,
			    PresetContentKind::Explanation, PresetClipLengthPolicy::None, 0.0, 0.0,
			    QStringLiteral("auto"), generic,
			    {QStringLiteral("clear explanation"), QStringLiteral("one idea from setup to resolution")}),
		makeProfile(QStringLiteral("story_arc"), QStringLiteral("Story arc"), PresetArchetype::StoryArc,
			    PresetContentKind::Story, PresetClipLengthPolicy::None, 0.0, 0.0, QStringLiteral("auto"),
			    generic, {QStringLiteral("story with setup conflict and payoff")}),
		makeProfile(QStringLiteral("opinion"), QStringLiteral("Opinion / hot take"), PresetArchetype::Opinion,
			    PresetContentKind::Opinion, PresetClipLengthPolicy::None, 0.0, 0.0, QStringLiteral("auto"),
			    generic,
			    {QStringLiteral("focused opinion"), QStringLiteral("claim with reasoning and resolution")}),
		makeProfile(QStringLiteral("tutorial_step"), QStringLiteral("Tutorial step"),
			    PresetArchetype::TutorialStep, PresetContentKind::Tutorial, PresetClipLengthPolicy::None,
			    0.0, 0.0, QStringLiteral("auto"), generic,
			    {QStringLiteral("tutorial step"), QStringLiteral("action with completion")}),
	};
}

QVector<QPair<QString, QString>> presetProfileOptions()
{
	QVector<QPair<QString, QString>> result;
	for (const CurationPresetProfile &profile : presetProfiles())
		result.append({profile.id, profile.label});
	return result;
}

CurationPresetProfile presetProfileForId(const QString &presetId)
{
	const QString normalized = normalizePresetProfileId(presetId);
	for (const CurationPresetProfile &profile : presetProfiles()) {
		if (profile.id == normalized)
			return profile;
	}
	return presetProfiles().front();
}

QString presetProfileLabelForId(const QString &presetId)
{
	return presetProfileForId(presetId).label;
}

bool isViewerMessageResponsePrompt(const QString &prompt)
{
	return promptContainsAll(prompt, {QStringLiteral("viewer message"), QStringLiteral("same message")}) ||
	       promptContainsAll(prompt,
				 {QStringLiteral("viewer message"), QStringLiteral("first resolved response")}) ||
	       promptContainsAll(prompt, {QStringLiteral("viewer message"), QStringLiteral("next viewer message")}) ||
	       promptContainsAll(prompt, {QStringLiteral("mensagem"), QStringLiteral("espectador")}) ||
	       promptContainsAll(prompt, {QStringLiteral("pergunta"), QStringLiteral("chat")});
}

bool isExplanationProfilePrompt(const QString &prompt)
{
	const QString lower = prompt.toLower();
	return lower.contains(QStringLiteral("explanation")) ||
	       lower.contains(QStringLiteral("explaining")) || lower.contains(QStringLiteral("explain")) ||
	       lower.contains(QStringLiteral("explicação")) || lower.contains(QStringLiteral("explicacao")) ||
	       lower.contains(QStringLiteral("explicar")) || lower.contains(QStringLiteral("concept")) ||
	       lower.contains(QStringLiteral("conceito")) || lower.contains(QStringLiteral("method")) ||
	       lower.contains(QStringLiteral("método")) || lower.contains(QStringLiteral("metodo")) ||
	       lower.contains(QStringLiteral("language learning")) || lower.contains(QStringLiteral("learning german")) ||
	       lower.contains(QStringLiteral("learn german")) || lower.contains(QStringLiteral("german learning")) ||
	       lower.contains(QStringLiteral("german vocabulary")) || lower.contains(QStringLiteral("aprender alemão")) ||
	       lower.contains(QStringLiteral("aprender alemao")) || lower.contains(QStringLiteral("alemão")) ||
	       lower.contains(QStringLiteral("alemao")) || lower.contains(QStringLiteral("spaced repetition")) ||
	       lower.contains(QStringLiteral("spaced repetitions")) || lower.contains(QStringLiteral("flashcard")) ||
	       lower.contains(QStringLiteral("vocabulary retention")) || lower.contains(QStringLiteral("unlock method")) ||
	       lower.contains(QStringLiteral("dolly")) || lower.contains(QStringLiteral("sensei")) ||
	       lower.contains(QStringLiteral("academic"));
}

QString resolvePresetProfileId(const CurationSettings &settings, const QString &prompt)
{
	const QString explicitPreset = normalizePresetProfileId(settings.curationPreset);
	if (explicitPreset != autoPresetProfileId())
		return explicitPreset;

	if (isViewerMessageResponsePrompt(prompt) || isViewerMessageResponsePrompt(settings.aiPrompt))
		return viewerMessageResponsePresetProfileId();

	const QString metadata = (settings.genre + QLatin1Char(' ') + settings.topicKeywords.join(QLatin1Char(' ')) +
				  QLatin1Char(' ') + prompt + QLatin1Char(' ') + settings.aiPrompt)
					 .toLower();

	const bool tutorialMetadata =
		metadata.contains(QStringLiteral("tutorial")) || metadata.contains(QStringLiteral("step by step")) ||
		metadata.contains(QStringLiteral("passo a passo")) || metadata.contains(QStringLiteral("how to configure")) ||
		metadata.contains(QStringLiteral("como configurar"));
	if (tutorialMetadata)
		return QStringLiteral("tutorial_step");

	const bool adviceMetadata =
		metadata.contains(QStringLiteral("advice")) || metadata.contains(QStringLiteral("conselho")) ||
		metadata.contains(QStringLiteral("recommendation")) || metadata.contains(QStringLiteral("recomendação")) ||
		metadata.contains(QStringLiteral("recomendacao")) || metadata.contains(QStringLiteral("relationship")) ||
		metadata.contains(QStringLiteral("relacionamento"));
	if (adviceMetadata)
		return QStringLiteral("advice_answer");

	// Educational topics should resolve to the explanation profile even when the file title
	// contains generic live/stream words. Otherwise Auto incorrectly falls into the viewer
	// Q&A gate and rejects useful explanatory clips as missing a viewer-message cue.
	if (isExplanationProfilePrompt(metadata))
		return QStringLiteral("explanation");

	const bool liveOrViewerMetadata =
		metadata.contains(QStringLiteral("chat")) || metadata.contains(QStringLiteral("q&a")) ||
		metadata.contains(QStringLiteral("viewer")) || metadata.contains(QStringLiteral("comment")) ||
		metadata.contains(QStringLiteral("message")) || metadata.contains(QStringLiteral("stream")) ||
		metadata.contains(QStringLiteral("espectador")) || metadata.contains(QStringLiteral("comentário")) ||
		metadata.contains(QStringLiteral("comentario")) || metadata.contains(QStringLiteral("mensagem")) ||
		metadata.contains(QStringLiteral("pergunta"));

	if (liveOrViewerMetadata)
		return viewerMessageResponsePresetProfileId();

	return autoPresetProfileId();
}

CurationPresetProfile presetProfileForSettings(const CurationSettings &settings, const QString &prompt)
{
	return presetProfileForId(resolvePresetProfileId(settings, prompt));
}

} // namespace Curation
