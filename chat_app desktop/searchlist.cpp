#include "searchlist.h"
#include "adduseritem.h"
#include "avatarutil.h"
#include "findsuccessdlg.h"
#include "groupavatartransfer.h"
#include "tcpmgr.h"
#include "usermgr.h"
#include <QEvent>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

SearchList::SearchList(QWidget *parent) : QListWidget(parent)
{
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setSpacing(2);
    viewport()->installEventFilter(this);
    BuildUi();
    auto tcp = TcpMgr::GetInstance();
    connect(tcp.get(), &TcpMgr::sig_user_search, this, &SearchList::ShowUserResult);
    connect(tcp.get(), &TcpMgr::sig_group_search_result, this, &SearchList::HandleSearch);
    connect(tcp.get(), &TcpMgr::sig_group_mutation_result, this, &SearchList::HandleMutation);
    // TcpMgr 先持久化成员证明，再发此缓存快照；这里仅暂存，不根据后台回包导航。
    connect(tcp.get(), &TcpMgr::sig_group_info, this,
            [this](const GroupInfo &group, bool confirmed) {
        if (confirmed && !_requestId.isEmpty() && IsCurrentSession()
            && (_requestType == ID_CREATE_GROUP_REQ
                || (_requestType == ID_JOIN_GROUP_REQ && group.threadId == _group.threadId)))
            _confirmedGroup = group;
    });
}

void SearchList::BuildUi()
{
    const QStringList labels{tr("查找用户"), tr("添加群聊"), tr("创建群聊")};
    const QIcon icons[]{QIcon(QStringLiteral(":/res/search_user.png")),
                        QIcon(QStringLiteral(":/res/search_group.png")),
                        QIcon(QStringLiteral(":/res/create_group.png"))};
    for (int index = 0; index < labels.size(); ++index) {
        auto *tip = new AddUserItem(this);
        tip->SetTip(labels[index], icons[index]);
        auto *item = new QListWidgetItem(this);
        item->setSizeHint(tip->sizeHint());
        item->setData(Qt::UserRole, index);
        setItemWidget(item, tip);
    }
    connect(this, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (item == _cardItem) return;
        Activate(static_cast<Mode>(item->data(Qt::UserRole).toInt() + 1));
    });
    _card = new QWidget(this);
    _card->setObjectName(QStringLiteral("group_search_card"));
    _card->setMinimumWidth(0);
    auto *layout = new QVBoxLayout(_card);
    layout->setContentsMargins(12, 10, 12, 12);
    layout->setSpacing(8);
    _heading = new QLabel(_card);
    _heading->setObjectName(QStringLiteral("group_search_heading"));
    layout->addWidget(_heading);
    auto *identity = new QHBoxLayout;
    identity->setSpacing(8);
    _avatar = new QLabel(_card);
    _avatar->setFixedSize(40, 40);
    identity->addWidget(_avatar, 0, Qt::AlignTop);
    auto *details = new QVBoxLayout;
    details->setSpacing(3);
    _name = new QLabel(_card);
    _name->setObjectName(QStringLiteral("group_search_name"));
    _name->setWordWrap(true);
    _name->setMinimumWidth(0);
    _number = new QLabel(_card);
    _number->setObjectName(QStringLiteral("group_search_number"));
    _number->setTextInteractionFlags(Qt::TextSelectableByMouse);
    _number->setWordWrap(true);
    _number->setMinimumWidth(0);
    details->addWidget(_name);
    identity->addLayout(details, 1);
    layout->addLayout(identity);
    layout->addWidget(_number);
    _formEdit = new QLineEdit(_card);
    _formEdit->setObjectName(QStringLiteral("group_search_form"));
    _formEdit->setMinimumWidth(0);
    layout->addWidget(_formEdit);
    _submit = new QPushButton(_card);
    _submit->setObjectName(QStringLiteral("group_search_submit"));
    layout->addWidget(_submit);
    _status = new QLabel(_card);
    _status->setObjectName(QStringLiteral("group_search_status"));
    _status->setWordWrap(true);
    _status->setMinimumWidth(0);
    layout->addWidget(_status);
    _cardItem = new QListWidgetItem(this);
    _cardItem->setFlags(Qt::ItemIsEnabled);
    setItemWidget(_cardItem, _card);
    connect(_submit, &QPushButton::clicked, this, &SearchList::Submit);
    connect(_formEdit, &QLineEdit::returnPressed, this, &SearchList::Submit);
    ResetSearch();
}

