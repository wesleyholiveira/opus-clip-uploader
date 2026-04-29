#ifdef __cplusplus
extern "C" {
#endif

#include <curl/curl.h>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#ifdef __cplusplus
}
#endif

#include <ui/ui.hpp>
#include <utils/config.hpp>

#include <memory>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QString>
#include <QStringList>
#include <QWidget>

OBS_DECLARE_MODULE()

static bool curlInitialized = false;
static bool uploadDialogOpen = false;

static QDateTime recordingStartedAt;
static QDateTime recordingStoppedAt;
static QString recordingDirectory;

static QString obs_current_recording_output_path()
{
	using ObsCharPtr = std::unique_ptr<char, decltype(&bfree)>;
	ObsCharPtr safePath(obs_frontend_get_current_record_output_path(), bfree);

	if (!safePath || !safePath.get() || safePath.get()[0] == '\0')
		return {};

	return QString::fromUtf8(safePath.get());
}

static QString obs_last_recording_file()
{
	using ObsCharPtr = std::unique_ptr<char, decltype(&bfree)>;
	ObsCharPtr safePath(obs_frontend_get_last_recording(), bfree);

	if (!safePath || !safePath.get() || safePath.get()[0] == '\0')
		return {};

	const QString path = QString::fromUtf8(safePath.get());
	const QFileInfo info(path);

	if (info.isFile())
		return info.absoluteFilePath();

	obs_log(LOG_WARNING, "[clip-cropper] OBS last recording is not a valid file: %s", path.toUtf8().constData());

	return {};
}

static QString resolve_recording_directory()
{
	const QString raw = obs_current_recording_output_path();
	const QFileInfo rawInfo(raw);

	if (rawInfo.isDir())
		return rawInfo.absoluteFilePath();

	if (rawInfo.isFile())
		return rawInfo.absolutePath();

	const QString lastRecording = obs_last_recording_file();

	if (!lastRecording.isEmpty())
		return QFileInfo(lastRecording).absolutePath();

	return {};
}

static QStringList video_files_modified_between(const QString &directoryPath, const QDateTime &start,
						const QDateTime &end)
{
	QStringList result;

	QDir dir(directoryPath);

	if (!dir.exists())
		return result;

	const QStringList filters = {"*.mp4", "*.mkv", "*.mov", "*.flv"};

	const QFileInfoList files = dir.entryInfoList(filters, QDir::Files, QDir::Name);

	for (const QFileInfo &file : files) {
		const QString absolutePath = file.absoluteFilePath();
		const QDateTime modified = file.lastModified();

		if (modified >= start && modified <= end) {
			result.append(absolutePath);
		}
	}

	result.removeDuplicates();
	result.sort();

	obs_log(LOG_INFO, "[clip-cropper] Recording scan in %s found %d new file(s).",
		directoryPath.toUtf8().constData(), result.size());

	return result;
}

static QStringList resolve_recording_files_for_upload()
{
	QString dir = recordingDirectory;

	if (dir.isEmpty()) {
		const QString lastRecording = obs_last_recording_file();

		if (!lastRecording.isEmpty())
			dir = QFileInfo(lastRecording).absolutePath();
	}

	if (dir.isEmpty())
		return {};

	QStringList paths = video_files_modified_between(dir, recordingStartedAt, recordingStoppedAt);

	if (!paths.isEmpty()) {
		obs_log(LOG_INFO, "[clip-cropper] Using %d recording file(s) from filtered scan.", paths.size());

		return paths;
	}

	const QString lastRecording = obs_last_recording_file();

	if (!lastRecording.isEmpty()) {
		obs_log(LOG_INFO, "[clip-cropper] Fallback using last recording only: %s",
			lastRecording.toUtf8().constData());

		return {lastRecording};
	}

	return {};
}

static void reset_recording_state()
{
	recordingDirectory.clear();
	recordingStartedAt = {};
	recordingStoppedAt = {};
}

