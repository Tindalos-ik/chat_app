#include "applyfrienditem.h"
#include "ui_applyfrienditem.h"
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>

ApplyFriendItem::ApplyFriendItem(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::ApplyFriendItem)
{
    ui->setupUi(this);
}

ApplyFriendItem::~ApplyFriendItem()
{
    delete ui;
}

QSize ApplyFriendItem::sizeHint() const
{
    return QSize(260, 60);
}

void ApplyFriendItem::SetInfo(const QString &name, const QString &icon, const QString &status)
{
    ui->user_name_lb->setText(name);

    // 圆形头像（和列表头像同一套画法）
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

    // 右侧状态：可操作的"好友申请"用主题绿提示，其余灰色
    ui->status_lb->setText(status);
    if (status == QStringLiteral("好友申请")) {
        ui->status_lb->setStyleSheet(QStringLiteral("color: #07c160;"));
    } else {
        ui->status_lb->setStyleSheet(QStringLiteral("color: #999999;"));
    }
}
