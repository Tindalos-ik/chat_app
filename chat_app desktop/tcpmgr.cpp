#include "tcpmgr.h"
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <limits>
#include "usermgr.h"
#include "userdata.h"

TcpMgr::~TcpMgr()
{

}

bool TcpMgr::IsConnected() const
{
    return _socket.state() == QAbstractSocket::ConnectedState;
}

//Qt封装的是异步，我们要在构造函数里面完成各种信号的槽，保证服务的流程进行
TcpMgr::TcpMgr() : _host(""), _port(0), _b_recy_pending(false), _message_id(0), _message_len(0)
{
    //连接服务器，在这里不好写第一个参数，我们去到LoginDialog里面写
    //connect(, &LoginDialog::sig_connect_tcp, this, &TcpMgr::slot_tcp_connect);

    //_socket连接服务器成功之后，发送信号通知一下
    connect(&_socket, &QTcpSocket::connected, [this](){
        qDebug() << "connect to server" << Qt::endl;
        emit sig_con_success(true);
    });

    // 记录断开原因，便于区分客户端主动退出、服务端关闭和网络错误。
    connect(&_socket, &QTcpSocket::disconnected, [this]{
        qWarning() << "disconnected from server:" << _socket.errorString();
        _buffer.clear();
        _b_recy_pending = false;
    });

    //在有数据可读时候进行处理
    connect(&_socket,&QTcpSocket::readyRead,[this](){
        //读取所有数据到缓冲区
        _buffer.append(_socket.readAll());

        // 一次 readyRead 可能包含多个完整包；半包则保留已读包头，等下次数据补齐包体。
        while (true) {
            //解析头部，消息头是消息id + 消息长度 每个是short，两个字节
            if(!_b_recy_pending){
                //检查缓冲区中的数据是否足够解析出一个消息头，不够就返回
                //使用static_cast<> 实现更加安全的类型转换
                if(_buffer.size() < static_cast<int>(sizeof(quint16)*2)){
                    return;
                }

                QDataStream stream(&_buffer, QIODevice::ReadOnly);
                stream.setVersion(QDataStream::Qt_6_0);
                stream.setByteOrder(QDataStream::BigEndian);

                //预读取消息id和消息长度
                stream >> _message_id >> _message_len;

                //将buffer中前四个字节移除，mid截取一段
                _buffer = _buffer.mid(sizeof(quint16)*2);

                //输出读取的数据
                qDebug() << "message id : " << _message_id
                         << "message len : " << _message_len << Qt::endl;
                _b_recy_pending = true;
            }

            //buffer剩余长度是否满足消息体长度，不满足就退出继续等待接受
            if(_buffer.size() < _message_len){
                return;
            }


            //读取消息体，给到回调函数处理
            QByteArray messageBody = _buffer.mid(0,_message_len);
            qDebug() << "receive message : " << messageBody << Qt::endl;
            _buffer = _buffer.mid(_message_len);
            _b_recy_pending = false;

            //处理收到的数据
            auto iter = _handler.find(ReqId(_message_id));
            if(iter == _handler.end()){
                qDebug() << "id error" << Qt::endl;
                continue;
            }

            //执行处理函数
            iter.value()(ReqId(_message_id), _message_len, messageBody);
        }

    });

    //处理错误，直接问ai
    connect(&_socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::errorOccurred),
            [this](QAbstractSocket::SocketError socketError){
                Q_UNUSED(socketError);
                qDebug() << "Error : " << _socket.errorString();
            });


    //连接发送信号用来发送数据，在哪里发送信号呢？可以是对话框中点击发送消息信号，在很多地方都可以，我们设计好槽函数及参数就可以统一处理
    // 其他地方把数据传过来，这里发给服务器
    connect(this, &TcpMgr::sig_send_data, this, &TcpMgr::slot_send_data);

    // 注册消息
    initHandlers();
}

