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

#include <QString>
#include <QWidget>

OBS_DECLARE_MODULE()

static bool curlInitialized = false;

static void on_frontend_event(enum obs_frontend_event event, void *private_data)
{
    UNUSED_PARAMETER(private_data);

    QWidget *parent = (QWidget *)obs_frontend_get_main_window();

    switch (event) {
        case OBS_FRONTEND_EVENT_RECORDING_STARTED:
            obs_log(LOG_INFO, "Recording started. Checking Google OAuth token.");

            ensure_google_access_token(parent);
            break;

        case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
            open_confirm_dialog(private_data);
            break;

        default:
            break;
    }
}

bool obs_module_load(void)
{
    CURLcode globalResult = curl_global_init(CURL_GLOBAL_DEFAULT);

    if (globalResult != CURLE_OK) {
        obs_log(
            LOG_ERROR,
            "curl_global_init failed: %s",
            curl_easy_strerror(globalResult)
        );

        return false;
    }

    curlInitialized = true;

    obs_frontend_add_tools_menu_item(
        "Clip Cropper Settings",
        open_settings,
        nullptr
    );

    obs_frontend_add_event_callback(on_frontend_event, nullptr);

    const QString savedSettings = load_settings();

    if (savedSettings.isEmpty()) {
        obs_log(LOG_INFO, "Clip Cropper loaded with no Google access token");
    } else {
        obs_log(LOG_INFO, "Clip Cropper loaded with saved Google access token");
    }

    obs_log(
        LOG_INFO,
        "plugin loaded successfully (version %s)",
        PLUGIN_VERSION
    );

    return true;
}

void obs_module_unload(void)
{
    save_settings(nullptr);
    obs_frontend_remove_event_callback(on_frontend_event, nullptr);

    if (curlInitialized) {
        curl_global_cleanup();
        curlInitialized = false;
    }

    obs_log(LOG_INFO, "plugin unloaded");
}