#pragma once

#include <QString>
#include <QStringList>

inline constexpr const char *OPENAI_MODEL_DISABLED = "disabled";

QString obsText(const char *key);
const QString &clipCropperTitle();

QStringList get_recording_paths_for_upload();
QString get_opus_api_key();
QString get_openai_api_key();
QString get_openai_model();
