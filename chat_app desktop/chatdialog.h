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
    void addChatUserList(); // 添加用户列表

private:
    Ui::ChatDialog *ui;
    int _cur_mode = 0;   // 0=聊天列表 1=好友列表，搜索清空后回到当前模式

    bool _b_loading = false;   // 加载更多防抖标志（示例数据用）
};

#endif // CHATDIALOG_H
