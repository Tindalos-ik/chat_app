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

private:
    ChatRole m_role;
    QLabel *m_pNameLabel;
    QLabel *m_pIconLabel;
    QLabel *m_pSendStatusLabel;
    QWidget *m_pBubble;
};

#endif // CHATITEMBASE_H
