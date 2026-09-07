#include "chatdialog.h"
#include "ui_chatdialog.h"
#include "statewidget.h"
#include <QApplication>
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
#include <QMouseEvent>
#include "tcpmgr.h"
#include "usermgr.h"
#include <QRandomGenerator>

namespace {
// 示例头像资源，循环使用
const QString kHeadIcons[] = {":/res/head_1.jpg", ":/res/head_2.jpg", ":/res/head_3.jpg",
                              ":/res/head_4.jpg", ":/res/head_5.jpg"};

void ClearListItems(QListWidget *list, int firstIndex)
{
    for (int index = list->count() - 1; index >= firstIndex; --index) {
        auto *item = list->item(index);
        auto *widget = list->itemWidget(item);
        list->removeItemWidget(item);
        delete widget;
        delete list->takeItem(index);
    }
}
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

    // 侧边栏按钮统一设置六种状态（外观在 stylesheet.qss 里按 state 切换）
    ui->side_chat_lb->SetState("normal", "hover", "pressed",
                               "selected", "selected_hover", "selected_pressed");
    ui->side_contact_lb->SetState("normal", "hover", "pressed",
                                  "selected", "selected_hover", "selected_pressed");
    ui->side_settings_lb->SetState("normal", "hover", "pressed",
                                   "selected", "selected_hover", "selected_pressed");

    // 加入互斥组：一次只能高亮一个
    AddLBGroup(ui->side_chat_lb);
    AddLBGroup(ui->side_contact_lb);
    AddLBGroup(ui->side_settings_lb);

    connect(ui->side_chat_lb, &StateWidget::clicked, this, &ChatDialog::slot_side_chat);
    connect(ui->side_contact_lb, &StateWidget::clicked, this, &ChatDialog::slot_side_contact);
    connect(ui->side_settings_lb, &StateWidget::clicked, this, &ChatDialog::slot_side_setting);

    ui->side_chat_lb->SetSelected(true); // 默认选中聊天

    // 搜索框有内容 -> 切到搜索列表（索引2）；清空 -> 回到当前模式对应的列表
    connect(ui->search_edit, &QLineEdit::textChanged, this, [this](const QString &text){
        ui->list_stack->setCurrentIndex(text.isEmpty() ? _cur_mode : 2);
    });

    // 搜索列表：点"查找用户"提示项时拿到搜索框文字
    ui->search_list->SetSearchEdit(ui->search_edit);

    // 聊天列表滚到底部 -> 加载更多（ChatUserList 只负责发信号，数据由这里补）
    connect(ui->session_list, &ChatUserList::sig_loading_chat_user,
            this, &ChatDialog::slot_loading_chat_user);

