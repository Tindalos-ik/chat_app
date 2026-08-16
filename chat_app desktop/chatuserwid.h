#ifndef CHATUSERWID_H
#define CHATUSERWID_H

#include <QWidget>
#include <QSize>

namespace Ui {
class ChatUserWid;
}

// 聊天/好友/搜索列表的列表项控件
// 用法：new 一个 ChatUserWid，setItemWidget 到 QListWidget 的 item 上
class ChatUserWid : public QWidget
{
    Q_OBJECT

public:
    explicit ChatUserWid(QWidget *parent = nullptr);
    ~ChatUserWid();

    QSize sizeHint() const override;   // 告诉列表项占多大空间

    void SetUserName(const QString &name);
    void SetChatMsg(const QString &msg);      // 最后一条消息预览
    void SetTime(const QString &time);
    void SetHeadIcon(const QString &icon_path); // 加载头像并裁剪成圆形
    void ShowRedPoint(bool show);              // 未读红点

private:
    Ui::ChatUserWid *ui;
};

#endif // CHATUSERWID_H
