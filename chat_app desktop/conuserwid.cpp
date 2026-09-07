#include "conuserwid.h"
#include "ui_conuserwid.h"
#include "avatarutil.h"

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

void ConUserWid::SetInfo(int uid, const QString &name, const QString &icon)
{
    _uid = uid;
    SetItemType(ListItemType::CONTACT_USER_ITEM); // 标记为普通好友，点击才能分发到好友信息页
    SetUserName(name);
    SetHeadIcon(icon);
}

void ConUserWid::SetUserName(const QString &name)
{
    _name = name;
    ui->user_name_lb->setText(name);
}

void ConUserWid::SetHeadIcon(const QString &icon_path)
{
    _icon = AvatarUtil::ResolvePath(_uid, icon_path);
    AvatarUtil::SetRoundAvatar(ui->icon_lb, _uid, _icon);
}

void ConUserWid::ShowRedPoint(bool show)
{
    ui->red_point->setVisible(show);
}

int ConUserWid::GetUid() const
{
    return _uid;
}

QString ConUserWid::GetName() const
{
    return _name;
}

QString ConUserWid::GetIcon() const
{
    return _icon;
}