void SearchList::SetSearchEdit(QWidget *edit)
{
    if (_searchEdit) disconnect(_searchEdit, nullptr, this, nullptr);
    _searchEdit = qobject_cast<QLineEdit *>(edit);
    if (!_searchEdit) return;
    connect(_searchEdit, &QLineEdit::textChanged, this, [this] { ResetSearch(); });
    connect(_searchEdit, &QLineEdit::returnPressed, this, [this] {
        if (_mode == Mode::Group) SearchGroup();
        else Activate(Mode::User);
    });
}

void SearchList::SetAvatarTransfer(GroupAvatarTransfer *transfer)
{
    if (_avatarTransfer) disconnect(_avatarTransfer, nullptr, this, nullptr);
    _avatarTransfer = transfer;
    if (!transfer) return;
    connect(transfer, &GroupAvatarTransfer::sig_avatar_ready, this,
            [this](int uid, quint64 generation, qint64 threadId, qint64,
                   qint64 version, const QString &resource, const QString &path) {
        if (IsCurrentSession() && uid == _accountUid && generation == _loginGeneration
            && _mode == Mode::Group && threadId == _group.threadId
            && version == _group.avatarVersion && resource == _group.avatarResourceId)
            AvatarUtil::SetRoundGroupAvatar(_avatar, path);
    });
}

void SearchList::ResetSearch()
{
    // 用户搜索协议没有 requestId；同代次串行等待，重登不继承旧等待。
    if (_userWireGeneration != TcpMgr::GetInstance()->LoginGeneration()) _userWirePending = false;
    _mode = Mode::None;
    _requestId.clear();
    _requestType = 0;
    _accountUid = 0;
    _loginGeneration = 0;
    _searchedThread = 0;
    _group = {};
    _confirmedGroup = {};
    _applicationPending = false;
    _userResultWanted = false;
    _formEdit->clear();
    clearSelection();
    _cardItem->setHidden(true);
}

void SearchList::CaptureSession()
{
    const auto user = UserMgr::GetInstance()->GetUserInfo();
    _accountUid = user ? user->_uid : 0;
    _loginGeneration = TcpMgr::GetInstance()->LoginGeneration();
}

bool SearchList::IsCurrentSession() const
{
    const auto user = UserMgr::GetInstance()->GetUserInfo();
    return user && _accountUid > 0 && user->_uid == _accountUid
        && _loginGeneration == TcpMgr::GetInstance()->LoginGeneration();
}

void SearchList::Activate(Mode mode)
{
    if (!_requestId.isEmpty() && mode == _mode) return;
    if (mode == Mode::User && _mode == Mode::User && _userWirePending
        && _userResultWanted && IsCurrentSession()) return;
    ResetSearch();
    _mode = mode;
    setCurrentRow(static_cast<int>(mode) - 1);
    CaptureSession();
    _cardItem->setHidden(false);
    UpdateCard();
    if (mode == Mode::Group) {
        SearchGroup();
    } else if (mode == Mode::User) {
        if (!_searchEdit || _searchEdit->text().trimmed().isEmpty()) {
            SetStatus(tr("请输入用户 UID 或昵称"));
            return;
        }
        if (_userWirePending) {
            SetStatus(tr("上一条用户查找仍在处理中，请稍后重试"));
            return;
        }
        _userWirePending = true;
        _userWireGeneration = _loginGeneration;
        _userResultWanted = true;
        SetStatus(tr("正在查找用户…"));
        _submit->setEnabled(false);
        const QJsonObject request{{"uid", _searchEdit->text()}};
        emit TcpMgr::GetInstance()->sig_send_data(ID_SEARCH_USER_REQ,
                    QJsonDocument(request).toJson(QJsonDocument::Compact));
    } else {
        _formEdit->setFocus();
    }
}

