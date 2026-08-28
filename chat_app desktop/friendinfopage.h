#ifndef FRIENDINFOPAGE_H
#define FRIENDINFOPAGE_H

#include <QWidget>

namespace Ui {
class FriendInfoPage;
}

// 好友信息页（联系人列表点好友后展示）
// 内容：大头像 + 名字/性别 + 昵称/备注 + 底部"发消息/语音/视频"按钮
class FriendInfoPage : public QWidget
{
    Q_OBJECT

public:
    explicit FriendInfoPage(QWidget *parent = nullptr);
    ~FriendInfoPage();

    // 填充好友信息：头像路径、名字、性别(0男/1女)、昵称、备注
    // （等 UserInfo 数据模型就绪后，可以改成直接传对象）
    void SetUserInfo(const QString &icon, const QString &name, int sex,
                     const QString &nick, const QString &bak);

signals:
    void sig_jump_chat_item(); // 点"发消息"：跳转到与该好友的聊天页（TODO: 未实现）

private slots:
    void on_msg_chat_clicked();

private:
    Ui::FriendInfoPage *ui;
    QString _icon; // 当前好友头像路径
};

#endif // FRIENDINFOPAGE_H
