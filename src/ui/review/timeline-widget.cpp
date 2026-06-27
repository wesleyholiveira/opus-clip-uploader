#include "ui/review/timeline-widget.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSizePolicy>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

TimelineWidget::TimelineWidget(QWidget *parent) : QWidget(parent)
{
	setMinimumHeight(38);
	setMouseTracking(true);
	setCursor(Qt::PointingHandCursor);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	seekThrottle.invalidate();
}

void TimelineWidget::setDuration(qint64 value)
{
	durationMs = std::max<qint64>(0, value);
	positionMs = std::clamp(positionMs, qint64(0), durationMs);
	resetVisibleRangeIfInvalid();
	update();
}

void TimelineWidget::setPosition(qint64 value)
{
	if (draggingPosition || draggingMarker)
		return;

	positionMs = clampPosition(value);
	update();
}

qint64 TimelineWidget::position() const
{
	return positionMs;
}

void TimelineWidget::setMarkers(const QList<qint64> &newMarkers)
{
	markers = newMarkers;
	if (selectedMarkerIndex >= markers.size())
		selectedMarkerIndex = -1;
	update();
}

void TimelineWidget::setSelectedMarkerIndex(int index)
{
	selectedMarkerIndex = index >= 0 && index < markers.size() ? index : -1;
	update();
}

void TimelineWidget::setClipRanges(const QVector<ClipDuration> &newRanges)
{
	clipRanges = newRanges;
	update();
}

void TimelineWidget::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);

	const QRectF track = trackRect();
	const qreal radius = track.height() / 2.0;

	painter.setPen(Qt::NoPen);
	painter.setBrush(QColor(65, 65, 65));
	painter.drawRoundedRect(track, radius, radius);

	const qreal progressX = xForPosition(positionMs);
	QRectF progress = track;
	progress.setRight(progressX);
	painter.setBrush(QColor(210, 50, 60));
	painter.drawRoundedRect(progress, radius, radius);

	const qint64 viewStart = visibleStart();
	const qint64 viewEnd = visibleEnd();
	const qreal scale = zoomScale();
	for (const auto &range : clipRanges) {
		const qint64 startMs = static_cast<qint64>(range.startSec * 1000.0);
		const qint64 endMs = static_cast<qint64>(range.endSec * 1000.0);
		if (durationMs <= 0 || endMs <= startMs || endMs < viewStart || startMs > viewEnd)
			continue;

		QRectF rangeRect = track.adjusted(0.0, -4.0 * scale, 0.0, 4.0 * scale);
		rangeRect.setLeft(xForPosition(std::max(startMs, viewStart)));
		rangeRect.setRight(xForPosition(std::min(endMs, viewEnd)));
		painter.setPen(QPen(QColor(255, 210, 60), 2));
		painter.setBrush(QColor(255, 193, 7, 170));
		painter.drawRoundedRect(rangeRect, rangeRect.height() / 2.0, rangeRect.height() / 2.0);
		painter.setPen(Qt::NoPen);
	}

	for (int i = 0; i < markers.size(); ++i) {
		const qint64 marker = markers[i];
		if (durationMs <= 0 || marker < viewStart || marker > viewEnd)
			continue;

		const bool selected = i == selectedMarkerIndex;
		const qreal x = xForPosition(marker);
		const qreal markerHalfWidth = selected ? 7.0 * scale : 6.0 * scale;
		const qreal markerBodyHeight = 9.0 * scale;
		const qreal markerTailHeight = 5.0 * scale;
		const qreal top = track.top() - (10.0 + 6.0 * scale);

		QPainterPath markerPath;
		markerPath.moveTo(x - markerHalfWidth, top);
		markerPath.lineTo(x + markerHalfWidth, top);
		markerPath.lineTo(x + markerHalfWidth, top + markerBodyHeight);
		markerPath.lineTo(x + 2.0 * scale, top + markerBodyHeight);
		markerPath.lineTo(x, top + markerBodyHeight + markerTailHeight);
		markerPath.lineTo(x - 2.0 * scale, top + markerBodyHeight);
		markerPath.lineTo(x - markerHalfWidth, top + markerBodyHeight);
		markerPath.closeSubpath();

		painter.setPen(QPen(selected ? QColor(255, 255, 255) : QColor(20, 20, 20), selected ? 2 : 1));
		painter.setBrush(selected ? QColor(255, 225, 90) : QColor(255, 193, 7));
		painter.drawPath(markerPath);
	}

	painter.setPen(Qt::NoPen);
	painter.setBrush(QColor(245, 245, 245));
	const bool dragging = draggingPosition || draggingMarker;
	const qreal thumbRadius = (dragging ? 8.0 : 6.0) * scale;
	painter.drawEllipse(QPointF(progressX, track.center().y()), thumbRadius, thumbRadius);
	painter.setPen(QPen(QColor(35, 35, 35), 1));
	painter.setBrush(Qt::NoBrush);
	painter.drawEllipse(QPointF(progressX, track.center().y()), thumbRadius, thumbRadius);
}

