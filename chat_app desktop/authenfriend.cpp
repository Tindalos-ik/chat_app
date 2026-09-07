#include "authenfriend.h"
#include "ui_authenfriend.h"
#include <QDebug>
#include <QJsonObject>
#include <QJsonDocument>
#include "tcpmgr.h"
#include "usermgr.h"
#include "avatarutil.h"

AuthenFriend::AuthenFriend(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::AuthenFriend)
{
    ui->setupUi(this);
    // 无边框 + 模态，和其他弹窗风格一致
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    setModal(true);

    // 备注名默认填申请者名字，可修改
    ui->back_ed->setPlaceholderText(QStringLiteral("设置备注名"));
}

AuthenFriend::~AuthenFriend()
{
    delete ui;
}

void AuthenFriend::SetApplyInfo(std::shared_ptr<ApplyInfo> apply)
{
    _apply_info = apply;

    AvatarUtil::SetRoundAvatar(ui->icon_lb, apply->_uid, apply->_icon);

    ui->name_lb->setText(apply->_name);
    ui->msg_lb->setText(apply->_desc);
    ui->back_ed->setPlaceholderText(apply->_name); // 默认备注名 = 申请者名字
}

void AuthenFriend::on_sure_btn_clicked()
{
    // 同意：发信号让页面把状态改成"已添加"
    // TODO: 后端协议就绪后，这里把 uid/备注 发给服务器（ID_AUTH_FRIEND_REQ）
    qDebug() << "auth friend agreed:" << _apply_info->_name;

    QJsonObject jsonObj;
    jsonObj["fromuid"] = _apply_info->_uid;
    jsonObj["touid"] = UserMgr::GetInstance()->GetUid();
    QString bakname = ui->back_ed->text();
    if(bakname.isEmpty()) bakname = _apply_info->_name;
    jsonObj["bakname"] = bakname;
    QJsonDocument doc(jsonObj);
    QByteArray jsonData = doc.toJson(QJsonDocument::Compact); // 压缩传回来，节省空间
    // 发送tcp请求给chat_server，让tcpmgr发送给服务器
    emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_AUTH_FRIEND_REQ, jsonData);

    emit sig_auth_agreed(_apply_info->_name);
    hide();
    deleteLater();
}

void AuthenFriend::on_cancel_btn_clicked()
{
    // 拒绝：直接关掉，条目状态保持不变
    qDebug() << "auth friend refused:" << _apply_info->_name;
    hide();
    deleteLater();
}
