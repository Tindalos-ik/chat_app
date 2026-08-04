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
#include "global.h"

class TcpMgr : public QObject, public Singleton<TcpMgr>, //继承QObject是为了能够信号和槽机制，并且要第一个继承
               public std::enable_shared_from_this<TcpMgr>
{
    friend Singleton<TcpMgr>;
    Q_OBJECT
public:
    ~TcpMgr();

private:
    TcpMgr();

    void initHandlers();
    QMap<ReqId, std::function<void(ReqId id, int len, QByteArray data)>> _handler; //消息id对应的回调函数

    QTcpSocket _socket; //客户端这边只需要一个socket就可以了，很简单
    QString _host;
    uint16_t _port;
    QByteArray _buffer; //接收缓冲区，一个动态扩展结构，tcp是面向字节流的
    bool _b_recy_pending; //接收状态标志，标记当前是否正在等待一个完整的数据包，true代表上一个数据没有收全，收全了才能扔给handler处理
    quint16 _message_id; //消息 ID，标识消息的类型，比如是登录回包，
    quint16 _message_len;

public slots:
    void slot_tcp_connect(ServerInfo);
    void slot_send_data(ReqId reqId, QString data);

signals:
    void sig_con_success(bool bsuccess);
    void sig_send_data(ReqId reqId, QString data);
    void sig_switch_chatdlg();
    void sig_login_failed(int);
};

#endif // TCPMGR_H