void TcpMgr::initHandlers()
{
    //连接聊天服务器和发送消息过去是分开的
    //登录聊天服务器回包，这一块是和tcp服务通信，和logindialog里面的不一样
    _handler.insert(ReqId::ID_CHAT_LOGIN_RSP, [this](ReqId id, int len, QByteArray data){
        Q_UNUSED(len);
        qDebug() << "handle id is " << id << "data is " << data;

        //将字节流转换为json文档
        QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

        //检查转换是否成功
        if(jsonDoc.isNull()){
            qDebug() << "failed to create QJsonDocument";
            emit sig_login_failed(ErrorCodes::ERR_JSON);
            return;
        }

        //将json文档转换为json对象
        QJsonObject json_obj = jsonDoc.object();

        if(!json_obj.contains("error")){ //正常解析成功会有error键
            int err = ErrorCodes::ERR_JSON;
            qDebug() << "Login Failed, err is Json Prase Err" << err;
            emit sig_login_failed(err);
            return;
        }

        int err = json_obj["error"].toInt();

        if(err != ErrorCodes::SUCCESS){
            qDebug() << "Login Failed, err is" << err;
            emit sig_login_failed(err);
            return;
        }

        int uid = json_obj["uid"].toInt();
        int sex = json_obj["sex"].toInt();
        QString name = json_obj["user"].toString();
        QString icon = json_obj["icon"].toString();
        QString desc = json_obj["desc"].toString();
        QString nick = json_obj["nick"].toString();
        auto userinfo = std::make_shared<UserInfo>(uid, name, nick, desc, sex, icon);
        auto userMgr = UserMgr::GetInstance();
        userMgr->SetUserInfo(std::move(userinfo));
        userMgr->SetToken(json_obj["token"].toString());

        std::vector<std::shared_ptr<ApplyInfo>> applyList;
        const QJsonArray applyArray = json_obj.value("apply_list").toArray();
        for (const auto &value : applyArray) {
            const QJsonObject apply = value.toObject();
            applyList.push_back(std::make_shared<ApplyInfo>(
                apply.value("uid").toInt(),
                apply.value("name").toString(),
                apply.value("desc").toString(),
                apply.value("icon").toString(),
                apply.value("nick").toString(),
                apply.value("sex").toInt(),
                apply.value("status").toInt()));
        }
        userMgr->SetApplyList(std::move(applyList));

        std::vector<std::shared_ptr<UserInfo>> friendList;
        const QJsonArray friendArray = json_obj.value("friend_list").toArray();
        for (const auto &value : friendArray) {
            const QJsonObject friendObject = value.toObject();
            friendList.push_back(std::make_shared<UserInfo>(
                friendObject.value("uid").toInt(),
                friendObject.value("name").toString(),
                friendObject.value("nick").toString(),
                friendObject.value("desc").toString(),
                friendObject.value("sex").toInt(),
                friendObject.value("icon").toString()));
        }
        userMgr->SetFriendList(std::move(friendList));

        emit sig_switch_chatdlg();
    });

    // 搜索列表搜索回包
    _handler.insert(ReqId::ID_SEARCH_USER_RSP, [this](ReqId id, int len, QByteArray data){
        Q_UNUSED(len);
        qDebug() << "handle id is " << id << "data is " << data;

        // 搜索失败统一走 sig_user_search(nullptr)，让 SearchList 弹 FindFailDlg
        auto emit_search_fail = [this]{
            std::shared_ptr<SearchInfo> null_si;
            emit sig_user_search(null_si);
        };

        //将字节流转换为json文档
        QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

        //检查转换是否成功
        if(jsonDoc.isNull()){
            qDebug() << "failed to create QJsonDocument";
            emit_search_fail();
            return;
        }

        //将json文档转换为json对象
        QJsonObject json_obj = jsonDoc.object();

        if(!json_obj.contains("error")){ //正常解析成功会有error键
            int err = ErrorCodes::ERR_JSON;
            qDebug() << "Search Failed, err is Json Parse Err" << err;
            emit_search_fail();
            return;
        }

        int err = json_obj["error"].toInt();

        if(err != ErrorCodes::SUCCESS){
            qDebug() << "Search Failed, err is" << err;
            emit_search_fail();
            return;
        }

        auto si = std::make_shared<SearchInfo>(json_obj["uid"].toInt(),
                                               json_obj["user"].toString(),
                                               json_obj["nick"].toString(),
                                               json_obj["desc"].toString(),
                                               json_obj["sex"].toInt());

        emit sig_user_search(si);
    });

    // 客户端监听服务器发来的添加好友请求，由新朋友那一栏接收信号做出反应
    // 这个id是通知用户好友申请
    _handler.insert(ID_NOTIFY_ADD_FRIEND_REQ, [this](ReqId id, int len, QByteArray data){
        Q_UNUSED(len);
        qDebug() << "handle id is " << id << "data is " << data;

        //将字节流转换为json文档
        QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

        //检查转换是否成功
        if(jsonDoc.isNull()){
            qDebug() << "failed to create QJsonDocument";
            return;
        }

        //将json文档转换为json对象
        QJsonObject json_obj = jsonDoc.object();

        if(!json_obj.contains("error")){ //正常解析成功会有error键
            int err = ErrorCodes::ERR_JSON;
            qDebug() << "notify friend Failed, err is Json Parse Err" << err;
            return;
        }

        int err = json_obj["error"].toInt();

        if(err != ErrorCodes::SUCCESS){
            qDebug() << "notify friend Failed, err is" << err;
            return;
        }

        // 当前 ChatServer 协议字段：applyuid、name、nick、desc、sex、icon。
        auto apply_user = std::make_shared<AddFriendApply>(json_obj.value("applyuid").toInt(),
                                               json_obj.value("name").toString(),
                                               json_obj.value("nick").toString(),
                                               json_obj.value("desc").toString(),
                                               json_obj.value("sex").toInt(),
                                               json_obj.value("icon").toString());
        emit sig_friend_apply(apply_user);
    });

    // 认证者收到 1014 回包、原申请人收到 1015 通知，二者携带相同的好友资料。
    auto handle_auth_friend = [this](ReqId id, int len, QByteArray data){
        Q_UNUSED(len);
        qDebug() << "handle id is " << id << "data is " << data;

        //将字节流转换为json文档
        QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

        //检查转换是否成功
        if(jsonDoc.isNull()){
            qDebug() << "failed to create QJsonDocument";
            return;
        }

        //将json文档转换为json对象
        QJsonObject json_obj = jsonDoc.object();

        if(!json_obj.contains("error")){ //正常解析成功会有error键
            int err = ErrorCodes::ERR_JSON;
            qDebug() << "auth friend Failed, err is Json Parse Err" << err;
            return;
        }

        int err = json_obj["error"].toInt();

        if(err != ErrorCodes::SUCCESS){
            qDebug() << "auth friend Failed, err is" << err;
            return;
        }

        int uid = json_obj["uid"].toInt();
        QString name = json_obj["name"].toString();
        QString nick = json_obj["nick"].toString();
        QString bakname = json_obj["bakname"].toString();
        QString desc = json_obj["desc"].toString();
        int sex = json_obj["sex"].toInt();
        QString icon = json_obj["icon"].toString();

        auto friendinfo = std::make_shared<FriendInfo>(uid, name, nick, desc, icon, bakname, sex);

        emit sig_auth_friend(friendinfo);

    };

    _handler.insert(ID_AUTH_FRIEND_RSP, handle_auth_friend);
    _handler.insert(ID_NOTIFY_AUTH_FRIEND_REQ, handle_auth_friend);

    _handler.insert(ID_TEXT_CHAT_MSG_RSP, [this](ReqId id, int len, QByteArray data){
        Q_UNUSED(id);
        Q_UNUSED(len);
        const QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
        if (!jsonDoc.isObject()) {
            qWarning() << "text chat response is not a JSON object";
            return;
        }

        const QJsonObject jsonObj = jsonDoc.object();
        const int error = jsonObj.value("error").toInt(ErrorCodes::ERR_JSON);
        const QJsonArray textArray = jsonObj.value("textArray").toArray();
        for (const QJsonValue &value : textArray) {
            qDebug() << "text message" << value.toObject().value("msgid").toString()
                     << (error == ErrorCodes::SUCCESS ? "sent" : "failed")
                     << "error:" << error;
        }
    });

    _handler.insert(ID_NOTIFY_TEXT_CHAT_MSG_REQ, [this](ReqId id, int len, QByteArray data){
        Q_UNUSED(id);
        Q_UNUSED(len);
        const QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
        if (!jsonDoc.isObject()) {
            qWarning() << "text chat notification is not a JSON object";
            return;
        }

        const QJsonObject jsonObj = jsonDoc.object();
        if (jsonObj.value("error").toInt(ErrorCodes::ERR_JSON) != ErrorCodes::SUCCESS) {
            qWarning() << "text chat notification failed:" << jsonObj.value("error").toInt();
            return;
        }

        const int fromUid = jsonObj.value("fromuid").toInt();
        const int toUid = jsonObj.value("touid").toInt();
        const QJsonArray textArray = jsonObj.value("textArray").toArray();
        qInfo() << "received text chat notification, from:" << fromUid
                << "to:" << toUid << "count:" << textArray.size();
        for (const QJsonValue &value : textArray) {
            const QJsonObject textObj = value.toObject();
            auto message = std::make_shared<TextChatData>(textObj.value("msgid").toString(),
                                                           textObj.value("content").toString(),
                                                           fromUid, toUid);
            emit sig_text_chat(message);
        }
    });

}

