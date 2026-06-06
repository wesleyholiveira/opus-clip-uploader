#pragma once

#include <QString>
#include <QWidget>
#include <QStringList>

void open_settings(void *private_data);
void open_confirm_dialog(void *private_data);
<<<<<<< HEAD
void ensure_opus_api_key(QWidget *parent);
=======
void ensure_google_access_token(QWidget *parent);
>>>>>>> 0eeb6e7bf9a5b9468c9ebe555467b37448358893

void set_pending_recording_paths(const QStringList &paths);
void clear_pending_recording_paths();
