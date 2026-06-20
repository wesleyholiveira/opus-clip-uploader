#include "curation/compiler/prompt-compiler-internal.hpp"

#include "curation/analysis/named-reference-detector.hpp"
#include "curation/curation-preset.hpp"
#include "curation/curation-rules.hpp"

using namespace Curation;

QString Curation::renderPlanToOpusPrompt(const QJsonObject &plan,
						 const RecordingTranscript &selectedRangeTranscript,
						 const CurationSettings &curationSettings)
{
	const bool noStrongClip = plan.value(QStringLiteral("no_strong_clip_found")).toBool(false);
	if (noStrongClip) {
		QString reason = jsonStringValue(plan, QStringLiteral("reason"));
		if (reason.isEmpty())
			reason = QStringLiteral(
				"No complete self-contained moment with a clear payoff was found in the selected range.");
		return QString::fromLatin1(semanticGateFailurePrefixLiteral()) + QLatin1Char(' ') + reason;
	}

	const QString arcType = jsonStringValue(plan, QStringLiteral("arc_type"));
	QString target = stripLeadingFindTarget(jsonStringValue(plan, QStringLiteral("main_target")));
	QString contextPhrase = cleanPlanPhrase(jsonStringValue(plan, QStringLiteral("context_phrase")));
	QString opening = cleanPlanPhrase(jsonStringValue(plan, QStringLiteral("opening_criteria")));
	QString development = cleanPlanPhrase(jsonStringValue(plan, QStringLiteral("development_criteria")));
	QString continuity = cleanPlanPhrase(jsonStringValue(plan, QStringLiteral("continuity_criteria")));
	QString ending = cleanPlanPhrase(jsonStringValue(plan, QStringLiteral("ending_criteria")));
	QString boundary = cleanPlanPhrase(jsonStringValue(plan, QStringLiteral("boundary_criteria")));

	if (target.isEmpty())
		target = deterministicPromptTarget(QString(), selectedRangeTranscript, curationSettings);
	if (opening.isEmpty())
		opening = QStringLiteral("start with enough local setup");
	if (development.isEmpty())
		development = QStringLiteral("follow the same continuous idea");
	if (continuity.isEmpty())
		continuity = QStringLiteral("minimal dead air and no unrelated topic switches");
	if (ending.isEmpty())
		ending = QStringLiteral("resolve the local point clearly");
	if (boundary.isEmpty())
		boundary = QStringLiteral("unfinished setup, list fragments, or transitions into a different topic");

	const QString planHint = trimmedJoinNonEmpty({arcType, target, contextPhrase, opening, development, continuity,
						      ending, boundary},
						     QStringLiteral(" "));
	const Intent intent = resolveIntent(curationSettings, selectedRangeTranscript, planHint);
	if (intent.resolvedPresetId == CurationPreset::viewerMessageResponsePresetId() &&
	    !transcriptHasReferenceBackedUnlockMethod(selectedRangeTranscript))
		return renderViewerMessagePlanPrompt(plan, selectedRangeTranscript, curationSettings, planHint);

	QString sentence1;
	QString sentence2;
	QString sentence3;

	if (transcriptHasReferenceBackedUnlockMethod(selectedRangeTranscript)) {
		const QStringList references = importantNamedReferences(selectedRangeTranscript);
		const QString reference = references.isEmpty() ? QStringLiteral("the named method") : references.first();
		const QString referenceBackedTarget =
			deterministicPromptTarget(QString(), selectedRangeTranscript, curationSettings);
		if (!referenceBackedTarget.isEmpty())
			target = referenceBackedTarget;

		sentence1 = QStringLiteral("%1 the speaker %2 %3.")
				    .arg(scopeFindPhrase(selectedRangeTranscript, curationSettings, planHint),
					 arcVerbForType(arcType), target);
		sentence2 =
			QStringLiteral(
				"Prioritize moments that introduce %1 with the key/unlock metaphor before later pronouns, then show how one piece of language logic becomes easier to understand with continuous spoken development and minimal dead air.")
				.arg(reference);
		sentence3 = QStringLiteral(
			"Prefer clips that end after the local point is resolved, with clean natural boundaries and without unfinished setup, list items, long pauses, or mid-thought transitions.");
	} else {
		sentence1 = QStringLiteral("%1 the speaker %2 %3%4.")
				    .arg(scopeFindPhrase(selectedRangeTranscript, curationSettings, planHint),
					 arcVerbForType(arcType), target,
					 contextPhrase.isEmpty() ? QString() : QStringLiteral(" ") + contextPhrase);
		sentence2 = QStringLiteral("Prioritize moments that %1, then %2, keeping %3.")
				    .arg(opening, development, continuity);
		sentence3 = QStringLiteral("Prefer clips that %1, with clean natural boundaries and without %2.")
				    .arg(ending, boundary);
	}

	return QStringLiteral("%1 %2 %3").arg(sentence1, sentence2, sentence3).simplified();
}