    // 构造时通常尚未登录，真正的账号名由 MainWindow::SlotSwitchChat 再刷新。
    UpdateUserTitle();
    // 点击聊天列表条目 -> 聊天标题换成对应联系人，并切回聊天页
    connect(ui->session_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item){
        auto *wid = qobject_cast<ChatUserWid*>(ui->session_list->itemWidget(item));
        if (wid == nullptr) {
            return;
        }
        ui->chat_title_label->setText(wid->GetName()); // 标题显示联系人名字
        ui->chat_stack->setCurrentWidget(ui->chat_page); // 回到聊天页
    });

    connect(ui->contact_list, &ConUserList::sig_loading_con_user,
            this, &ChatDialog::slot_loading_con_user);

    // 联系人列表："新的朋友"入口点击 -> 右侧切到好友申请页（已添加/未添加好友列表）
    connect(ui->contact_list, &ConUserList::sig_switch_apply_friend_page, this, [this]{
        // 打开申请页即视为已读，清除联系人入口和侧栏提示红点。
        ui->contact_list->ShowRedPoint(false);
        ui->side_contact_lb->ShowRedPoint(false);
        ui->chat_stack->setCurrentWidget(ui->apply_friend_page);
    });

    // 联系人列表：点好友 -> 右侧切到好友信息页并填充数据
    // TODO
    connect(ui->contact_list, &ConUserList::sig_switch_friend_info_page, this, [this](ConUserWid *wid){
        if (wid == nullptr) {
            return;
        }
        const QString name = wid->GetName();
        const QString icon = wid->GetIcon();
        const int sex = qHash(name) % 2; // 0=男 1=女
        ui->friend_info_page->SetUserInfo(icon, name, sex,
                                          name + QStringLiteral("的昵称"),
                                          name + QStringLiteral("的备注"));
        ui->chat_stack->setCurrentWidget(ui->friend_info_page); // 切到好友信息页
    });

    // 好友信息页点"发消息"：聊天标题换成该好友并切到聊天页（TODO: 加载历史消息）
    connect(ui->friend_info_page, &FriendInfoPage::sig_jump_chat_item, this, [this](const QString &name, const QString &icon){
        Q_UNUSED(icon); // 头像后续做会话头像时用
        ui->chat_title_label->setText(name); // 标题显示对应联系人
        ui->chat_stack->setCurrentWidget(ui->chat_page);
    });

    // 输入框回车（不带 Shift）→ 发送
    connect(ui->input_edit, &MessageTextEdit::send,
            this, &ChatDialog::on_send_btn_clicked);

    // 初始化聊天列表

    // 初始化好友列表

    // 全局监听鼠标点击，判断是否要清空搜索框
    // 注意：必须挂在 qApp 上，挂在 this 上收不到子控件（搜索框/列表/按钮）的点击事件
    qApp->installEventFilter(this);

    // tcp服务器发送好友申请信号，聊天界面做出响应
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_friend_apply, this, &ChatDialog::slot_apply_friend);

    // tcpmgr发来好友认证信号，聊天界面做出响应
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_auth_friend, this, &ChatDialog::slot_auth_friend);
}

ChatDialog::~ChatDialog()
{
    delete ui;
}

void ChatDialog::UpdateUserTitle()
{
    const QString name = UserMgr::GetInstance()->GetName().trimmed();
    if (name.isEmpty()) {
        return;
    }

    // 中间聊天标题显示当前账号；顶部标签始终显示当前登录用户。
    ui->chat_title_label->setText(name);
    ui->user_label->setText(name);
}

void ChatDialog::RefreshLoginData()
{
    UpdateUserTitle();
    ClearListItems(ui->session_list, 0);
    ClearListItems(ui->contact_list, 2);
    initChatUserList();
    initConUserList();
    ui->apply_friend_page->ReloadApplyList();
}

void ChatDialog::initChatUserList()
{
    auto friend_list = UserMgr::GetInstance()->GetFriendList();
    for(const auto &obj : friend_list){
        addChatUserWid(ui->session_list, obj->_name, "你好", "", obj->_icon, false);
    }
}

void ChatDialog::initConUserList()
{
    auto friend_list = UserMgr::GetInstance()->GetFriendList();
    for(const auto &obj : friend_list){
        addConUserWid(ui->contact_list, obj->_uid, obj->_name, obj->_icon);
    }
}


void ChatDialog::addChatUserWid(QListWidget *list, const QString &name, const QString &msg, const QString &time, const QString &icon, bool red)
{
    auto *item = new QListWidgetItem;
    auto *wid = new ChatUserWid;
    wid->SetUserName(name);
    wid->SetChatMsg(msg);
    wid->SetTime(time);
    if(icon.isEmpty()){
        // 生成 [0, 100) 之间的整数，即 0 到 99
        int value = QRandomGenerator::global()->bounded(100);
        QString ic = kHeadIcons[value%5];// 如果没有头像信息，随机数弄一下
        wid->SetHeadIcon(ic);
    }else{
        wid->SetHeadIcon(icon);
    }
    wid->ShowRedPoint(red);
    item->setSizeHint(wid->sizeHint());
    list->addItem(item);
    list->setItemWidget(item, wid);
}

