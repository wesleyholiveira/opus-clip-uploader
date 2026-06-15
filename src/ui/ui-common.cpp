#include "ui/ui-common.hpp"

#include <obs-module.h>

#include <utils/config.hpp>

#include <QCloseEvent>
#include <QDialog>
#include <QEvent>
#include <QKeyEvent>
#include <QObject>
#include <QPointer>
#include <QWidget>

#include <functional>
#include <utility>


namespace
{
constexpr const char *PROGRESS_FINISHED_PROPERTY = "clipCropperProgressFinished";

class ProgressWindowCancelFilter final : public QObject
{
public:
	ProgressWindowCancelFilter(QDialog *dialog, std::function<void()> cancelHandler)
		: QObject(dialog), dialog_(dialog), cancelHandler_(std::move(cancelHandler))
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
}

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
	return PluginConfig::getValue("openai_model", OPENAI_MODEL_DISABLED).trimmed();
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
