#ifdef __cplusplus
extern "C" {
#endif

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#ifdef __cplusplus
}
#endif

#include <ui/ui.hpp>
#include <ui/ui-common.hpp>
#include <transcription/realtime-transcription-service.hpp>
#include <utils/config.hpp>

#include <memory>

#include <QAction>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QString>
#include <QStringList>
#include <QWidget>

void open_settings(void *private_data);
void open_confirm_dialog(void *private_data);
void open_video_editor(void *private_data);
void ensure_opus_api_key(QWidget *parent);
void set_pending_recording_paths(const QStringList &paths);

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("clip-cropper", "en-US")

static QDateTime recordingStartedAt;
static QDateTime recordingStoppedAt;
static QString recordingDirectory;

static void add_clip_cropper_qt_plugin_path()
{
	const QString obsPluginDir =
		QDir::fromNativeSeparators(QCoreApplication::applicationDirPath() + "/../../obs-plugins/64bit");

	QCoreApplication::addLibraryPath(obsPluginDir);

	blog(LOG_INFO, "[clip-cropper] Added Qt plugin path: %s", obsPluginDir.toUtf8().constData());

	for (const QString &path : QCoreApplication::libraryPaths()) {
		blog(LOG_INFO, "[clip-cropper] Qt library path: %s", path.toUtf8().constData());
	}
}

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
		const QDateTime modified = file.lastModified();

		if (modified >= start && modified <= end)
			result.append(file.absoluteFilePath());
	}

	result.removeDuplicates();
	result.sort();

	obs_log(LOG_INFO, "[clip-cropper] Recording scan in %s found %d new file(s).",
		directoryPath.toUtf8().constData(), result.size());

	return result;
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

static QStringList resolve_recording_files_for_upload()
{
	QString dir = recordingDirectory;

	if (dir.isEmpty())
		dir = resolve_recording_directory();

	if (dir.isEmpty())
		return {};

	QStringList paths = video_files_modified_between(dir, recordingStartedAt, recordingStoppedAt);

	if (!paths.isEmpty())
		return paths;

	const QString lastRecording = obs_last_recording_file();

	if (!lastRecording.isEmpty())
		return {lastRecording};

	return {};
}

static void reset_recording_state()
{
	recordingDirectory = resolve_recording_directory();
	recordingStartedAt = {};
	recordingStoppedAt = {};
}

static QWidget *main_window()
{
	return reinterpret_cast<QWidget *>(obs_frontend_get_main_window());
}

static bool is_openai_model_enabled()
{
	const QString model = get_openai_model();
	return !model.isEmpty() && model != QStringLiteral("disabled");
}

static void log_on_demand_transcription_status()
{
	if (!is_openai_model_enabled()) {
		obs_log(LOG_INFO,
			"[clip-cropper] OpenAI model is disabled. Audio transcription will be skipped on review.");
		return;
	}

	obs_log(LOG_INFO,
		"[clip-cropper] OpenAI model is enabled. Audio transcription will run from the video file via ffmpeg when the review flow starts.");
}

static void ensure_opus_api_key_on_ui_thread()
{
	QWidget *parent = main_window();

	if (!parent) {
		obs_log(LOG_ERROR, "[clip-cropper] Main window is null. Cannot validate Opus Clip API key.");
		return;
	}

	QMetaObject::invokeMethod(
		parent,
		[parent]() {
			obs_log(LOG_INFO, "[clip-cropper] Checking Opus Clip API key on UI thread.");
			ensure_opus_api_key(parent);
		},
		Qt::QueuedConnection);
}