void SearchList::SearchGroup()
{
    if (!_requestId.isEmpty() || _mode != Mode::Group) return;
    bool valid = false;
    const QString text = _searchEdit ? _searchEdit->text().trimmed() : QString();
    const qint64 thread = text.toLongLong(&valid);
    for (const QChar ch : text) valid = valid && ch >= QLatin1Char('0') && ch <= QLatin1Char('9');
    if (!valid || thread <= 0) {
        SetStatus(tr("请在上方输入完整群号，再点击查找群聊"), true);
        return;
    }
    _searchedThread = thread;
    _group = {};
    _applicationPending = false;
    _confirmedGroup = {};
    CaptureSession();
    _requestType = ID_SEARCH_GROUP_REQ;
    _requestId = TcpMgr::GetInstance()->SearchGroup(thread);
    UpdateCard();
    SetStatus(_requestId.isEmpty() ? tr("暂时无法查找，请检查连接后重试") : tr("正在查找群聊…"),
              _requestId.isEmpty());
}

void SearchList::Submit()
{
    if (!_requestId.isEmpty()) return;
    if (_mode == Mode::User) { Activate(Mode::User); return; }
    if (_mode == Mode::Group && _group.threadId <= 0) { SearchGroup(); return; }
    if (_mode == Mode::Group && _group.member && _group.active) {
        if (IsCurrentSession()) emit sig_enter_group(_group.threadId);
        return;
    }
    if (_mode == Mode::Group && (_applicationPending || !_group.active)) return;
    if (_mode != Mode::Create && _mode != Mode::Group) return;
    const QString text = _formEdit->text();
    if (_mode == Mode::Create && (text.trimmed().isEmpty() || text.toUtf8().size() > 255)) {
        SetStatus(tr("请输入群名（不超过 255 字节）"), true);
        return;
    }
    if (_mode == Mode::Group && text.toUtf8().size() > 256) {
        SetStatus(tr("申请说明不能超过 256 字节"), true);
        return;
    }
    CaptureSession();
    _confirmedGroup = {};
    _requestType = _mode == Mode::Create ? ID_CREATE_GROUP_REQ : ID_JOIN_GROUP_REQ;
    _requestId = _mode == Mode::Create ? TcpMgr::GetInstance()->CreateGroup(text)
                                     : TcpMgr::GetInstance()->JoinGroup(_group.threadId, text);
    UpdateCard();
    SetStatus(_requestId.isEmpty() ? tr("暂时无法提交，请检查连接后重试") : tr("正在提交…"),
              _requestId.isEmpty());
}

void SearchList::HandleSearch(const QString &requestId, int error, const GroupInfo &group)
{
    if (_mode != Mode::Group || requestId != _requestId || _requestId.isEmpty()
        || _requestType != ID_SEARCH_GROUP_REQ || !IsCurrentSession()) return;
    _requestId.clear();
    if (error != ErrorCodes::SUCCESS || group.threadId != _searchedThread) {
        UpdateCard();
        SetStatus(tr("未找到群聊或查找失败（%1），可重新查找").arg(error), true);
        return;
    }
    _group = group;
    UpdateCard();
    SetStatus(group.member ? tr("你已加入此群")
              : group.approvalRequired ? tr("提交申请后需等待管理员审批") : tr("可填写申请说明后加入"));
    if (_avatarTransfer && group.member && group.active) _avatarTransfer->RequestAvatar(group);
}

