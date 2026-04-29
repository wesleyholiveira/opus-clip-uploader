#pragma once

#include <QString>
#include <QWidget>
#include <QStringList>

void open_settings(void *private_data);
void open_confirm_dialog(void *private_data);
void ensure_google_access_token(QWidget *parent);

void set_pending_recording_paths(const QStringList &paths);
void clear_pending_recording_paths();
