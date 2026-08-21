#include "chatdialog.h"
#include "ui_chatdialog.h"
#include <QAction>
#include <QIcon>
#include <QLineEdit>
#include <QStringLiteral>
#include <QTimer>
#include "chatuserwid.h"
#include "conuserwid.h"
#include "loadingdlg.h"
#include "chatuserlist.h"
#include "conuserlist.h"
#include "global.h"
#include "picturebubble.h"
#include "textbubble.h"
#include "messagetextedit.h"
#include "chatitembase.h"

namespace {
// 示例头像资源，循环使用
const QString kHeadIcons[] = {":/res/head_1.jpg", ":/res/head_2.jpg", ":/res/head_3.jpg",
                              ":/res/head_4.jpg", ":/res/head_5.jpg"};
}

ChatDialog::ChatDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::ChatDialog)
{
    ui->setupUi(this);
    ui->list_stack->setCurrentIndex(0); // 默认显示聊天列表（session_list）

    // 聊天区与输入区之间的分隔条：消息区占满剩余空间，输入区高度可拖拽调节（80~300）
    ui->chat_splitter->setStretchFactor(0, 1); // 消息区可拉伸
    ui->chat_splitter->setStretchFactor(1, 0); // 输入区保持固定
    ui->chat_splitter->setSizes({480, 150});   // 初始分配高度

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

    // 聊天列表滚到底部 -> 加载更多（ChatUserList 只负责发信号，数据由这里补）
    connect(ui->session_list, &ChatUserList::sig_loading_chat_user,
            this, &ChatDialog::slot_loading_chat_user);

    connect(ui->contact_list, &ConUserList::sig_loading_con_user,
            this, &ChatDialog::slot_loading_con_user);

    // 输入框回车（不带 Shift）→ 发送
    connect(ui->input_edit, &MessageTextEdit::send,
            this, &ChatDialog::on_send_btn_clicked);

    addChatUserList();
    addConUserList();

}

ChatDialog::~ChatDialog()
{
    delete ui;
}

void ChatDialog::addChatUserList()
{
    for (int i = _loaded_chat_count; i <= _loaded_chat_count+30; ++i) {
        addChatUserWid(ui->session_list,
                       QStringLiteral("用户%1").arg(i),
                       QStringLiteral("第 %1 条消息内容").arg(i),
                       QStringLiteral("%1:%2").arg(9 + i % 10).arg(i % 60),
                       kHeadIcons[i % 5], i % 3 == 0);
    }
}

void ChatDialog::addConUserList()
{
    for (int i = _loaded_con_count; i <= _loaded_con_count+30; ++i) {
        addConUserWid(ui->contact_list,
                       QStringLiteral("用户%1").arg(i),
                       kHeadIcons[i % 5]);
    }
}

void ChatDialog::addChatUserWid(QListWidget *list, const QString &name, const QString &msg, const QString &time, const QString &icon, bool red)
{
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
}

void ChatDialog::addConUserWid(QListWidget *list, const QString &name, const QString &icon)
{
    auto *item = new QListWidgetItem;
    auto *wid = new ConUserWid;
    wid->SetUserName(name);
    wid->SetHeadIcon(icon);
    item->setSizeHint(wid->sizeHint());
    list->addItem(item);
    list->setItemWidget(item, wid);
}

void ChatDialog::slot_loading_chat_user()
{
    if (_b_loading) {
        return;
    }

    _b_loading = true;   // 防抖：加载期间忽略重复触发

    LoadingDlg *loadingDialog = new LoadingDlg(this);
    loadingDialog->show();

    qDebug() << "add new data to list";

    addChatUserList();
    // 加载完成之后关闭对话框
    loadingDialog->deleteLater();

    _b_loading = false;
}

void ChatDialog::slot_loading_con_user()
{
    if(_b_loading){
        return;
    }

    _b_loading = true;
    LoadingDlg *loadingDialog = new LoadingDlg(this);
    loadingDialog->show();
    qDebug() << "add new data to list";
    addConUserList();
    loadingDialog->deleteLater();

    _b_loading = false;
}


void ChatDialog::on_send_btn_clicked()
{
    auto pTextEdit = ui->input_edit;
    ChatRole role = ChatRole::Self;
    QString userName = QStringLiteral("klein");
    QString userIcon = ":/res/head_1.jpg";

    const QVector<MsgInfo>& msgList = pTextEdit->getMsgList();
    for(int i=0; i<msgList.size(); ++i)
    {
        QString type = msgList[i].msgFlag;
        ChatItemBase *pChatItem = new ChatItemBase(role);
        pChatItem->setUserName(userName);
        pChatItem->setUserIcon(QPixmap(userIcon));
        QWidget *pBubble = nullptr;
        if(type == "text")
        {
            pBubble = new TextBubble(role, msgList[i].content);
        }
        else if(type == "image")
        {
            QPixmap pix(msgList[i].content); // 优先按原图路径加载，清晰度更好
            if(pix.isNull())
            {
                pix = msgList[i].pixmap;     // 源文件读不到时用输入框里的缩略图兜底
            }
            pBubble = new PictureBubble(role, pix);
        }
        else if(type == "file")
        {
            // 文件消息：先按预览图展示（真正的文件发送待实现）
            pBubble = new PictureBubble(role, msgList[i].pixmap);
        }
        if(pBubble != nullptr)
        {
            pChatItem->setWidget(pBubble);
            ui->chat_data->appendChatItem(pChatItem);
        }
    }
}

