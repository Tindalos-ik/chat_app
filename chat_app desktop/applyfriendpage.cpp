#include "applyfriendpage.h"
#include "ui_applyfriendpage.h"
#include "applyfrienditem.h"
#include <QListWidgetItem>

ApplyFriendPage::ApplyFriendPage(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::ApplyFriendPage)
{
    ui->setupUi(this);
    LoadTestData(); // 进来就能看到测试数据
}

ApplyFriendPage::~ApplyFriendPage()
{
    delete ui;
}

void ApplyFriendPage::AddFriendItem(const QString &name, const QString &icon, const QString &status)
{
    // 一条目 = ApplyFriendItem（头像 + 名字 + 右侧状态标签）
    auto *wid = new ApplyFriendItem;
    wid->SetInfo(name, icon, status);

    auto *item = new QListWidgetItem;
    item->setSizeHint(wid->sizeHint());
    ui->friend_list->addItem(item);
    ui->friend_list->setItemWidget(item, wid);
}

void ApplyFriendPage::LoadTestData()
{
    // 别人申请添加我：状态"好友申请"（绿色提示，待我同意）
    AddFriendItem(QStringLiteral("张伟"), QStringLiteral(":/res/head_2.jpg"), QStringLiteral("好友申请"));
    AddFriendItem(QStringLiteral("刘晓"), QStringLiteral(":/res/head_1.jpg"), QStringLiteral("好友申请"));

    // 我发出的申请：状态"等待对方同意"
    AddFriendItem(QStringLiteral("李雷"), QStringLiteral(":/res/head_3.jpg"), QStringLiteral("等待对方同意"));
    AddFriendItem(QStringLiteral("韩梅梅"), QStringLiteral(":/res/head_4.jpg"), QStringLiteral("等待对方同意"));
    AddFriendItem(QStringLiteral("王小虎"), QStringLiteral(":/res/head_5.jpg"), QStringLiteral("等待对方同意"));

    // 历史已添加的好友：状态"已添加"
    AddFriendItem(QStringLiteral("小明"), QStringLiteral(":/res/head_1.jpg"), QStringLiteral("已添加"));
    AddFriendItem(QStringLiteral("小红"), QStringLiteral(":/res/head_2.jpg"), QStringLiteral("已添加"));
    AddFriendItem(QStringLiteral("小刚"), QStringLiteral(":/res/head_3.jpg"), QStringLiteral("已添加"));
}
