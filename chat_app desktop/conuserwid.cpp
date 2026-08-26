#include "conuserwid.h"
#include "ui_conuserwid.h"
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>

ConUserWid::ConUserWid(QWidget *parent)
    : ListItemBase(parent)
    , ui(new Ui::ConUserWid)
{
    ui->setupUi(this);
}

ConUserWid::~ConUserWid()
{
    delete ui;
}

QSize ConUserWid::sizeHint() const
{
    return QSize(260, 60);
}

void ConUserWid::SetUserName(const QString &name)
{
    _name = name;
    ui->user_name_lb->setText(name);
}

void ConUserWid::SetHeadIcon(const QString &icon_path)
{
    _icon = icon_path;
    QPixmap original(icon_path);
    if (original.isNull()) {
        return;
    }
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

void ConUserWid::ShowRedPoint(bool show)
{
    ui->red_point->setVisible(show);
}

QString ConUserWid::GetName() const
{
    return _name;
}

QString ConUserWid::GetIcon() const
{
    return _icon;
}