void ChatDialog::addConUserWid(QListWidget *list, int uid, const QString &name, const QString &icon)
{
    auto *item = new QListWidgetItem;
    auto *wid = new ConUserWid;
    if(icon.isEmpty()){
        // 生成 [0, 100) 之间的整数，即 0 到 99
        int value = QRandomGenerator::global()->bounded(100);
        wid->SetInfo(uid, name, kHeadIcons[value%5]); // 如果没有头像信息，随机数弄一下
    }else{
        wid->SetInfo(uid, name, icon); // 联系人信息接口：类型 + 名字 + 头像一步到位
    }
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

    // TODO 加载更多聊天用户

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

    // TODO 加载更多好友

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

void ChatDialog::slot_side_chat()
{
    ClearLabelState(ui->side_chat_lb);
    _cur_mode = 0;
    ui->list_stack->setCurrentIndex(0); // 聊天列表
}

void ChatDialog::slot_side_contact()
{
    ClearLabelState(ui->side_contact_lb);
    _cur_mode = 1;
    ui->list_stack->setCurrentIndex(1); // 好友列表
}

void ChatDialog::slot_side_setting()
{
    // 设置页还没有实现：先不切换页面，但按钮保持选中态（与聊天/好友一致）
    ClearLabelState(ui->side_settings_lb);
}

// 申请好友槽函数，显示新的申请信息和红点
void ChatDialog::slot_apply_friend(std::shared_ptr<AddFriendApply> &apply)
{
    qDebug() << "receive apply friend slot, applyuid is " << apply->_fromuid << " name is "
             << apply->_name << " desc is " << apply->_desc;

    bool b_already = UserMgr::GetInstance()->AlreadyApply(apply->_fromuid);
    if(b_already){
        return; // 已经申请过了，不再申请
    }

    UserMgr::GetInstance()->AddApplyList(std::make_shared<ApplyInfo>(apply));
    ui->side_contact_lb->ShowRedPoint(true); // 显示红点
    ui->contact_list->ShowRedPoint(true);
    ui->apply_friend_page->AddNewApply(apply); // 加入新的信息
}

void ChatDialog::slot_auth_friend(std::shared_ptr<FriendInfo> &friend_info)
{
    // 好友列表中加一个新好友
    auto userinfo = std::make_shared<UserInfo>(friend_info->_uid, friend_info->_name, friend_info->_nick,
                                               friend_info->_desc,friend_info->_sex, friend_info->_icon);
    UserMgr::GetInstance()->AddFriendList(userinfo); // 更新好友列表
    addConUserWid(ui->contact_list, friend_info->_uid, friend_info->_name, friend_info->_icon);
    // 更新好友信息界面 TODO

    // 聊天会话列表也要加
    addChatUserWid(ui->session_list, friend_info->_name, "你好", "", friend_info->_icon, true);
}

bool ChatDialog::eventFilter(QObject *watched, QEvent *event)
{
    // 鼠标点击事件
    if(event->type() == QEvent::MouseButtonPress){
        QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
        handleGlobalMousePress(mouseEvent);
    }
    return QDialog::eventFilter(watched, event);
}

void ChatDialog::handleGlobalMousePress(QMouseEvent *mouseEvent)
{
    // 只有搜索框里有内容时才需要处理（_cur_mode 只表示聊天/好友，不表示搜索态）
    if (ui->search_edit->text().isEmpty()) {
        return;
    }

    // 有模态弹窗打开（比如 FindSuccessDlg）时不处理，避免点弹窗也清掉搜索框
    if (QApplication::activeModalWidget() != nullptr) {
        return;
    }

    // 点击搜索框本身：不清理，方便继续编辑搜索内容
    QPoint posInSearchEdit = ui->search_edit->mapFromGlobal(mouseEvent->globalPosition().toPoint());
    if (ui->search_edit->rect().contains(posInSearchEdit)) {
        return;
    }

    // 点击搜索列表内部（比如"查找用户"提示项）：不清理
    QPoint posInSearchList = ui->search_list->mapFromGlobal(mouseEvent->globalPosition().toPoint());
    if (ui->search_list->rect().contains(posInSearchList)) {
        return;
    }

    // 点击了搜索列表之外：清空搜索框
    // textChanged 信号会触发，自动切回 _cur_mode 对应的列表（聊天/好友）
    ui->search_edit->clear();
}

void ChatDialog::AddLBGroup(StateWidget *lb)
{
    _lb_list.push_back(lb);
}

void ChatDialog::ClearLabelState(StateWidget *lb)
{
    for (auto *ele : _lb_list) {
        if (ele != lb) {
            ele->ClearState();
        }
    }
}
