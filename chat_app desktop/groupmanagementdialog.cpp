// 文件职责：群成员、角色、转让、离群、公告及头像界面；参数采用当前成员版本，最终权限由服务端事务判定。
#include "groupmanagementdialog.h"

#include "global.h"
#include "grouprequestsdlg.h"
#include "groupavatartransfer.h"
#include "avatarutil.h"
#include "localchatstoragemgr.h"
#include "tcpmgr.h"
#include "usermgr.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QImage>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace {
GroupInfo FromCache(const LocalChatThread &thread)
{
    GroupInfo group;
    group.threadId = thread.threadId;
    group.name = thread.title;
    group.ownerUid = thread.ownerUid;
    group.active = thread.active;
    group.version = thread.version;
    group.approvalRequired = thread.approvalRequired;
    group.announcement = thread.announcement;
    group.announcementVersion = thread.announcementVersion;
    group.announcementUpdatedAtMs = thread.announcementUpdatedAtMs;
    group.announcementAuthorUid = thread.announcementAuthorUid;
    group.avatarResourceId = thread.avatarResourceId;
    group.avatarVersion = thread.avatarVersion;
    group.avatarMimeType = thread.avatarMimeType;
    group.avatarSize = thread.avatarSize;
    group.avatarWidth = thread.avatarWidth;
    group.avatarHeight = thread.avatarHeight;
    group.member = thread.active;
    group.role = thread.role;
    group.membershipVersion = thread.membershipVersion;
    group.source = QStringLiteral("cache");
    return group;
}

QString MutationError(int error)
{
    if (error == ErrorCodes::GroupUnavailable) return QObject::tr("群已解散或不可用。");
    if (error == ErrorCodes::GroupNotMember) return QObject::tr("你已不是此群的有效成员。");
    if (error == ErrorCodes::GroupPermissionDenied) return QObject::tr("服务端拒绝此角色权限。");
    return QObject::tr("群操作失败（%1），请重新加载后再试。").arg(error);
}
}

GroupManagementDialog::GroupManagementDialog(const GroupInfo &group,
                                             GroupAvatarTransfer *avatarTransfer, QWidget *parent)
    : QDialog(parent), _group(group), _avatarTransfer(avatarTransfer)
{
    const auto user = UserMgr::GetInstance()->GetUserInfo();
    _accountUid = user ? user->_uid : 0;
    _loginGeneration = TcpMgr::GetInstance()->LoginGeneration();
    setWindowTitle(tr("群信息与管理"));
    setObjectName(QStringLiteral("group_info_dialog"));
    resize(600, 580);
    BuildUi();
    ConnectSignals();
    UpdateControls();
    RequestMemberPage(0);
    if (_avatarTransfer) _avatarTransfer->RequestAvatar(_group);
}

