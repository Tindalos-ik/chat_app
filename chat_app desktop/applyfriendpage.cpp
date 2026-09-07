#include "applyfriendpage.h"
#include "ui_applyfriendpage.h"
#include "applyfrienditem.h"
#include "authenfriend.h"
#include "usermgr.h"
#include "avatarutil.h"
#include <QLabel>
#include <QListWidgetItem>

ApplyFriendPage::ApplyFriendPage(QWidget *parent)
    : QWidget(parent), ui(new Ui::ApplyFriendPage)
{
    ui->setupUi(this);

    // 空列表时给出明确提示，收到第一条申请后自动隐藏。
    _empty_label = new QLabel(QStringLiteral("暂无好友申请"), this);
    _empty_label->setAlignment(Qt::AlignCenter);
    _empty_label->setStyleSheet(QStringLiteral("color:#999999; font-size:13px;"));
    ui->root_layout->insertWidget(1, _empty_label, 1);

    ReloadApplyList();
}

ApplyFriendPage::~ApplyFriendPage() { delete ui; }

void ApplyFriendPage::ReloadApplyList()
{
    ui->friend_list->clear();
    _apply_items.clear();

    const auto cached = UserMgr::GetInstance()->GetApplyList();
    for (const auto &applyInfo : cached) {
        addApplyInfo(applyInfo, false);
    }
    updateEmptyState();
}

void ApplyFriendPage::AddNewApply(std::shared_ptr<AddFriendApply> apply)
{
    if (!apply || apply->_fromuid <= 0 || _apply_items.contains(apply->_fromuid)) {
        return;
    }

    auto applyInfo = std::make_shared<ApplyInfo>(apply->_fromuid, apply->_name,
                                                  apply->_desc,
                                                  AvatarUtil::ResolvePath(apply->_fromuid, apply->_icon),
                                                  apply->_nick,
                                                  apply->_sex, 0);
    addApplyInfo(applyInfo, true);
}

void ApplyFriendPage::addApplyInfo(const std::shared_ptr<ApplyInfo> &applyInfo, bool prepend)
{
    if (!applyInfo || applyInfo->_uid <= 0 || _apply_items.contains(applyInfo->_uid)) {
        return;
    }

    applyInfo->SetIcon(AvatarUtil::ResolvePath(applyInfo->_uid, applyInfo->_icon));

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
    applyInfo->_desc = applyInfo->_desc.isEmpty() ? "请求添加你为好友" : applyInfo->_desc;
    dialog->SetApplyInfo(applyInfo);
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
