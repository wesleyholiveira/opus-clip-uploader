#pragma once

#include <QString>

class PluginConfig {
public:
	static void setValue(const QString &key, const QString &value);
	static QString getValue(const QString &key, const QString &defaultValue = {});

	static void removeValue(const QString &key);

private:
	static char *getConfigPath();
	static obs_data_t *loadConfig();
	static bool ensureConfigDir();
};
