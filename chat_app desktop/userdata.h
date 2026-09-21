#ifndef USERDATA_H
#define USERDATA_H

#include <QString>
#include <QList>
#include <QtGlobal>
#include <memory>

struct UserInfo {
    UserInfo() = default;
    UserInfo(int uid, QString name, QString nick, QString desc, int sex, QString icon);
    int _uid = 0;
    QString _name;
    QString _nick;
    QString _desc;
    int _sex = 0;
    QString _icon;
};

// 搜索结果：搜索 uid/name 后服务器返回的用户信息
struct SearchInfo
{
    SearchInfo(int uid, QString name, QString nick, QString desc, int sex);

    int _uid;
    QString _name;
    QString _nick;
    QString _desc;
    int _sex;
};

struct AddFriendApply{
    AddFriendApply(int fromuid, QString name, QString nick, QString desc, int sex, QString icon);

    int _fromuid;
    QString _name;
    QString _nick;
    QString _desc;
    int _sex;
    QString _icon;
};

struct ApplyInfo {
    // status: 0 待处理，1 已同意；页面据此切换“添加”按钮和状态文本。
    ApplyInfo(int uid, QString name, QString desc,
              QString icon, QString nick, int sex, int status);

    ApplyInfo(std::shared_ptr<AddFriendApply> addinfo);

    void SetIcon(QString head);
    int _uid;
    QString _name;
    QString _desc;
    QString _icon;
    QString _nick;
    int _sex;
    int _status;
};

struct FriendInfo{
    FriendInfo(int uid, QString name, QString nick, QString desc,
               QString icon, QString bakname, int sex);

    int _uid;
    QString _name;
    QString _nick;
    QString _desc;
    QString _icon;
    QString _bakname;
    int _sex;
};

// 会话形式和消息类型与服务端的 thread/type 设计保持一致，后续扩展群聊、图片和文件时
// 不必再替换消息容器的类型。
enum class ChatFormType : quint8 {
    Private = 0,
    Group = 1
};

enum class ChatMsgType : quint8 {
    Text = 0,
    Image = 1,
    File = 2,
    Audio = 3,
    System = 4
};

// 所有聊天消息的公共数据。服务端确认后的 messageId 用于持久化和同步；
// uniqueId 是客户端发送前生成的 UUID，用来匹配发送回包和本地待确认消息。
class ChatDataBase {
public:
    ChatDataBase(qint64 messageId, qint64 threadId, ChatFormType formType,
                 ChatMsgType messageType, QString content, qint64 senderId);
    ChatDataBase(QString uniqueId, qint64 threadId, ChatFormType formType,
                 ChatMsgType messageType, QString content, qint64 senderId);
    virtual ~ChatDataBase() = default;

    qint64 GetMessageId() const;
    qint64 GetThreadId() const;
    ChatFormType GetFormType() const;
    ChatMsgType GetMsgType() const;
    const QString &GetContent() const;
    qint64 GetSendUid() const;
    const QString &GetUniqueId() const;
    int GetStatus() const;

    void SetMessageId(qint64 messageId);
    void SetThreadId(qint64 threadId);
    void SetStatus(int status);

private:
    QString _uniqueId;
    qint64 _messageId = 0;
    qint64 _threadId = 0;
    ChatFormType _formType = ChatFormType::Private;
    ChatMsgType _messageType = ChatMsgType::Text;
    QString _content;
    qint64 _senderId = 0;
    int _status = 0;
};

// 文本消息保留旧字段和旧构造函数，现有 TcpMgr/ChatDialog 的文本收发无需改动。
// 新同步协议接入后可使用第二个构造函数保存服务端 messageId 和 threadId。
class TextChatData : public ChatDataBase {
public:
    TextChatData(QString msgId, QString content, int fromUid, int toUid);
    TextChatData(qint64 messageId, QString uniqueId, qint64 threadId,
                 QString content, qint64 senderId, qint64 recvId,
                 int status = 0);

    // 兼容层：旧文本收发链路直接访问这些字段；新代码优先使用 ChatDataBase getter。
    QString _msg_id;
    QString _msg_content;
    int _from_uid = 0;
    int _to_uid = 0;
};

// 好友认证的好友资料与附加消息必须作为一个整体交给界面：先把好友加入 UserMgr，
// 再用 textmsgs 中的 thread_id 创建本地正式会话，避免消息先到而找不到对端资料。
struct FriendAuthResult {
    std::shared_ptr<FriendInfo> friendInfo;
    QList<std::shared_ptr<TextChatData>> textMessages;
};

#endif // USERDATA_H