void GroupManagementDialog::BuildUi()
{
    auto *layout = new QVBoxLayout(this);
    _summary = new QLabel(this);
    _summary->setObjectName(QStringLiteral("group_info_summary"));
    _summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(_summary);

    auto *tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("group_info_tabs"));
    layout->addWidget(tabs);

    auto *membersPage = new QWidget(tabs);
    auto *membersLayout = new QVBoxLayout(membersPage);
    auto *inviteRow = new QHBoxLayout;
    _friendPicker = new QComboBox(membersPage);
    _friendPicker->setObjectName(QStringLiteral("group_friend_picker"));
    _invite = new QPushButton(tr("邀请好友"), membersPage);
    _invite->setObjectName(QStringLiteral("group_invite_button"));
    inviteRow->addWidget(_friendPicker, 1);
    inviteRow->addWidget(_invite);
    membersLayout->addLayout(inviteRow);
    _members = new QListWidget(membersPage);
    _members->setObjectName(QStringLiteral("group_members_list"));
    membersLayout->addWidget(_members, 1);
    auto *memberActions = new QHBoxLayout;
    _memberMore = new QPushButton(tr("加载更多成员"), membersPage);
    _memberMore->setObjectName(QStringLiteral("group_members_more_button"));
    _memberMore->setEnabled(false);
    _role = new QPushButton(tr("任命管理员"), membersPage);
    _role->setObjectName(QStringLiteral("group_role_button"));
    _role->setEnabled(false);
    _kick = new QPushButton(tr("踢出选中成员"), membersPage);
    _kick->setObjectName(QStringLiteral("group_kick_button"));
    _kick->setEnabled(false);
    _transfer = new QPushButton(tr("转让群主"), membersPage);
    _transfer->setObjectName(QStringLiteral("group_transfer_owner_button"));
    _transfer->setEnabled(false);
    memberActions->addWidget(_memberMore);
    memberActions->addWidget(_role);
    memberActions->addWidget(_kick);
    memberActions->addWidget(_transfer);
    membersLayout->addLayout(memberActions);
    tabs->addTab(membersPage, tr("成员"));

    auto *settingsPage = new QWidget(tabs);
    auto *settingsLayout = new QVBoxLayout(settingsPage);
    auto *avatarRow = new QHBoxLayout;
    _avatarPreview = new QLabel(settingsPage);
    _avatarPreview->setObjectName(QStringLiteral("group_avatar_preview"));
    _avatarPreview->setFixedSize(72, 72);
    _avatarPreview->setAlignment(Qt::AlignCenter);
    AvatarUtil::SetRoundGroupAvatar(_avatarPreview, QString());
    _chooseAvatar = new QPushButton(tr("选择 PNG/JPEG 并发布群头像"), settingsPage);
    _chooseAvatar->setObjectName(QStringLiteral("group_avatar_choose_button"));
    avatarRow->addWidget(_avatarPreview);
    avatarRow->addWidget(_chooseAvatar);
    avatarRow->addStretch();
    settingsLayout->addLayout(avatarRow);
    _approvalRequired = new QCheckBox(tr("新成员申请需要管理员审批"), settingsPage);
    _approvalRequired->setObjectName(QStringLiteral("group_approval_required"));
    settingsLayout->addWidget(_approvalRequired);
    _approvals = new QPushButton(tr("待审批申请"), settingsPage);
    _approvals->setObjectName(QStringLiteral("group_pending_approvals_button"));
    settingsLayout->addWidget(_approvals);
    settingsLayout->addWidget(new QLabel(tr("群公告（纯文本，最多 1024 UTF-8 字节）"), settingsPage));
    _announcement = new QPlainTextEdit(settingsPage);
    _announcement->setObjectName(QStringLiteral("group_announcement_editor"));
    _announcement->setReadOnly(true);
    settingsLayout->addWidget(_announcement, 1);
    _announcementMetadata = new QLabel(settingsPage);
    settingsLayout->addWidget(_announcementMetadata);
    _editAnnouncement = new QPushButton(tr("编辑公告"), settingsPage);
    _editAnnouncement->setObjectName(QStringLiteral("group_announcement_edit_button"));
    settingsLayout->addWidget(_editAnnouncement, 0, Qt::AlignRight);
    auto *lifecycle = new QHBoxLayout;
    _leave = new QPushButton(tr("主动退出群聊"), settingsPage);
    _leave->setObjectName(QStringLiteral("group_leave_button"));
    _dissolve = new QPushButton(tr("解散群聊"), settingsPage);
    _dissolve->setObjectName(QStringLiteral("group_dissolve_button"));
    lifecycle->addWidget(_leave);
    lifecycle->addStretch();
    lifecycle->addWidget(_dissolve);
    settingsLayout->addLayout(lifecycle);
    tabs->addTab(settingsPage, tr("设置与公告"));

    auto *close = new QPushButton(tr("关闭"), this);
    layout->addWidget(close, 0, Qt::AlignRight);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(_memberMore, &QPushButton::clicked, this, [this] { RequestMemberPage(_nextMemberUid); });
    connect(_members, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *, QListWidgetItem *) { UpdateSelectedMemberActions(); });
    connect(_invite, &QPushButton::clicked, this, &GroupManagementDialog::InviteSelectedFriend);
    connect(_role, &QPushButton::clicked, this, &GroupManagementDialog::ChangeSelectedRole);
    connect(_transfer, &QPushButton::clicked, this, &GroupManagementDialog::TransferSelectedOwner);
    connect(_kick, &QPushButton::clicked, this, &GroupManagementDialog::KickSelectedMember);
    connect(_leave, &QPushButton::clicked, this, &GroupManagementDialog::LeaveGroup);
    connect(_dissolve, &QPushButton::clicked, this, &GroupManagementDialog::DissolveGroup);
    connect(_approvals, &QPushButton::clicked, this, &GroupManagementDialog::OpenApprovals);
    connect(_editAnnouncement, &QPushButton::clicked,
            this, &GroupManagementDialog::EditAnnouncement);
    connect(_chooseAvatar, &QPushButton::clicked,
            this, &GroupManagementDialog::ChooseAndPublishAvatar);
    connect(_approvalRequired, &QCheckBox::toggled, this, [this](bool checked) {
        if (!CanOwnManage()) { UpdateControls(); return; }
        const QString requestId = TcpMgr::GetInstance()->SetGroupApproval(_group.threadId, checked);
        if (requestId.isEmpty()) {
            UpdateControls();
            QMessageBox::warning(this, tr("群审批设置"), tr("设置请求无法发送。"));
            return;
        }
        RecordMutation(requestId, Action::Approval);
    });
    const auto friends = UserMgr::GetInstance()->GetFriendList();
    for (const auto &friendInfo : friends) {
        if (!friendInfo || friendInfo->_uid <= 0 || friendInfo->_uid == _accountUid) continue;
        const QString label = friendInfo->_nick.isEmpty() ? friendInfo->_name : friendInfo->_nick;
        _friendPicker->addItem(tr("%1（UID %2）").arg(label, QString::number(friendInfo->_uid)),
                               QVariant::fromValue<qlonglong>(friendInfo->_uid));
    }
}

