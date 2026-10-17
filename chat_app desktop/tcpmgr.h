#ifndef TCPMGR_H
#define TCPMGR_H

/******************************************************************************
 *
 * @file       tcpmgr.h
 * @brief      管理当前登录账号TCP帧、群请求关联、完整快照与连续历史游标；不以客户端UID授予权限。
 *
 * @author     klein
 *****************************************************************************/
#include "singleton.h"
#include <QTcpSocket> //需要在cmakelists中添加  Qt::Network 库
#include <functional>
#include <QObject>
#include <QMap>
#include <QElapsedTimer>
#include <QTimer>
#include <QList>
#include <QSet>
#include "global.h"
#include <QJsonObject>
#include "userdata.h"
#include "localchatstoragemgr.h"

class VideoTestController;
class TcpMgr : public QObject, public Singleton<TcpMgr>, //继承QObject是为了能够信号和槽机制，并且要第一个继承
               public std::enable_shared_from_this<TcpMgr>
{
    friend Singleton<TcpMgr>;
    Q_OBJECT
public:
    ~TcpMgr();
    bool IsConnected() const;
    /** 返回当前 TCP 登录代次；异步群头像回调据此丢弃已切账号/重登的结果。 */
    quint64 LoginGeneration() const;
    void CloseConnection();
    // 当前账号共用唯一测试控制器；收发窗口不随好友资料页重复创建。
    VideoTestController *VideoTest() const { return _videoTest; }
    // 只发送业务控制，不传客户端身份；UUID用于丢弃过期响应。
    QString SendVideoTest(const QString &op, int toUid = 0,
                          const QString &testId = {}, const QString &reason = {});
    // 群API返回本次request_id；空串表示参数/连接无效。服务端只信任登录session UID。
    QString CreateGroup(const QString &name);
    QString SearchGroup(qint64 threadId);
    QString JoinGroup(qint64 threadId, const QString &explanation = QString());
    QString LoadGroupMembers(qint64 threadId, qint64 afterUid = 0);
    QString KickGroupMember(qint64 threadId, qint64 targetUid,
                            qint64 expectedMembershipVersion);
    QString DissolveGroup(qint64 threadId);
    QString InviteGroupMember(qint64 threadId, qint64 targetUid);
    QString LoadGroupRequests(const QString &scope, qint64 afterId = 0, qint64 threadId = 0);
    QString DecideGroupInvitation(qint64 invitationId, qint64 threadId, bool accept);
    QString DecideGroupApplication(qint64 applicationId, qint64 threadId, bool approve);
    QString SetGroupAdmin(qint64 threadId, qint64 targetUid, int role,
                          qint64 expectedMembershipVersion);
    QString TransferGroupOwner(qint64 threadId, qint64 targetUid,
                               qint64 expectedMembershipVersion);
    QString LeaveGroup(qint64 threadId, qint64 expectedMembershipVersion);
    QString SetGroupApproval(qint64 threadId, bool required);
    QString UpdateGroupAnnouncement(qint64 threadId, const QString &announcement,
                                    qint64 expectedVersion);
    QString UpdateGroupAvatar(qint64 threadId, const QString &resourceId);
    // 应用准备退出时调用：先停止异步回调，保证析构阶段不再触发界面逻辑。
    void PrepareForShutdown();

private:
    TcpMgr();

    void initHandlers();
    void initSigAndSlot();
    void StartHeartbeat(); // TCP 登录成功后开始周期性发送 1023
    void StopHeartbeat();  // 断开或主动关闭时停止定时器并清空等待状态
    void SendHeartbeat();  // 定时器回调：检测上次 1024，随后发送新的 1023
    // 从0发起本账号完整群快照；不使用私聊MaxKnownThreadId，失败/断轮不按半页失效。
    void RequestGroupList();
    // threadId为已知有效群，按该群连续message_id历史游标补拉，实时notify/ACK不推进游标。
    void RequestGroupMessages(qint64 threadId);
    void SendGroupListPage(qint64 afterThreadId);
    void FinishGroupList(bool successful);
    QString SendGroupMutation(ReqId requestType, QJsonObject request, qint64 threadId);
    QHash<QString, int> _pendingGroupRequests;
    QHash<QString, qint64> _pendingGroupThreads;
    QHash<QString, QString> _pendingGroupOperationIds;
    QHash<QString, quint64> _pendingGroupLoginGenerations;
    QHash<QString, QString> _pendingGroupScopes;
    QSet<qint64> _groupSnapshotIds;
    QHash<qint64, LocalChatThread> _groupSnapshotGroups;
    QSet<qint64> _groupSnapshotConfirmedIds;
    bool _groupListInProgress = false;
    bool _groupListRefreshQueued = false;
    qint64 _groupSnapshotCursor = 0;
    QString _groupListPendingRequestId;
    qint64 _groupListPendingAfter = 0;
    quint64 _groupSnapshotGeneration = 0;
    QHash<qint64, quint64> _groupStateEpochs;
    QHash<QString, quint64> _pendingGroupStateEpochs;
    QHash<QString, qint64> _pendingGroupAccounts;
    qint64 _groupListAccountUid = 0;
    quint64 _loginGeneration = 0;
    quint64 _groupListLoginGeneration = 0;
    QHash<qint64, quint64> _groupSnapshotProofEpochs;
    VideoTestController *_videoTest = nullptr;
    QMap<ReqId, std::function<void(ReqId id, int len, QByteArray data)>> _handler; //消息id对应的回调函数

    QTcpSocket* _socket; //客户端这边只需要一个socket就可以了，很简单
    QString _host;
    uint16_t _port;
    QByteArray _buffer; //接收缓冲区，一个动态扩展结构，tcp是面向字节流的
    bool _b_recy_pending; //接收状态标志，标记当前是否正在等待一个完整的数据包，true代表上一个数据没有收全，收全了才能扔给handler处理
    bool _logged_in; // 已收到聊天服务器登录成功回包
    bool _disconnect_notified; // 一次连接只通知一次下线/断线，避免重复弹窗
    bool _is_shutting_down; // QApplication 退出阶段为 true，禁止再处理 socket 事件
    quint16 _message_id; //消息 ID，标识消息的类型，比如是登录回包，
    quint16 _message_len;
    QTimer* _heartbeat_timer;             // 客户端心跳发送与超时检查定时器
    QElapsedTimer _last_heartbeat_rsp;    // 最近一次收到有效 1024 的本地单调时间

public slots:
    void slot_tcp_connect(ServerInfo);
    void slot_send_data(ReqId reqId, QByteArray data);

signals:
    void sig_video_test(const QJsonObject &message, bool notification);
    // 在账号切换/连接关闭之前触发，媒体可先尝试向对端发送stop。
    void sig_video_test_reset();
    void sig_con_success(bool bsuccess); // 获取聊天服务器
    void sig_send_data(ReqId reqId, QByteArray data);
    void sig_switch_chatdlg(); // 发送给mainwindow
    void sig_login_failed(int); // 发送给logindialog
    void sig_update_profile_result(int error); // 保留既有个人资料更新响应，与群管理独立。
    void sig_user_search(std::shared_ptr<SearchInfo>& si); // 发送给searchlist，用于显示搜索结果
    void sig_friend_apply(std::shared_ptr<AddFriendApply>& si); // 发送给chatdialog
    // 认证成功的好友资料及附加初始消息（含服务端 thread_id/message_id）。
    void sig_auth_friend(std::shared_ptr<FriendAuthResult>&);
    // 1028 回包：服务端创建或找到唯一私聊后返回的正式会话 ID。
    void sig_create_private_chat(int uid, int otherUid, qint64 threadId);
    void sig_text_chat(std::shared_ptr<TextChatData>&); // 收到对方推送的文本消息
    // 文本请求回包按客户端 UUID 通知 UI，用于清除发送中状态或显示发送失败图标。
    void sig_text_chat_send_result(const QString &messageId, bool success, int deliveryState = 0);
    // 图片的二进制不经过 ChatServer；这里只把 1034/1035 的资源元数据交给界面，
    // 由 ChatImageTransferTask 再通过 ResourceClient 下载。
    void sig_image_chat_send_result(std::shared_ptr<ImageChatData>& image, bool success);
    void sig_image_chat(std::shared_ptr<ImageChatData>& image);
    // 1041 文件发送确认和 1042 对端通知，使用独立解析的文件元数据。
    void sig_file_chat_send_result(std::shared_ptr<FileChatData>& file, bool success);
    void sig_file_chat(std::shared_ptr<FileChatData>& file);
    // 1030 已成功写入 SQLite；界面按 threadId 刷新摘要或当前聊天页。
    void sig_local_chat_synced(qint64 threadId);
    void sig_message_delivery_updated(qint64 threadId, const QList<qint64> &messageIds,
                                      int deliveryState);
    void sig_thread_read_cursor(qint64 threadId, qint64 readerUid, qint64 readThroughMessageId);
    void sig_off_line();
    void sig_connection_lost(); // 已登录连接被服务端关闭，但未完整收到踢人通知
    void sig_heartbeat_timeout(); // 60 秒未收到有效心跳回复，交给主窗口显示专用提示
    void sig_group_info(const GroupInfo &group, bool confirmedMembership);
    // 搜索展示携带原请求身份及错误；仅改变前端接线，不授予成员身份。
    void sig_group_search_result(const QString &requestId, int error, const GroupInfo &group);
    void sig_group_operation_result(const QString &requestId, int error);
    void sig_group_state(qint64 threadId, bool active, const QString &reason, qint64 targetUid);
    // Pages carry their server group version so open dialogs can discard pages captured before a role change.
    void sig_group_members(qint64 threadId, const QString &requestId, qint64 groupVersion,
                           const QList<GroupMemberInfo> &members, qint64 nextUid, bool loadMore);
    void sig_group_mutation_result(const QString &requestId, int requestType,
                                   int error, const QJsonObject &response);
    void sig_group_requests(const QString &requestId, const QString &scope, qint64 threadId,
                            int error, qint64 groupVersion,
                            const QList<GroupAdmissionInfo> &entries, qint64 nextId, bool loadMore);
    void sig_group_event(const QString &eventType, const GroupInfo &group,
                         const GroupAdmissionInfo &entry, qint64 targetUid);
};

#endif // TCPMGR_H
