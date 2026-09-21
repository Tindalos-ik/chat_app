#include "chatdialog.h"
#include "ui_chatdialog.h"
#include "statewidget.h"
#include <QApplication>
#include <QAction>
#include <QIcon>
#include <QLineEdit>
#include <QStringLiteral>
#include <QTimer>
#include <QDateTime>
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
#include "localchatstoragemgr.h"
#include "settingdialog.h"
#include <algorithm>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMessageBox>

namespace {
constexpr int kLocalHistoryPageSize = 50;

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

    // 设置作为右侧主区域的一页显示，而非独立弹窗。
    _setting_page = new SettingDialog(ui->chat_stack);
    _setting_page->setWindowFlags(Qt::Widget);
    ui->chat_stack->addWidget(_setting_page);
    connect(_setting_page, &SettingDialog::sig_setting_cancel,
            this, &ChatDialog::slot_hide_setting);

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
        if (wid == nullptr || !wid->GetUserInfo()) {
            return;
        }
        SetCurrentChatUser(wid->GetUserInfo(), wid->GetThreadId());
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
        const auto friendList = UserMgr::GetInstance()->GetFriendList();
        const auto iter = std::find_if(friendList.cbegin(), friendList.cend(),
                                       [wid](const std::shared_ptr<UserInfo> &info) {
                                           return info && info->_uid == wid->GetUid();
                                       });
        if (iter == friendList.cend()) {
            return;
        }
        SetCurrentChatUser(*iter);
        ui->friend_info_page->SetUserInfo((*iter)->_uid, (*iter)->_icon,
                                          (*iter)->_name, (*iter)->_sex,
                                          (*iter)->_nick, (*iter)->_name);
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
            this, &ChatDialog::slot_send_message);
    // 显式连接，不依赖 on_<object>_<signal> 的命名约定，避免重复连接或 UI 改名后失效。
    connect(ui->send_btn, &QPushButton::clicked, this, &ChatDialog::slot_send_message);

    // 初始化聊天列表

    // 初始化好友列表

    // 全局监听鼠标点击，判断是否要清空搜索框
    // 注意：必须挂在 qApp 上，挂在 this 上收不到子控件（搜索框/列表/按钮）的点击事件
    qApp->installEventFilter(this);

    // tcp服务器发送好友申请信号，聊天界面做出响应
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_friend_apply, this, &ChatDialog::slot_apply_friend);

    // tcpmgr发来好友认证信号，聊天界面做出响应
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_auth_friend, this, &ChatDialog::slot_auth_friend);
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_create_private_chat,
            this, &ChatDialog::slot_create_private_chat);
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_text_chat, this, &ChatDialog::slot_text_chat);
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_text_chat_send_result,
            this, &ChatDialog::slot_text_chat_send_result);
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_local_chat_synced,
            this, &ChatDialog::slot_local_chat_synced);
    connect(ui->chat_data, &ChatView::sig_reach_top,
            this, &ChatDialog::slot_load_older_local_messages);

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
    _current_chatuser.reset();
    _current_thread_id = 0;
    UpdateUserTitle();
    ClearListItems(ui->session_list, 0);
    ClearListItems(ui->contact_list, 2);
    initChatUserList();
    initConUserList();
    ui->apply_friend_page->ReloadApplyList();
}

void ChatDialog::initChatUserList()
{
    // 登录时 SQLite 已按 UID 打开。先用持久化会话恢复每个好友对应的 threadId、
    // 最后消息和未读数；没有缓存的好友仍显示为可点击的临时列表项，threadId 为 0。
    QHash<qint64, LocalChatThread> cachedPrivateThreads;
    const auto localStorage = LocalChatStorageMgr::GetInstance();
    if (localStorage->IsReady()) {
        for (const LocalChatThread &thread : localStorage->CachedThreads()) {
            if (thread.threadType == QStringLiteral("private") && thread.peerUid > 0) {
                cachedPrivateThreads.insert(thread.peerUid, thread);
            }
        }
    }

    auto friend_list = UserMgr::GetInstance()->GetFriendList();
    for(const auto &obj : friend_list){
        const LocalChatThread thread = cachedPrivateThreads.value(obj->_uid);
        addChatUserWid(ui->session_list, obj,
                        thread.threadId > 0 ? thread.lastMessagePreview : QStringLiteral("你好"),
                        thread.lastMessageAtMs > 0
                            ? QDateTime::fromMSecsSinceEpoch(thread.lastMessageAtMs).toString(QStringLiteral("HH:mm"))
                            : QString(),
                        thread.unreadCount > 0, thread.threadId);
    }
}