static void ensure_google_oauth_on_ui_thread()
{
	QWidget *parent = reinterpret_cast<QWidget *>(obs_frontend_get_main_window());

	if (!parent) {
		obs_log(LOG_ERROR, "[clip-cropper] Main window is null. Cannot start OAuth.");
		return;
	}

	QMetaObject::invokeMethod(
		parent,
		[parent]() {
			obs_log(LOG_INFO, "[clip-cropper] Running Google OAuth on UI thread.");
			ensure_google_access_token(parent);
		},
		Qt::QueuedConnection);
}

static void open_confirm_dialog_on_ui_thread()
{
	QWidget *parent = reinterpret_cast<QWidget *>(obs_frontend_get_main_window());

	if (!parent) {
		obs_log(LOG_ERROR, "[clip-cropper] Main window is null.");
		return;
	}

	QMetaObject::invokeMethod(
		parent,
		[]() {
			if (uploadDialogOpen)
				return;

			uploadDialogOpen = true;

			open_confirm_dialog(nullptr);

			uploadDialogOpen = false;
		},
		Qt::QueuedConnection);
}

static void on_frontend_event(enum obs_frontend_event event, void *private_data)
{
	UNUSED_PARAMETER(private_data);

	switch (event) {
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		obs_log(LOG_INFO, "[clip-cropper] Recording started.");

		reset_recording_state();

		recordingStartedAt = QDateTime::currentDateTime().addSecs(-10);

		ensure_google_oauth_on_ui_thread();
		break;

	case OBS_FRONTEND_EVENT_RECORDING_STOPPED: {
		obs_log(LOG_INFO, "[clip-cropper] Recording stopped.");

		recordingStoppedAt = QDateTime::currentDateTime().addSecs(10);

		const QStringList paths = resolve_recording_files_for_upload();

		if (!paths.isEmpty()) {
			set_pending_recording_paths(paths);
		} else {
			obs_log(LOG_WARNING, "[clip-cropper] No valid files found.");
		}

		open_confirm_dialog_on_ui_thread();
		break;
	}

	default:
		break;
	}
}

static void clip_cropper_vertical_recording_started(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(cd);

	obs_log(LOG_INFO, "[clip-cropper] Vertical recording started.");

	reset_recording_state();

	recordingStartedAt = QDateTime::currentDateTime().addSecs(-10);
	ensure_google_oauth_on_ui_thread();
}

static void clip_cropper_vertical_recording_stopped(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);

	recordingStoppedAt = QDateTime::currentDateTime().addSecs(10);

	const char *path = calldata_string(cd, "path");

	if (path && path[0] != '\0') {
		const QString receivedPath = QString::fromUtf8(path);
		const QFileInfo info(receivedPath);

		if (info.isFile()) {
			set_pending_recording_paths({info.absoluteFilePath()});
		} else if (info.isDir()) {
			QStringList files = video_files_modified_between(info.absoluteFilePath(), recordingStartedAt,
									 recordingStoppedAt);

			if (!files.isEmpty()) {
				set_pending_recording_paths(files);
			}
		}
	}

	open_confirm_dialog_on_ui_thread();
}

bool obs_module_load(void)
{
	const CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);

	if (result != CURLE_OK) {
		obs_log(LOG_ERROR, "curl init failed: %s", curl_easy_strerror(result));
		return false;
	}

	curlInitialized = true;

	proc_handler_t *ph = obs_get_proc_handler();

	proc_handler_add(ph, "void clip_cropper_vertical_recording_started()", clip_cropper_vertical_recording_started,
			 nullptr);

	proc_handler_add(ph, "void clip_cropper_vertical_recording_stopped()", clip_cropper_vertical_recording_stopped,
			 nullptr);

	obs_frontend_add_tools_menu_item("Clip Cropper Settings", open_settings, nullptr);
	obs_frontend_add_event_callback(on_frontend_event, nullptr);

	obs_log(LOG_INFO, "plugin loaded (version %s)", PLUGIN_VERSION);

	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);

	if (curlInitialized) {
		curl_global_cleanup();
		curlInitialized = false;
	}

	obs_log(LOG_INFO, "plugin unloaded");
}