void GroupManagementDialog::ConnectSignals()
{
    const auto tcp = TcpMgr::GetInstance();
    connect(tcp.get(), &TcpMgr::sig_group_info, this,
            [this](const GroupInfo &group, bool) { UpdateGroup(group); });
    connect(tcp.get(), &TcpMgr::sig_group_members, this,
            [this](qint64 threadId, const QString &requestId, qint64 groupVersion,
                   const QList<GroupMemberInfo> &members, qint64 nextUid, bool loadMore) {
        HandleMembers(threadId, requestId, groupVersion, members, nextUid, loadMore);
    });
    connect(tcp.get(), &TcpMgr::sig_group_operation_result, this,
            [this](const QString &requestId, int error) { HandleOperationResult(requestId, error); });
    connect(tcp.get(), &TcpMgr::sig_group_mutation_result, this,
            [this](const QString &requestId, int requestType, int error, const QJsonObject &response) {
        HandleMutationResult(requestId, requestType, error, response);
    });
    connect(tcp.get(), &TcpMgr::sig_group_state, this,
            [this](qint64 threadId, bool active, const QString &, qint64) {
        HandleGroupInvalidation(threadId, active);
    });
    if (_avatarTransfer) {
        connect(_avatarTransfer, &GroupAvatarTransfer::sig_avatar_ready,
                this, &GroupManagementDialog::HandleAvatarReady);
        connect(_avatarTransfer, &GroupAvatarTransfer::sig_avatar_upload_finished,
                this, &GroupManagementDialog::HandleAvatarUploadFinished);
    }
}

bool GroupManagementDialog::ReadCurrentGroup(LocalChatThread *thread) const
{
    if (!thread || _accountUid <= 0) return false;
    const auto user = UserMgr::GetInstance()->GetUserInfo();
    if (!user || user->_uid != _accountUid) return false;
    for (const LocalChatThread &cached : LocalChatStorageMgr::GetInstance()->LoadAllThreads()) {
        if (cached.threadId == _group.threadId && cached.threadType == QStringLiteral("group")) {
            *thread = cached;
            return true;
        }
    }
    return false;
}

bool GroupManagementDialog::CanUseCurrentGroup() const
{
    LocalChatThread thread;
    return ReadCurrentGroup(&thread) && thread.active
        && thread.version == _group.version
        && thread.membershipVersion == _group.membershipVersion;
}

bool GroupManagementDialog::CanManageRoles() const
{
    LocalChatThread thread;
    return CanUseCurrentGroup() && ReadCurrentGroup(&thread) && thread.role == 2;
}

bool GroupManagementDialog::CanManageAnnouncement() const
{
    LocalChatThread thread;
    return CanUseCurrentGroup() && ReadCurrentGroup(&thread) && thread.role >= 1;
}

bool GroupManagementDialog::CanOwnManage() const
{
    LocalChatThread thread;
    return CanUseCurrentGroup() && ReadCurrentGroup(&thread) && thread.role == 2;
}

void GroupManagementDialog::UpdateGroup(const GroupInfo &group)
{
    // 群元数据与成员版本分别单调；旧事件不能回退角色或重开已撤销的管理窗口。
    if (group.threadId != _group.threadId || group.version < _group.version
        || group.membershipVersion < _group.membershipVersion) return;
    const bool changed = group.version != _group.version
        || group.membershipVersion != _group.membershipVersion || group.role != _group.role
        || group.ownerUid != _group.ownerUid || group.active != _group.active
        || group.announcementVersion != _group.announcementVersion
        || group.avatarVersion != _group.avatarVersion
        || group.avatarResourceId != _group.avatarResourceId;
    if (!changed) return;
    _group = group;
    if (_avatarTransfer) _avatarTransfer->RequestAvatar(_group);
    if (_approvalsDialog) _approvalsDialog->UpdateGroup(group);
    UpdateControls();
    if (!group.active || !CanUseCurrentGroup()) {
        if (_approvalsDialog) _approvalsDialog->close();
        close();
        return;
    }
    ResetMemberPage(true);
}

