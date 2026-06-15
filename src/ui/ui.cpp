#include "ui/ui.hpp"

#include "ui/ui-actions.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <QStringList>

static QStringList pendingRecordingPaths;

void set_pending_recording_paths(const QStringList &paths)
{
	pendingRecordingPaths = paths;

	for (const QString &path : pendingRecordingPaths) {
		obs_log(LOG_INFO, "Pending recording path set: %s", path.toUtf8().constData());
	}
}

void clear_pending_recording_paths()
{
	pendingRecordingPaths.clear();
	obs_log(LOG_INFO, "Pending recording paths cleared");
}

QStringList get_recording_paths_for_upload()
{
	return pendingRecordingPaths;
}

QStringList take_recording_paths_for_upload()
{
	QStringList paths = pendingRecordingPaths;
	pendingRecordingPaths.clear();

	if (!paths.isEmpty())
		obs_log(LOG_INFO, "Pending recording paths consumed");

	return paths;
}

void open_settings(void *private_data)
{
	open_settings_impl(private_data);
}

void open_confirm_dialog(void *private_data)
{
	open_confirm_dialog_impl(private_data);
}

void open_video_editor(void *private_data)
{
	open_video_editor_impl(private_data);
}

void ensure_opus_api_key(QWidget *parent)
{
	ensure_opus_api_key_impl(parent);
}
