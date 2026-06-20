#include "curation/rules/curation-rule-helpers.hpp"

namespace Curation {

ClipStrategy defaultClipStrategyForScope(const QString &scope)
{
	if (scope == QStringLiteral("large_range_multiple_clips") ||
	    scope == QStringLiteral("medium_range_multiple_independent_clips"))
		return ClipStrategy::MultipleIndependentClips;
	return ClipStrategy::BestMoment;
}

bool hasSingleConfirmedRange(const CurationSettings &settings)
{
	return settings.clipDurations.size() == 1;
}

QString combinedRuleText(const CurationSettings &settings, const QString &hint)
{
	return QStringLiteral("%1 %2 %3 %4")
		.arg(settings.genre, settings.topicKeywords.join(QLatin1Char(' ')), settings.aiPrompt, hint)
		.toLower()
		.simplified();
}

bool containsAny(const QString &text, const QStringList &phrases)
{
	for (const QString &phrase : phrases) {
		if (text.contains(phrase, Qt::CaseInsensitive))
			return true;
	}
	return false;
}

} // namespace Curation
