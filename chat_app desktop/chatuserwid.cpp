#include "chatuserwid.h"
#include "ui_chatuserwid.h"
#include "avatarutil.h"
#include <QLabel>
#include <QPainter>
#include <QFontMetrics>

ChatUserWid::ChatUserWid(QWidget *parent)
    : ListItemBase(parent)
    , ui(new Ui::ChatUserWid)
{
    ui->setupUi(this);
    ui->red_point->hide(); // 默认不显示未读红点
    _groupUnreadBadge = new QLabel(this);
    _groupUnreadBadge->setObjectName(QStringLiteral("group_unread_badge"));
    _groupUnreadBadge->setAlignment(Qt::AlignCenter);
    _groupUnreadBadge->setStyleSheet(QStringLiteral(
        "background-color: #e94b4b; color: white; border-radius: 9px; font-size: 11px;"));
    _groupUnreadBadge->hide();
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
    _isGroup = false;
    _groupInfo = {};
    _groupUnreadCount = 0;
    _groupUnreadBadge->hide();
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
    if (_isGroup) {
        ui->red_point->hide();
        _groupUnreadBadge->setVisible(show && _groupUnreadCount > 0);
        return;
    }
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
void ChatUserWid::SetGroupInfo(const GroupInfo &group)
{
    _userInfo.reset();
    _groupInfo = group;
    _isGroup = true;
    _threadId = group.threadId;
    SetUserName(group.name);
    _icon.clear(); // 群列表头像固定，不按个人UID/昵称推导，也没有上传入口。
    static const QPixmap groupAvatar = [] {
        QPixmap avatar(44, 44);
        avatar.fill(Qt::transparent);
        QPainter painter(&avatar);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#467bc7")));
        painter.drawEllipse(avatar.rect());
        QFont font = painter.font();
        font.setPixelSize(24);
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(avatar.rect(), Qt::AlignCenter, QStringLiteral("群"));
        return avatar;
    }();
    ui->icon_lb->setPixmap(groupAvatar);
    ui->red_point->hide();
}

bool ChatUserWid::IsGroup() const { return _isGroup; }

GroupInfo ChatUserWid::GetGroupInfo() const { return _groupInfo; }

void ChatUserWid::SetGroupUnreadCount(int count)
{
    if (!_isGroup) return;
    _groupUnreadCount = qMax(0, count);
    const QString text = QString::number(_groupUnreadCount);
    _groupUnreadBadge->setText(text);
    _groupUnreadBadge->setAccessibleName(tr("群未读消息：%1").arg(text));
    _groupUnreadBadge->setGeometry(42, 8,
        qMax(18, _groupUnreadBadge->fontMetrics().horizontalAdvance(text) + 8), 18);
    _groupUnreadBadge->setVisible(_groupUnreadCount > 0);
    _groupUnreadBadge->raise();
}
