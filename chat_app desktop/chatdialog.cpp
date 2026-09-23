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
#include <QDebug>
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
#include "chatimagetransfer.h"
#include <algorithm>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMessageBox>
#include <QFileDialog>
#include <QPushButton>
#include <QFileInfo>

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

    // 聊天图片任务集中复用 ResourceClient。任务内部会串行分片，UI 只关心上传完成、
    // 发送确认和最终原子落盘后的图片路径。
    _image_transfer = new ChatImageTransferTask(this);
    connect(_image_transfer, &ChatImageTransferTask::uploadFinished, this,
            [this](const ImageChatData &image) {
        if (!TcpMgr::GetInstance()->IsConnected()) {
            const auto item = _pending_image_items.take(image.msgId);
            if (item) {
                item->SetSendFailed(true);
            }
            return;
        }
        QJsonObject imageObject;
        imageObject["msgid"] = image.msgId;
        imageObject["resource_id"] = image.resourceId;
        imageObject["name"] = image.name;
        imageObject["mime_type"] = image.mimeType;
        imageObject["file_size"] = static_cast<double>(image.fileSize);
        imageObject["width"] = image.width;
        imageObject["height"] = image.height;
        QJsonObject envelope;
        envelope["fromuid"] = image.fromUid;
        envelope["touid"] = image.toUid;
        envelope["imageArray"] = QJsonArray{imageObject};
        // 这里只发小型 JSON 引用；图片的原始二进制已经由 ResourceClient 上传完成。
        emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_IMAGE_CHAT_MSG_REQ,
            QJsonDocument(envelope).toJson(QJsonDocument::Compact));
    });
    connect(_image_transfer, &ChatImageTransferTask::fileUploadFinished, this,
            [this](const FileChatData &file) {
        if (!TcpMgr::GetInstance()->IsConnected()) {
            if (auto item = _pending_file_items.value(file.msgId)) item->SetSendFailed(true);
            _failed_file_msg_ids.insert(file.msgId);
            return;
        }
        _pending_file_metadata.insert(file.msgId, file);
        QJsonObject item;
        // ResourceServer 已完成字节上传，此处只通过 1040 发送文件引用。聊天 TCP 包中
        // 不包含本地路径和文件内容，fileArray 字段与服务端文件消息合同保持一致。
        item["msgid"] = file.msgId; item["resource_id"] = file.resourceId;
        item["name"] = file.name; item["mime_type"] = file.mimeType;
        item["file_size"] = static_cast<double>(file.fileSize);
        QJsonObject envelope;
        envelope["fromuid"] = file.fromUid; envelope["touid"] = file.toUid;
        envelope["fileArray"] = QJsonArray{item};
        emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_FILE_CHAT_MSG_REQ,
            QJsonDocument(envelope).toJson(QJsonDocument::Compact));
    });
    connect(_image_transfer, &ChatImageTransferTask::fileDownloadFailed, this,
            [this](const FileChatData &file, const QString &reason) {
        QMessageBox::warning(this, tr("文件下载失败"), reason);
        qWarning() << "chat file download failed:" << file.resourceId << reason;
    });
    connect(_image_transfer, &ChatImageTransferTask::fileDownloadFinished, this,
            [this](const FileChatData &file, const QString &path) {
        QMessageBox::information(this, tr("文件已保存"), tr("文件已保存到：\n%1").arg(path));
        qInfo() << "chat file saved:" << file.resourceId << path;
    });
    connect(_image_transfer, &ChatImageTransferTask::uploadFailed, this,
            [this](const QString &msgId, const QString &message) {
        qWarning() << "chat image upload failed:" << message;
        auto item = _pending_image_items.take(msgId);
        if (!item) {
            item = _pending_file_items.value(msgId);
            if (item) _failed_file_msg_ids.insert(msgId);
        }
        if (item) {
            item->SetSendFailed(true);
        }
    });
    connect(_image_transfer, &ChatImageTransferTask::imageReady, this,
            [this](const ImageChatData &image, const QString &localPath) {
        appendDownloadedImage(image, localPath);
    });
    connect(_image_transfer, &ChatImageTransferTask::imageDownloadFailed, this,
            [this](const ImageChatData &image, const QString &message) {
        qWarning() << "chat image download failed:" << message;
        // 历史图片下载失败后撤销“已请求”标记；用户重新进入会话时可以再次尝试，
        // 不会因为一次网络波动永久卡在空白消息状态。
        if (image.messageId > 0) {
            _requested_image_message_ids.remove(image.messageId);
            const auto placeholder = _image_placeholder_items.value(image.messageId);
            if (placeholder) {
                const auto self = UserMgr::GetInstance()->GetUserInfo();
                const ChatRole role = self && image.fromUid == self->_uid
                    ? ChatRole::Self : ChatRole::Other;
                placeholder->setWidget(new TextBubble(role, tr("[图片加载失败，点击重试对话框后可重新下载]")));
            }
        }
        const auto item = _pending_image_items.take(image.msgId);
        if (item) {
            item->SetSendFailed(true);
        }

        // 收到图片尚未有气泡可标记为失败；只要它属于正在查看的会话，就立即给出
        // 原因和重试入口。后台会话不弹窗打断用户，稍后进入该会话仍会重新排队下载。
        if (image.threadId != _current_thread_id || image.threadId <= 0) {
            return;
        }
        const auto answer = QMessageBox::question(
            this, tr("图片下载失败"),
            tr("%1\n\n请检查登录状态或网络后重试。").arg(message),
            QMessageBox::Retry | QMessageBox::Cancel, QMessageBox::Retry);
        if (answer == QMessageBox::Retry) {
            if (image.messageId > 0) {
                _requested_image_message_ids.insert(image.messageId);
                const auto placeholder = _image_placeholder_items.value(image.messageId);
                if (placeholder) {
                    const auto self = UserMgr::GetInstance()->GetUserInfo();
                    const ChatRole role = self && image.fromUid == self->_uid
                        ? ChatRole::Self : ChatRole::Other;
                    placeholder->setWidget(new TextBubble(role, tr("[图片加载中…]")));
                }
            }
            _image_transfer->enqueueDownload(image);
        }
    });

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
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_image_chat_send_result,
            this, &ChatDialog::slot_image_chat_send_result);
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_image_chat,
            this, &ChatDialog::slot_image_chat);
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_file_chat_send_result,
            this, &ChatDialog::slot_file_chat_send_result);
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_file_chat,
            this, &ChatDialog::slot_file_chat);
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_local_chat_synced,
            this, &ChatDialog::slot_local_chat_synced);
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_message_delivery_updated,
            this, &ChatDialog::slot_message_delivery_updated);
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
        ImageChatData imageToUpload;
        bool needsImageUpload = false;
        bool needsFileUpload = false;
        FileChatData fileToUpload;

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
            imageToUpload.msgId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            imageToUpload.fromUid = userinfo->_uid;
            imageToUpload.toUid = _current_chatuser->_uid;
            _pending_image_items.insert(imageToUpload.msgId, pChatItem);
            needsImageUpload = true;
        }
        else if(type == "file")
        {
            fileToUpload.msgId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            fileToUpload.fromUid = userinfo->_uid;
            fileToUpload.toUid = _current_chatuser->_uid;
            fileToUpload.name = QFileInfo(msgList[i].content).fileName();
            fileToUpload.fileSize = QFileInfo(msgList[i].content).size();
            _pending_file_items.insert(fileToUpload.msgId, pChatItem);
            _pending_file_metadata.insert(fileToUpload.msgId, fileToUpload);
            _pending_file_source_paths.insert(fileToUpload.msgId, msgList[i].content);
            auto *fileCard = new QPushButton(
                tr("文件：%1\n%2 字节 · 点击选择保存位置")
                    .arg(fileToUpload.name).arg(fileToUpload.fileSize));
            fileCard->setMinimumSize(230, 72);
            fileCard->setStyleSheet(QStringLiteral(
                "QPushButton { text-align:left; padding:10px; border:1px solid #c8d3df; "
                "border-radius:6px; background:#f5f8fb; color:#263746; } "
                "QPushButton:hover { background:#e9f1f8; }"));
            const QString fileMsgId = fileToUpload.msgId;
            connect(fileCard, &QPushButton::clicked, this, [this, fileMsgId] {
                const auto it = _pending_file_metadata.constFind(fileMsgId);
                if (_failed_file_msg_ids.contains(fileMsgId)) {
                    if (QMessageBox::question(this, tr("重试发送文件"), tr("文件发送失败，是否重试？"),
                                              QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
                        const auto source = _pending_file_source_paths.constFind(fileMsgId);
                        if (source != _pending_file_source_paths.cend()) {
                            _failed_file_msg_ids.remove(fileMsgId);
                            if (auto row = _pending_file_items.value(fileMsgId)) row->SetSendFailed(false);
                            _image_transfer->enqueueFileUpload(*source, _pending_file_metadata.value(fileMsgId));
                        }
                    }
                    return;
                }
                if (it == _pending_file_metadata.cend() || it->resourceId.isEmpty()
                    || it->threadId <= 0) {
                    QMessageBox::information(this, tr("文件处理中"),
                                             tr("等待 ChatServer 确认文件消息后即可下载。"));
                    return;
                }
                const QString path = QFileDialog::getSaveFileName(this, tr("保存文件"), it->name);
                if (!path.isEmpty()) _image_transfer->enqueueFileDownload(*it, path);
            });
            pBubble = fileCard;
            needsFileUpload = true;
        }
        if(pBubble != nullptr)
        {
            pChatItem->setWidget(pBubble);
            ui->chat_data->appendChatItem(pChatItem);
        }
        if (needsImageUpload) {
            // 先展示本地预览，再排队上传；任务失败会把同一行标红，绝不把本地路径
            // 或图片字节拼入 1033 的聊天 TCP 包。
            _image_transfer->enqueueUpload(msgList[i].content, imageToUpload);
        }
        if (needsFileUpload) {
            _image_transfer->enqueueFileUpload(msgList[i].content, fileToUpload);
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

void ChatDialog::slot_text_chat_send_result(const QString &messageId, bool success, int deliveryState)
{
    auto iter = _pending_text_items.find(messageId);
    if (iter == _pending_text_items.end()) {
        // 切换会话或 SQLite 确认重绘后，旧气泡可能已经被销毁；此时无需再更新 UI。
        return;
    }

    const QPointer<ChatItemBase> chatItem = iter.value();
    if (success) {
        if (chatItem) {
            chatItem->SetDeliveryState(deliveryState > 0 ? deliveryState : 1);
        }
        _pending_text_items.erase(iter);
        return;
    }

    if (chatItem) {
        chatItem->SetSendFailed(true);
    }
    // 失败图标已经展示，不需要长期保留 UUID -> 控件映射；重试会生成新的请求状态。
    _pending_text_items.erase(iter);
}

void ChatDialog::slot_image_chat_send_result(std::shared_ptr<ImageChatData> &image, bool success)
{
    if (!image) {
        return;
    }
    auto iter = _pending_image_items.find(image->msgId);
    if (!success) {
        if (iter != _pending_image_items.end()) {
            if (iter.value()) {
                iter.value()->SetSendFailed(true);
            }
            _pending_image_items.erase(iter);
        }
        return;
    }
    if (iter != _pending_image_items.end() && iter.value()) {
        iter.value()->SetDeliveryState(image->deliveryState);
        if (image->messageId > 0) {
            _outgoing_image_items.insert(image->messageId, iter.value());
        }
    }
    // 1034 成功后仍走下载缓存：发送端也能验证 resource_id 可读取，并把之后历史/重绘
    // 可用的稳定本地文件保存下来。appendDownloadedImage 会识别 pending 项，不重复插入气泡。
    _image_transfer->enqueueDownload(*image);
}

void ChatDialog::slot_image_chat(std::shared_ptr<ImageChatData> &image)
{
    if (!image) {
        return;
    }
    const auto currentUser = UserMgr::GetInstance()->GetUserInfo();
    const auto storage = LocalChatStorageMgr::GetInstance();
    const auto friends = UserMgr::GetInstance()->GetFriendList();
    const auto peer = std::find_if(friends.cbegin(), friends.cend(), [&image](const auto &user) {
        return user && user->_uid == image->fromUid;
    });
    if (!currentUser || image->toUid != currentUser->_uid || peer == friends.cend()
        || !storage->IsReady() || image->threadId <= 0 || image->messageId <= 0) {
        qWarning() << "ignore image notification missing private-chat metadata";
        return;
    }

    // 1035 与文本、文件实时通知使用同一条“先落 SQLite，再刷新 UI”路径。这样通知属于
    // 未打开会话时也不会因下载回调被丢弃；稍后进入会话仍能从正式历史创建图片占位行。
    LocalChatThread thread;
    thread.threadId = image->threadId;
    thread.threadType = QStringLiteral("private");
    thread.title = (*peer)->_name;
    thread.peerUid = (*peer)->_uid;
    for (const LocalChatThread &cached : storage->CachedThreads()) {
        if (cached.threadId == thread.threadId) {
            thread = cached;
            thread.threadType = QStringLiteral("private");
            thread.title = (*peer)->_name;
            thread.peerUid = (*peer)->_uid;
            break;
        }
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 cursor = storage->SyncCursors().value(thread.threadId, 0);
    const bool messageAlreadyKnown = storage->HasMessage(image->messageId);
    const bool currentThread = _current_chatuser && _current_chatuser->_uid == image->fromUid
        && _current_thread_id == thread.threadId;
    if (image->messageId >= thread.lastMessageId) {
        thread.lastMessageId = image->messageId;
        thread.lastMessagePreview = QStringLiteral("[图片]");
        thread.lastMessageAtMs = now;
    }
    thread.updatedAtMs = now;
    if (!messageAlreadyKnown && !currentThread) ++thread.unreadCount;

    QJsonObject metadata;
    metadata["msgid"] = image->msgId;
    metadata["resource_id"] = image->resourceId;
    metadata["name"] = image->name;
    metadata["mime_type"] = image->mimeType;
    metadata["file_size"] = static_cast<double>(image->fileSize);
    metadata["width"] = image->width;
    metadata["height"] = image->height;
    metadata["message_id"] = QString::number(image->messageId);
    metadata["thread_id"] = QString::number(image->threadId);
    LocalChatMessage stored;
    stored.messageId = image->messageId;
    stored.threadId = image->threadId;
    stored.senderId = image->fromUid;
    stored.recvId = image->toUid;
    stored.contentType = QStringLiteral("image");
    stored.content = QString::fromUtf8(QJsonDocument(metadata).toJson(QJsonDocument::Compact));
    stored.createdAtMs = now;
    stored.updatedAtMs = now;
    stored.serverStatus = 0;
    stored.sendState = 3;
    stored.deliveryState = image->deliveryState;
    stored.isRead = currentThread;
    // 实时 1035 可能先于登录 1030 到达，只保存消息并维持原同步游标；否则单条较新的
    // 实时消息会让 1029 跳过尚未补齐的较早消息，表现为接收端“吞消息”。
    if (!storage->SaveReceivedMessages(thread, {stored}, cursor)) {
        qWarning() << "save real-time image chat message failed:" << storage->LastError();
        return;
    }
    emit TcpMgr::GetInstance()->sig_local_chat_synced(thread.threadId);
}

void ChatDialog::slot_file_chat_send_result(std::shared_ptr<FileChatData> &file, bool success)
{
    // 1041 用客户端 msgid 找回乐观文件卡片。失败时保留源路径和卡片供用户点击重试；
    // 成功时使用服务端正式 messageId/threadId 更新投递状态和后续手动下载权限。
    if (!file) return;
    const auto item = _pending_file_items.value(file->msgId);
    if (!success) {
        if (item) item->SetSendFailed(true);
        _failed_file_msg_ids.insert(file->msgId);
        return;
    }
    if (item) item->SetDeliveryState(file->deliveryState > 0 ? file->deliveryState : 1);
    // 1041 中的正式 threadId/messageId 原样保留；缺失时不从当前窗口猜测会话归属。
    _pending_file_metadata.insert(file->msgId, *file);
    _failed_file_msg_ids.remove(file->msgId);
    _pending_file_source_paths.remove(file->msgId);
    _pending_file_items.remove(file->msgId);
    if (item && file->messageId > 0) _outgoing_file_items.insert(file->messageId, item);
}

void ChatDialog::slot_file_chat(std::shared_ptr<FileChatData> &file)
{
    if (!file) return;
    const auto currentUser = UserMgr::GetInstance()->GetUserInfo();
    const auto localStorage = LocalChatStorageMgr::GetInstance();
    const auto friends = UserMgr::GetInstance()->GetFriendList();
    const auto peer = std::find_if(friends.cbegin(), friends.cend(), [&file](const auto &user) {
        return user && user->_uid == file->fromUid;
    });
    if (!currentUser || file->toUid != currentUser->_uid || peer == friends.cend()
        || !localStorage->IsReady() || file->threadId <= 0 || file->messageId <= 0) {
        qWarning() << "ignore file notification missing private-chat metadata";
        return;
    }
    // 与实时文本通知一样，事务同时保存消息、会话摘要和同步游标；UI 是否打开此会话
    // 不影响落库。当前会话由 sig_local_chat_synced 从 SQLite 重绘，其他会话只刷新摘要和未读数。
    LocalChatThread thread;
    thread.threadId = file->threadId;
    thread.threadType = QStringLiteral("private");
    thread.title = (*peer)->_name;
    thread.peerUid = (*peer)->_uid;
    for (const LocalChatThread &cached : localStorage->CachedThreads()) {
        if (cached.threadId == thread.threadId) {
            thread = cached;
            thread.threadType = QStringLiteral("private");
            thread.title = (*peer)->_name;
            thread.peerUid = (*peer)->_uid;
            break;
        }
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 cursor = localStorage->SyncCursors().value(thread.threadId, 0);
    const bool messageAlreadyKnown = localStorage->HasMessage(file->messageId);
    const bool currentThread = _current_chatuser && _current_chatuser->_uid == file->fromUid
        && _current_thread_id == thread.threadId;
    if (file->messageId >= thread.lastMessageId) {
        thread.lastMessageId = file->messageId;
        thread.lastMessagePreview = QStringLiteral("[文件]");
        thread.lastMessageAtMs = now;
    }
    thread.updatedAtMs = now;
    if (!messageAlreadyKnown && !currentThread) ++thread.unreadCount;
    QJsonObject metadata;
    metadata["msgid"] = file->msgId;
    metadata["resource_id"] = file->resourceId;
    metadata["name"] = file->name;
    metadata["mime_type"] = file->mimeType;
    metadata["file_size"] = static_cast<double>(file->fileSize);
    metadata["message_id"] = QString::number(file->messageId);
    metadata["thread_id"] = QString::number(file->threadId);
    LocalChatMessage stored;
    stored.messageId = file->messageId;
    stored.threadId = file->threadId;
    stored.senderId = file->fromUid;
    stored.recvId = file->toUid;
    stored.contentType = QStringLiteral("file");
    stored.content = QString::fromUtf8(QJsonDocument(metadata).toJson(QJsonDocument::Compact));
    stored.createdAtMs = now;
    stored.updatedAtMs = now;
    stored.serverStatus = 0;
    stored.sendState = 3;
    stored.isRead = currentThread;
    // 1042 只负责实时加速，不能宣称更早历史已经连续同步完成；游标仅由 1030 推进。
    if (!localStorage->SaveReceivedMessages(thread, {stored}, cursor)) {
        qWarning() << "save real-time file chat message failed:" << localStorage->LastError();
        return;
    }
    emit TcpMgr::GetInstance()->sig_local_chat_synced(thread.threadId);
}

ChatItemBase *ChatDialog::CreateFileChatItem(const FileChatData &file)
{
    // 文件卡片只展示已核验的元数据。点击前不请求 ResourceServer；点击后先让用户选择
    // 保存路径，再把原始 FileChatData 和目标路径交给串行传输任务。
    const auto self = UserMgr::GetInstance()->GetUserInfo();
    if (!self) return nullptr;
    const bool sentBySelf = file.fromUid == self->_uid;
    const auto friends = UserMgr::GetInstance()->GetFriendList();
    const auto peer = std::find_if(friends.cbegin(), friends.cend(), [sentBySelf, &file](const auto &user) {
        return user && user->_uid == (sentBySelf ? file.toUid : file.fromUid);
    });
    if (!sentBySelf && peer == friends.cend()) return nullptr;
    const auto &display = sentBySelf ? self : *peer;
    auto *row = new ChatItemBase(sentBySelf ? ChatRole::Self : ChatRole::Other);
    row->setUserName(display->_name);
    row->setUserAvatar(display->_uid, display->_icon);
    auto *card = new QPushButton(tr("文件：%1\n%2 字节 · 点击选择保存位置")
                                     .arg(file.name).arg(file.fileSize));
    card->setMinimumSize(230, 72);
    card->setStyleSheet(QStringLiteral(
        "QPushButton { text-align:left; padding:10px; border:1px solid #c8d3df; "
        "border-radius:6px; background:#f5f8fb; color:#263746; } "
        "QPushButton:hover { background:#e9f1f8; }"));
    connect(card, &QPushButton::clicked, this, [this, file] {
        // 只有用户点击卡片并确认保存路径后，才向 ResourceServer 请求任何文件分片。
        const QString path = QFileDialog::getSaveFileName(this, tr("保存文件"), file.name);
        if (!path.isEmpty()) _image_transfer->enqueueFileDownload(file, path);
    });
    row->setWidget(card);
    if (sentBySelf) row->SetDeliveryState(file.deliveryState > 0 ? file.deliveryState : 1);
    return row;
}

bool ChatDialog::ParseStoredFileMessage(const LocalChatMessage &message, FileChatData *file) const
{
    // SQLite content 保存的是 1030/1042 的紧凑文件元数据 JSON。这里独立解析文件字段，
    // 不复用图片解析器，也不要求 width/height，messageId/threadId 以消息列为准。
    if (!file || message.contentType != QStringLiteral("file") || message.messageId <= 0
        || message.threadId <= 0) return false;
    const auto doc = QJsonDocument::fromJson(message.content.toUtf8());
    if (!doc.isObject()) return false;
    const auto item = doc.object();
    bool sizeOk = false;
    const qint64 size = item.value("file_size").toVariant().toLongLong(&sizeOk);
    file->messageId = message.messageId; file->threadId = message.threadId;
    file->msgId = item.value("msgid").toString();
    file->resourceId = item.value("resource_id").toString();
    file->name = item.value("name").toString();
    file->mimeType = item.value("mime_type").toString();
    file->fileSize = size; file->fromUid = static_cast<int>(message.senderId);
    file->toUid = static_cast<int>(message.recvId);
    return sizeOk && size > 0 && size <= 100LL * 1024 * 1024
        && !file->msgId.isEmpty() && !file->resourceId.isEmpty() && !file->name.isEmpty()
        && file->fromUid > 0 && file->toUid > 0;
}

void ChatDialog::QueueOrAppendStoredFile(const LocalChatMessage &message)
{
    FileChatData file;
    if (!ParseStoredFileMessage(message, &file)) {
        qWarning() << "ignore invalid cached file message:" << message.messageId;
        return;
    }
    // 历史只显示元数据卡片；下载由卡片点击触发，不在会话打开或同步时自动拉取。
    // 卡片实际 append 后即可发送 1036，因为“已显示”描述 UI 可见状态，与文件下载无关。
    if (auto *row = CreateFileChatItem(file)) {
        ui->chat_data->appendChatItem(row);
        const auto self = UserMgr::GetInstance()->GetUserInfo();
        if (self && file.fromUid != self->_uid)
            SendDisplayAcknowledgement(file.threadId, {file.messageId});
    }
}

void ChatDialog::appendDownloadedImage(const ImageChatData &image, const QString &localPath)
{
    // 自己发送的预览已经在 1033 前插入；资源缓存完成只解除 pending 状态，不能再追加一行。
    auto pending = _pending_image_items.find(image.msgId);
    if (pending != _pending_image_items.end()) {
        const bool optimisticPreviewStillVisible = !pending.value().isNull();
        _pending_image_items.erase(pending);
        if (optimisticPreviewStillVisible) return;
        // 同步刷新可能已删除原乐观气泡并从 SQLite 建立了正式占位行。此时 QPointer
        // 已为空，下载结果必须继续向下替换正式占位行，否则这条图片会永久停在加载中。
    }

    const auto self = UserMgr::GetInstance()->GetUserInfo();
    const bool sentBySelf = self && image.fromUid == self->_uid;
    const bool belongsToCurrentChat = self && _current_chatuser
        && ((sentBySelf && image.toUid == _current_chatuser->_uid)
            || (!sentBySelf && image.toUid == self->_uid
                && image.fromUid == _current_chatuser->_uid));
    if (!belongsToCurrentChat) {
        // 当前没有打开该会话时不应把图片插入其他聊天窗口；日志给出丢弃原因，后续可
        // 通过聊天历史同步补齐该消息。
        qInfo() << "downloaded image is not for current chat view, self="
                << (self ? self->_uid : 0) << "current_peer="
                << (_current_chatuser ? _current_chatuser->_uid : 0)
                << "from=" << image.fromUid << "to=" << image.toUid;
        return;
    }
    if (image.threadId > 0 && image.threadId != _current_thread_id) {
        // 用户在下载期间切到其他会话时，不把 A 会话的图片错误追加到 B 会话。
        return;
    }
    if (image.messageId > 0 && _displayed_image_message_ids.contains(image.messageId)) {
        return;
    }
    const auto friends = UserMgr::GetInstance()->GetFriendList();
    const auto friendIter = std::find_if(friends.cbegin(), friends.cend(), [&image](const auto &friendInfo) {
        return friendInfo && friendInfo->_uid == image.fromUid;
    });
    if (!sentBySelf && friendIter == friends.cend()) {
        qWarning() << "ignore image from a non-friend:" << image.fromUid;
        return;
    }
    const QPixmap pixmap(localPath);
    if (pixmap.isNull()) {
        qWarning() << "downloaded image cannot be loaded:" << localPath;
        return;
    }
    const ChatRole role = sentBySelf ? ChatRole::Self : ChatRole::Other;
    auto chatItem = _image_placeholder_items.take(image.messageId);
    if (chatItem) {
        // 历史加载时已经按 message_id 顺序插入占位行，下载完成只原位替换内容，
        // 不能再次 append，否则所有异步图片都会堆到聊天窗口最底部。
        chatItem->setWidget(new PictureBubble(role, pixmap));
    } else {
        const auto &displayUser = sentBySelf ? self : *friendIter;
        chatItem = new ChatItemBase(role);
        chatItem->setUserName(displayUser->_name);
        chatItem->setUserAvatar(displayUser->_uid, displayUser->_icon);
        chatItem->setWidget(new PictureBubble(role, pixmap));
        ui->chat_data->appendChatItem(chatItem);
    }
    if (image.messageId > 0) {
        _displayed_image_message_ids.insert(image.messageId);
        if (!sentBySelf) {
            _pending_incoming_image_ids.remove(image.messageId);
            SendDisplayAcknowledgement(image.threadId, {image.messageId});
            TrySendCurrentReadReceipt();
        }
    }
    qInfo() << "received image appended to chat view, resource_id=" << image.resourceId;
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
        // 同一条认证消息可因 TCP/gRPC 重试再次到达；用正式 message_id 查询消息表
        // 去重，不能把“是否已经落库”误等同于连续同步游标已经覆盖该消息。
        const qint64 syncCursor = localStorage->SyncCursors().value(thread.threadId, 0);
        const bool messageAlreadyKnown = localStorage->HasMessage(message->GetMessageId());
        if (!messageAlreadyKnown && receivedFromFriend
            && (!_current_chatuser || _current_chatuser->_uid != friendInfo->_uid)) {
            ++thread.unreadCount;
        }

        // 好友认证通知与 1019/1035/1042 一样属于单条实时消息，只负责落库，不能用
        // 自身 message_id 推进 1030 连续游标，否则会跳过认证通知之前尚未补齐的消息。
        if (!localStorage->SaveReceivedMessages(thread, {localMessage}, syncCursor)) {
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
    if (!currentUser || !localStorage->IsReady() || message->GetThreadId() <= 0
        || message->GetMessageId() <= 0) {
        qWarning() << "ignore text chat without confirmed server metadata";
        return;
    }

    // ID_NOTIFY_TEXT_CHAT_MSG_REQ 只接受服务端持久化完成的消息。实时通知和登录增量
    // 回包可能重复或交错抵达，因此统一写 SQLite，由主键与同步游标负责去重；UI 只从
    // 提交成功后的 SQLite 摘要刷新。
    LocalChatThread thread;
    thread.threadId = message->GetThreadId();
    thread.threadType = QStringLiteral("private");
    thread.title = (*iter)->_name;
    thread.peerUid = (*iter)->_uid;

    for (const LocalChatThread &cachedThread : localStorage->CachedThreads()) {
        if (cachedThread.threadId == thread.threadId) {
            thread = cachedThread;
            thread.threadType = QStringLiteral("private");
            thread.title = (*iter)->_name;
            thread.peerUid = (*iter)->_uid;
            break;
        }
    }

    const qint64 receivedAtMs = message->GetCreatedAtMs();
    const bool isCurrentThread = _current_chatuser
                                 && _current_chatuser->_uid == (*iter)->_uid
                                 && _current_thread_id == thread.threadId;
    const bool receivedFromFriend = message->GetSendUid() != currentUser->_uid;
    const qint64 syncCursor = localStorage->SyncCursors().value(thread.threadId, 0);
    const bool messageAlreadyKnown = localStorage->HasMessage(message->GetMessageId());
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

    // 1019 与图片/文件通知一样只落库、不推进连续同步游标，避免登录期间越过离线缺口。
    if (!localStorage->SaveReceivedMessages(thread, {localMessage}, syncCursor)) {
        qWarning() << "save real-time text chat message failed:" << localStorage->LastError();
        return;
    }
    emit TcpMgr::GetInstance()->sig_local_chat_synced(thread.threadId);
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

void ChatDialog::slot_message_delivery_updated(qint64 threadId, const QList<qint64> &messageIds,
                                               int deliveryState)
{
    for (qint64 messageId : messageIds) {
        const auto textItem = _outgoing_text_items.find(messageId);
        if (textItem != _outgoing_text_items.end() && textItem.value()) {
            textItem.value()->SetDeliveryState(deliveryState);
        }
        const auto imageItem = _outgoing_image_items.find(messageId);
        if (imageItem != _outgoing_image_items.end() && imageItem.value()) {
            imageItem.value()->SetDeliveryState(deliveryState);
        }
        const auto fileItem = _outgoing_file_items.find(messageId);
        if (fileItem != _outgoing_file_items.end() && fileItem.value()) {
            fileItem.value()->SetDeliveryState(deliveryState);
        }
    }
    Q_UNUSED(threadId);
}

void ChatDialog::SendDisplayAcknowledgement(qint64 threadId, const QList<qint64> &messageIds)
{
    if (threadId <= 0 || messageIds.isEmpty() || !TcpMgr::GetInstance()->IsConnected()) {
        return;
    }
    QJsonArray ids;
    for (qint64 messageId : messageIds) {
        if (messageId > 0) {
            // 64 位 ID 用字符串，避免 Qt JSON double 造成精度丢失。
            ids.append(QString::number(messageId));
        }
    }
    if (ids.isEmpty()) {
        return;
    }
    QJsonObject request;
    request["thread_id"] = QString::number(threadId);
    request["message_ids"] = ids;
    emit TcpMgr::GetInstance()->sig_send_data(ID_MESSAGE_DISPLAY_ACK_REQ,
                                              QJsonDocument(request).toJson(QJsonDocument::Compact));
}

void ChatDialog::SendReadReceipt(qint64 threadId, qint64 readThroughMessageId)
{
    if (threadId <= 0 || readThroughMessageId <= 0 || !TcpMgr::GetInstance()->IsConnected()) {
        return;
    }
    QJsonObject request;
    request["thread_id"] = QString::number(threadId);
    request["read_through_message_id"] = QString::number(readThroughMessageId);
    emit TcpMgr::GetInstance()->sig_send_data(ID_MARK_THREAD_READ_REQ,
                                              QJsonDocument(request).toJson(QJsonDocument::Compact));
}

void ChatDialog::TrySendCurrentReadReceipt()
{
    if (_current_thread_id <= 0 || _current_read_receipt_target_id <= 0
        || !_pending_incoming_image_ids.isEmpty()
        || !TcpMgr::GetInstance()->IsConnected()) {
        return;
    }
    // 1038 是按游标确认，发送 last_message_id 会连带把更早消息全部标成已读。
    // 因此当前页任意一张入站图片尚未真正替换为 PictureBubble 时，都不能发送该游标。
    const qint64 readThrough = _current_read_receipt_target_id;
    _current_read_receipt_target_id = 0;
    SendReadReceipt(_current_thread_id, readThrough);
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
    if (sentBySelf) {
        chatItem->SetDeliveryState(message.serverStatus == 1 ? 4 : message.deliveryState);
        if (message.messageId > 0) {
            _outgoing_text_items.insert(message.messageId, chatItem);
        }
    }
    return chatItem;
}

bool ChatDialog::ParseStoredImageMessage(const LocalChatMessage &message, ImageChatData *image) const
{
    if (!image || message.contentType != QStringLiteral("image") || message.messageId <= 0
        || message.threadId <= 0) {
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(message.content.toUtf8());
    if (!document.isObject()) {
        return false;
    }
    const QJsonObject item = document.object();
    bool sizeOk = false;
    const qint64 fileSize = item.value("file_size").toVariant().toLongLong(&sizeOk);
    const QString msgId = item.value("msgid").toString();
    const QString resourceId = item.value("resource_id").toString();
    const QString name = item.value("name").toString();
    const QString mimeType = item.value("mime_type").toString();
    const int width = item.value("width").toInt();
    const int height = item.value("height").toInt();
    if (msgId.isEmpty() || resourceId.isEmpty() || name.isEmpty() || !sizeOk || fileSize <= 0
        || width <= 0 || height <= 0 || !mimeType.startsWith(QStringLiteral("image/"))) {
        return false;
    }

    image->messageId = message.messageId;
    image->threadId = message.threadId;
    image->msgId = msgId;
    image->resourceId = resourceId;
    image->name = name;
    image->mimeType = mimeType;
    image->fileSize = fileSize;
    image->width = width;
    image->height = height;
    image->fromUid = static_cast<int>(message.senderId);
    image->toUid = static_cast<int>(message.recvId);
    return image->fromUid > 0 && image->toUid > 0;
}

ChatItemBase *ChatDialog::QueueStoredImageMessage(const LocalChatMessage &message,
                                                   bool appendToView)
{
    if (message.threadId != _current_thread_id || message.messageId <= 0
        || _requested_image_message_ids.contains(message.messageId)
        || _displayed_image_message_ids.contains(message.messageId)) {
        return nullptr;
    }
    ImageChatData image;
    if (!ParseStoredImageMessage(message, &image)) {
        qWarning() << "ignore invalid cached image message:" << message.messageId;
        const auto self = UserMgr::GetInstance()->GetUserInfo();
        if (!self || !_current_chatuser) return nullptr;
        const bool sentBySelf = message.senderId == self->_uid;
        const auto &displayUser = sentBySelf ? self : _current_chatuser;
        const ChatRole role = sentBySelf ? ChatRole::Self : ChatRole::Other;
        auto *invalidRow = new ChatItemBase(role);
        invalidRow->setUserName(displayUser->_name);
        invalidRow->setUserAvatar(displayUser->_uid, displayUser->_icon);
        invalidRow->setWidget(new TextBubble(role, tr("[图片消息元数据无效，无法加载]")));
        if (!sentBySelf) {
            // 消息虽然以错误占位行可见，但图片正文没有显示，不能越过它发送已读游标。
            _pending_incoming_image_ids.insert(message.messageId);
        }
        if (appendToView) ui->chat_data->appendChatItem(invalidRow);
        return invalidRow;
    }

    const auto self = UserMgr::GetInstance()->GetUserInfo();
    if (!self || !_current_chatuser) return nullptr;
    const bool sentBySelf = image.fromUid == self->_uid;
    const auto &displayUser = sentBySelf ? self : _current_chatuser;
    const ChatRole role = sentBySelf ? ChatRole::Self : ChatRole::Other;
    auto *placeholder = new ChatItemBase(role);
    placeholder->setUserName(displayUser->_name);
    placeholder->setUserAvatar(displayUser->_uid, displayUser->_icon);
    placeholder->setWidget(new TextBubble(role, tr("[图片加载中…]")));
    if (sentBySelf) {
        placeholder->SetDeliveryState(message.serverStatus == 1 ? 4 : message.deliveryState);
        _outgoing_image_items.insert(message.messageId, placeholder);
    } else {
        // 入站图片在 PictureBubble 原位替换成功前构成已读屏障，1038 不能越过它。
        _pending_incoming_image_ids.insert(message.messageId);
    }
    _image_placeholder_items.insert(message.messageId, placeholder);
    _requested_image_message_ids.insert(message.messageId);
    if (appendToView) ui->chat_data->appendChatItem(placeholder);
    qInfo() << "queue offline image message, message_id=" << image.messageId
            << "resource_id=" << image.resourceId;
    _image_transfer->enqueueDownload(image);
    return placeholder;
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
    const auto self = UserMgr::GetInstance()->GetUserInfo();
    QList<qint64> displayedMessageIds;
    for (const LocalChatMessage &message : olderMessages) {
        if (message.contentType == QStringLiteral("text")) {
            if (auto *chatItem = CreateStoredTextChatItem(message, _current_chatuser)) {
                historyItems.append(chatItem);
                if (self && message.senderId != self->_uid && message.recvId == self->_uid) {
                    displayedMessageIds.append(message.messageId);
                }
            }
        } else if (message.contentType == QStringLiteral("image")) {
            if (auto *chatItem = QueueStoredImageMessage(message, false)) {
                historyItems.append(chatItem);
            }
        } else if (message.contentType == QStringLiteral("file")) {
            FileChatData file;
            if (ParseStoredFileMessage(message, &file)) {
                if (auto *chatItem = CreateFileChatItem(file)) {
                    historyItems.append(chatItem);
                    if (self && file.fromUid != self->_uid)
                        displayedMessageIds.append(file.messageId);
                }
            }
        }
    }
    if (!historyItems.isEmpty()) {
        ui->chat_data->prependChatItems(historyItems);
        SendDisplayAcknowledgement(loadingThreadId, displayedMessageIds);
    }
    // LoadMessagesBefore 的结果按 message_id 正序返回，首项就是下一次分页边界。
    _oldest_local_message_id = olderMessages.first().messageId;
    _has_more_local_history = olderMessages.size() >= kLocalHistoryPageSize;
    _loading_older_local_history = false;
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
    _requested_image_message_ids.clear();
    _displayed_image_message_ids.clear();
    _image_placeholder_items.clear();
    _pending_incoming_image_ids.clear();
    _current_read_receipt_target_id = 0;
    ui->chat_title_label->setText(_current_chatuser->_name);
    ui->chat_stack->setCurrentWidget(ui->chat_page);

    // 会话切换时必须先移除旧气泡，再按 message_id 正序读取本地最近记录；
    // LocalChatStorageMgr 已在 SQL 中完成分页和排序，界面层不再自行倒序处理。
    ui->chat_data->ClearChatItems();
    if (threadId > 0 && storage->IsReady()) {
        const QList<LocalChatMessage> recentMessages = storage->LoadRecentMessages(
            threadId, kLocalHistoryPageSize);
        const auto self = UserMgr::GetInstance()->GetUserInfo();
        QList<qint64> displayedMessageIds;
        for (const LocalChatMessage &message : recentMessages) {
            if (message.contentType == QStringLiteral("text")) {
                AppendStoredTextMessage(message, _current_chatuser);
                if (self && message.senderId != self->_uid && message.recvId == self->_uid) {
                    displayedMessageIds.append(message.messageId);
                }
            } else if (message.contentType == QStringLiteral("image")) {
                QueueStoredImageMessage(message);
            } else if (message.contentType == QStringLiteral("file")) {
                QueueOrAppendStoredFile(message);
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
        // 文本已在上面的 appendChatItem 后实际可见，才允许 1036。图片由下载回调在
        // PictureBubble 插入后单独确认，不能把“已排队下载”误报为已显示。
        SendDisplayAcknowledgement(threadId, displayedMessageIds);
        // 只确认本次实际从 SQLite 取出并参与渲染的最新消息，不能使用会话摘要中
        // 可能尚未同步到本地的 last_message_id，否则接收端缺消息时发送端仍会显示已读。
        if (!recentMessages.isEmpty())
            _current_read_receipt_target_id = recentMessages.last().messageId;
        TrySendCurrentReadReceipt();
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
