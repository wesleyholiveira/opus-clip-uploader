#pragma once

#include <QString>
#include <QStringList>

namespace Curation {

bool textHasViewerExchangeSignals(const QString &text);
double emotionalScoreForText(const QString &text, QStringList *matchedCues = nullptr);
double adviceScoreForText(const QString &text);
double explanationScoreForText(const QString &text);
double storyScoreForText(const QString &text);
double opinionScoreForText(const QString &text);
double tutorialScoreForText(const QString &text);

} // namespace Curation
