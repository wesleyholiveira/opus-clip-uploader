#pragma once

#include <QWidget>

#include <functional>

class QMouseEvent;
class QPaintEvent;

class PlaybackIconButton final : public QWidget {
public:
	explicit PlaybackIconButton(QWidget *parent = nullptr);
	void setPlaying(bool value);

	std::function<void()> clicked;

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;

private:
	bool playing = false;
};
