#ifndef CHATDIALOG_H
#define CHATDIALOG_H

#include <QDialog>
#include "chatuserlist.h"

namespace Ui {
class ChatDialog;
}

class ChatDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ChatDialog(QWidget *parent = nullptr);
    ~ChatDialog();
    void addChatUserList();
    void addConUserList();

private slots:
    void slot_loading_chat_user();   // 聊天列表滚到底部时加载更多

private:
    Ui::ChatDialog *ui;
    int _cur_mode = 0;   // 0=聊天列表 1=好友列表，搜索清空后回到当前模式

    bool _b_loading = false;       // 防抖标志：加载期间忽略重复触发
    int _loaded_chat_count = 30;   // 已加载的聊天会话条数（示例数据计数）

    void addChatUserWid(QListWidget *list, const QString &name,
                        const QString &msg, const QString &time,
                        const QString &icon, bool red); // 添加用户列表
    void addConUserWid(QListWidget *list, const QString &name, const QString &icon); //添加好友列表
};

#endif // CHATDIALOG_H
