#include "chatuserwid.h"
#include "ui_chatuserwid.h"
#include "avatarutil.h"

ChatUserWid::ChatUserWid(QWidget *parent)
    : ListItemBase(parent)
    , ui(new Ui::ChatUserWid)
{
    ui->setupUi(this);
    ui->red_point->hide(); // 默认不显示未读红点
}

ChatUserWid::~ChatUserWid()
{
    delete ui;
}

QSize ChatUserWid::sizeHint() const
{
    return QSize(260, 75);
}

void ChatUserWid::SetUserInfo(const std::shared_ptr<UserInfo> &userInfo)
{
    _userInfo = userInfo;
    if (!_userInfo) {
        SetUserName({});
        SetHeadIcon({});
        return;
    }

    SetUserName(_userInfo->_name);
    SetHeadIcon(_userInfo->_icon);
}

std::shared_ptr<UserInfo> ChatUserWid::GetUserInfo() const
{
    return _userInfo;
}

void ChatUserWid::SetUserName(const QString &name)
{
    _name = name;
    ui->user_name_lb->setText(name);
}

void ChatUserWid::SetChatMsg(const QString &msg)
{
    ui->user_chat_lb->setText(msg);
}

void ChatUserWid::SetTime(const QString &time)
{
    ui->time_lb->setText(time);
}

void ChatUserWid::SetHeadIcon(const QString &icon_path)
{
    const int uid = _userInfo ? _userInfo->_uid : 0;
    _icon = AvatarUtil::ResolvePath(uid, icon_path);
    AvatarUtil::SetRoundAvatar(ui->icon_lb, uid, _icon);
}

void ChatUserWid::ShowRedPoint(bool show)
{
    ui->red_point->setVisible(show);
}

void ChatUserWid::SetThreadId(qint64 threadId)
{
    // 这里只保存标识，不直接发网络请求；点击列表项时 ChatDialog 取出该值决定当前会话。
    _threadId = threadId;
}

qint64 ChatUserWid::GetThreadId() const
{
    return _threadId;
}

QString ChatUserWid::GetName() const
{
    return _name;
}

QString ChatUserWid::GetIcon() const
{
    return _icon;
}
