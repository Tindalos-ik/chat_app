#include "friendinfopage.h"
#include "ui_friendinfopage.h"
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>
#include <QDebug>

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

void FriendInfoPage::SetUserInfo(const QString &icon, const QString &name, int sex,
                                 const QString &nick, const QString &bak)
{
    _icon = icon;

    // 头像：加载并裁剪成圆形（和列表头像同一套画法）
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
    // 点"发消息"：跳到与该好友的聊天界面（TODO: 聊天页切换逻辑未实现）
    qDebug() << "msg chat btn clicked, icon =" << _icon;
    emit sig_jump_chat_item();
}
