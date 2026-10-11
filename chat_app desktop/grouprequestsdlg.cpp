// 文件职责：真实群邀请/申请分页与明确决策界面；终态、账号和窗口代次阻止迟到 pending 回包恢复权限。
#include "grouprequestsdlg.h"

#include "global.h"
#include "localchatstoragemgr.h"
#include "tcpmgr.h"
#include "usermgr.h"

#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {
QString StatusText(const QString &status)
{
    if (status == QStringLiteral("pending")) return QObject::tr("待处理");
    if (status == QStringLiteral("accepted") || status == QStringLiteral("approved"))
        return QObject::tr("已同意");
    if (status == QStringLiteral("rejected")) return QObject::tr("已拒绝");
    if (status == QStringLiteral("invalid")) return QObject::tr("已失效");
    return status;
}
}

GroupRequestsDialog::GroupRequestsDialog(QWidget *parent)
    : QDialog(parent)
{
    const auto user = UserMgr::GetInstance()->GetUserInfo();
    _accountUid = user ? user->_uid : 0;
    setWindowTitle(tr("群邀请与申请"));
    setObjectName(QStringLiteral("group_requests_dialog"));
    resize(560, 440);
    BuildAccountViews();
    ConnectSignals();
    RequestPage(QStringLiteral("inbox"), true);
    RequestPage(QStringLiteral("applications"), true);
    RequestPage(QStringLiteral("results"), true);
}
void GroupRequestsDialog::ConnectSignals()
{
    const auto tcp = TcpMgr::GetInstance();
    connect(tcp.get(), &TcpMgr::sig_group_requests, this,
            [this](const QString &requestId, const QString &scope, qint64 threadId,
                   int error, qint64 groupVersion, const QList<GroupAdmissionInfo> &entries,
                   qint64 nextId, bool loadMore) {
        HandlePage(requestId, scope, threadId, error, groupVersion, entries, nextId, loadMore);
    });
    connect(tcp.get(), &TcpMgr::sig_group_mutation_result, this,
            [this](const QString &requestId, int requestType, int error, const QJsonObject &response) {
        HandleMutation(requestId, requestType, error, response);
    });
    connect(tcp.get(), &TcpMgr::sig_group_state, this,
            [this](qint64 threadId, bool active, const QString &, qint64) {
        HandleGroupInvalidation(threadId, active);
    });
    if (_approvalsOnly) {
        connect(tcp.get(), &TcpMgr::sig_group_info, this,
                [this](const GroupInfo &group, bool) { UpdateGroup(group); });
    }
    // 审批页也接收申请状态事件：新申请不一定推进群版本，必须独立刷新分页代次。
    connect(tcp.get(), &TcpMgr::sig_group_event, this,
            [this](const QString &eventType, const GroupInfo &,
                   const GroupAdmissionInfo &entry, qint64) {
        HandleAdmissionEvent(eventType, entry);
    });
}


GroupRequestsDialog::GroupRequestsDialog(const GroupInfo &group, QWidget *parent)
    : QDialog(parent), _group(group), _approvalsOnly(true)
{
    const auto user = UserMgr::GetInstance()->GetUserInfo();
    _accountUid = user ? user->_uid : 0;
    setWindowTitle(tr("待审批申请：%1").arg(group.name));
    setObjectName(QStringLiteral("group_approvals_dialog"));
    resize(560, 440);
    BuildApprovalView();
    ConnectSignals();
    if (CanManageApprovals()) RequestPage(QStringLiteral("approvals"), true);
    else _revoked = true;
}

