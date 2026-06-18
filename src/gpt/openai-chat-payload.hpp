#pragma once

#include <QJsonObject>
#include <QString>

namespace OpenAiChatPayload {

QJsonObject build(const QString &model, const QString &inputText, int maxTokens, bool jsonMode = true);

} // namespace OpenAiChatPayload
