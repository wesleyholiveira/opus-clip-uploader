#include "ui/ui-common.hpp"

#include <obs-module.h>

#include <utils/config.hpp>

#include <QCloseEvent>
#include <QDialog>
#include <QEvent>
#include <QKeyEvent>
#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QPointer>
#include <QWidget>

#include <cmath>
#include <memory>
#include <functional>
#include <utility>

namespace {
using ObsCharPtr = std::unique_ptr<char, decltype(&bfree)>;

QString obs_module_data_file_path(const QString &relativePath)
{
	const QByteArray relativePathBytes = relativePath.toUtf8();
	ObsCharPtr modulePath(obs_module_file(relativePathBytes.constData()), bfree);

	if (!modulePath || !modulePath.get() || modulePath.get()[0] == '\0')
		return {};

	return QDir::fromNativeSeparators(QString::fromUtf8(modulePath.get()));
}

QString legacy_app_data_plugin_dir()
{
	const QString appData = QString::fromLocal8Bit(qgetenv("APPDATA"));

	if (!appData.trimmed().isEmpty())
		return QDir::fromNativeSeparators(appData) + QStringLiteral("/obs-studio/plugins/clip-cropper");

	return QDir::homePath() + QStringLiteral("/AppData/Roaming/obs-studio/plugins/clip-cropper");
}
} // namespace

namespace {
constexpr const char *PROGRESS_FINISHED_PROPERTY = "clipCropperProgressFinished";

class ProgressWindowCancelFilter final : public QObject {
public:
	ProgressWindowCancelFilter(QDialog *dialog, std::function<void()> cancelHandler)
		: QObject(dialog),
		  dialog_(dialog),
		  cancelHandler_(std::move(cancelHandler))
	{
	}

	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (!dialog_ || watched != dialog_)
			return QObject::eventFilter(watched, event);

		if (dialog_->property(PROGRESS_FINISHED_PROPERTY).toBool())
			return QObject::eventFilter(watched, event);

		if (event->type() == QEvent::Close) {
			requestCancel();
			event->ignore();
			return true;
		}

		if (event->type() == QEvent::KeyPress) {
			auto *keyEvent = static_cast<QKeyEvent *>(event);
			if (keyEvent->key() == Qt::Key_Escape) {
				requestCancel();
				event->ignore();
				return true;
			}
		}

		return QObject::eventFilter(watched, event);
	}

private:
	void requestCancel()
	{
		if (cancelHandler_)
			cancelHandler_();
	}

	QPointer<QDialog> dialog_;
	std::function<void()> cancelHandler_;
};
} // namespace

BackgroundProgressDialog::BackgroundProgressDialog(QWidget *parent) : QDialog(parent) {}

void BackgroundProgressDialog::setCloseCancelHandler(std::function<void()> handler)
{
	closeCancelHandler_ = std::move(handler);
}

void BackgroundProgressDialog::setCloseCancelEnabled(bool enabled)
{
	closeCancelEnabled_ = enabled;
}

void BackgroundProgressDialog::reject()
{
	requestCloseCancel();
}

void BackgroundProgressDialog::closeEvent(QCloseEvent *event)
{
	if (!property(PROGRESS_FINISHED_PROPERTY).toBool() && closeCancelEnabled_ && closeCancelHandler_) {
		requestCloseCancel();
		event->ignore();
		return;
	}

	QDialog::closeEvent(event);
}

void BackgroundProgressDialog::requestCloseCancel()
{
	if (property(PROGRESS_FINISHED_PROPERTY).toBool() || !closeCancelEnabled_) {
		QDialog::reject();
		return;
	}

	if (closeCancelHandler_) {
		closeCancelHandler_();
		return;
	}

	QDialog::reject();
}

QString obsText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

const QString &clipCropperTitle()
{
	static const QString title(QStringLiteral("Clip Cropper"));
	return title;
}

QString format_duration_seconds(double durationSeconds)
{
	if (!std::isfinite(durationSeconds) || durationSeconds <= 0.0)
		durationSeconds = 0.0;

	const qint64 totalSeconds = static_cast<qint64>(std::round(durationSeconds));
	const qint64 hours = totalSeconds / 3600;
	const qint64 minutes = (totalSeconds % 3600) / 60;
	const qint64 seconds = totalSeconds % 60;

	return QStringLiteral("%1:%2:%3")
		.arg(hours, 2, 10, QChar('0'))
		.arg(minutes, 2, 10, QChar('0'))
		.arg(seconds, 2, 10, QChar('0'));
}

int estimate_opus_credits(double durationSeconds)
{
	if (!std::isfinite(durationSeconds) || durationSeconds <= 0.0)
		return 0;

	return static_cast<long long>(std::ceil(durationSeconds / 60.0));
}

QString get_opus_api_key()
{
	return PluginConfig::getValue("opus_api_key").trimmed();
}

QString get_openai_api_key()
{
	return PluginConfig::getValue("openai_api_key").trimmed();
}

QString get_openai_model()
{
	QString model = PluginConfig::getValue("openai_model", OPENAI_MODEL_DISABLED).trimmed();
	if (model.isEmpty())
		model = QStringLiteral("disabled");

	if (model != QStringLiteral("disabled") && !configured_whisper_model_exists()) {
		blog(LOG_WARNING, "[clip-cropper] Whisper model was not found. Disabling OpenAI model setting.");
		PluginConfig::setValue("openai_model", OPENAI_MODEL_DISABLED);
		return QStringLiteral("disabled");
	}

	return model;
}