void GroupRequestsDialog::BuildAccountViews()
{
    auto *layout = new QVBoxLayout(this);
    _tabs = new QTabWidget(this);
    layout->addWidget(_tabs);
    for (const auto &tab : {qMakePair(QStringLiteral("inbox"), tr("收到的邀请")),
                            qMakePair(QStringLiteral("applications"), tr("我的申请")),
                            qMakePair(QStringLiteral("results"), tr("处理结果"))}) {
        auto *page = new QWidget(_tabs);
        auto *pageLayout = new QVBoxLayout(page);
        auto *list = new QListWidget(page);
        const QString listName = tab.first == QStringLiteral("inbox")
            ? QStringLiteral("group_invitation_inbox")
            : tab.first == QStringLiteral("applications")
                ? QStringLiteral("group_application_list") : QStringLiteral("group_admission_results");
        list->setObjectName(listName);
        auto *more = new QPushButton(tr("加载更多"), page);
        more->setEnabled(false);
        pageLayout->addWidget(list);
        pageLayout->addWidget(more);
        ScopeView view;
        view.list = list;
        view.more = more;
        _views.insert(tab.first, view);
        _tabs->addTab(page, tab.second);
        connect(more, &QPushButton::clicked, this, [this, scope = tab.first] {
            RequestPage(scope, false);
        });
        connect(list, &QListWidget::currentItemChanged, this,
                [this](QListWidgetItem *, QListWidgetItem *) { UpdateActions(); });
    }
    auto *actions = new QHBoxLayout;
    _acceptButton = new QPushButton(tr("接受邀请"), this);
    _rejectButton = new QPushButton(tr("拒绝邀请"), this);
    actions->addWidget(_acceptButton);
    actions->addWidget(_rejectButton);
    actions->addStretch();
    auto *close = new QPushButton(tr("关闭"), this);
    actions->addWidget(close);
    layout->addLayout(actions);
    connect(_acceptButton, &QPushButton::clicked, this, [this] { Decide(true); });
    connect(_rejectButton, &QPushButton::clicked, this, [this] { Decide(false); });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        const QWidget *page = _tabs->widget(index);
        _activeScope = page ? page->property("scope").toString() : QString();
        if (_activeScope.isEmpty()) {
            for (auto it = _views.cbegin(); it != _views.cend(); ++it) {
                if (it->list && it->list->parentWidget() == page) { _activeScope = it.key(); break; }
            }
        }
        UpdateActions();
    });
    _activeScope = QStringLiteral("inbox");
    UpdateActions();
}

void GroupRequestsDialog::BuildApprovalView()
{
    auto *layout = new QVBoxLayout(this);
    auto *summary = new QLabel(tr("%1（群号 %2）").arg(_group.name, QString::number(_group.threadId)), this);
    layout->addWidget(summary);
    auto *list = new QListWidget(this);
    list->setObjectName(QStringLiteral("group_pending_approvals"));
    auto *more = new QPushButton(tr("加载更多待审批申请"), this);
    more->setEnabled(false);
    ScopeView view;
    view.list = list;
    view.more = more;
    _views.insert(QStringLiteral("approvals"), view);
    layout->addWidget(list);
    layout->addWidget(more);
    connect(more, &QPushButton::clicked, this, [this] {
        RequestPage(QStringLiteral("approvals"), false);
    });
    connect(list, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *, QListWidgetItem *) { UpdateActions(); });
    auto *actions = new QHBoxLayout;
    _acceptButton = new QPushButton(tr("批准申请"), this);
    _rejectButton = new QPushButton(tr("拒绝申请"), this);
    actions->addWidget(_acceptButton);
    actions->addWidget(_rejectButton);
    actions->addStretch();
    auto *close = new QPushButton(tr("关闭"), this);
    actions->addWidget(close);
    layout->addLayout(actions);
    connect(_acceptButton, &QPushButton::clicked, this, [this] { Decide(true); });
    connect(_rejectButton, &QPushButton::clicked, this, [this] { Decide(false); });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    _activeScope = QStringLiteral("approvals");
    UpdateActions();
}

bool GroupRequestsDialog::CanManageApprovals() const
{
    if (!_approvalsOnly || _revoked || _accountUid <= 0) return false;
    const auto user = UserMgr::GetInstance()->GetUserInfo();
    if (!user || user->_uid != _accountUid) return false;
    for (const LocalChatThread &thread : LocalChatStorageMgr::GetInstance()->LoadAllThreads()) {
        if (thread.threadId == _group.threadId && thread.threadType == QStringLiteral("group"))
            return thread.active && thread.version == _group.version
                && thread.membershipVersion == _group.membershipVersion && thread.role >= 1;
    }
    return false;
}

void GroupRequestsDialog::RequestPage(const QString &scope, bool reset)
{
    auto it = _views.find(scope);
    if (it == _views.end() || (!reset && !it->requestId.isEmpty())) return;
    if (_approvalsOnly && !CanManageApprovals()) {
        _revoked = true;
        UpdateActions();
        return;
    }
    ScopeView &view = it.value();
    // 新代次先撤销旧 requestId，保证超时/重放的旧分页不会重建可操作行。
    if (reset) {
        if (!view.requestId.isEmpty()) _pendingGenerations.remove(view.requestId);
        ++view.generation;
        view.requestId.clear();
        view.nextId = 0;
        view.loadMore = false;
        if (view.list) view.list->clear();
        if (view.more) view.more->setEnabled(false);
    }
    const QString requestId = TcpMgr::GetInstance()->LoadGroupRequests(
        scope, view.nextId, _approvalsOnly ? _group.threadId : 0);
    if (requestId.isEmpty()) {
        QMessageBox::warning(this, tr("群申请"), tr("请求未发送，请检查连接后重试。"));
        return;
    }
    view.requestId = requestId;
    _pendingGenerations.insert(requestId, view.generation);
    if (view.more) view.more->setEnabled(false);
}

