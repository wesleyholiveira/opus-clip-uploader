#pragma once

#include <QTableWidget>

#include <functional>

class QContextMenuEvent;
class QKeyEvent;

class ReviewClipTableWidget final : public QTableWidget {
public:
	using QTableWidget::QTableWidget;

	std::function<void()> removeSelectedRows;
	std::function<void(double)> nudgeSelectedRange;
	std::function<bool(int)> isDiagnosticRow;
	std::function<void(int)> showMarkerDiagnostics;
	std::function<void(int)> approveMarker;
	std::function<void(int)> rejectMarker;
	std::function<void(int)> markMarkerAdjusted;
	std::function<void(int)> approveEditedMarker;
	std::function<void(int)> ignoreMarker;
	std::function<void(int)> seekMarkerStart;
	std::function<void(int)> seekMarkerEnd;

protected:
	void keyPressEvent(QKeyEvent *event) override;
	void contextMenuEvent(QContextMenuEvent *event) override;
};