void TimelineWidget::mousePressEvent(QMouseEvent *event)
{
	const int markerIndex = markerIndexAt(event->position().x(), event->position().y());
	if (markerIndex >= 0) {
		draggingPosition = false;
		draggingMarker = true;
		selectedMarkerIndex = markerIndex;
		positionMs = markers[markerIndex];
		update();
		if (markerSelected)
			markerSelected(markerIndex);
		if (commitSeekRequested)
			commitSeekRequested(positionMs);
		event->accept();
		return;
	}

	selectedMarkerIndex = -1;
	if (markerSelected)
		markerSelected(-1);
	draggingPosition = true;
	setPositionFromMouse(event->position().x(), true);
	event->accept();
}

void TimelineWidget::mouseMoveEvent(QMouseEvent *event)
{
	if (draggingMarker) {
		setMarkerFromMouse(event->position().x(), false);
		event->accept();
		return;
	}

	if (!draggingPosition)
		return;

	setPositionFromMouse(event->position().x(), false);
	event->accept();
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent *event)
{
	if (draggingMarker) {
		setMarkerFromMouse(event->position().x(), true);
		draggingMarker = false;
		update();
		event->accept();
		return;
	}

	if (!draggingPosition) {
		QWidget::mouseReleaseEvent(event);
		return;
	}

	setPositionFromMouse(event->position().x(), true);
	draggingPosition = false;
	if (commitSeekRequested)
		commitSeekRequested(positionMs);
	update();
	event->accept();
}

void TimelineWidget::wheelEvent(QWheelEvent *event)
{
	if (!event)
		return;

	if (!(event->modifiers() & Qt::ControlModifier) || durationMs <= 0) {
		QWidget::wheelEvent(event);
		return;
	}

	const int delta = event->angleDelta().y();
	if (delta == 0) {
		QWidget::wheelEvent(event);
		return;
	}

	zoomAt(event->position().x(), delta);
	event->accept();
}

QRectF TimelineWidget::trackRect() const
{
	const qreal trackHeight = 7.0 * zoomScale();
	return QRectF(12.0, height() / 2.0 - trackHeight / 2.0, std::max(1, width() - 24), trackHeight);
}

qreal TimelineWidget::zoomScale() const
{
	if (durationMs <= 0)
		return 1.0;
	const qreal ratio = qreal(durationMs) / qreal(std::max<qint64>(1, visibleSpan()));
	return std::clamp(std::sqrt(ratio), qreal(1.0), qreal(2.4));
}

qint64 TimelineWidget::visibleStart() const
{
	return std::clamp(visibleStartMs, qint64(0), durationMs);
}

qint64 TimelineWidget::visibleEnd() const
{
	if (durationMs <= 0)
		return 0;
	const qint64 end = visibleEndMs > visibleStartMs ? visibleEndMs : durationMs;
	return std::clamp(end, qint64(0), durationMs);
}

qint64 TimelineWidget::visibleSpan() const
{
	return std::max<qint64>(1, visibleEnd() - visibleStart());
}

qint64 TimelineWidget::clampPosition(qint64 value) const
{
	return std::clamp(value, qint64(0), std::max<qint64>(0, durationMs));
}

qint64 TimelineWidget::clampVisibleStart(qint64 start, qint64 span) const
{
	if (durationMs <= 0 || span >= durationMs)
		return 0;
	return std::clamp(start, qint64(0), durationMs - span);
}

void TimelineWidget::resetVisibleRangeIfInvalid()
{
	if (durationMs <= 0) {
		visibleStartMs = 0;
		visibleEndMs = 0;
		setMinimumHeight(38);
		return;
	}

	if (visibleEndMs <= visibleStartMs || visibleEndMs > durationMs) {
		visibleStartMs = 0;
		visibleEndMs = durationMs;
	}
}