QStringList whisper_model_search_paths(const QString &modelFile)
{
	QString normalizedModelFile = modelFile.trimmed();
	if (normalizedModelFile.isEmpty())
		normalizedModelFile = QStringLiteral("ggml-base.bin");

	QStringList candidates;

	if (QFileInfo(normalizedModelFile).isAbsolute()) {
		candidates.append(QDir::fromNativeSeparators(normalizedModelFile));
		return candidates;
	}

	const QString moduleDataModel = obs_module_data_file_path(QStringLiteral("models/%1").arg(normalizedModelFile));
	if (!moduleDataModel.trimmed().isEmpty())
		candidates.append(moduleDataModel);

	const QString legacyPluginDir = legacy_app_data_plugin_dir();
	if (!legacyPluginDir.trimmed().isEmpty()) {
		candidates.append(QDir(legacyPluginDir).filePath(QStringLiteral("models/%1").arg(normalizedModelFile)));
		candidates.append(
			QDir(legacyPluginDir).filePath(QStringLiteral("data/models/%1").arg(normalizedModelFile)));
	}

	const QString localModelsDir = QDir::current().filePath(QStringLiteral("models/%1").arg(normalizedModelFile));
	candidates.append(localModelsDir);

	candidates.removeDuplicates();
	return candidates;
}

bool whisper_model_exists(const QString &modelFile)
{
	QString normalizedModelFile = modelFile.trimmed();
	if (normalizedModelFile.isEmpty())
		normalizedModelFile = QStringLiteral("ggml-base.bin");

	const QStringList candidates = whisper_model_search_paths(normalizedModelFile);
	for (const QString &candidate : candidates) {
		if (QFileInfo(candidate).isFile())
			return true;
	}

	return false;
}

bool configured_whisper_model_exists()
{
	const QString legacyModelPath = PluginConfig::getValue("whisper_model_path").trimmed();
	if (!legacyModelPath.isEmpty() && QFileInfo(legacyModelPath).isFile())
		return true;

	QString modelFile = PluginConfig::getValue("whisper_model_file", "ggml-base.bin").trimmed();
	if (modelFile.isEmpty())
		modelFile = QStringLiteral("ggml-base.bin");

	return whisper_model_exists(modelFile);
}

QString resolve_whisper_model_path()
{
	QString modelFile = PluginConfig::getValue("whisper_model_file", "ggml-base.bin").trimmed();
	if (modelFile.isEmpty())
		modelFile = QStringLiteral("ggml-base.bin");

	const QString legacyModelPath = PluginConfig::getValue("whisper_model_path").trimmed();
	if (!legacyModelPath.isEmpty() && QFileInfo::exists(legacyModelPath))
		return QDir::fromNativeSeparators(legacyModelPath);

	const QStringList candidates = whisper_model_search_paths(modelFile);
	for (const QString &candidate : candidates) {
		if (QFileInfo(candidate).isFile())
			return candidate;
	}

	if (!candidates.isEmpty())
		return candidates.first();

	return modelFile;
}

void configure_background_progress_window(QWidget *window, bool allowClose)
{
	if (!window)
		return;

	/*
	 * Progress windows must behave like real top-level windows.
	 *
	 * Keeping OBS as the native/Qt parent makes Windows treat the dialog as an owned
	 * child window. Owned windows can be minimized without getting their own taskbar
	 * entry, so the user has no obvious way to restore the progress dialog while the
	 * background operation is still running.
	 *
	 * Detaching the QWidget parent here keeps the QObject lifetime explicit
	 * (callers already close/deleteLater these dialogs) and gives the window its own
	 * taskbar/Alt+Tab entry on desktop window managers that support it.
	 */
	window->setParent(nullptr);
	window->setAttribute(Qt::WA_QuitOnClose, false);
	window->setWindowModality(Qt::NonModal);

	Qt::WindowFlags flags = Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
				Qt::WindowMinimizeButtonHint;
	if (allowClose)
		flags |= Qt::WindowCloseButtonHint;

	window->setWindowFlags(flags);
}

void bind_progress_window_cancel(QDialog *dialog, std::function<void()> cancelHandler)
{
	if (!dialog || !cancelHandler)
		return;

	dialog->setProperty(PROGRESS_FINISHED_PROPERTY, false);

	if (auto *backgroundDialog = dynamic_cast<BackgroundProgressDialog *>(dialog)) {
		backgroundDialog->setCloseCancelEnabled(true);
		backgroundDialog->setCloseCancelHandler(std::move(cancelHandler));
		return;
	}

	dialog->installEventFilter(new ProgressWindowCancelFilter(dialog, std::move(cancelHandler)));
}

void mark_progress_window_finished(QDialog *dialog)
{
	if (!dialog)
		return;

	dialog->setProperty(PROGRESS_FINISHED_PROPERTY, true);

	if (auto *backgroundDialog = dynamic_cast<BackgroundProgressDialog *>(dialog))
		backgroundDialog->setCloseCancelEnabled(false);
}