void ChatDialog::initConUserList()
{
    auto friend_list = UserMgr::GetInstance()->GetFriendList();
    for(const auto &obj : friend_list){
        addConUserWid(ui->contact_list, obj->_uid, obj->_name, obj->_icon);
    }
}


void ChatDialog::addChatUserWid(QListWidget *list, const std::shared_ptr<UserInfo> &userInfo,
                                const QString &msg, const QString &time, bool red,
                                qint64 threadId)
{
    if (!userInfo) {
        return;
    }
    auto *item = new QListWidgetItem;
    auto *wid = new ChatUserWid;
    wid->SetUserInfo(userInfo);
    wid->SetThreadId(threadId);
    wid->SetChatMsg(msg);
    wid->SetTime(time);
    wid->ShowRedPoint(red);
    item->setSizeHint(wid->sizeHint());
    list->addItem(item);
    list->setItemWidget(item, wid);
}

void ChatDialog::addConUserWid(QListWidget *list, int uid, const QString &name, const QString &icon)
{
    auto *item = new QListWidgetItem;
    auto *wid = new ConUserWid;
    wid->SetInfo(uid, name, icon);
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


void ChatDialog::slot_send_message()
{
    auto pTextEdit = ui->input_edit;
    ChatRole role = ChatRole::Self;
    auto userinfo = UserMgr::GetInstance()->GetUserInfo();
    if (!userinfo) {
        QMessageBox::warning(this, tr("发送失败"), tr("当前未登录，请重新登录。"));
        return;
    }

    if (!_current_chatuser) {
        QMessageBox::warning(this, tr("发送失败"), tr("请先在聊天列表或联系人列表中选择聊天对象。"));
        return;
    }

    if (!TcpMgr::GetInstance()->IsConnected()) {
        QMessageBox::warning(this, tr("发送失败"), tr("未连接聊天服务器，请重新登录后再发送。"));
        qWarning() << "cannot send message: TCP socket is not connected";
        return;
    }

    QString userName = userinfo->_name;
    QString userIcon = userinfo->_icon;
    int uid = userinfo->_uid;

    const QVector<MsgInfo>& msgList = pTextEdit->getMsgList();
    QJsonArray textArray;
    int textBytes = 0;
    constexpr int kMaxTextBatchBytes = 1024;

    const auto sendTextBatch = [&] {
        if (textArray.isEmpty()) {
            return;
        }

        QJsonObject textObj;
        textObj["fromuid"] = userinfo->_uid;
        textObj["touid"] = _current_chatuser->_uid;
        textObj["textArray"] = textArray;
        const QByteArray jsonData = QJsonDocument(textObj).toJson(QJsonDocument::Compact);
        emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_TEXT_CHAT_MSG_REQ, jsonData);
        textArray = QJsonArray();
        textBytes = 0;
    };

    for(int i=0; i<msgList.size(); ++i)
    {
        QString type = msgList[i].msgFlag;
        ChatItemBase *pChatItem = new ChatItemBase(role);
        pChatItem->setUserName(userName);
        pChatItem->setUserAvatar(uid, userIcon);
        QWidget *pBubble = nullptr;

        if(type == "text")
        {
            pBubble = new TextBubble(role, msgList[i].content);

            // 组装信息发送
            // 生成唯一id，id和消息绑定可以用于后面判断消息是否送达
            QUuid uuid = QUuid::createUuid();
            QString uuid_str = uuid.toString();

            QJsonObject obj;
            QByteArray utf8Message = msgList[i].content.toUtf8();
            if (!textArray.isEmpty() && textBytes + utf8Message.size() > kMaxTextBatchBytes) {
                sendTextBatch();
            }
            obj["content"] = QString::fromUtf8(utf8Message);
            obj["msgid"] = uuid_str;
            textArray.append(obj);
            textBytes += utf8Message.size();
            auto txt_msg = std::make_shared<TextChatData>(uuid_str, obj["content"].toString(), userinfo->_uid, _current_chatuser->_uid);
            // 发送前先展示气泡；服务器回包通过同一个 UUID 找回这一行，失败时在
            // 气泡左端显示 send_fail.png。成功后删除映射，确认消息由 SQLite 重绘。
            _pending_text_items.insert(uuid_str, pChatItem);
            emit sig_append_send_chat_msg(txt_msg);
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

    sendTextBatch();
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
    ClearLabelState(ui->side_settings_lb);
    ui->chat_stack->setCurrentWidget(_setting_page);
}

void ChatDialog::slot_hide_setting()
{
    ui->chat_stack->setCurrentWidget(ui->chat_page);
    slot_side_chat();
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

void ChatDialog::slot_auth_friend(std::shared_ptr<FriendAuthResult> &authResult)
{
    if (!authResult || !authResult->friendInfo) {
        return;
    }

    const auto &friendInfo = authResult->friendInfo;
    auto userinfo = std::make_shared<UserInfo>(friendInfo->_uid, friendInfo->_name, friendInfo->_nick,
                                               friendInfo->_desc, friendInfo->_sex, friendInfo->_icon);
    UserMgr::GetInstance()->AddFriendList(userinfo);

    // 认证通知可能因重连或 gRPC 重试重复到达；列表项按 uid 去重，消息则交给
    // SQLite 的 message_id 主键幂等写入。
    bool hasContactItem = false;
    for (int index = 2; index < ui->contact_list->count(); ++index) {
        auto *item = ui->contact_list->item(index);
        auto *widget = qobject_cast<ConUserWid *>(ui->contact_list->itemWidget(item));
        if (widget && widget->GetUid() == userinfo->_uid) {
            hasContactItem = true;
            break;
        }
    }
    if (!hasContactItem) {
        addConUserWid(ui->contact_list, userinfo->_uid, userinfo->_name, userinfo->_icon);
    }

    bool hasChatItem = false;
    for (int index = 0; index < ui->session_list->count(); ++index) {
        auto *item = ui->session_list->item(index);
        auto *widget = qobject_cast<ChatUserWid *>(ui->session_list->itemWidget(item));
        if (widget && widget->GetUserInfo() && widget->GetUserInfo()->_uid == userinfo->_uid) {
            hasChatItem = true;
            break;
        }
    }
    if (!hasChatItem) {
        // 好友认证携带的消息已经是服务端确认过的首条会话消息。创建列表项时就把
        // 最新一条作为摘要，避免先显示固定“你好”、稍后才被 SQLite 回写覆盖。
        std::shared_ptr<TextChatData> latestMessage;
        for (const auto &message : authResult->textMessages) {
            if (message && (!latestMessage
                            || message->GetMessageId() > latestMessage->GetMessageId())) {
                latestMessage = message;
            }
        }
        const QString preview = latestMessage ? latestMessage->GetContent() : QStringLiteral("你好");
        const QString time = latestMessage
                                 ? QDateTime::currentDateTime().toString(QStringLiteral("HH:mm"))
                                 : QString();
        const qint64 threadId = latestMessage ? latestMessage->GetThreadId() : 0;
        addChatUserWid(ui->session_list, userinfo, preview, time, false, threadId);
    }

    SaveFriendAuthMessages(userinfo, authResult->textMessages);
}

void ChatDialog::slot_text_chat_send_result(const QString &messageId, bool success)
{
    auto iter = _pending_text_items.find(messageId);
    if (iter == _pending_text_items.end()) {
        // 切换会话或 SQLite 确认重绘后，旧气泡可能已经被销毁；此时无需再更新 UI。
        return;
    }

    const QPointer<ChatItemBase> chatItem = iter.value();
    if (success) {
        _pending_text_items.erase(iter);
        return;
    }

    if (chatItem) {
        chatItem->SetSendFailed(true);
    }
    // 失败图标已经展示，不需要长期保留 UUID -> 控件映射；重试会生成新的请求状态。
    _pending_text_items.erase(iter);
}

void ChatDialog::SaveFriendAuthMessages(const std::shared_ptr<UserInfo> &friendInfo,
                                        const QList<std::shared_ptr<TextChatData>> &messages)
{
    const auto currentUser = UserMgr::GetInstance()->GetUserInfo();
    const auto localStorage = LocalChatStorageMgr::GetInstance();
    if (!friendInfo || !currentUser || !localStorage->IsReady()) {
        return;
    }

    for (const auto &message : messages) {
        if (!message || message->GetThreadId() <= 0 || message->GetMessageId() <= 0) {
            continue;
        }

        LocalChatThread thread;
        thread.threadId = message->GetThreadId();
        thread.threadType = QStringLiteral("private");
        thread.title = friendInfo->_name;
        thread.peerUid = friendInfo->_uid;
        thread.lastMessageId = message->GetMessageId();
        thread.lastMessagePreview = message->GetContent();
        // AddFriendMsg 没有 created_at 字段；以客户端收到认证通知的时间作为本地排序时间。
        thread.lastMessageAtMs = QDateTime::currentMSecsSinceEpoch();
        thread.updatedAtMs = thread.lastMessageAtMs;

        // 合并旧摘要，重复认证通知不能把较新的会话预览或未读数回退。
        for (const LocalChatThread &cachedThread : localStorage->CachedThreads()) {
            if (cachedThread.threadId != thread.threadId) {
                continue;
            }
            thread.unreadCount = cachedThread.unreadCount;
            if (cachedThread.lastMessageId > thread.lastMessageId) {
                thread.lastMessageId = cachedThread.lastMessageId;
                thread.lastMessagePreview = cachedThread.lastMessagePreview;
                thread.lastMessageAtMs = cachedThread.lastMessageAtMs;
            }
            break;
        }

        LocalChatMessage localMessage;
        localMessage.messageId = message->GetMessageId();
        localMessage.threadId = message->GetThreadId();
        localMessage.senderId = message->GetSendUid();
        localMessage.recvId = message->_to_uid;
        localMessage.contentType = QStringLiteral("text");
        localMessage.content = message->GetContent();
        localMessage.createdAtMs = QDateTime::currentMSecsSinceEpoch();
        localMessage.updatedAtMs = localMessage.createdAtMs;
        localMessage.serverStatus = 0;
        localMessage.sendState = 3;
        const bool receivedFromFriend = message->GetSendUid() != currentUser->_uid;
        localMessage.isRead = !receivedFromFriend;
        // 同一条认证消息可因 TCP/gRPC 重试再次到达；同步游标已经覆盖该 message_id
        // 时只做 SQLite 的幂等更新，不能再次累计未读数。
        const bool messageAlreadyKnown = localStorage->SyncCursors().value(thread.threadId, 0)
                                         >= message->GetMessageId();
        if (!messageAlreadyKnown && receivedFromFriend
            && (!_current_chatuser || _current_chatuser->_uid != friendInfo->_uid)) {
            ++thread.unreadCount;
        }

        if (!localStorage->SaveReceivedMessages(thread, {localMessage}, message->GetMessageId())) {
            qWarning() << "save friend authentication message failed:" << localStorage->LastError();
            continue;
        }

        // 把服务端确认的 thread_id 绑定到现有 UI 项，之后点击该好友即可进入正式会话。
        for (int index = 0; index < ui->session_list->count(); ++index) {
            auto *item = ui->session_list->item(index);
            auto *widget = qobject_cast<ChatUserWid *>(ui->session_list->itemWidget(item));
            if (widget && widget->GetUserInfo() && widget->GetUserInfo()->_uid == friendInfo->_uid) {
                widget->SetThreadId(thread.threadId);
                widget->SetChatMsg(thread.lastMessagePreview);
                widget->SetTime(QDateTime::fromMSecsSinceEpoch(thread.lastMessageAtMs)
                                    .toString(QStringLiteral("HH:mm")));
                widget->ShowRedPoint(thread.unreadCount > 0);
                break;
            }
        }
        if (_current_chatuser && _current_chatuser->_uid == friendInfo->_uid) {
            _current_thread_id = thread.threadId;
        }
    }
}

void ChatDialog::slot_create_private_chat(int uid, int otherUid, qint64 threadId)
{
    // 1028 可能在切换账号或断线重连后才到达，先确认它属于当前登录账号，
    // 防止旧会话回包污染新账号的本地缓存和聊天列表。
    const auto currentUser = UserMgr::GetInstance()->GetUserInfo();
    if (!currentUser || currentUser->_uid != uid || threadId <= 0) {
        qWarning() << "ignore private chat response for another user or invalid thread:" << uid << threadId;
        return;
    }

    const auto friends = UserMgr::GetInstance()->GetFriendList();
    const auto iter = std::find_if(friends.cbegin(), friends.cend(), [otherUid](const auto &friendInfo) {
        return friendInfo && friendInfo->_uid == otherUid;
    });
    if (iter == friends.cend()) {
        qWarning() << "private chat peer is not in local friend list:" << otherUid;
        return;
    }

    // 先持久化正式会话。之后登录时 LocalChatStorageMgr 会加载它，并把 threadId 重新绑定到列表项。
    LocalChatThread thread;
    thread.threadId = threadId;
    thread.threadType = QStringLiteral("private");
    thread.title = (*iter)->_name;
    thread.peerUid = otherUid;
    thread.updatedAtMs = QDateTime::currentMSecsSinceEpoch();
    const auto localStorage = LocalChatStorageMgr::GetInstance();
    // 重复点击“发消息”会再次收到同一 threadId。保留已有的最后消息和未读数，
    // 不能因为一次幂等创建请求把聊天列表摘要重置为空。
    for (const LocalChatThread &cachedThread : localStorage->CachedThreads()) {
        if (cachedThread.threadId == threadId) {
            thread.lastMessageId = cachedThread.lastMessageId;
            thread.lastMessagePreview = cachedThread.lastMessagePreview;
            thread.lastMessageAtMs = cachedThread.lastMessageAtMs;
            thread.unreadCount = cachedThread.unreadCount;
            break;
        }
    }
    if (localStorage->IsReady() && !localStorage->UpsertThread(thread)) {
        qWarning() << "save private chat locally failed:" << localStorage->LastError();
    }

    // 同一好友只能有一个私聊；已有临时列表项时补上 threadId，而不是重复添加 UI 项。
    ChatUserWid *chatUserWid = nullptr;
    for (int index = 0; index < ui->session_list->count(); ++index) {
        auto *item = ui->session_list->item(index);
        auto *widget = qobject_cast<ChatUserWid *>(ui->session_list->itemWidget(item));
        if (widget && widget->GetUserInfo() && widget->GetUserInfo()->_uid == otherUid) {
            chatUserWid = widget;
            break;
        }
    }
    if (chatUserWid) {
        chatUserWid->SetThreadId(threadId);
    } else {
        addChatUserWid(ui->session_list, *iter, QStringLiteral("你好"), QString(), false, threadId);
    }

    // 用户可能在等待回包期间已经停留在该好友的聊天页；此时补齐当前会话 ID，
    // 后续发送消息和拉取历史即可直接使用 _current_thread_id。
    if (_current_chatuser && _current_chatuser->_uid == otherUid) {
        _current_thread_id = threadId;
    }
}

void ChatDialog::slot_text_chat(std::shared_ptr<TextChatData> &message)
{
    if (!message) {
        return;
    }

    const auto friends = UserMgr::GetInstance()->GetFriendList();
    const auto iter = std::find_if(friends.cbegin(), friends.cend(), [message](const auto &friendInfo) {
        return friendInfo && friendInfo->_uid == message->_from_uid;
    });
    if (iter == friends.cend()) {
        qWarning() << "text chat sender is not in the local friend list, uid:" << message->_from_uid;
        return;
    }

    const auto currentUser = UserMgr::GetInstance()->GetUserInfo();
    const auto localStorage = LocalChatStorageMgr::GetInstance();
    const bool hasConfirmedServerId = message->GetThreadId() > 0
                                      && message->GetMessageId() > 0;
    if (hasConfirmedServerId && currentUser && localStorage->IsReady()) {
        // 1019 已携带服务端确认后的 message_id/thread_id。实时通知和登录增量
        // 回包可能重复或交错抵达，因此和同步回包一样统一写 SQLite，由主键与
        // 同步游标负责去重；UI 只从提交成功后的 SQLite 摘要刷新。
        LocalChatThread thread;
        thread.threadId = message->GetThreadId();
        thread.threadType = QStringLiteral("private");
        thread.title = (*iter)->_name;
        thread.peerUid = (*iter)->_uid;

        for (const LocalChatThread &cachedThread : localStorage->CachedThreads()) {
            if (cachedThread.threadId == thread.threadId) {
                thread = cachedThread;
                // 服务端消息携带的参与人优先级更高，避免旧缓存中的 peerUid 错误。
                thread.threadType = QStringLiteral("private");
                thread.title = (*iter)->_name;
                thread.peerUid = (*iter)->_uid;
                break;
            }
        }

        const qint64 receivedAtMs = message->GetCreatedAtMs() > 0
                                        ? message->GetCreatedAtMs()
                                        : QDateTime::currentMSecsSinceEpoch();
        const bool isCurrentThread = _current_chatuser
                                     && _current_chatuser->_uid == (*iter)->_uid
                                     && _current_thread_id == thread.threadId;
        const bool receivedFromFriend = message->GetSendUid() != currentUser->_uid;
        const bool messageAlreadyKnown = localStorage->SyncCursors().value(thread.threadId, 0)
                                         >= message->GetMessageId();
        if (message->GetMessageId() >= thread.lastMessageId) {
            thread.lastMessageId = message->GetMessageId();
            thread.lastMessagePreview = message->GetContent();
            thread.lastMessageAtMs = receivedAtMs;
        }
        thread.updatedAtMs = receivedAtMs;
        if (!messageAlreadyKnown && receivedFromFriend && !isCurrentThread) {
            ++thread.unreadCount;
        }

        LocalChatMessage localMessage;
        localMessage.messageId = message->GetMessageId();
        localMessage.threadId = thread.threadId;
        localMessage.senderId = message->GetSendUid();
        localMessage.recvId = message->_to_uid;
        localMessage.contentType = QStringLiteral("text");
        localMessage.content = message->GetContent();
        localMessage.createdAtMs = receivedAtMs;
        localMessage.updatedAtMs = receivedAtMs;
        localMessage.serverStatus = message->GetStatus();
        localMessage.sendState = 3;
        // 用户正在查看此正式会话时，这条消息无需短暂计为未读；否则保留未读状态。
        localMessage.isRead = !receivedFromFriend || isCurrentThread;

        if (!localStorage->SaveReceivedMessages(thread, {localMessage},
                                                message->GetMessageId())) {
            qWarning() << "save real-time text chat message failed:" << localStorage->LastError();
            return;
        }
        emit TcpMgr::GetInstance()->sig_local_chat_synced(thread.threadId);
        return;
    }

    // 旧 1019 没有 server message_id/thread_id，无法安全地落入 SQLite（会和
    // 增量同步的主键冲突），继续维持旧的内存通知与展示行为。
    if (!_current_chatuser || _current_chatuser->_uid != message->_from_uid) {
        _unread_text_messages[message->_from_uid].append(message);
        UpdateChatSessionPreview(*iter, message->_msg_content, true);
        qInfo() << "text chat stored as unread, from:" << message->_from_uid;
        return;
    }

    UpdateChatSessionPreview(*iter, message->_msg_content, false);
    AppendReceivedTextMessage(message, *iter);
}

void ChatDialog::AppendReceivedTextMessage(const std::shared_ptr<TextChatData> &message,
                                           const std::shared_ptr<UserInfo> &sender)
{
    if (!message || !sender) {
        return;
    }

    auto *chatItem = new ChatItemBase(ChatRole::Other);
    chatItem->setUserName(sender->_name);
    chatItem->setUserAvatar(sender->_uid, sender->_icon);
    chatItem->setWidget(new TextBubble(ChatRole::Other, message->_msg_content));
    ui->chat_data->appendChatItem(chatItem);
}

void ChatDialog::slot_local_chat_synced(qint64 threadId)
{
    const auto storage = LocalChatStorageMgr::GetInstance();
    if (threadId <= 0 || !storage->IsReady()) {
        return;
    }

    // SQLite 是消息与摘要的唯一事实来源。每次同步完成后从缓存摘要刷新 UI，
    // 避免 TCP 分页回包顺序不同导致左侧列表显示了过期的最后消息。
    LocalChatThread thread;
    bool found = false;
    for (const LocalChatThread &cachedThread : storage->CachedThreads()) {
        if (cachedThread.threadId == threadId) {
            thread = cachedThread;
            found = true;
            break;
        }
    }
    if (!found) {
        return;
    }

    bool hasSessionItem = false;
    for (int index = 0; index < ui->session_list->count(); ++index) {
        auto *item = ui->session_list->item(index);
        auto *widget = qobject_cast<ChatUserWid *>(ui->session_list->itemWidget(item));
        if (widget && widget->GetThreadId() == threadId) {
            widget->SetChatMsg(thread.lastMessagePreview);
            widget->SetTime(thread.lastMessageAtMs > 0
                                ? QDateTime::fromMSecsSinceEpoch(thread.lastMessageAtMs)
                                      .toString(QStringLiteral("HH:mm"))
                                : QString());
            widget->ShowRedPoint(thread.unreadCount > 0);
            hasSessionItem = true;
            break;
        }
    }

    // 实时 1019 可能早于会话列表回包到达。只要好友资料已在本地，就在 SQLite
    // 事务完成后补建列表项，避免消息已落库但用户在左侧看不到这段正式会话。
    if (!hasSessionItem && thread.threadType == QStringLiteral("private")) {
        const auto friends = UserMgr::GetInstance()->GetFriendList();
        const auto friendIter = std::find_if(friends.cbegin(), friends.cend(),
                                             [&thread](const auto &friendInfo) {
            return friendInfo && friendInfo->_uid == thread.peerUid;
        });
        if (friendIter != friends.cend()) {
            addChatUserWid(ui->session_list, *friendIter, thread.lastMessagePreview,
                           thread.lastMessageAtMs > 0
                               ? QDateTime::fromMSecsSinceEpoch(thread.lastMessageAtMs)
                                     .toString(QStringLiteral("HH:mm"))
                               : QString(),
                           thread.unreadCount > 0, thread.threadId);
        }
    }

    if (_current_chatuser && _current_thread_id == threadId) {
        // 当前会话已经可见时重新从 SQLite 读取，确保增量消息立即显示，同时不会
        // 依赖网络回包中的临时对象跨线程保存。
        SetCurrentChatUser(_current_chatuser, threadId);
    }
}

void ChatDialog::AppendStoredTextMessage(const LocalChatMessage &message,
                                         const std::shared_ptr<UserInfo> &friendInfo)
{
    if (auto *chatItem = CreateStoredTextChatItem(message, friendInfo)) {
        ui->chat_data->appendChatItem(chatItem);
    }
}

ChatItemBase *ChatDialog::CreateStoredTextChatItem(const LocalChatMessage &message,
                                                    const std::shared_ptr<UserInfo> &friendInfo)
{
    const auto self = UserMgr::GetInstance()->GetUserInfo();
    if (!self || !friendInfo) {
        return nullptr;
    }

    // 历史消息由 sender_id 决定左右侧，而不是由当前打开账号推测；这样离线同步到的
    // 自己发送消息也会保持右侧气泡，好友发送消息保持左侧气泡。
    const bool sentBySelf = message.senderId == self->_uid;
    const auto &displayUser = sentBySelf ? self : friendInfo;
    auto *chatItem = new ChatItemBase(sentBySelf ? ChatRole::Self : ChatRole::Other);
    chatItem->setUserName(displayUser->_name);
    chatItem->setUserAvatar(displayUser->_uid, displayUser->_icon);
    chatItem->setWidget(new TextBubble(sentBySelf ? ChatRole::Self : ChatRole::Other,
                                       message.content));
    return chatItem;
}

void ChatDialog::slot_load_older_local_messages()
{
    LoadOlderLocalMessages();
}

void ChatDialog::LoadOlderLocalMessages()
{
    const auto storage = LocalChatStorageMgr::GetInstance();
    if (_loading_older_local_history || !_has_more_local_history || _current_thread_id <= 0
        || _oldest_local_message_id <= 0 || !_current_chatuser || !storage->IsReady()) {
        return;
    }

    _loading_older_local_history = true;
    const qint64 loadingThreadId = _current_thread_id;
    const QList<LocalChatMessage> olderMessages = storage->LoadMessagesBefore(
        loadingThreadId, _oldest_local_message_id, kLocalHistoryPageSize);
    // SQLite 查询目前在 UI 线程同步完成，但仍保留会话 ID 校验；后续若改成异步任务，
    // 用户在查询期间切换会话时不会把 A 的历史插入 B 的窗口。
    if (loadingThreadId != _current_thread_id) {
        _loading_older_local_history = false;
        return;
    }
    if (olderMessages.isEmpty()) {
        _has_more_local_history = false;
        _loading_older_local_history = false;
        return;
    }

    QList<QWidget *> historyItems;
    for (const LocalChatMessage &message : olderMessages) {
        if (message.contentType == QStringLiteral("text")) {
            if (auto *chatItem = CreateStoredTextChatItem(message, _current_chatuser)) {
                historyItems.append(chatItem);
            }
        }
    }
    if (!historyItems.isEmpty()) {
        ui->chat_data->prependChatItems(historyItems);
    }
    // LoadMessagesBefore 的结果按 message_id 正序返回，首项就是下一次分页边界。
    _oldest_local_message_id = olderMessages.first().messageId;
    _has_more_local_history = olderMessages.size() >= kLocalHistoryPageSize;
    _loading_older_local_history = false;
}

void ChatDialog::UpdateChatSessionPreview(const std::shared_ptr<UserInfo> &userInfo,
                                          const QString &message, bool unread)
{
    if (!userInfo) {
        return;
    }

    for (int index = 0; index < ui->session_list->count(); ++index) {
        auto *item = ui->session_list->item(index);
        auto *widget = qobject_cast<ChatUserWid *>(ui->session_list->itemWidget(item));
        if (widget && widget->GetUserInfo() && widget->GetUserInfo()->_uid == userInfo->_uid) {
            // 会话列表只保留最新一条摘要；每次收到该会话的新消息时同步刷新显示时间。
            widget->SetChatMsg(message);
            widget->SetTime(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm")));
            widget->ShowRedPoint(unread);
            return;
        }
    }

    // 首次收到消息时创建会话项，摘要和时间必须同时初始化，不能只显示联系人信息。
    addChatUserWid(ui->session_list, userInfo, message,
                   QDateTime::currentDateTime().toString(QStringLiteral("HH:mm")), unread);
}

void ChatDialog::SetCurrentChatUser(const std::shared_ptr<UserInfo> &chatUser, qint64 threadId)
{
    if (!chatUser) {
        return;
    }
    const auto storage = LocalChatStorageMgr::GetInstance();
    // 从联系人页进入时没有 ChatUserWid 传来的 threadId，按 peerUid 在本地摘要中补齐。
    if (threadId <= 0 && storage->IsReady()) {
        for (const LocalChatThread &thread : storage->CachedThreads()) {
            if (thread.threadType == QStringLiteral("private") && thread.peerUid == chatUser->_uid) {
                threadId = thread.threadId;
                break;
            }
        }
    }
    _current_chatuser = chatUser;
    _current_thread_id = threadId;
    _oldest_local_message_id = 0;
    _has_more_local_history = false;
    _loading_older_local_history = false;
    ui->chat_title_label->setText(_current_chatuser->_name);
    ui->chat_stack->setCurrentWidget(ui->chat_page);

    // 会话切换时必须先移除旧气泡，再按 message_id 正序读取本地最近记录；
    // LocalChatStorageMgr 已在 SQL 中完成分页和排序，界面层不再自行倒序处理。
    ui->chat_data->ClearChatItems();
    if (threadId > 0 && storage->IsReady()) {
        const QList<LocalChatMessage> recentMessages = storage->LoadRecentMessages(
            threadId, kLocalHistoryPageSize);
        for (const LocalChatMessage &message : recentMessages) {
            if (message.contentType == QStringLiteral("text")) {
                AppendStoredTextMessage(message, _current_chatuser);
            }
        }
        if (!recentMessages.isEmpty()) {
            _oldest_local_message_id = recentMessages.first().messageId;
            // 满一页仅表示“可能还有更早记录”；到顶部再查一页，空结果才最终停止。
            _has_more_local_history = recentMessages.size() >= kLocalHistoryPageSize;
        }
        // 此时用户已经进入该会话，SQLite 中此前未读的历史消息应被标记已读。
        if (!storage->MarkThreadRead(threadId)) {
            qWarning() << "mark local chat thread read failed:" << storage->LastError();
        }
    }

    const auto unread = _unread_text_messages.take(_current_chatuser->_uid);
    for (const auto &message : unread) {
        AppendReceivedTextMessage(message, _current_chatuser);
    }
    for (int index = 0; index < ui->session_list->count(); ++index) {
        auto *item = ui->session_list->item(index);
        auto *widget = qobject_cast<ChatUserWid *>(ui->session_list->itemWidget(item));
        if (widget && widget->GetUserInfo() && widget->GetUserInfo()->_uid == _current_chatuser->_uid) {
            widget->ShowRedPoint(false);
            break;
        }
    }
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
