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

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QIODevice>
#include <QSaveFile>

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

int PluginConfig::removeValuesWithPrefixes(const QStringList &prefixes)
{
	QStringList normalizedPrefixes;
	for (const QString &prefix : prefixes) {
		const QString trimmed = prefix.trimmed();
		if (!trimmed.isEmpty())
			normalizedPrefixes.append(trimmed);
	}

	if (normalizedPrefixes.isEmpty())
		return 0;

	ensureConfigDir();

	char *configPath = getConfigPath();
	if (!configPath)
		return 0;

	const QString path = QString::fromUtf8(configPath);
	bfree(configPath);

	QFile file(path);
	if (!file.exists() || !file.open(QIODevice::ReadOnly))
		return 0;

	const QByteArray data = file.readAll();
	file.close();

	QJsonParseError parseError;
	const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
	if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
		obs_log(LOG_WARNING,
			"[clip-cropper] Could not purge config cache keys because settings.json is not a JSON object: %s",
			parseError.errorString().toUtf8().constData());
		return 0;
	}

	QJsonObject root = doc.object();
	QStringList keysToRemove;
	for (const QString &key : root.keys()) {
		for (const QString &prefix : normalizedPrefixes) {
			if (key.startsWith(prefix)) {
				keysToRemove.append(key);
				break;
			}
		}
	}

	for (const QString &key : keysToRemove)
		root.remove(key);

	if (keysToRemove.isEmpty())
		return 0;

	QSaveFile out(path);
	if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		obs_log(LOG_ERROR, "[clip-cropper] Failed to open settings.json for cache purge: %s",
			path.toUtf8().constData());
		return 0;
	}

	out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	if (!out.commit()) {
		obs_log(LOG_ERROR, "[clip-cropper] Failed to save settings.json after cache purge: %s",
			path.toUtf8().constData());
		return 0;
	}

	obs_log(LOG_INFO, "[clip-cropper] Purged %d cached config key(s).", keysToRemove.size());
	return keysToRemove.size();
}
