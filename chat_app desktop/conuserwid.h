#ifndef CONUSERWID_H
#define CONUSERWID_H

#include <QWidget>
#include <QSize>
#include "listitembase.h"

namespace Ui {
class ConUserWid;
}

// 好友列表的列表项控件：只显示头像 + 名字
// （聊天列表的项用 ChatUserWid，带最后消息/时间/红点）
class ConUserWid : public ListItemBase
{
    Q_OBJECT

public:
    explicit ConUserWid(QWidget *parent = nullptr);
    ~ConUserWid();

    QSize sizeHint() const override;   // 返回 260x60

    void SetUserName(const QString &name);
    void SetHeadIcon(const QString &icon_path); // 加载头像并裁剪成圆形

private:
    Ui::ConUserWid *ui;
};

#endif // CONUSERWID_H
