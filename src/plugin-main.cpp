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

#include <QMetaObject>
#include <QString>
#include <QWidget>

OBS_DECLARE_MODULE()

static bool curlInitialized = false;
static bool uploadDialogOpen = false;

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
		obs_log(LOG_ERROR, "[clip-cropper] Main window is null. Cannot open confirm dialog.");
		return;
	}

	QMetaObject::invokeMethod(
		parent,
		[]() {
			if (uploadDialogOpen) {
				obs_log(LOG_INFO, "[clip-cropper] Upload dialog already open. Ignoring.");
				return;
			}

			uploadDialogOpen = true;
			obs_log(LOG_INFO, "[clip-cropper] Opening confirm dialog on UI thread.");
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
		obs_log(LOG_INFO, "[clip-cropper] Native OBS recording started.");
		ensure_google_oauth_on_ui_thread();
		break;

	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		obs_log(LOG_INFO, "[clip-cropper] Native OBS recording stopped.");
		open_confirm_dialog_on_ui_thread();
		break;

	default:
		break;
	}
}

static void clip_cropper_vertical_recording_started(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(cd);

	obs_log(LOG_INFO, "[clip-cropper] PROC received: vertical recording started.");
	ensure_google_oauth_on_ui_thread();
}

static void clip_cropper_vertical_recording_stopped(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);

	const char *path = calldata_string(cd, "path");

	if (path && path[0] != '\0') {
		set_pending_recording_path(QString::fromUtf8(path));
		obs_log(LOG_INFO, "[clip-cropper] Vertical recording path received: %s", path);
	} else {
		obs_log(LOG_WARNING, "[clip-cropper] Vertical recording stopped without path.");
	}

	obs_log(LOG_INFO, "[clip-cropper] PROC received: vertical recording stopped.");
	open_confirm_dialog_on_ui_thread();
}

bool obs_module_load(void)
{
	CURLcode globalResult = curl_global_init(CURL_GLOBAL_DEFAULT);

	if (globalResult != CURLE_OK) {
		obs_log(LOG_ERROR, "curl_global_init failed: %s", curl_easy_strerror(globalResult));
		return false;
	}

	curlInitialized = true;

	proc_handler_t *ph = obs_get_proc_handler();

	proc_handler_add(ph, "void clip_cropper_vertical_recording_started()", clip_cropper_vertical_recording_started,
			 nullptr);

	proc_handler_add(ph, "void clip_cropper_vertical_recording_stopped()", clip_cropper_vertical_recording_stopped,
			 nullptr);

	obs_log(LOG_INFO, "[clip-cropper] Registered proc handlers for Vertical Canvas.");

	obs_frontend_add_tools_menu_item("Clip Cropper Settings", open_settings, nullptr);
	obs_frontend_add_event_callback(on_frontend_event, nullptr);

	const QString savedSettings = PluginConfig::getValue("google_access_token");

	if (savedSettings.isEmpty()) {
		obs_log(LOG_INFO, "Clip Cropper loaded with no Google access token");
	} else {
		obs_log(LOG_INFO, "Clip Cropper loaded with saved Google access token");
	}

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);

	return true;
}

void obs_module_unload(void)
{
	PluginConfig::setValue("google_access_token", nullptr);
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);

	if (curlInitialized) {
		curl_global_cleanup();
		curlInitialized = false;
	}

	obs_log(LOG_INFO, "plugin unloaded");
}