void GroupRequestsDialog::UpdateGroup(const GroupInfo &group)
{
    if (!_approvalsOnly || group.threadId != _group.threadId || group.version < _group.version) return;
    const bool changed = group.version != _group.version
        || group.membershipVersion != _group.membershipVersion || group.role != _group.role
        || group.ownerUid != _group.ownerUid || group.active != _group.active;
    _group = group;
    if (!CanManageApprovals()) {
        _revoked = true;
        for (ScopeView &view : _views) {
            ++view.generation;
            if (!view.requestId.isEmpty()) _pendingGenerations.remove(view.requestId);
            view.requestId.clear();
            view.list->clear();
            view.more->setEnabled(false);
        }
        QMessageBox::information(this, tr("审批权限已变更"),
            tr("你已不再是此群的有效管理员，待审批列表已关闭。"));
        close();
        return;
    }
    if (changed) RequestPage(QStringLiteral("approvals"), true);
}

void GroupRequestsDialog::HandlePage(const QString &requestId, const QString &scope, qint64 threadId,
                                    int error, qint64 groupVersion,
                                    const QList<GroupAdmissionInfo> &entries, qint64 nextId, bool loadMore)
{
    // 分页只能在本窗口代次及同一群版本内落地；撤权或版本变化时旧页立即失效。
    auto it = _views.find(scope);
    if (it == _views.end() || it->requestId != requestId
        || !_pendingGenerations.contains(requestId)
        || _pendingGenerations.take(requestId) != it->generation
        || (_approvalsOnly && (scope != QStringLiteral("approvals") || threadId != _group.threadId))
        || (!_approvalsOnly && (scope == QStringLiteral("approvals") || threadId != 0))) return;
    ScopeView &view = it.value();
    view.requestId.clear();
    if (_approvalsOnly && (!CanManageApprovals() || groupVersion != _group.version)) {
        if (!CanManageApprovals()) {
            _revoked = true;
            close();
        } else {
            RequestPage(scope, true);
        }
        return;
    }
    if (error != ErrorCodes::SUCCESS) {
        if (_approvalsOnly && (error == ErrorCodes::GroupNotMember
            || error == ErrorCodes::GroupPermissionDenied)) {
            _revoked = true;
            QMessageBox::information(this, tr("审批权限已变更"),
                tr("服务端已撤销此群审批权限，待审批列表已关闭。"));
            close();
        } else {
            QMessageBox::warning(this, tr("群申请"), tr("加载失败（%1）。").arg(error));
        }
        UpdateActions();
        return;
    }
    qint64 lastId = view.nextId;
    for (const GroupAdmissionInfo &entry : entries) {
        if (entry.id <= lastId || entry.threadId <= 0
            || (scope == QStringLiteral("inbox") && entry.kind != QStringLiteral("invitation"))
            || (scope == QStringLiteral("applications")
                && entry.kind != QStringLiteral("application"))
            || ((scope == QStringLiteral("results") || scope == QStringLiteral("approvals"))
                && entry.kind != QStringLiteral("invitation")
                && entry.kind != QStringLiteral("application"))
            || (scope == QStringLiteral("approvals")
                && entry.kind != QStringLiteral("application"))) {
            view.list->clear();
            view.nextId = 0;
            view.loadMore = false;
            QMessageBox::warning(this, tr("群申请"), tr("服务端申请页顺序或类型无效，已丢弃。"));
            UpdateActions();
            return;
        }
        lastId = entry.id;
        const QString effectiveStatus = RememberAdmissionStatus(entry);
        QString text;
        if (entry.kind == QStringLiteral("invitation")) {
            if (scope == QStringLiteral("inbox")) {
                text = tr("群邀请：%1（群号 %2），邀请人 UID %3，状态：%4")
                    .arg(entry.groupName, QString::number(entry.threadId),
                         QString::number(entry.inviterUid), StatusText(effectiveStatus));
            } else {
                text = tr("群邀请结果：%1（群号 %2），状态：%3")
                    .arg(entry.groupName, QString::number(entry.threadId), StatusText(effectiveStatus));
            }
        } else {
            const QString prefix = scope == QStringLiteral("approvals")
                ? tr("UID %1 申请加入") : tr("入群申请");
            if (scope == QStringLiteral("approvals")) {
                text = tr("%1 %2（%3，群号 %4），状态：%5")
                    .arg(prefix, QString::number(entry.applicantUid), entry.groupName,
                         QString::number(entry.threadId), StatusText(effectiveStatus));
            } else {
                text = tr("%1：%2（群号 %3），状态：%4")
                    .arg(prefix, entry.groupName, QString::number(entry.threadId),
                         StatusText(effectiveStatus));
            }
        }
        if (!entry.explanation.isEmpty()) text += tr("\n说明：%1").arg(entry.explanation);
        auto *item = new QListWidgetItem(text, view.list);
        item->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(entry.id));
        item->setData(Qt::UserRole + 1, QVariant::fromValue<qlonglong>(entry.threadId));
        item->setData(Qt::UserRole + 2, entry.kind);
        item->setData(Qt::UserRole + 3, effectiveStatus);
        item->setData(Qt::UserRole + 4, QVariant::fromValue<qlonglong>(entry.applicantUid));
        item->setData(Qt::UserRole + 5, QVariant::fromValue<qlonglong>(entry.inviterUid));
    }
    const bool canContinue = loadMore && !entries.isEmpty() && nextId == lastId && nextId > view.nextId;
    if (loadMore && !canContinue) {
        view.list->clear();
        view.nextId = 0;
        view.loadMore = false;
        QMessageBox::warning(this, tr("群申请"), tr("服务端分页游标无效，已丢弃当前列表。"));
        UpdateActions();
        return;
    }
    view.nextId = canContinue ? nextId : lastId;
    view.loadMore = canContinue;
    if (view.more) view.more->setEnabled(canContinue);
    UpdateActions();
}

