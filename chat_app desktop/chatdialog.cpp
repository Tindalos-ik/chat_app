#include "chatdialog.h"
#include "ui_chatdialog.h"
#include <QAction>
#include <QIcon>
#include <QLineEdit>
#include <QStringLiteral>
#include <QTimer>
#include "chatuserwid.h"
#include "conuserwid.h"
#include "chatuserlist.h"

ChatDialog::ChatDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::ChatDialog)
{
    ui->setupUi(this);
    ui->list_stack->setCurrentIndex(0); // 默认显示聊天列表（session_list）

    // 一些操作都是通过action实现的，比如右键菜单，弹出别的东西
    QAction *searchAction = new QAction(ui->search_edit);
    searchAction->setIcon(QIcon(":/res/search.png"));
    // leadingposition 头部位置，所以这样就把搜索图标放在搜索栏前面了
    ui->search_edit->addAction(searchAction, QLineEdit::LeadingPosition);
    ui->search_edit->setPlaceholderText(QStringLiteral("搜索"));
    // 同样的，清除动作也可以这么做

    // 侧边栏：聊天按钮 -> 聊天列表（索引0）
    connect(ui->side_chat_btn, &QPushButton::clicked, this, [this]{
        _cur_mode = 0;
        ui->list_stack->setCurrentIndex(0);
    });

    // 侧边栏：好友按钮 -> 好友列表（索引1）
    connect(ui->side_contact_btn, &QPushButton::clicked, this, [this]{
        _cur_mode = 1;
        ui->list_stack->setCurrentIndex(1);
    });

    // 搜索框有内容 -> 切到搜索列表（索引2）；清空 -> 回到当前模式对应的列表
    connect(ui->search_edit, &QLineEdit::textChanged, this, [this](const QString &text){
        ui->list_stack->setCurrentIndex(text.isEmpty() ? _cur_mode : 2);
    });

    // ===== 示例数据（测试完可删）=====
    auto addDemoItem = [this](QListWidget *list, const QString &name,
                              const QString &msg, const QString &time,
                              const QString &icon, bool red) {
        auto *item = new QListWidgetItem;
        auto *wid = new ChatUserWid;
        wid->SetUserName(name);
        wid->SetChatMsg(msg);
        wid->SetTime(time);
        wid->SetHeadIcon(icon);
        wid->ShowRedPoint(red);
        item->setSizeHint(wid->sizeHint());
        list->addItem(item);
        list->setItemWidget(item, wid);
    };

    // 聊天列表塞 30 条，保证能滚出滚动条、测试滚轮
    const QString icons[] = {":/res/head_1.jpg", ":/res/head_2.jpg", ":/res/head_3.jpg",
                             ":/res/head_4.jpg", ":/res/head_5.jpg"};
    for (int i = 1; i <= 30; ++i) {
        addDemoItem(ui->session_list,
                    QStringLiteral("用户%1").arg(i),
                    QStringLiteral("第 %1 条消息内容").arg(i),
                    QStringLiteral("%1:%2").arg(9 + i % 10).arg(i % 60),
                    icons[i % 5], i % 3 == 0);
    }
    // 好友列表：只有头像 + 名字（ConUserWid）
    auto addConItem = [this](QListWidget *list, const QString &name, const QString &icon) {
        auto *item = new QListWidgetItem;
        auto *wid = new ConUserWid;
        wid->SetUserName(name);
        wid->SetHeadIcon(icon);
        item->setSizeHint(wid->sizeHint());
        list->addItem(item);
        list->setItemWidget(item, wid);
    };
    addConItem(ui->contact_list, "王五", ":/res/head_3.jpg");
    addConItem(ui->contact_list, "赵六", ":/res/head_4.jpg");


}

ChatDialog::~ChatDialog()
{
    delete ui;
}