void GroupManagementDialog::UpdateControls()
{
    LocalChatThread current;
    const bool usable = CanUseCurrentGroup() && ReadCurrentGroup(&current);
    if (_summary) _summary->setText(tr("%1\n群号：%2\n群主 UID：%3\n你的角色：%4（成员版本 %5）")
        .arg(_group.name, QString::number(_group.threadId), QString::number(_group.ownerUid),
             RoleName(_group.role), QString::number(_group.membershipVersion)));
    if (_announcement && _announcement->isReadOnly()
        && _announcement->toPlainText() != _group.announcement)
        _announcement->setPlainText(_group.announcement);
    if (_announcementMetadata) {
        QString metadata = tr("公告版本 %1").arg(_group.announcementVersion);
        if (_group.announcementAuthorUid > 0)
            metadata += tr("，作者 UID %1").arg(_group.announcementAuthorUid);
        if (_group.announcementUpdatedAtMs > 0)
            metadata += tr("，更新时间 %1").arg(QDateTime::fromMSecsSinceEpoch(
                _group.announcementUpdatedAtMs).toLocalTime().toString(Qt::ISODate));
        _announcementMetadata->setText(metadata);
    }
    if (_approvalRequired) {
        const QSignalBlocker blocker(_approvalRequired);
        _approvalRequired->setChecked(_group.approvalRequired);
        _approvalRequired->setEnabled(usable && _group.role == 2);
    }
    if (_chooseAvatar)
        _chooseAvatar->setEnabled(usable && _group.role >= 1 && _pendingAvatarTaskId.isEmpty());
    if (_friendPicker) _friendPicker->setEnabled(usable && _friendPicker->count() > 0);
    if (_invite) _invite->setEnabled(usable && _friendPicker && _friendPicker->count() > 0);
    if (_role) _role->setVisible(_group.role == 2);
    if (_transfer) _transfer->setVisible(_group.role == 2);
    if (_leave) {
        _leave->setEnabled(usable && _group.role != 2);
        if (_group.role == 2) _leave->setToolTip(tr("群主必须先转让群主或解散群聊。"));
    }
    if (_dissolve) _dissolve->setVisible(_group.role == 2);
    if (_approvals) _approvals->setVisible(_group.role >= 1 && _group.active);
    if (_editAnnouncement) {
        _editAnnouncement->setEnabled(usable && _group.role >= 1);
        _editAnnouncement->setText(_announcement && _announcement->isReadOnly()
            ? tr("编辑公告") : tr("保存公告"));
    }
    if (_announcement) {
        const bool editing = _announcement->isReadOnly() == false;
        _announcement->setReadOnly(!editing || !usable || _group.role < 1);
    }
    if (_memberMore) _memberMore->setEnabled(usable && _memberRequestId.isEmpty()
        && _nextMemberUid > _lastMemberUid);
    UpdateSelectedMemberActions();
}

void GroupManagementDialog::ResetMemberPage(bool loadFirst)
{
    ++_memberGeneration;
    if (!_memberRequestId.isEmpty()) _memberRequests.remove(_memberRequestId);
    _memberRequestId.clear();
    _lastMemberUid = 0;
    _nextMemberUid = 0;
    _membersByUid.clear();
    if (_members) _members->clear();
    if (_memberMore) _memberMore->setEnabled(false);
    if (loadFirst && CanUseCurrentGroup()) RequestMemberPage(0);
}

void GroupManagementDialog::RequestMemberPage(qint64 afterUid)
{
    if (!_memberRequestId.isEmpty() || !CanUseCurrentGroup()) return;
    const QString requestId = TcpMgr::GetInstance()->LoadGroupMembers(_group.threadId, afterUid);
    if (requestId.isEmpty()) {
        QMessageBox::warning(this, tr("群成员"), tr("成员页请求无法发送。"));
        return;
    }
    _memberRequestId = requestId;
    _memberRequests.insert(requestId, MemberPage{_memberGeneration, _group.version, afterUid});
    if (_memberMore) _memberMore->setEnabled(false);
}