void GroupRequestsDialog::HandleMutation(const QString &requestId, int requestType, int error,
                                         const QJsonObject &response)
{
    if (requestId != _decisionRequestId) return;
    _decisionRequestId.clear();
    if (_acceptButton) _acceptButton->setEnabled(true);
    if (_rejectButton) _rejectButton->setEnabled(true);
    if (error != ErrorCodes::SUCCESS) {
        QMessageBox::warning(this, tr("处理群申请"), tr("服务端拒绝操作（%1）。").arg(error));
        _decisionAdmissionId = 0;
        _decisionThreadId = 0;
        _decisionKind.clear();
        if (_approvalsOnly && (error == ErrorCodes::GroupNotMember
            || error == ErrorCodes::GroupPermissionDenied)) close();
        else UpdateActions();
        return;
    }
    const QJsonObject entryObject = response.value("entry").toObject();
    QString status = response.value("status").toString();
    if (status.isEmpty()) status = entryObject.value("status").toString();
    if (_decisionAdmissionId > 0 && !status.isEmpty()) {
        GroupAdmissionInfo entry;
        entry.id = _decisionAdmissionId;
        entry.threadId = _decisionThreadId;
        entry.kind = _decisionKind;
        entry.status = status;
        RememberAdmissionStatus(entry);
    }
    const bool conflict = response.value("status_conflict").toBool();
    const bool already = response.value("already_processed").toBool();
    _decisionAdmissionId = 0;
    _decisionThreadId = 0;
    _decisionKind.clear();
    if (conflict || already) {
        QMessageBox::information(this, tr("群申请状态已更新"),
            tr("该申请已由其他操作处理，当前状态：%1。").arg(StatusText(status)));
    } else {
        QMessageBox::information(this, tr("群申请"),
            requestType == ID_DECIDE_GROUP_INVITE_REQ ? tr("邀请处理完成。") : tr("申请处理完成。"));
    }
    RefreshCurrentViews();
}

void GroupRequestsDialog::HandleGroupInvalidation(qint64 threadId, bool active)
{
    if (!_approvalsOnly || threadId != _group.threadId || active) return;
    _revoked = true;
    for (ScopeView &view : _views) {
        ++view.generation;
        if (!view.requestId.isEmpty()) _pendingGenerations.remove(view.requestId);
        view.requestId.clear();
        if (view.more) view.more->setEnabled(false);
    }
    close();
}

QString GroupRequestsDialog::AdmissionKey(const QString &kind, qint64 id)
{
    return kind + QLatin1Char(':') + QString::number(id);
}

