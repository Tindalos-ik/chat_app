#ifndef CONUSERLIST_H
#define CONUSERLIST_H

#include <QObject>
#include <QListWidget>
#include <QWheelEvent>
#include <QScrollEvent>
#include <QDebug>

class ConUserWid;

// 联系人列表（QListWidget）
// 结构：顶部"新的朋友"入口 + "联系人"分组标题 + 好友列表
//  点击条目按类型分发："新的朋友" -> sig_switch_apply_friend_page，
//  普通好友 -> sig_switch_friend_info_page
class ConUserList:public QListWidget
{
    Q_OBJECT
public:
    ConUserList(QWidget* parent = nullptr);

    // 显示/隐藏"新的朋友"入口的红点（有新好友申请时由外部调用）
    void ShowRedPoint(bool bshow = true);

protected:
    bool eventFilter(QObject *watched, QEvent *event)override;

signals:
    void sig_loading_con_user(); // 加载联系人信号
    void sig_switch_apply_friend_page(); // 点击"新的朋友"：切到好友申请界面
    void sig_switch_friend_info_page(ConUserWid *wid); // 点击好友：携带好友项，切到好友信息界面

public slots:
    void slot_item_clicked(QListWidgetItem* item); // 点击展示好友信息

private:
    void addContactUserList(); // 顶部加"新的朋友"入口和"联系人"分组标题

    ConUserWid* _add_friend_item;   // "新的朋友"入口项（带红点）
    QListWidgetItem* _grpupitem;    // "联系人"分组标题项
};

#endif // CONUSERLIST_H
