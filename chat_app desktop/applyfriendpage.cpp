#include "applyfriendpage.h"
#include "ui_applyfriendpage.h"
#include "applyfrienditem.h"
#include "authenfriend.h"
#include "usermgr.h"
#include <QLabel>
#include <QListWidgetItem>

namespace {
// 当前工程没有头像下载模块，服务端未返回头像时使用内置头像作为兜底。
const QString kHeadIcons[] = {":/res/head_1.jpg", ":/res/head_2.jpg", ":/res/head_3.jpg",
                              ":/res/head_4.jpg", ":/res/head_5.jpg"};
}

ApplyFriendPage::ApplyFriendPage(QWidget *parent)
    : QWidget(parent), ui(new Ui::ApplyFriendPage)
{
    ui->setupUi(this);

    // 空列表时给出明确提示，收到第一条申请后自动隐藏。
    _empty_label = new QLabel(QStringLiteral("暂无好友申请"), this);
    _empty_label->setAlignment(Qt::AlignCenter);
    _empty_label->setStyleSheet(QStringLiteral("color:#999999; font-size:13px;"));
    ui->root_layout->insertWidget(1, _empty_label, 1);

    // 页面创建可能早于 TCP 通知，因此先恢复 UserMgr 中已经缓存的申请。
    const auto cached = UserMgr::GetInstance()->GetApplyList();
    for (const auto &applyInfo : cached) {
        addApplyInfo(applyInfo, false);
    }
    updateEmptyState();
}

ApplyFriendPage::~ApplyFriendPage() { delete ui; }

void ApplyFriendPage::AddNewApply(std::shared_ptr<AddFriendApply> apply)
{
    if (!apply || apply->_fromuid <= 0 || _apply_items.contains(apply->_fromuid)) {
        return;
    }

    // 通知中的 icon 为空时选择稳定的本地头像，避免每次刷新头像跳变。
    QString icon = apply->_icon;
    if (icon.isEmpty()) {
        icon = kHeadIcons[apply->_fromuid % (sizeof(kHeadIcons) / sizeof(kHeadIcons[0]))];
    }
    auto applyInfo = std::make_shared<ApplyInfo>(apply->_fromuid, apply->_name,
                                                  apply->_desc, icon, apply->_nick,
                                                  apply->_sex, 0);
    addApplyInfo(applyInfo, true);
}

void ApplyFriendPage::addApplyInfo(const std::shared_ptr<ApplyInfo> &applyInfo, bool prepend)
{
    if (!applyInfo || applyInfo->_uid <= 0 || _apply_items.contains(applyInfo->_uid)) {
        return;
    }

    // 历史记录缺少头像时使用本地兜底资源。
    if (applyInfo->_icon.isEmpty()) {
        applyInfo->SetIcon(kHeadIcons[applyInfo->_uid % (sizeof(kHeadIcons) / sizeof(kHeadIcons[0]))]);
    }

    auto *itemWidget = new ApplyFriendItem;
    itemWidget->SetInfo(applyInfo);
    auto *item = new QListWidgetItem;
    item->setSizeHint(itemWidget->sizeHint());
    // 操作由条目里的按钮负责，列表本身不再响应选择，避免误触弹窗。
    item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
    if (prepend) {
        ui->friend_list->insertItem(0, item);
    } else {
        ui->friend_list->addItem(item);
    }
    ui->friend_list->setItemWidget(item, itemWidget);
    _apply_items.insert(applyInfo->_uid, itemWidget);

    connect(itemWidget, &ApplyFriendItem::sig_auth_friend, this,
            [this, itemWidget](std::shared_ptr<ApplyInfo> info) {
                showAuthDialog(itemWidget, info);
            });
    updateEmptyState();
}

void ApplyFriendPage::showAuthDialog(ApplyFriendItem *item,
                                     const std::shared_ptr<ApplyInfo> &applyInfo)
{
    if (!item || !applyInfo) return;
    auto *dialog = new AuthenFriend(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setModal(true);
    dialog->SetApplyInfo(applyInfo->_name, applyInfo->_icon,
                         applyInfo->_desc.isEmpty()
                             ? QStringLiteral("请求添加你为好友") : applyInfo->_desc);
    // 当前认证响应协议尚未接入页面，先在认证成功信号后更新本地状态。
    connect(dialog, &AuthenFriend::sig_auth_agreed, this,
            [item](const QString &) { item->ShowAddBtn(false); });
    dialog->show();
}

void ApplyFriendPage::updateEmptyState()
{
    if (_empty_label) {
        _empty_label->setVisible(ui->friend_list->count() == 0);
    }
}