void GroupManagementDialog::HandleMembers(qint64 threadId, const QString &requestId,
                                          qint64 groupVersion,
                                          const QList<GroupMemberInfo> &members,
                                          qint64 nextUid, bool loadMore)
{
    if (threadId != _group.threadId || requestId != _memberRequestId
        || !_memberRequests.contains(requestId)) return;
    const MemberPage page = _memberRequests.take(requestId);
    // 每个页面必须同时命中窗口代次、请求 ID、群版本与当前账号缓存权限。
    _memberRequestId.clear();
    if (page.generation != _memberGeneration || page.groupVersion != _group.version
        || groupVersion != _group.version || !CanUseCurrentGroup()) {
        LocalChatThread current;
        if (ReadCurrentGroup(&current) && current.active && current.version >= _group.version) {
            UpdateGroup(FromCache(current));
            if (groupVersion < _group.version) ResetMemberPage(true);
        }
        return;
    }
    qint64 lastUid = page.afterUid;
    for (const GroupMemberInfo &member : members) {
        if (member.uid <= lastUid || member.role < 0 || member.role > 2
            || member.membershipVersion <= 0) {
            ResetMemberPage(false);
            QMessageBox::warning(this, tr("群成员"), tr("成员页数据无效，已丢弃。"));
            return;
        }
        lastUid = member.uid;
    }
    if (loadMore && (members.isEmpty() || nextUid != lastUid || nextUid <= page.afterUid)) {
        ResetMemberPage(false);
        QMessageBox::warning(this, tr("群成员"), tr("成员分页游标无效，已丢弃。"));
        return;
    }
    ApplyMemberPage(members);
    _lastMemberUid = qMax(_lastMemberUid, lastUid);
    _nextMemberUid = loadMore ? nextUid : 0;
    if (_memberMore) _memberMore->setEnabled(loadMore && CanUseCurrentGroup());
    UpdateSelectedMemberActions();
}

void GroupManagementDialog::ApplyMemberPage(const QList<GroupMemberInfo> &members)
{
    for (const GroupMemberInfo &member : members) {
    // 刷新页用 _knownMembers 保留版本地板；相同版本重用旧行，较旧版本不覆盖角色。
        const auto old = _knownMembers.constFind(member.uid);
        if (old != _knownMembers.cend() && old->membershipVersion >= member.membershipVersion) {
            _membersByUid.insert(member.uid, old.value());
            continue;
        }
        _knownMembers.insert(member.uid, member);
        _membersByUid.insert(member.uid, member);
    }
    QList<qint64> uids = _membersByUid.keys();
    std::sort(uids.begin(), uids.end());
    _members->clear();
    for (qint64 uid : uids) {
        const GroupMemberInfo member = _membersByUid.value(uid);
        auto *item = new QListWidgetItem(tr("%1（UID %2，%3）")
            .arg(member.nick.isEmpty() ? QString::number(member.uid) : member.nick,
                 QString::number(member.uid), RoleName(member.role)), _members);
        item->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(member.uid));
        item->setData(Qt::UserRole + 1, member.role);
        item->setData(Qt::UserRole + 2, QVariant::fromValue<qlonglong>(member.membershipVersion));
        item->setData(Qt::UserRole + 3, QVariant::fromValue<qlonglong>(_group.version));
    }
    UpdateSelectedMemberActions();
}

void GroupManagementDialog::HandleOperationResult(const QString &requestId, int error)
{
    if (requestId != _memberRequestId) return;
    _memberRequests.remove(requestId);
    _memberRequestId.clear();
    if (error == ErrorCodes::SUCCESS) return;
    if (error == ErrorCodes::GroupNotMember || error == ErrorCodes::GroupUnavailable) {
        close();
    } else {
        if (_memberMore) _memberMore->setEnabled(false);
        QMessageBox::warning(this, tr("群成员"), MutationError(error));
    }
}

void GroupManagementDialog::HandleMutationResult(const QString &requestId, int requestType,
                                                 int error, const QJsonObject &response)
{
    const auto found = _mutations.find(requestId);
    if (found == _mutations.end()) return;
    const Action action = found.value();
    _mutations.erase(found);
    if (error != ErrorCodes::SUCCESS) {
        QMessageBox::warning(this, tr("群管理"), MutationError(error));
        if (error == ErrorCodes::GroupNotMember || error == ErrorCodes::GroupUnavailable) close();
        else UpdateControls();
        return;
    }
    if (response.value("status_conflict").toBool() || response.value("already_processed").toBool()) {
        const QJsonObject entry = response.value("entry").toObject();
        QMessageBox::information(this, tr("群状态已更新"),
            tr("服务端已有更新状态（%1），已按当前群信息刷新。")
                .arg(entry.value("status").toString()));
    } else if (action == Action::Invite) {
        QMessageBox::information(this, tr("群邀请"), tr("邀请已提交，等待对方处理。"));
    } else if (action == Action::Kick) {
        QMessageBox::information(this, tr("踢出群成员"), tr("成员移除已提交。"));
    } else if (action == Action::Leave) {
        QMessageBox::information(this, tr("退出群聊"), tr("已退出群聊；本地历史仍可只读查看。"));
    } else if (action == Action::Transfer) {
        QMessageBox::information(this, tr("转让群主"), tr("群主转让已提交。"));
    } else if (action == Action::Dissolve) {
        QMessageBox::information(this, tr("解散群聊"), tr("解散请求已提交。"));
    } else if (action == Action::Role) {
        QMessageBox::information(this, tr("成员角色"), tr("角色变更已提交。"));
    } else if (action == Action::Announcement) {
        _announcement->setReadOnly(true);
        _editAnnouncement->setText(tr("编辑公告"));
        QMessageBox::information(this, tr("群公告"), tr("公告已提交。"));
    } else if (action == Action::Approval) {
        QMessageBox::information(this, tr("审批设置"), tr("审批设置已提交。"));
    }
    Q_UNUSED(requestType);
    UpdateControls();
}

