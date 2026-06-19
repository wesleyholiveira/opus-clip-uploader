#include "ui/advanced-settings-tree.hpp"

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

#include <QCheckBox>
#include <QFrame>
#include <QHeaderView>
#include <QLabel>
#include <QSize>
#include <QSizePolicy>
#include <QString>
#include <QVBoxLayout>

#include <algorithm>

static QString obsText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

AdvancedSettingsTree::AdvancedSettingsTree(QWidget *parent) : QTreeWidget(parent)
{
	setColumnCount(1);
	setHeaderHidden(true);
	setRootIsDecorated(true);
	setItemsExpandable(true);
	setAnimated(true);
	setMinimumHeight(190);
	setWordWrap(false);
	setUniformRowHeights(false);
	setTextElideMode(Qt::ElideNone);
	setIndentation(16);
	setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

	header()->setStretchLastSection(true);
	header()->setSectionResizeMode(0, QHeaderView::Stretch);

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
	advancedItem->setText(0, obsText("AdvancedSettings"));
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
	item->setToolTip(0, label);
	item->setFirstColumnSpanned(true);

	auto *fieldWidget = new QWidget(this);
	auto *fieldLayout = new QVBoxLayout(fieldWidget);
	fieldLayout->setContentsMargins(4, 4, 4, 8);
	fieldLayout->setSpacing(4);

	auto *checkbox = qobject_cast<QCheckBox *>(widget);
	if (checkbox) {
		item->setText(0, QString());
		checkbox->setText(label);
		checkbox->setToolTip(label);
		checkbox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		fieldLayout->addWidget(checkbox);
	} else {
		item->setText(0, QString());
		auto *labelWidget = new QLabel(label, fieldWidget);
		labelWidget->setWordWrap(true);
		labelWidget->setTextInteractionFlags(Qt::TextSelectableByMouse);
		labelWidget->setToolTip(label);
		labelWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
		fieldLayout->addWidget(labelWidget);
		if (widget) {
			widget->setMinimumWidth(320);
			widget->setSizePolicy(QSizePolicy::Expanding, widget->sizePolicy().verticalPolicy());
			fieldLayout->addWidget(widget);
		}
	}

	fieldWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	setItemWidget(item, 0, fieldWidget);

	const int rowHeight = std::max(checkbox ? 36 : 58, fieldWidget->sizeHint().height());
	item->setSizeHint(0, QSize(0, rowHeight));

	return item;
}

QTreeWidgetItem *AdvancedSettingsTree::rootItem() const
{
	return advancedItem;
}
