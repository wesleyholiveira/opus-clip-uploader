#pragma once

#include <QString>
#include <QStringList>

namespace Curation::Scoring {

QStringList llamaCppModelSearchPaths(const QString &configuredModel);
QString resolveLlamaCppModelPath(const QString &configuredModel);

} // namespace Curation::Scoring