qreal TimelineWidget::xForPosition(qint64 value) const
{
	const QRectF track = trackRect();
	if (durationMs <= 0)
		return track.left();

	const qint64 start = visibleStart();
	const qint64 span = visibleSpan();
	const qreal ratio = qreal(std::clamp(value, start, visibleEnd()) - start) / qreal(span);
	return track.left() + ratio * track.width();
}

qint64 TimelineWidget::positionForX(qreal x) const
{
	const QRectF track = trackRect();
	const qreal clampedX = std::clamp(x, track.left(), track.right());
	const qreal ratio = (clampedX - track.left()) / std::max<qreal>(1.0, track.width());
	return clampPosition(visibleStart() + static_cast<qint64>(ratio * qreal(visibleSpan())));
}

int TimelineWidget::markerIndexAt(qreal x, qreal y) const
{
	if (durationMs <= 0 || markers.isEmpty())
		return -1;

	const QRectF track = trackRect();
	const qreal scale = zoomScale();
	const QRectF hitBand(track.left() - 8.0 * scale, track.top() - 22.0 * scale, track.width() + 16.0 * scale,
			     track.height() + 34.0 * scale);
	if (!hitBand.contains(QPointF(x, y)))
		return -1;

	int bestIndex = -1;
	qreal bestDistance = 10.0 * scale;
	for (int i = 0; i < markers.size(); ++i) {
		const qint64 marker = markers[i];
		if (marker < visibleStart() || marker > visibleEnd())
			continue;

		const qreal distance = std::abs(xForPosition(marker) - x);
		if (distance <= bestDistance) {
			bestDistance = distance;
			bestIndex = i;
		}
	}

	return bestIndex;
}

void TimelineWidget::setPositionFromMouse(qreal x, bool forceSeek)
{
	positionMs = positionForX(x);
	update();

	if (!previewSeekRequested)
		return;

	const bool shouldSeek = forceSeek || !seekThrottle.isValid() || seekThrottle.elapsed() >= 80;
	if (!shouldSeek)
		return;

	seekThrottle.restart();
	previewSeekRequested(positionMs);
}

void TimelineWidget::setMarkerFromMouse(qreal x, bool commit)
{
	if (selectedMarkerIndex < 0 || selectedMarkerIndex >= markers.size())
		return;

	const qint64 markerPosition = positionForX(x);
	markers[selectedMarkerIndex] = markerPosition;
	positionMs = markerPosition;
	update();

	if (commit) {
		if (markerMoveCommitted)
			markerMoveCommitted(selectedMarkerIndex, markerPosition);
		return;
	}

	if (!markerMovePreviewRequested)
		return;

	const bool shouldSeek = !seekThrottle.isValid() || seekThrottle.elapsed() >= 80;
	if (!shouldSeek)
		return;

	seekThrottle.restart();
	markerMovePreviewRequested(selectedMarkerIndex, markerPosition);
}

void TimelineWidget::zoomAt(qreal x, int wheelDelta)
{
	resetVisibleRangeIfInvalid();
	if (durationMs <= 0)
		return;

	const qint64 currentSpan = visibleSpan();
	const qint64 minimumSpan = std::min<qint64>(durationMs, 5000);
	const double factor = wheelDelta > 0 ? 0.80 : 1.25;
	qint64 newSpan = static_cast<qint64>(std::round(qreal(currentSpan) * factor));
	newSpan = std::clamp(newSpan, minimumSpan, std::max<qint64>(minimumSpan, durationMs));

	const qint64 anchor = positionForX(x);
	const double anchorRatio = qreal(anchor - visibleStart()) / qreal(currentSpan);
	qint64 newStart = anchor - static_cast<qint64>(std::round(anchorRatio * qreal(newSpan)));
	newStart = clampVisibleStart(newStart, newSpan);

	visibleStartMs = newSpan >= durationMs ? 0 : newStart;
	visibleEndMs = newSpan >= durationMs ? durationMs : visibleStartMs + newSpan;

	const int minHeight = 38 + static_cast<int>(std::round((zoomScale() - 1.0) * 22.0));
	setMinimumHeight(std::clamp(minHeight, 38, 76));
	updateGeometry();
	update();
}
