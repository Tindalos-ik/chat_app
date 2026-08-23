#ifndef ADDUSERITEM_H
#define ADDUSERITEM_H

#include <QWidget>
#include "listitembase.h"

namespace Ui {
class AddUserItem;
}

// 搜索列表顶部的"查找用户"提示项
// 点击它 = 用搜索框内容发起添加好友请求（当前客户端还没有搜索协议，先留接口）
class AddUserItem : public ListItemBase
{
    Q_OBJECT

public:
    explicit AddUserItem(QWidget *parent = nullptr);
    ~AddUserItem();

    QSize sizeHint() const override {
        return QSize(250, 70); // 和其他列表项保持统一高度
    }

private:
    Ui::AddUserItem *ui;
};

#endif // ADDUSERITEM_H
