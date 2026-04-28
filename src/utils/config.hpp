#pragma once

#include <QString>

void save_access_token(const QString &accessToken);
QString load_access_token();

void save_drive_folder_name(const QString &folderName);
QString load_drive_folder_name();
