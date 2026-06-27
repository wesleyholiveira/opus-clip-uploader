#include "ui/review/review-clip-table-widget.hpp"

#include <QAction>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QModelIndex>
#include <QItemSelectionModel>

void ReviewClipTableWidget::keyPressEvent(QKeyEvent *event)
{
	if (!event)
		return;

	const bool noModifiers = event->modifiers() == Qt::NoModifier;
	if (noModifiers && (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)) {
		if (event->isAutoRepeat()) {
			event->accept();
			return;
		}
		if (rowCount() <= 0) {
			event->accept();
			return;
		}
		if (removeSelectedRows) {
			removeSelectedRows();
			event->accept();
			return;
		}
	}

	const bool markerNudgeModifiers = event->modifiers() == (Qt::ControlModifier | Qt::AltModifier);
	if (markerNudgeModifiers && (event->key() == Qt::Key_Comma || event->key() == Qt::Key_Less)) {
		if (nudgeSelectedRange) {
			nudgeSelectedRange(-1.0);
			event->accept();
			return;
		}
	}

	if (markerNudgeModifiers && (event->key() == Qt::Key_Period || event->key() == Qt::Key_Greater)) {
		if (nudgeSelectedRange) {
			nudgeSelectedRange(1.0);
			event->accept();
			return;
		}
	}

	QTableWidget::keyPressEvent(event);
}

void ReviewClipTableWidget::contextMenuEvent(QContextMenuEvent *event)
{
	if (!event)
		return;

	const int row = rowAt(event->pos().y());
	if (row < 0 || row >= rowCount()) {
		QTableWidget::contextMenuEvent(event);
		return;
	}

	if (!selectionModel() || !selectionModel()->isRowSelected(row, QModelIndex()))
		selectRow(row);

	const bool diagnosticRow = isDiagnosticRow && isDiagnosticRow(row);
	QMenu menu(this);
	QAction *diagnosticsAction = nullptr;
	if (!diagnosticRow)
		diagnosticsAction = menu.addAction(QStringLiteral("View marker diagnostics..."));
	QAction *approveAction = nullptr;
	QAction *rejectAction = nullptr;
	QAction *adjustedAction = nullptr;
	QAction *approveEditedAction = nullptr;
	if (!diagnosticRow)
		menu.addSeparator();
	approveAction = menu.addAction(diagnosticRow ? QStringLiteral("Approve candidate with structured feedback...")
						     : QStringLiteral("Approve with structured feedback..."));
	rejectAction = menu.addAction(diagnosticRow ? QStringLiteral("Reject candidate with structured feedback...")
						    : QStringLiteral("Reject with structured feedback..."));
	adjustedAction = menu.addAction(diagnosticRow ? QStringLiteral("Mark candidate as needs adjustment...")
						      : QStringLiteral("Mark as adjusted with structured feedback..."));
	approveEditedAction = menu.addAction(diagnosticRow ? QStringLiteral("Approve edited range as good clip...")
							   : QStringLiteral("Approve edited marker as good clip..."));
	QAction *ignoreAction = menu.addAction(diagnosticRow ? QStringLiteral("Ignore candidate for dataset/training")
							     : QStringLiteral("Ignore marker for dataset/training"));
	menu.addSeparator();
	QAction *seekStartAction = menu.addAction(QStringLiteral("Go to range start"));
	QAction *seekEndAction = menu.addAction(QStringLiteral("Go to range end"));
	QAction *removeAction = nullptr;
	if (!diagnosticRow) {
		menu.addSeparator();
		removeAction = menu.addAction(QStringLiteral("Remove without rating"));
	}

	const QAction *selected = menu.exec(event->globalPos());
	if (!selected)
		return;
	if (diagnosticsAction && selected == diagnosticsAction && showMarkerDiagnostics) {
		showMarkerDiagnostics(row);
		return;
	}
	if (selected == approveAction && approveMarker) {
		approveMarker(row);
		return;
	}
	if (selected == rejectAction && rejectMarker) {
		rejectMarker(row);
		return;
	}
	if (selected == adjustedAction && markMarkerAdjusted) {
		markMarkerAdjusted(row);
		return;
	}
	if (approveEditedAction && selected == approveEditedAction && approveEditedMarker) {
		approveEditedMarker(row);
		return;
	}
	if (ignoreAction && selected == ignoreAction && ignoreMarker) {
		ignoreMarker(row);
		return;
	}
	if (selected == seekStartAction && seekMarkerStart) {
		seekMarkerStart(row);
		return;
	}
	if (selected == seekEndAction && seekMarkerEnd) {
		seekMarkerEnd(row);
		return;
	}
	if (removeAction && selected == removeAction && removeSelectedRows)
		removeSelectedRows();
}
