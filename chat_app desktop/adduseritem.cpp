#include "adduseritem.h"
#include "ui_adduseritem.h"

AddUserItem::AddUserItem(QWidget *parent)
    : ListItemBase(parent)
    , ui(new Ui::AddUserItem)
{
    ui->setupUi(this);
    setMinimumSize(0, 48);
    setMaximumSize(QWIDGETSIZE_MAX, 48);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    ui->add_tip->setFixedSize(24, 24);
    ui->right_tip->setFixedSize(16, 16);
    ui->message_tip->setMinimumWidth(0);
    ui->message_tip->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    ui->message_tip->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    // 标记成"添加用户"提示项，外层点击时按类型分流处理
    SetItemType(ListItemType::ADD_USER_TIP_ITEM);
}

AddUserItem::~AddUserItem()
{
    delete ui;
}

void AddUserItem::SetTip(const QString &text, const QIcon &icon)
{
    ui->message_tip->setText(text);
    ui->add_tip->setObjectName(QStringLiteral("search_entry_icon"));
    ui->add_tip->setPixmap(icon.pixmap(24, 24));
    ui->right_tip->setObjectName(QStringLiteral("search_entry_arrow"));
    ui->right_tip->setText(QStringLiteral("›"));
    ui->right_tip->setAlignment(Qt::AlignCenter);
}
