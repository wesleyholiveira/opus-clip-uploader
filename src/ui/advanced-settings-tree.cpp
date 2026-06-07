#include "ui/advanced-settings-tree.hpp"

#include <QFrame>

AdvancedSettingsTree::AdvancedSettingsTree(QWidget *parent) : QTreeWidget(parent)
{
	setColumnCount(2);
	setHeaderHidden(true);
	setRootIsDecorated(true);
	setItemsExpandable(true);
	setAnimated(true);
	setMinimumHeight(130);

	setFrameShape(QFrame::NoFrame);
	setAutoFillBackground(false);
	setAttribute(Qt::WA_TranslucentBackground);
	viewport()->setAutoFillBackground(false);
	viewport()->setAttribute(Qt::WA_TranslucentBackground);

	setStyleSheet(R"(
		QTreeWidget {
			background: transparent;
			border: none;
		}
		QTreeWidget::viewport {
			background: transparent;
		}
		QTreeWidget::item {
			background: transparent;
		}
	)");

	advancedItem = new QTreeWidgetItem();
	advancedItem->setText(0, "Advanced Settings");
	advancedItem->setExpanded(false);
	addTopLevelItem(advancedItem);

	QObject::connect(this, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item, int column) {
		Q_UNUSED(column);

		if (item == advancedItem)
			advancedItem->setExpanded(!advancedItem->isExpanded());
	});
}

QTreeWidgetItem *AdvancedSettingsTree::addField(const QString &label, QWidget *widget)
{
	auto *item = new QTreeWidgetItem(advancedItem);
	item->setText(0, label);
	setItemWidget(item, 1, widget);
	resizeColumnToContents(0);
	return item;
}

QTreeWidgetItem *AdvancedSettingsTree::rootItem() const
{
	return advancedItem;
}