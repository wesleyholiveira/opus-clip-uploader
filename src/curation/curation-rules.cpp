#include "curation/curation-rules.hpp"

#include "curation/curation-preset.hpp"

namespace {

static Curation::ClipStrategy strategyForScope(const QString &scope)
{
	if (scope == QStringLiteral("large_range_multiple_clips") ||
	    scope == QStringLiteral("medium_range_multiple_independent_clips"))
		return Curation::ClipStrategy::MultipleIndependentClips;
	return Curation::ClipStrategy::BestMoment;
}

static bool shouldPreferSingleBestClipForSingleConfirmedRange(const CurationSettings &settings, const QString &presetId)
{
	// A single confirmed range is already an editorial constraint. For presets that search for one
	// coherent answer/explanation, keep the Opus search focused on the strongest complete clip
	// inside that range instead of broadening the prompt into multiple independent clips just
	// because the selected range is long.
	const QString id = CurationPreset::normalizeId(presetId);
	return settings.clipDurations.size() == 1 &&
	       (id == CurationPreset::viewerMessageResponsePresetId() || id == QStringLiteral("explanation"));
}

static QVector<Curation::BoundaryPolicy> boundaryPoliciesForPreset(const QString &presetId)
{
	const QString id = CurationPreset::normalizeId(presetId);
	QVector<Curation::BoundaryPolicy> policies{Curation::BoundaryPolicy::NaturalResolution};

	if (id == CurationPreset::viewerMessageResponsePresetId() || id == QStringLiteral("advice_answer") ||
	    id == QStringLiteral("emotional_reaction")) {
		policies.append(Curation::BoundaryPolicy::StopBeforeNextViewerMessage);
		policies.append(Curation::BoundaryPolicy::StopBeforeHousekeeping);
		policies.append(Curation::BoundaryPolicy::StopBeforeTopicShift);
	}

	return policies;
}

} // namespace

namespace Curation {

Intent resolveIntent(const CurationSettings &settings, const RecordingTranscript &selectedRangeTranscript,
		     const QString &hint)
{
	const Signals result = analyzeSignals(selectedRangeTranscript, settings, hint);
	const QString requestedPresetId = CurationPreset::normalizeId(settings.curationPreset);
	QString resolvedPresetId = requestedPresetId;

	if (resolvedPresetId == CurationPreset::autoPresetId()) {
		if (CurationPreset::isViewerMessageResponsePrompt(hint) ||
		    CurationPreset::isViewerMessageResponsePrompt(settings.aiPrompt) || result.likelyViewerExchange) {
			resolvedPresetId = CurationPreset::viewerMessageResponsePresetId();
		} else {
			resolvedPresetId = CurationPreset::resolveId(settings, hint);
		}
	}

	Intent intent;
	intent.requestedPresetId = requestedPresetId;
	intent.resolvedPresetId = resolvedPresetId;
	intent.selectedDurationSec = result.selectedDurationSec;
	intent.scope = scopeForDuration(result.selectedDurationSec);
	intent.viewerSignals = result.likelyViewerExchange;
	intent.archetype = archetypeFromPresetId(resolvedPresetId);
	intent.contentKind = contentKindFromArchetype(intent.archetype, result.likelyViewerExchange);
	intent.strategy = shouldPreferSingleBestClipForSingleConfirmedRange(settings, resolvedPresetId)
				  ? Curation::ClipStrategy::BestMoment
				  : strategyForScope(intent.scope);
	intent.boundaryPolicies = boundaryPoliciesForPreset(resolvedPresetId);
	return intent;
}

QString resolvePresetId(const CurationSettings &settings, const RecordingTranscript &selectedRangeTranscript,
			const QString &hint)
{
	return resolveIntent(settings, selectedRangeTranscript, hint).resolvedPresetId;
}

bool shouldUseMultipleClips(const Intent &intent)
{
	return isMultipleClipStrategy(intent.strategy);
}

} // namespace Curation
