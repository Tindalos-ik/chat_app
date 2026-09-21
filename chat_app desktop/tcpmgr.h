#ifndef TCPMGR_H
#define TCPMGR_H

/******************************************************************************
 *
 * @file       tcpmgr.h
 * @brief      在客户端实现一个tcp连接管理者
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
#include "global.h"
#include "userdata.h"

class TcpMgr : public QObject, public Singleton<TcpMgr>, //继承QObject是为了能够信号和槽机制，并且要第一个继承
               public std::enable_shared_from_this<TcpMgr>
{
    friend Singleton<TcpMgr>;
    Q_OBJECT
public:
    ~TcpMgr();
    bool IsConnected() const;

    void CloseConnection(); //客户端下线时候调用
    // 应用准备退出时调用：先停止异步回调，保证析构阶段不再触发界面逻辑。
    void PrepareForShutdown();

private:
    TcpMgr();

    void initHandlers();
    void initSigAndSlot();
    void StartHeartbeat(); // TCP 登录成功后开始周期性发送 1023
    void StopHeartbeat();  // 断开或主动关闭时停止定时器并清空等待状态
    void SendHeartbeat();  // 定时器回调：检测上次 1024，随后发送新的 1023
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
    void sig_con_success(bool bsuccess); // 获取聊天服务器
    void sig_send_data(ReqId reqId, QByteArray data);
    void sig_switch_chatdlg(); // 发送给mainwindow
    void sig_login_failed(int); // 发送给logindialog
    void sig_user_search(std::shared_ptr<SearchInfo>& si); // 发送给searchlist，用于显示搜索结果
    void sig_friend_apply(std::shared_ptr<AddFriendApply>& si); // 发送给chatdialog
    // 认证成功的好友资料及附加初始消息（含服务端 thread_id/message_id）。
    void sig_auth_friend(std::shared_ptr<FriendAuthResult>&);
    // 1028 回包：服务端创建或找到唯一私聊后返回的正式会话 ID。
    void sig_create_private_chat(int uid, int otherUid, qint64 threadId);
    void sig_text_chat(std::shared_ptr<TextChatData>&); // 收到对方推送的文本消息
    // 1030 已成功写入 SQLite；界面按 threadId 刷新摘要或当前聊天页。
    void sig_local_chat_synced(qint64 threadId);
    void sig_off_line();
    void sig_connection_lost(); // 已登录连接被服务端关闭，但未完整收到踢人通知
    void sig_heartbeat_timeout(); // 60 秒未收到有效心跳回复，交给主窗口显示专用提示
    void sig_update_profile_result(int error); // 当前用户资料更新结果
};

#endif // TCPMGR_H
