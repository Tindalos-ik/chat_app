#ifndef GROUPMANAGEMENTDIALOG_H
#define GROUPMANAGEMENTDIALOG_H

#include <QDialog>
#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include "userdata.h"

class QCheckBox;
class QComboBox;
class GroupAvatarTransfer;
class QLabel;
class QListWidget;
struct LocalChatThread;
class QPlainTextEdit;
class QPushButton;
class GroupRequestsPage;
// 文件职责：展示指定群的成员、权限、入群审批与群公告，并以缓存版本保护所有异步界面更新。
class GroupManagementDialog final : public QDialog
{
    Q_OBJECT
public:
    /** 初始化群管理窗体；group 是账号缓存群状态，avatarTransfer 仅处理群头像，parent 为界面父对象。 */
    explicit GroupManagementDialog(const GroupInfo &group, GroupAvatarTransfer *avatarTransfer,
                                   QWidget *parent = nullptr);
    /** 合并同群的新权威状态；忽略线程不匹配或版本回退的 group。 */
    void UpdateGroup(const GroupInfo &group);

private:
    struct MemberPage {
        quint64 generation = 0;
        qint64 groupVersion = 0;
        qint64 afterUid = 0;
    };
    enum class Action { Invite, Kick, Role, Transfer, Leave, Dissolve, Approval, Announcement };

    /** 按当前账号、群和缓存状态创建成员/设置界面；无参数，无返回值。 */
    void BuildUi();
    /** 连接 TCP、存储派生的群状态和窗口控件信号；无参数，无返回值。 */
    void ConnectSignals();
    /** 读取当前登录账号在本地缓存的群；thread 接收最新状态，返回是否找到该群。 */
    bool ReadCurrentGroup(LocalChatThread *thread) const;
    /** 检查当前账号、群版本和成员版本仍可用于操作；返回当前可用性。 */
    bool CanUseCurrentGroup() const;
    /** 检查当前账号是否是仍有效的群主；返回是否有任免管理员权限。 */
    bool CanManageRoles() const;
    /** 检查当前账号为群主或管理员；公告、头像发布及审批管理按钮据此预检。 */
    bool CanManageAnnouncement() const;
    /** 检查当前账号是否为仍有效的群主；返回是否可变更群级设置。 */
    bool CanOwnManage() const;
    /** 按缓存的权威权限更新按钮、公告和群状态；无参数，无返回值。 */
    void UpdateControls();
    /** 开启新的成员分页代次并可选加载首页；loadFirst 为真时立即请求。 */
    void ResetMemberPage(bool loadFirst);
    /** 请求 afterUid 之后的成员页；请求绑定窗口代次及群版本。 */
    void RequestMemberPage(qint64 afterUid);
    /** 接收成员页；参数绑定 threadId、requestId、groupVersion、成员、下一 UID 与续页标志。 */
    void HandleMembers(qint64 threadId, const QString &requestId, qint64 groupVersion,
                       const QList<GroupMemberInfo> &members, qint64 nextUid, bool loadMore);
    /** 处理当前成员页请求的错误码；requestId 用于丢弃旧分页结果。 */
    void HandleOperationResult(const QString &requestId, int error);
    /** 处理群操作响应；requestId、requestType、error 与 response 关联原始操作和服务端结果。 */
    void HandleMutationResult(const QString &requestId, int requestType, int error,
                              const QJsonObject &response);
    /** 处理针对本账号的成员状态失效事件；threadId 标识群，active 仅供失效判定。 */
    void HandleGroupInvalidation(qint64 threadId, bool active);
    /** 合并成员页到可见列表；按成员版本拒绝旧页且重用等版本已知行。 */
    void ApplyMemberPage(const QList<GroupMemberInfo> &members);
    /** 根据当前选中成员和缓存版本更新各成员操作按钮；无参数，无返回值。 */
    void UpdateSelectedMemberActions();
    /** 向所选好友发送群邀请；无参数，无返回值。 */
    void InviteSelectedFriend();
    /** 任免所选普通成员的管理员角色；无参数，无返回值。 */
    void ChangeSelectedRole();
    /** 将群主身份转让给所选成员；无参数，无返回值。 */
    void TransferSelectedOwner();
    /** 按当前角色权限移出所选成员；无参数，无返回值。 */
    void KickSelectedMember();
    /** 经确认后由当前成员自愿退出群；无参数，无返回值。 */
    void LeaveGroup();
    /** 经确认后由群主解散群；无参数，无返回值。 */
    void DissolveGroup();
    /** 打开并绑定当前群的待审批申请页；无参数，无返回值。 */
    void OpenApprovals();
    /** 展示完整公告并提交经字节长度校验的编辑；无参数，无返回值。 */
    void EditAnnouncement();
    /** 选择/解码校验本地 PNG/JPEG 并预览后，向 ResourceServer 上传且向 ChatServer 发布。 */
    void ChooseAndPublishAvatar();
    /** 仅显示仍匹配账号/登录代次/thread/avatarVersion/resource 的已发布群头像。 */
    void HandleAvatarReady(int accountUid, quint64 loginGeneration, qint64 threadId,
                           qint64 groupVersion, qint64 avatarVersion,
                           const QString &resourceId, const QString &cachePath);
    /** 仅处理本窗口待完成的上传；taskId/账号/代次/thread/原avatarVersion均须匹配。 */
    void HandleAvatarUploadFinished(const QString &taskId, int accountUid,
                                    quint64 loginGeneration, qint64 threadId,
                                    qint64 expectedAvatarVersion, const QString &resourceId,
                                    bool success, const QString &message);
    /** 将服务端角色值转换为界面文本；role 是 0/1/2，返回本地化名称。 */
    QString RoleName(int role) const;
    /** 记录异步操作身份；requestId 关联响应，action 标记操作类型。 */
    void RecordMutation(const QString &requestId, Action action);

    GroupInfo _group;
    GroupAvatarTransfer *_avatarTransfer = nullptr;
    quint64 _loginGeneration = 0;
    QString _pendingAvatarTaskId;
    qint64 _pendingAvatarExpectedVersion = 0;
    QString _pendingAvatarResourceId;
    qint64 _accountUid = 0;
    quint64 _memberGeneration = 0;
    QHash<QString, MemberPage> _memberRequests;
    QString _memberRequestId;
    qint64 _lastMemberUid = 0;
    qint64 _nextMemberUid = 0;
    QHash<qint64, GroupMemberInfo> _membersByUid;
    QHash<qint64, GroupMemberInfo> _knownMembers;
    QHash<QString, Action> _mutations;
    QPointer<QDialog> _approvalsDialog;
    QPointer<GroupRequestsPage> _approvalsPage;
    QLabel *_summary = nullptr;
    QLabel *_announcementMetadata = nullptr;
    QPlainTextEdit *_announcement = nullptr;
    QLabel *_avatarPreview = nullptr;
    QPushButton *_chooseAvatar = nullptr;
    QComboBox *_friendPicker = nullptr;
    QListWidget *_members = nullptr;
    QPushButton *_memberMore = nullptr;
    QPushButton *_invite = nullptr;
    QPushButton *_role = nullptr;
    QPushButton *_kick = nullptr;
    QPushButton *_transfer = nullptr;
    QPushButton *_leave = nullptr;
    QPushButton *_dissolve = nullptr;
    QPushButton *_editAnnouncement = nullptr;
    QPushButton *_approvals = nullptr;
    QCheckBox *_approvalRequired = nullptr;
};

#endif