void GroupManagementDialog::HandleGroupInvalidation(qint64 threadId, bool active)
{
    if (threadId != _group.threadId || active) return;
    ++_memberGeneration;
    _memberRequests.clear();
    _memberRequestId.clear();
    if (_approvalsDialog) _approvalsDialog->close();
    close();
}


void GroupManagementDialog::UpdateSelectedMemberActions()
{
    if (!_role || !_transfer || !_kick) return;
    const QListWidgetItem *item = _members ? _members->currentItem() : nullptr;
    const bool owner = CanManageRoles();
    const bool manager = CanManageAnnouncement();
    const int role = item ? item->data(Qt::UserRole + 1).toInt() : -1;
    const qint64 uid = item ? item->data(Qt::UserRole).toLongLong() : 0;
    const qint64 version = item ? item->data(Qt::UserRole + 2).toLongLong() : 0;
    const bool rowCurrent = item && item->data(Qt::UserRole + 3).toLongLong() == _group.version
        && version > 0 && _knownMembers.value(uid).membershipVersion == version;
    const bool manageable = owner && rowCurrent && uid != _accountUid && (role == 0 || role == 1);
    _role->setVisible(owner);
    _role->setEnabled(manageable);
    _role->setText(role == 1 ? tr("移除管理员") : tr("任命管理员"));
    _kick->setVisible(manager);
    _kick->setEnabled(manager && rowCurrent && uid != _accountUid
        && (role == 0 || (owner && role == 1)));
    _transfer->setVisible(owner);
    _transfer->setEnabled(manageable);
    if (_invite) _invite->setEnabled(CanUseCurrentGroup() && _friendPicker && _friendPicker->count() > 0);
}

void GroupManagementDialog::InviteSelectedFriend()
{
    if (!CanUseCurrentGroup() || !_friendPicker || _friendPicker->currentIndex() < 0) return;
    const qint64 uid = _friendPicker->currentData().toLongLong();
    if (uid <= 0 || uid == _accountUid) return;
    const QString requestId = TcpMgr::GetInstance()->InviteGroupMember(_group.threadId, uid);
    if (requestId.isEmpty()) {
        QMessageBox::warning(this, tr("群邀请"), tr("邀请未发送，请检查连接。"));
        return;
    }
    RecordMutation(requestId, Action::Invite);
}

void GroupManagementDialog::ChangeSelectedRole()
{
    if (!CanManageRoles() || !_members || !_members->currentItem()) return;
    const QListWidgetItem *item = _members->currentItem();
    const qint64 uid = item->data(Qt::UserRole).toLongLong();
    const int role = item->data(Qt::UserRole + 1).toInt();
    const qint64 version = item->data(Qt::UserRole + 2).toLongLong();
    if (uid <= 0 || uid == _accountUid || (role != 0 && role != 1) || version <= 0
        || item->data(Qt::UserRole + 3).toLongLong() != _group.version) return;
    const int newRole = role == 1 ? 0 : 1;
    const QString action = newRole == 1 ? tr("任命为管理员") : tr("移除管理员");
    if (QMessageBox::question(this, tr("成员角色"),
        tr("确定将 UID %1 %2？").arg(QString::number(uid), action)) != QMessageBox::Yes
        || !CanManageRoles()) return;
    const QString requestId = TcpMgr::GetInstance()->SetGroupAdmin(
        _group.threadId, uid, newRole, version);
    if (requestId.isEmpty()) {
        QMessageBox::warning(this, tr("成员角色"), tr("角色变更未发送，请刷新成员列表。"));
        return;
    }
    RecordMutation(requestId, Action::Role);
}

