#ifdef __cplusplus
extern "C" {
#endif

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#ifdef __cplusplus
}
#endif

#include <QString>

void save_settings(const QString &apiKey)
{
	obs_log(LOG_INFO, "Salvando config");

	char *configDir = obs_module_config_path("");
	if (configDir) {
		os_mkdirs(configDir);
		bfree(configDir);
	}

	char *configPath = obs_module_config_path("settings.json");
	if (!configPath) {
		obs_log(LOG_ERROR, "Falha ao obter caminho de config");
		return;
	}

	obs_data_t *settings = obs_data_create();

	QByteArray apiKeyBytes = apiKey.toUtf8();

	obs_data_set_string(settings, "drive_api_key", apiKeyBytes.constData());

	bool saved = obs_data_save_json(settings, configPath);

	obs_log(LOG_INFO, "Config path: %s | saved: %s", configPath, saved ? "true" : "false");

	obs_data_release(settings);
	bfree(configPath);
}

QString load_settings()
{
	char *configPath = obs_module_config_path("settings.json");

	if (!configPath) {
		obs_log(LOG_ERROR, "Falha ao obter caminho de config");
		return {};
	}

	obs_log(LOG_INFO, "Carregando config de: %s", configPath);

	obs_data_t *settings = obs_data_create_from_json_file_safe(configPath, "bak");

	const char *apiKey = obs_data_get_string(settings, "drive_api_key");

	QString result = QString::fromUtf8(apiKey);

	obs_log(LOG_INFO, "API Key carregada? %s", result.isEmpty() ? "false" : "true");

	obs_data_release(settings);
	bfree(configPath);

	return result;
}