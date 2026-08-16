#include "listitembase.h"
#include <QStyleOption>
#include <QPainter>

ListItemBase::ListItemBase(QWidget *parent)
    : QWidget(parent)
{
}

void ListItemBase::SetItemType(ListItemType itemType)
{
    _itemType = itemType;
}

ListItemType ListItemBase::GetItemType() const
{
    return _itemType;
}

// 关键：让 QSS 能作用于自定义 QWidget 子类（否则样式表失效）
void ListItemBase::paintEvent(QPaintEvent *event)
{
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}