void GroupManagementDialog::KickSelectedMember()
{
    LocalChatThread current;
    // 按当前角色预筛目标，仍由服务端授权；目标成员版本随踢出请求一起校验。
    if (!CanUseCurrentGroup() || !ReadCurrentGroup(&current) || current.role < 1
        || !_members || !_members->currentItem()) return;
    const QListWidgetItem *item = _members->currentItem();
    const qint64 uid = item->data(Qt::UserRole).toLongLong();
    const int role = item->data(Qt::UserRole + 1).toInt();
    const qint64 version = item->data(Qt::UserRole + 2).toLongLong();
    const bool allowedTargetRole = role == 0 || (current.role == 2 && role == 1);
    if (uid <= 0 || uid == _accountUid || !allowedTargetRole || version <= 0
        || item->data(Qt::UserRole + 3).toLongLong() != _group.version
        || _knownMembers.value(uid).membershipVersion != version) return;
    if (QMessageBox::question(this, tr("踢出群成员"),
        tr("确定将 UID %1 移出群？").arg(QString::number(uid))) != QMessageBox::Yes
        || !CanUseCurrentGroup() || !ReadCurrentGroup(&current) || current.role < 1
        || (role == 1 && current.role != 2)) return;
    const QString requestId = TcpMgr::GetInstance()->KickGroupMember(
        _group.threadId, uid, version);
    if (requestId.isEmpty()) {
        QMessageBox::warning(this, tr("踢出群成员"), tr("踢出请求未发送，请刷新成员列表。"));
        return;
    }
    RecordMutation(requestId, Action::Kick);
}

void GroupManagementDialog::TransferSelectedOwner()
{
    if (!CanManageRoles() || !_members || !_members->currentItem()) return;
    const QListWidgetItem *item = _members->currentItem();
    const qint64 uid = item->data(Qt::UserRole).toLongLong();
    const int role = item->data(Qt::UserRole + 1).toInt();
    const qint64 version = item->data(Qt::UserRole + 2).toLongLong();
    if (uid <= 0 || uid == _accountUid || (role != 0 && role != 1) || version <= 0
        || item->data(Qt::UserRole + 3).toLongLong() != _group.version) return;
    if (QMessageBox::question(this, tr("转让群主"),
        tr("确定将群主身份转让给 UID %1？转让后你将成为普通成员，且不能撤销本操作。")
            .arg(QString::number(uid))) != QMessageBox::Yes || !CanManageRoles()) return;
    const QString requestId = TcpMgr::GetInstance()->TransferGroupOwner(_group.threadId, uid, version);
    if (requestId.isEmpty()) {
        QMessageBox::warning(this, tr("转让群主"), tr("转让请求未发送，请刷新成员列表。"));
        return;
    }
    RecordMutation(requestId, Action::Transfer);
}

void GroupManagementDialog::LeaveGroup()
{
    if (_group.role == 2) {
        QMessageBox::information(this, tr("退出群聊"), tr("群主不能主动退出；请先转让群主或解散群聊。"));
        return;
    }
    LocalChatThread current;
    // 离群只使本账号成员状态失效，携带最新 membership_version 保留本地消息历史。
    if (!CanUseCurrentGroup() || !ReadCurrentGroup(&current) || current.role == 2
        || current.membershipVersion <= 0) return;
    if (QMessageBox::question(this, tr("退出群聊"),
        tr("确定退出此群？群内历史会保留为只读本地记录。")) != QMessageBox::Yes
        || !CanUseCurrentGroup()) return;
    const QString requestId = TcpMgr::GetInstance()->LeaveGroup(_group.threadId,
                                                                 current.membershipVersion);
    if (requestId.isEmpty()) {
        QMessageBox::warning(this, tr("退出群聊"), tr("退出请求未发送。"));
        return;
    }
    RecordMutation(requestId, Action::Leave);
}

void GroupManagementDialog::DissolveGroup()
{
    if (!CanOwnManage()) return;
    if (QMessageBox::question(this, tr("解散群聊"),
        tr("解散后群成员将不能继续收发群消息，但本地历史仍保留为只读。确定解散？"))
        != QMessageBox::Yes || !CanOwnManage()) return;
    const QString requestId = TcpMgr::GetInstance()->DissolveGroup(_group.threadId);
    if (requestId.isEmpty()) {
        QMessageBox::warning(this, tr("解散群聊"), tr("解散请求未发送。"));
        return;
    }
    RecordMutation(requestId, Action::Dissolve);
}

void GroupManagementDialog::OpenApprovals()
{
    if (!CanManageAnnouncement()) return;
    if (_approvalsDialog) { _approvalsDialog->raise(); return; }
    _approvalsDialog = new GroupRequestsDialog(_group, this);
    _approvalsDialog->setAttribute(Qt::WA_DeleteOnClose);
    _approvalsDialog->show();
}

