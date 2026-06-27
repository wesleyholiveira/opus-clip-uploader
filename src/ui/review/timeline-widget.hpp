#pragma once

#include "models/curation-settings.hpp"

#include <QElapsedTimer>
#include <QList>
#include <QWidget>

#include <functional>

class QMouseEvent;
class QPaintEvent;
class QWheelEvent;

class TimelineWidget final : public QWidget {
public:
	explicit TimelineWidget(QWidget *parent = nullptr);

	void setDuration(qint64 value);
	void setPosition(qint64 value);
	qint64 position() const;
	void setMarkers(const QList<qint64> &newMarkers);
	void setSelectedMarkerIndex(int index);
	void setClipRanges(const QVector<ClipDuration> &newRanges);

	std::function<void(qint64)> previewSeekRequested;
	std::function<void(qint64)> commitSeekRequested;
	std::function<void(int)> markerSelected;
	std::function<void(int, qint64)> markerMovePreviewRequested;
	std::function<void(int, qint64)> markerMoveCommitted;

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;

private:
	qint64 durationMs = 0;
	qint64 positionMs = 0;
	qint64 visibleStartMs = 0;
	qint64 visibleEndMs = 0;
	QList<qint64> markers;
	QVector<ClipDuration> clipRanges;
	bool draggingPosition = false;
	bool draggingMarker = false;
	int selectedMarkerIndex = -1;
	QElapsedTimer seekThrottle;

	QRectF trackRect() const;
	qreal zoomScale() const;
	qint64 visibleStart() const;
	qint64 visibleEnd() const;
	qint64 visibleSpan() const;
	qint64 clampPosition(qint64 value) const;
	qint64 clampVisibleStart(qint64 start, qint64 span) const;
	void resetVisibleRangeIfInvalid();
	qreal xForPosition(qint64 value) const;
	qint64 positionForX(qreal x) const;
	int markerIndexAt(qreal x, qreal y) const;
	void setPositionFromMouse(qreal x, bool forceSeek);
	void setMarkerFromMouse(qreal x, bool commit);
	void zoomAt(qreal x, int wheelDelta);
};
