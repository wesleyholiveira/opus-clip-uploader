#ifdef __cplusplus
extern "C" {
#endif

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include "plugin-support.h"

#ifdef __cplusplus
}
#endif

#include "config.hpp"

char *PluginConfig::getConfigPath()
{
	return obs_module_config_path("settings.json");
}

bool PluginConfig::ensureConfigDir()
{
	char *configDir = obs_module_config_path("");

	if (!configDir) {
		obs_log(LOG_ERROR, "[clip-cropper] Failed to get config directory");
		return false;
	}

	os_mkdirs(configDir);
	bfree(configDir);

	return true;
}

obs_data_t *PluginConfig::loadConfig()
{
	char *configPath = getConfigPath();

	if (!configPath) {
		obs_log(LOG_ERROR, "[clip-cropper] Failed to get config path");
		return obs_data_create();
	}

	obs_data_t *settings = obs_data_create_from_json_file_safe(configPath, "bak");

	bfree(configPath);

	if (!settings) {
		return obs_data_create();
	}

	return settings;
}

void PluginConfig::setValue(const QString &key, const QString &value)
{
	if (key.trimmed().isEmpty()) {
		obs_log(LOG_WARNING, "[clip-cropper] Ignoring empty config key");
		return;
	}

	ensureConfigDir();

	char *configPath = getConfigPath();

	if (!configPath) {
		obs_log(LOG_ERROR, "[clip-cropper] Failed to get config path");
		return;
	}

	obs_data_t *settings = loadConfig();

	obs_data_set_string(settings, key.toUtf8().constData(), value.toUtf8().constData());

	const bool saved = obs_data_save_json(settings, configPath);

	obs_log(LOG_INFO, "[clip-cropper] Config saved. key=%s saved=%s", key.toUtf8().constData(),
		saved ? "true" : "false");

	obs_data_release(settings);
	bfree(configPath);
}

QString PluginConfig::getValue(const QString &key, const QString &defaultValue)
{
	if (key.trimmed().isEmpty()) {
		obs_log(LOG_WARNING, "[clip-cropper] Empty config key requested");
		return defaultValue;
	}

	char *configPath = getConfigPath();

	if (!configPath) {
		obs_log(LOG_ERROR, "[clip-cropper] Failed to get config path");
		return defaultValue;
	}

	obs_data_t *settings = obs_data_create_from_json_file_safe(configPath, "bak");

	bfree(configPath);

	if (!settings) {
		return defaultValue;
	}

	const char *value = obs_data_get_string(settings, key.toUtf8().constData());

	QString result = value && value[0] != '\0' ? QString::fromUtf8(value) : defaultValue;

	obs_data_release(settings);

	return result;
}

void PluginConfig::removeValue(const QString &key)
{
	if (key.trimmed().isEmpty()) {
		return;
	}

	ensureConfigDir();

	char *configPath = getConfigPath();

	if (!configPath) {
		return;
	}

	obs_data_t *settings = loadConfig();

	obs_data_erase(settings, key.toUtf8().constData());

	obs_data_save_json(settings, configPath);

	obs_data_release(settings);
	bfree(configPath);
}
