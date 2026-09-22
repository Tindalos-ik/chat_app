#ifndef CHATITEMBASE_H
#define CHATITEMBASE_H
#include <QWidget>
#include <QGridLayout> // 网格布局
#include <QLabel>
#include "global.h"

class BubbleFrame;

class ChatItemBase : public QWidget
{
    Q_OBJECT
public:
    explicit ChatItemBase(ChatRole role, QWidget* parent = nullptr);
    void setUserName(const QString &name);
    void setUserAvatar(int uid, const QString &avatarPath);
    void setWidget(QWidget *w); // 气泡里面的内容
    // 发送失败时显示气泡旁的状态图标；成功或普通接收消息保持隐藏。
    void SetSendFailed(bool failed);
    // 仅自己发送的气泡显示：1~3 使用未读图标，4 使用已读图标；悬浮提示保留
    // 已保存、已送达、已显示三个服务端阶段，避免把“对方已显示”误解成“已读”。
    void SetDeliveryState(int state);

private:
    ChatRole m_role;
    QLabel *m_pNameLabel;
    QLabel *m_pIconLabel;
    QLabel *m_pSendStatusLabel;
    QWidget *m_pBubble;
};

#endif // CHATITEMBASE_H