void GroupManagementDialog::EditAnnouncement()
{
    if (!CanManageAnnouncement() || !_announcement || !_editAnnouncement) return;
    if (_announcement->isReadOnly()) {
        _announcement->setReadOnly(false);
        _editAnnouncement->setText(tr("保存公告"));
        _announcement->setFocus();
        return;
    }
    const QString text = _announcement->toPlainText();
    if (text.toUtf8().size() > 1024) {
        QMessageBox::warning(this, tr("群公告"), tr("公告不能超过 1024 UTF-8 字节。"));
        return;
    }
    const QString requestId = TcpMgr::GetInstance()->UpdateGroupAnnouncement(
        _group.threadId, text, _group.announcementVersion);
    if (requestId.isEmpty()) {
        QMessageBox::warning(this, tr("群公告"), tr("公告请求过长或无法发送；未截断文本。"));
        return;
    }
    RecordMutation(requestId, Action::Announcement);
}
void GroupManagementDialog::ChooseAndPublishAvatar()
{
    if (!CanManageAnnouncement() || !_avatarTransfer || !_chooseAvatar
        || !_pendingAvatarTaskId.isEmpty()) return;
    QFileDialog picker(this, tr("选择群头像"));
    picker.setFileMode(QFileDialog::ExistingFile);
    picker.setNameFilter(tr("PNG/JPEG 图片 (*.png *.jpg *.jpeg)"));
    picker.setOption(QFileDialog::DontUseNativeDialog);
    if (picker.exec() != QDialog::Accepted) return;
    const QString path = picker.selectedFiles().value(0);
    QImage preview;
    QString mime, error;
    if (!GroupAvatarTransfer::InspectImage(path, &preview, &mime, &error)) {
        QMessageBox::warning(this, tr("群头像"), error);
        return;
    }
    const QString taskId = _avatarTransfer->StartUpload(_group, path);
    if (taskId.isEmpty()) {
        QMessageBox::warning(this, tr("群头像"), tr("无法启动群头像上传；当前头像未更改。"));
        return;
    }
    AvatarUtil::SetRoundGroupAvatar(_avatarPreview, preview);
    _pendingAvatarTaskId = taskId;
    _pendingAvatarExpectedVersion = _group.avatarVersion;
    _pendingAvatarResourceId = taskId;
    UpdateControls();
}

void GroupManagementDialog::HandleAvatarReady(int accountUid, quint64 loginGeneration,
                                               qint64 threadId, qint64 groupVersion,
                                               qint64 avatarVersion, const QString &resourceId,
                                               const QString &cachePath)
{
    LocalChatThread current;
    if (accountUid != _accountUid || loginGeneration != _loginGeneration
        || TcpMgr::GetInstance()->LoginGeneration() != loginGeneration
        || threadId != _group.threadId || !ReadCurrentGroup(&current)
        || !current.active || current.membershipVersion <= 0
        || current.version < groupVersion || current.avatarVersion != avatarVersion
        || current.avatarResourceId != resourceId
        || GroupAvatarTransfer::CachePath(accountUid, FromCache(current)) != cachePath) return;
    AvatarUtil::SetRoundGroupAvatar(_avatarPreview, cachePath);
}

void GroupManagementDialog::HandleAvatarUploadFinished(
    const QString &taskId, int accountUid, quint64 loginGeneration, qint64 threadId,
    qint64 expectedAvatarVersion, const QString &resourceId, bool success, const QString &message)
{
    if (taskId != _pendingAvatarTaskId || accountUid != _accountUid
        || loginGeneration != _loginGeneration
        || TcpMgr::GetInstance()->LoginGeneration() != loginGeneration
        || threadId != _group.threadId
        || expectedAvatarVersion != _pendingAvatarExpectedVersion
        || resourceId != _pendingAvatarResourceId) return;
    _pendingAvatarTaskId.clear();
    _pendingAvatarExpectedVersion = 0;
    _pendingAvatarResourceId.clear();
    UpdateControls();
    if (success) {
        QMessageBox::information(this, tr("群头像"), message);
        LocalChatThread current;
        if (_avatarTransfer && ReadCurrentGroup(&current) && current.active)
            _avatarTransfer->RequestAvatar(FromCache(current));
        return;
    }
    LocalChatThread current;
    const QString cachePath = _avatarTransfer && ReadCurrentGroup(&current)
        ? _avatarTransfer->VerifiedCachePath(FromCache(current)) : QString();
    AvatarUtil::SetRoundGroupAvatar(_avatarPreview, cachePath);
    QMessageBox::warning(this, tr("群头像"), message);
}


QString GroupManagementDialog::RoleName(int role) const
{
    if (role == 2) return tr("群主");
    if (role == 1) return tr("管理员");
    return tr("成员");
}

void GroupManagementDialog::RecordMutation(const QString &requestId, Action action)
{
    if (!requestId.isEmpty()) _mutations.insert(requestId, action);
}
