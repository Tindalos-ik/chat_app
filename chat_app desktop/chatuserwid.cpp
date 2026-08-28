#include "chatuserwid.h"
#include "ui_chatuserwid.h"
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>

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
    _icon = icon_path;
    QPixmap original(icon_path);
    if (original.isNull()) {
        return;
    }
    original = original.scaled(ui->icon_lb->size(),
                               Qt::KeepAspectRatio, Qt::SmoothTransformation);

    // 画圆形头像：先画到透明画布上，再用圆形路径裁剪
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

void ChatUserWid::ShowRedPoint(bool show)
{
    ui->red_point->setVisible(show);
}

QString ChatUserWid::GetName() const
{
    return _name;
}

QString ChatUserWid::GetIcon() const
{
    return _icon;
}
