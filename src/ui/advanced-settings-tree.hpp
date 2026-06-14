#pragma once

#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QWidget>

class AdvancedSettingsTree : public QTreeWidget {
public:
	explicit AdvancedSettingsTree(QWidget *parent = nullptr);

	QTreeWidgetItem *addField(const QString &label, QWidget *widget);
	QTreeWidgetItem *rootItem() const;

private:
	QTreeWidgetItem *advancedItem = nullptr;
};
