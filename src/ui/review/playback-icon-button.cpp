#include "ui/review/playback-icon-button.hpp"

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

#include <QMouseEvent>
#include <QPainter>
#include <QSizePolicy>

namespace {
QString obsText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}
} // namespace

PlaybackIconButton::PlaybackIconButton(QWidget *parent) : QWidget(parent)
{
	setFixedSize(28, 28);
	setCursor(Qt::PointingHandCursor);
	setToolTip(obsText("Tooltip.Play"));
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void PlaybackIconButton::setPlaying(bool value)
{
	playing = value;
	setToolTip(playing ? obsText("Tooltip.Pause") : obsText("Tooltip.Play"));
	update();
}

void PlaybackIconButton::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(Qt::NoPen);
	painter.setBrush(QColor(230, 230, 230));

	if (playing) {
		painter.drawRoundedRect(QRectF(8, 6, 4, 16), 1.5, 1.5);
		painter.drawRoundedRect(QRectF(16, 6, 4, 16), 1.5, 1.5);
	} else {
		QPolygonF triangle;
		triangle << QPointF(10, 6) << QPointF(22, 14) << QPointF(10, 22);
		painter.drawPolygon(triangle);
	}
}

void PlaybackIconButton::mousePressEvent(QMouseEvent *event)
{
	if (event)
		event->accept();
}

void PlaybackIconButton::mouseReleaseEvent(QMouseEvent *event)
{
	if (!event)
		return;
	if (rect().contains(event->position().toPoint()) && clicked)
		clicked();
	event->accept();
}
