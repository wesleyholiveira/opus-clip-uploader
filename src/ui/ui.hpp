#pragma once

#include <QStringList>

class QWidget;

void open_settings(void *private_data);
void open_upload_review_flow(void *private_data);
void open_video_review(void *private_data);
void ensure_opus_api_key(QWidget *parent);

void set_pending_recording_paths(const QStringList &paths);
void clear_pending_recording_paths();
QStringList get_recording_paths_for_upload();
QStringList take_recording_paths_for_upload();
