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
    void ShowRedPoint(bool show); // 显示/隐藏右上角红点（新好友申请提示）
    QString GetName() const; // 取名字（好友信息页用）
    QString GetIcon() const; // 取头像路径（好友信息页用）

private:
    Ui::ConUserWid *ui;
    QString _name; // 好友名字
    QString _icon; // 头像资源路径
};

#endif // CONUSERWID_H