void TcpMgr::slot_tcp_connect(ServerInfo si)
{
    //客户端连接服务器
    qDebug() << "connecting to server..." << Qt::endl;
    _host = si.Host;
    _port = static_cast<uint16_t>(si.Port.toUInt()); //QString很好用
    _socket.connectToHost(_host, _port); //这个也是异步的，通过前面的回调函数知道结果
}



void TcpMgr::slot_send_data(ReqId reqId, QByteArray dataByte)
{
    if (!IsConnected()) {
        qWarning() << "cannot send TCP message: socket is not connected";
        return;
    }

    if (dataByte.size() > std::numeric_limits<quint16>::max()) {
        qWarning() << "cannot send TCP message: body exceeds protocol limit";
        return;
    }

    uint16_t id = reqId;

    // 计算长度，使用网络字节序转换
    quint16 len = static_cast<quint16>(dataByte.size());

    //创建一个QByteArray用于存储要发送的所有数据，也就是拼接一下
    QByteArray block;
    QDataStream out(&block, QIODevice::WriteOnly);

    //设置数据流使用网络字节序
    out.setByteOrder(QDataStream::BigEndian);

    out << id << len;
    block.append(dataByte);

    //发送数据
    if (_socket.write(block) < 0) {
        qWarning() << "TCP write failed:" << _socket.errorString();
    }
}
