#ifndef CHATDIALOG_H
#define CHATDIALOG_H

#include <QDialog>
#include <QListWidget>
#include <QVector>
#include "statewidget.h"

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
    void slot_loading_con_user();

    void on_send_btn_clicked();

    void slot_side_chat();      // 侧边栏：聊天
    void slot_side_contact();   // 侧边栏：联系人
    void slot_side_setting();   // 侧边栏：设置（页面未实现）

private:
    Ui::ChatDialog *ui;
    int _cur_mode = 0;   // 0=聊天列表 1=好友列表，搜索清空后回到当前模式
    QVector<StateWidget*> _lb_list;  // 侧边栏按钮组，保证一次只高亮一个

    bool _b_loading = false;       // 防抖标志：加载期间忽略重复触发
    int _loaded_chat_count = 0;   // 已加载的聊天会话条数（示例数据计数）
    int _loaded_con_count = 0;

    void addChatUserWid(QListWidget *list, const QString &name,
                        const QString &msg, const QString &time,
                        const QString &icon, bool red); // 添加用户列表
    void addConUserWid(QListWidget *list, const QString &name, const QString &icon); //添加好友列表

    void AddLBGroup(StateWidget *lb);            // 把侧边栏按钮加入互斥组
    void ClearLabelState(StateWidget *lb);       // 清除除 lb 之外所有按钮的选中态
};

#endif // CHATDIALOG_H
