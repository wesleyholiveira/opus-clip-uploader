#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

#include <functional>

class QCloseEvent;

class BackgroundProgressDialog final : public QDialog {
public:
	explicit BackgroundProgressDialog(QWidget *parent = nullptr);

	void setCloseCancelHandler(std::function<void()> handler);
	void setCloseCancelEnabled(bool enabled);

	void reject() override;

protected:
	void closeEvent(QCloseEvent *event) override;

private:
	void requestCloseCancel();

	std::function<void()> closeCancelHandler_;
	bool closeCancelEnabled_ = true;
};

inline constexpr const char *OPENAI_MODEL_DISABLED = "disabled";

QString obsText(const char *key);
const QString &clipCropperTitle();

QStringList get_recording_paths_for_upload();
QString get_opus_api_key();
QString get_openai_api_key();
QString get_openai_model();

void configure_background_progress_window(QWidget *window, bool allowClose);
void bind_progress_window_cancel(QDialog *dialog, std::function<void()> cancelHandler);
void mark_progress_window_finished(QDialog *dialog);