void SearchList::HandleMutation(const QString &requestId, int requestType, int error,
                                const QJsonObject &response)
{
    if (_requestId.isEmpty() || requestId != _requestId || requestType != _requestType
        || !IsCurrentSession()) return;
    _requestId.clear();
    if (error != ErrorCodes::SUCCESS) {
        UpdateCard();
        SetStatus(tr("操作失败（%1），请检查后重试").arg(error), true);
        return;
    }
    if (requestType == ID_JOIN_GROUP_REQ
        && response.value("admission").toString() == QStringLiteral("pending")) {
        _applicationPending = true;
        UpdateCard();
        SetStatus(tr("申请已提交，等待管理员审批；目前尚未加入群聊"));
        return;
    }
    const qint64 threadId = response.value("group").toObject().value("thread_id").toVariant().toLongLong();
    if (response.value("membership_granted").toBool() && _confirmedGroup.threadId == threadId
        && threadId > 0 && _confirmedGroup.active && _confirmedGroup.member) {
        const qint64 enteredThreadId = _confirmedGroup.threadId;
        ResetSearch();
        emit sig_enter_group(enteredThreadId);
        return;
    }
    UpdateCard();
    SetStatus(tr("操作已返回，但尚未确认当前成员身份，请重新查找群聊"), true);
}

void SearchList::ShowUserResult(std::shared_ptr<SearchInfo> &info)
{
    const bool wanted = _userWirePending && _userResultWanted && _mode == Mode::User && IsCurrentSession();
    _userWirePending = false;
    _userResultWanted = false;
    if (!wanted) return;
    _submit->setEnabled(true);
    if (!info) { SetStatus(tr("未找到用户，请检查 UID 或昵称后重试"), true); return; }
    SetStatus(tr("已找到用户"));
    auto *dialog = new FindSuccessDlg(this);
    dialog->setSearchInfo(info);
    dialog->setModal(true);
    dialog->show();
}

void SearchList::UpdateCard()
{
    const bool groupFound = _mode == Mode::Group && _group.threadId > 0;
    const bool create = _mode == Mode::Create;
    _heading->setText(create ? tr("创建群聊") : _mode == Mode::User ? tr("查找用户") : tr("添加群聊"));
    _avatar->setVisible(groupFound);
    _name->setVisible(groupFound);
    _number->setVisible(groupFound);
    _name->setTextFormat(Qt::PlainText);
    _name->setText(_group.name);
    _number->setText(tr("群号：%1").arg(_group.threadId));
    if (groupFound) {
        const QString path = _avatarTransfer && _group.member
            ? _avatarTransfer->VerifiedCachePath(_group) : QString();
        AvatarUtil::SetRoundGroupAvatar(_avatar, path);
    }
    _formEdit->setVisible(create || (groupFound && !_group.member && _group.active && !_applicationPending));
    _formEdit->setPlaceholderText(create ? tr("输入群名") : tr("申请说明（选填）"));
    _formEdit->setEnabled(_requestId.isEmpty());
    _submit->setText(create ? tr("创建并进入") : _mode == Mode::User ? tr("查找用户")
        : !groupFound ? tr("查找群聊") : !_group.active ? tr("群聊已停用")
        : _group.member ? tr("进入群聊") : _applicationPending ? tr("已申请，待审批")
        : _group.approvalRequired ? tr("提交入群申请") : tr("加入群聊"));
    _submit->setEnabled(_requestId.isEmpty() && !_applicationPending && (!groupFound || _group.active));
    SetStatus(create ? tr("创建后将直接进入新群聊") : QString());
    ResizeCard();
}

void SearchList::SetStatus(const QString &text, bool error)
{
    _status->setText(text);
    _status->setProperty("error", error);
    _status->style()->unpolish(_status);
    _status->style()->polish(_status);
    _status->setVisible(!text.isEmpty());
    ResizeCard();
}

void SearchList::ResizeCard()
{
    if (!_cardItem) return;
    const int width = qMax(1, viewport()->width() - 4);
    _card->setFixedWidth(width);
    _card->layout()->activate();
    const int height = _card->layout()->totalHeightForWidth(width);
    _cardItem->setSizeHint(QSize(width, height > 0 ? height : _card->sizeHint().height()));
}

bool SearchList::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == viewport() && event->type() == QEvent::Resize)
        QTimer::singleShot(0, this, &SearchList::ResizeCard);
    return QListWidget::eventFilter(watched, event);
}