static void open_confirm_dialog_on_ui_thread()
{
	QWidget *parent = main_window();

	if (!parent) {
		obs_log(LOG_ERROR, "[clip-cropper] Main window is null. Cannot open upload dialog.");
		return;
	}

	QMetaObject::invokeMethod(
		parent,
		[]() {
			obs_log(LOG_INFO, "[clip-cropper] Opening upload confirm dialog.");
			open_confirm_dialog(nullptr);
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
		log_on_demand_transcription_status();

		ensure_opus_api_key_on_ui_thread();
		break;

	case OBS_FRONTEND_EVENT_RECORDING_STOPPED: {
		obs_log(LOG_INFO, "[clip-cropper] Recording stopped.");

		recordingStoppedAt = QDateTime::currentDateTime().addSecs(10);

		const QStringList paths = resolve_recording_files_for_upload();

		if (paths.isEmpty()) {
			obs_log(LOG_WARNING,
				"[clip-cropper] No valid recording files found. Upload confirm dialog will not be shown.");
			break;
		}

		set_pending_recording_paths(paths);
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

	obs_log(LOG_INFO,
		"[clip-cropper] Vertical recording started. Ignoring to avoid duplicate normal recording flow.");
}

static void clip_cropper_vertical_recording_stopped(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(cd);

	obs_log(LOG_INFO, "[clip-cropper] Vertical recording stopped. Ignoring dialog open to avoid duplicate prompt.");
}

static QString obs_text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

static void show_missing_whisper_model_warning_on_ui_thread()
{
	const QString modelFile = PluginConfig::getValue("whisper_model_file", "ggml-base.bin").trimmed();
	const QString displayModel = modelFile.isEmpty() ? QStringLiteral("ggml-base.bin") : modelFile;
	const QString modelPath = resolve_whisper_model_path();

	blog(LOG_INFO, "[clip-cropper] Validating selected Whisper model at startup: file=%s path=%s",
	     displayModel.toUtf8().constData(), modelPath.toUtf8().constData());

	if (!modelPath.trimmed().isEmpty() && QFileInfo(modelPath).isFile()) {
		blog(LOG_INFO, "[clip-cropper] Selected Whisper model exists: %s", modelPath.toUtf8().constData());
		return;
	}

	blog(LOG_ERROR, "[clip-cropper] Selected Whisper model was not found: %s", modelPath.toUtf8().constData());

	const QStringList searchPaths = whisper_model_search_paths(displayModel);
	for (const QString &path : searchPaths) {
		blog(LOG_INFO, "[clip-cropper] Whisper model search path: %s", path.toUtf8().constData());
	}

	QWidget *parent = reinterpret_cast<QWidget *>(obs_frontend_get_main_window());
	const QString title = QStringLiteral("Clip Cropper - ") + obs_text("Dialog.MissingWhisperModelTitle");

	QMessageBox::critical(
		parent, title,
		obs_text("Message.MissingWhisperModel").arg(displayModel, QDir::toNativeSeparators(modelPath)));
}

static QString normalized_menu_title(const QString &title)
{
	QString result = title;
	result.remove('&');
	return result.trimmed().toLower();
}

static QMenu *find_tools_menu(QWidget *mainWindow)
{
	if (!mainWindow)
		return nullptr;

	QMenuBar *menuBar = mainWindow->findChild<QMenuBar *>();
	if (!menuBar)
		return nullptr;

	for (QAction *action : menuBar->actions()) {
		QMenu *menu = action ? action->menu() : nullptr;
		if (!menu)
			continue;

		const QString objectName = menu->objectName().toLower();
		const QString title = normalized_menu_title(menu->title());

		if (objectName.contains("tools") || title == "tools" || title == "ferramentas")
			return menu;
	}

	return nullptr;
}

static void add_clip_cropper_tools_submenu_on_ui_thread()
{
	QWidget *mainWindow = main_window();
	QMenu *toolsMenu = find_tools_menu(mainWindow);

	if (!toolsMenu) {
		obs_log(LOG_WARNING, "[clip-cropper] Tools menu not found. Falling back to flat Tools menu items.");
		obs_frontend_add_tools_menu_item("Clip Cropper - Settings", open_settings, nullptr);
		obs_frontend_add_tools_menu_item("Clip Cropper - Video editor", open_video_editor, nullptr);
		return;
	}

	QMenu *clipCropperMenu = nullptr;
	for (QAction *action : toolsMenu->actions()) {
		QMenu *menu = action ? action->menu() : nullptr;
		if (menu && menu->objectName() == QStringLiteral("clipCropperToolsMenu")) {
			clipCropperMenu = menu;
			break;
		}
	}

	if (!clipCropperMenu) {
		clipCropperMenu = new QMenu(QStringLiteral("Clip Cropper"), toolsMenu);
		clipCropperMenu->setObjectName(QStringLiteral("clipCropperToolsMenu"));
		toolsMenu->addMenu(clipCropperMenu);
	}

	clipCropperMenu->clear();

	QAction *settingsAction = clipCropperMenu->addAction(obs_text("Menu.Settings"));
	QObject::connect(settingsAction, &QAction::triggered, []() { open_settings(nullptr); });

	QAction *videoEditorAction = clipCropperMenu->addAction(obs_text("Menu.VideoEditor"));
	QObject::connect(videoEditorAction, &QAction::triggered, []() { open_video_editor(nullptr); });
}

bool obs_module_load(void)
{
	add_clip_cropper_qt_plugin_path();

	proc_handler_t *ph = obs_get_proc_handler();

	proc_handler_add(ph, "void clip_cropper_vertical_recording_started()", clip_cropper_vertical_recording_started,
			 nullptr);

	proc_handler_add(ph, "void clip_cropper_vertical_recording_stopped()", clip_cropper_vertical_recording_stopped,
			 nullptr);

	QMetaObject::invokeMethod(
		QCoreApplication::instance(), []() { add_clip_cropper_tools_submenu_on_ui_thread(); },
		Qt::QueuedConnection);
	QMetaObject::invokeMethod(
		QCoreApplication::instance(), []() { show_missing_whisper_model_warning_on_ui_thread(); },
		Qt::QueuedConnection);

	obs_frontend_add_event_callback(on_frontend_event, nullptr);

	obs_log(LOG_INFO, "plugin loaded (version %s)", PLUGIN_VERSION);

	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	obs_log(LOG_INFO, "plugin unloaded");
}