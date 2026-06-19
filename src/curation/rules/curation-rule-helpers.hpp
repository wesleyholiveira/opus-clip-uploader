#pragma once

#include "curation/curation-intent.hpp"
#include "models/curation-settings.hpp"

#include <QString>
#include <QStringList>

namespace Curation {

ClipStrategy defaultClipStrategyForScope(const QString &scope);
bool hasSingleConfirmedRange(const CurationSettings &settings);
QString combinedRuleText(const CurationSettings &settings, const QString &hint);
bool containsAny(const QString &text, const QStringList &phrases);

} // namespace Curation
