#include "authenfriend.h"
#include "ui_authenfriend.h"
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>
#include <QDebug>

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

void AuthenFriend::SetApplyInfo(const QString &name, const QString &icon, const QString &msg)
{
    _name = name;
    _icon = icon;

    // 圆形头像
    QPixmap original(icon);
    if (!original.isNull()) {
        original = original.scaled(ui->icon_lb->size(),
                                   Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPixmap rounded(original.size());
        rounded.fill(Qt::transparent);
        QPainter painter(&rounded);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        path.addEllipse(0, 0, original.width(), original.height());
        painter.setClipPath(path);
        painter.drawPixmap(0, 0, original);
        ui->icon_lb->setPixmap(rounded);
    }

    ui->name_lb->setText(name);
    ui->msg_lb->setText(msg);
    ui->back_ed->setPlaceholderText(name); // 默认备注名 = 申请者名字
}

void AuthenFriend::on_sure_btn_clicked()
{
    // 同意：发信号让页面把状态改成"已添加"
    // TODO: 后端协议就绪后，这里把 uid/备注 发给服务器（ID_AUTH_FRIEND_REQ）
    qDebug() << "auth friend agreed:" << _name;
    emit sig_auth_agreed(_name);
    hide();
    deleteLater();
}

void AuthenFriend::on_cancel_btn_clicked()
{
    // 拒绝：直接关掉，条目状态保持不变
    qDebug() << "auth friend refused:" << _name;
    hide();
    deleteLater();
}
