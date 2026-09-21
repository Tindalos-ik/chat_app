#ifndef CHATUSERWID_H
#define CHATUSERWID_H

#include <QWidget>
#include <QSize>
#include <QtGlobal>
#include <memory>
#include "listitembase.h"
#include "userdata.h"

namespace Ui {
class ChatUserWid;
}

// 聊天/好友/搜索列表的列表项控件
// 用法：new 一个 ChatUserWid，setItemWidget 到 QListWidget 的 item 上
class ChatUserWid : public ListItemBase
{
    Q_OBJECT

public:
    explicit ChatUserWid(QWidget *parent = nullptr);
    ~ChatUserWid();

    QSize sizeHint() const override;   // 告诉列表项占多大空间

    void SetUserInfo(const std::shared_ptr<UserInfo> &userInfo);
    std::shared_ptr<UserInfo> GetUserInfo() const;
    void SetUserName(const QString &name);
    void SetChatMsg(const QString &msg);      // 最后一条消息预览
    void SetTime(const QString &time);
    void SetHeadIcon(const QString &icon_path); // 加载头像并裁剪成圆形
    void ShowRedPoint(bool show);              // 未读红点
    // 服务端分配的正式会话 ID；0 表示尚未创建私聊。
    void SetThreadId(qint64 threadId);
    qint64 GetThreadId() const;
    QString GetName() const; // 取联系人名字（点击条目后聊天标题用）
    QString GetIcon() const; // 取头像路径

private:
    Ui::ChatUserWid *ui;
    std::shared_ptr<UserInfo> _userInfo;
    QString _name; // 联系人名字
    QString _icon; // 头像资源路径
    qint64 _threadId = 0;
};

#endif // CHATUSERWID_H
