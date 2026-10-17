#ifndef GROUPREQUESTSDLG_H
#define GROUPREQUESTSDLG_H

#include <QWidget>
#include <QHash>
#include <QPointer>
#include <QJsonObject>
#include "userdata.h"
class QLabel;
class QListWidget;
class QPushButton;
class QTabWidget;

// 文件职责：共享账号群通知与管理审批的分页、终态合并和决定逻辑。
class GroupRequestsPage final : public QWidget
{
    Q_OBJECT
public:
    /** 创建可嵌入的账号群通知；登录完成后由调用方 Refresh。 */
    explicit GroupRequestsPage(QWidget *parent = nullptr);
    /** 创建可嵌入的单群审批组件。 */
    GroupRequestsPage(const GroupInfo &group, QWidget *parent = nullptr);
    /** 合并同群最新状态并撤销已失去的审批权限；忽略线程或版本回退。 */
    void UpdateGroup(const GroupInfo &group);
    /** 刷新所有当前 scope；账号和登录代次不匹配时不发送请求。 */
    void Refresh();

signals:
    void sig_close_requested();

private:
    struct ScopeView {
        QListWidget *list = nullptr;
        QPushButton *more = nullptr;
        QLabel *status = nullptr;
        QPushButton *refresh = nullptr;
        QString outcome;
        qint64 nextId = 0;
        QString requestId;
        quint64 generation = 0;
        bool loadMore = false;
    };

    /** 检查创建组件时的账号及登录代次仍有效。 */
    bool IsCurrentAccount() const;
    /** 更新 scope 的内嵌加载状态并保留最近操作结果。 */
    void SetStatus(const QString &scope, const QString &text);
    void ConnectSignals();
    /** 创建个人邀请/申请/结果三类分页视图；无参数，无返回值。 */
    void BuildAccountViews();
    /** 创建单群待审批申请视图；无参数，无返回值。 */
    void BuildApprovalView();
    /** 请求 scope 的页面；reset 为真时推进代次并从头恢复全部状态。 */
    void RequestPage(const QString &scope, bool reset);
    /** 按当前页面和服务端权限更新决定按钮；无参数，无返回值。 */
    void UpdateActions();
    /** 决定当前选中的待处理记录；approve 指示接受或拒绝。 */
    void Decide(bool approve);
    /** 检查账号仍是本群管理员或群主且群版本匹配；返回是否有审批权。 */
    bool CanManageApprovals() const;
    /** 合并分页响应；身份参数绑定请求、scope、thread、群版本及下一游标。 */
    void HandlePage(const QString &requestId, const QString &scope, qint64 threadId,
                    int error, qint64 groupVersion,
                    const QList<GroupAdmissionInfo> &entries, qint64 nextId, bool loadMore);
    /** 处理决定结果；requestId/type 关联操作，error/response 表示服务端终态。 */
    void HandleMutation(const QString &requestId, int requestType, int error,
                        const QJsonObject &response);
    /** 处理该群针对当前账号的成员失效通知；threadId 标识群，active=false 表示审批权失效。 */
    void HandleGroupInvalidation(qint64 threadId, bool active);
    /** 处理指定 admission 的 typed event 并刷新对应请求页；eventType 标注事件类别。 */
    void HandleAdmissionEvent(const QString &eventType, const GroupAdmissionInfo &entry);
    /** 按 admission kind/id 合并终态；entry 为服务端记录，返回当前不可回退状态。 */
    QString RememberAdmissionStatus(const GroupAdmissionInfo &entry);
    /** 生成稳定 admission 身份键；kind 区分邀请/申请，id 是全局请求编号。 */
    static QString AdmissionKey(const QString &kind, qint64 id);
    /** 使当前页面请求游标回到零并重新读取；无参数，无返回值。 */
    void RefreshCurrentViews();

    GroupInfo _group;
    bool _approvalsOnly = false;
    bool _revoked = false;
    QTabWidget *_tabs = nullptr;
    QHash<QString, ScopeView> _views;
    QHash<QString, quint64> _pendingGenerations;
    QString _activeScope;
    QString _decisionRequestId;
    qint64 _decisionAdmissionId = 0;
    qint64 _decisionThreadId = 0;
    QString _decisionKind;
    QHash<QString, QString> _admissionStatuses;
    qint64 _accountUid = 0;
    quint64 _loginGeneration = 0;
    QPointer<QPushButton> _acceptButton;
    QPointer<QPushButton> _rejectButton;
};

#endif
