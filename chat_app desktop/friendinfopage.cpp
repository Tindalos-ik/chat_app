#include "friendinfopage.h"
#include "ui_friendinfopage.h"
#include "avatarutil.h"
#include "tcpmgr.h"
#include "usermgr.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>

FriendInfoPage::FriendInfoPage(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::FriendInfoPage)
{
    ui->setupUi(this);
}

FriendInfoPage::~FriendInfoPage()
{
    delete ui;
}

void FriendInfoPage::SetUserInfo(int uid, const QString &icon, const QString &name, int sex,
                                 const QString &nick, const QString &bak)
{
    _uid = uid;
    _name = name;
    _icon = AvatarUtil::ResolvePath(uid, icon);
    AvatarUtil::SetRoundAvatar(ui->icon_lb, uid, _icon);

    // 文字信息
    ui->name_lb->setText(name);
    ui->nick_lb->setText(nick);
    ui->bak_lb->setText(bak);

    // 性别图标：0=男 1=女（用 res 里的 male.png / female.png）
    ui->sex_lb->setPixmap(QPixmap(sex == 1 ? QStringLiteral(":/res/female.png")
                                            : QStringLiteral(":/res/male.png")));
}

void FriendInfoPage::on_msg_chat_clicked()
{
    const auto currentUser = UserMgr::GetInstance()->GetUserInfo();
    if (!currentUser || currentUser->_uid <= 0 || _uid <= 0 || currentUser->_uid == _uid) {
        qWarning() << "cannot create private chat: invalid current user or peer uid";
        return;
    }
    if (!TcpMgr::GetInstance()->IsConnected()) {
        qWarning() << "cannot create private chat: chat server is not connected";
        return;
    }

    // 协议 ID=1027。uid 保留在请求体中以兼容现有协议；服务端必须使用登录 session
    // 中的 uid 鉴权，不能信任这里的 uid。other_id 才是用户选择的聊天对象。
    QJsonObject request;
    request["uid"] = currentUser->_uid;
    request["other_id"] = _uid;
    const QByteArray requestData = QJsonDocument(request).toJson(QJsonDocument::Compact);
    emit TcpMgr::GetInstance()->sig_send_data(ID_CREATE_PRIVATE_CHAT_REQ, requestData);

    // 当前界面仍沿用原有切页行为；收到 1028 回包后应以其中 thread_id 建立会话数据。
    qDebug() << "create private chat request sent, peer uid:" << _uid;
    emit sig_jump_chat_item(_name, _icon);
}
