#include "ui/ui-common.hpp"

#include <obs-module.h>

#include <utils/config.hpp>

QString obsText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

const QString &clipCropperTitle()
{
	static const QString title(QStringLiteral("Clip Cropper"));
	return title;
}

QString get_opus_api_key()
{
	return PluginConfig::getValue("opus_api_key").trimmed();
}

QString get_openai_api_key()
{
	return PluginConfig::getValue("openai_api_key").trimmed();
}

QString get_openai_model()
{
	return PluginConfig::getValue("openai_model", OPENAI_MODEL_DISABLED).trimmed();
}
