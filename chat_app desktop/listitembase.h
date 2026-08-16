#ifndef LISTITEMBASE_H
#define LISTITEMBASE_H

#include <QWidget>
#include "global.h"

// 列表项基类：所有列表项（聊天/好友/分组/分割线）都继承它
// 1. 类型标签：外层点击/右键时按 GetItemType() 分流处理
// 2. paintEvent 走 QStyle，保证 QSS（hover/选中高亮）能作用到列表项上
class ListItemBase : public QWidget
{
    Q_OBJECT
public:
    explicit ListItemBase(QWidget *parent = nullptr);
    void SetItemType(ListItemType itemType);
    ListItemType GetItemType() const;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    ListItemType _itemType = InvalidItem;
};

#endif // LISTITEMBASE_H
