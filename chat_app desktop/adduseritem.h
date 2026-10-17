#ifndef ADDUSERITEM_H
#define ADDUSERITEM_H

#include <QWidget>
#include <QIcon>
#include "listitembase.h"

namespace Ui {
class AddUserItem;
}

// 搜索列表的紧凑入口；统一图标与间距，点击和选中状态由 SearchList 处理。
class AddUserItem : public ListItemBase
{
    Q_OBJECT

public:
    explicit AddUserItem(QWidget *parent = nullptr);
    ~AddUserItem();
    void SetTip(const QString &text, const QIcon &icon);

    QSize sizeHint() const override {
        return QSize(180, 48);
    }

private:
    Ui::AddUserItem *ui;
};

#endif // ADDUSERITEM_H