QString GroupRequestsDialog::RememberAdmissionStatus(const GroupAdmissionInfo &entry)
{
    const QString key = AdmissionKey(entry.kind, entry.id);
    const QString oldStatus = _admissionStatuses.value(key);
    // terminal 状态不可被同一 admission 的旧 pending 页覆盖；新申请必须使用新 ID。
    const auto terminal = [](const QString &status) {
        return status == QStringLiteral("accepted") || status == QStringLiteral("approved")
            || status == QStringLiteral("rejected") || status == QStringLiteral("invalid");
    };
    if (terminal(oldStatus)) return oldStatus;
    if (!entry.status.isEmpty()) {
        if (terminal(entry.status) || oldStatus.isEmpty())
            _admissionStatuses.insert(key, entry.status);
    }
    return _admissionStatuses.value(key, entry.status);
}

void GroupRequestsDialog::HandleAdmissionEvent(const QString &eventType,
                                               const GroupAdmissionInfo &entry)
{
    if ((eventType != QStringLiteral("inbox") && eventType != QStringLiteral("result"))
        || entry.id <= 0 || entry.threadId <= 0
        || (entry.kind != QStringLiteral("invitation")
            && entry.kind != QStringLiteral("application"))) return;
    if (_approvalsOnly) {
        if (entry.kind != QStringLiteral("application") || entry.threadId != _group.threadId
            || !CanManageApprovals()) return;
        RememberAdmissionStatus(entry);
        // 新申请不一定改变群版本；此事件单独推进审批页代次并从游标零恢复。
        RequestPage(QStringLiteral("approvals"), true);
        return;
    }
    RememberAdmissionStatus(entry);
    RefreshCurrentViews();
}

void GroupRequestsDialog::RefreshCurrentViews()
{
    if (_approvalsOnly) {
        RequestPage(QStringLiteral("approvals"), true);
    } else {
        RequestPage(QStringLiteral("inbox"), true);
        RequestPage(QStringLiteral("applications"), true);
        RequestPage(QStringLiteral("results"), true);
    }
}

void GroupRequestsDialog::UpdateActions()
{
    if (!_acceptButton || !_rejectButton) return;
    ScopeView *view = _views.contains(_activeScope) ? &_views[_activeScope] : nullptr;
    const QListWidgetItem *item = view && view->list ? view->list->currentItem() : nullptr;
    const bool pending = item && item->data(Qt::UserRole + 3).toString() == QStringLiteral("pending");
    const QString kind = item ? item->data(Qt::UserRole + 2).toString() : QString();
    const bool allowed = pending && !_decisionRequestId.size()
        && (!_approvalsOnly || CanManageApprovals());
    _acceptButton->setEnabled(allowed && ((_activeScope == QStringLiteral("inbox")
        && kind == QStringLiteral("invitation")) || (_activeScope == QStringLiteral("approvals")
        && kind == QStringLiteral("application"))));
    _rejectButton->setEnabled(_acceptButton->isEnabled());
    if (view && view->more) view->more->setEnabled(view->loadMore && view->requestId.isEmpty());
}

void GroupRequestsDialog::Decide(bool approve)
{
    if (_decisionRequestId.isEmpty()) {
        auto it = _views.find(_activeScope);
        if (it == _views.end() || !it->list || !it->list->currentItem()) return;
        const QListWidgetItem *item = it->list->currentItem();
        if (item->data(Qt::UserRole + 3).toString() != QStringLiteral("pending")) return;
        const qint64 id = item->data(Qt::UserRole).toLongLong();
        const qint64 threadId = item->data(Qt::UserRole + 1).toLongLong();
        const QString kind = item->data(Qt::UserRole + 2).toString();
        if (_approvalsOnly && !CanManageApprovals()) { close(); return; }
        if (_activeScope == QStringLiteral("inbox") && kind == QStringLiteral("invitation")) {
            _decisionRequestId = TcpMgr::GetInstance()->DecideGroupInvitation(id, threadId, approve);
        } else if (_activeScope == QStringLiteral("approvals")
                   && kind == QStringLiteral("application")) {
            _decisionRequestId = TcpMgr::GetInstance()->DecideGroupApplication(id, threadId, approve);
        }
        _decisionAdmissionId = id;
        _decisionThreadId = threadId;
        _decisionKind = kind;
        if (_decisionRequestId.isEmpty()) {
            _decisionAdmissionId = 0;
            _decisionThreadId = 0;
            _decisionKind.clear();
            QMessageBox::warning(this, tr("群申请"), tr("操作未发送，请检查连接和权限。"));
            return;
        }
        if (_acceptButton) _acceptButton->setEnabled(false);
        if (_rejectButton) _rejectButton->setEnabled(false);
        return;
    }
}

