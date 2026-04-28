#ifdef __cplusplus
extern "C" {
#endif

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs/util/platform.h>

#ifdef __cplusplus
}
#endif

#include "config.hpp"

void save_access_token(const QString &accessToken)
{
	char *configDir = obs_module_config_path("");
	if (configDir) {
		os_mkdirs(configDir);
		bfree(configDir);
	}

	char *configPath = obs_module_config_path("settings.json");
	if (!configPath)
		return;

	obs_data_t *settings = obs_data_create_from_json_file_safe(configPath, "bak");
	if (!settings)
		settings = obs_data_create();

	obs_data_set_string(settings, "google_access_token", accessToken.toUtf8().constData());

	obs_data_save_json(settings, configPath);

	obs_data_release(settings);
	bfree(configPath);
}

QString load_access_token()
{
	char *configPath = obs_module_config_path("settings.json");
	if (!configPath)
		return {};

	obs_data_t *settings = obs_data_create_from_json_file_safe(configPath, "bak");
	if (!settings) {
		bfree(configPath);
		return {};
	}

	QString result = QString::fromUtf8(obs_data_get_string(settings, "google_access_token"));

	obs_data_release(settings);
	bfree(configPath);

	return result;
}

void save_drive_folder_name(const QString &folderName)
{
	char *configDir = obs_module_config_path("");
	if (configDir) {
		os_mkdirs(configDir);
		bfree(configDir);
	}

	char *configPath = obs_module_config_path("settings.json");
	if (!configPath)
		return;

	obs_data_t *settings = obs_data_create_from_json_file_safe(configPath, "bak");
	if (!settings)
		settings = obs_data_create();

	obs_data_set_string(settings, "drive_folder_name", folderName.toUtf8().constData());

	obs_data_save_json(settings, configPath);

	obs_data_release(settings);
	bfree(configPath);
}

QString load_drive_folder_name()
{
	char *configPath = obs_module_config_path("settings.json");
	if (!configPath)
		return {};

	obs_data_t *settings = obs_data_create_from_json_file_safe(configPath, "bak");
	if (!settings) {
		bfree(configPath);
		return {};
	}

	QString result = QString::fromUtf8(obs_data_get_string(settings, "drive_folder_name"));

	obs_data_release(settings);
	bfree(configPath);

	return result;
}
