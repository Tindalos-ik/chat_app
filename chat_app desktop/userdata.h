// 文件作用：客户端用户、正式消息及群元数据DTO；thread/sender/target身份分开，DTO不授予服务端权限。
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
// 群元数据使用独立版本；membershipProof只在服务端明确返回当前成员证明时为true。
struct GroupInfo {
    qint64 threadId = 0;
    QString name;
    qint64 ownerUid = 0;
    bool active = true;
    qint64 version = 0;
    bool approvalRequired = false;
    QString announcement;
    qint64 announcementVersion = 0;
    qint64 announcementUpdatedAtMs = 0;
    qint64 announcementAuthorUid = 0;
    QString avatarResourceId;
    qint64 avatarVersion = 0;
    QString avatarMimeType;
    qint64 avatarSize = 0;
    int avatarWidth = 0;
    int avatarHeight = 0;
    bool member = false;
    int role = 0;
    qint64 membershipVersion = 0;
    bool membershipProof = false;
    QString source;
};

// 成员页条目：role为0普通、1管理员或2群主；昵称缺资料回退UID。
struct GroupMemberInfo {
    qint64 uid = 0;
    QString nick;
    int role = 0;
    qint64 membershipVersion = 0;
};
struct GroupAdmissionInfo {
    QString kind; // invitation / application
    qint64 id = 0;
    qint64 threadId = 0;
    QString groupName;
    qint64 inviterUid = 0;
    qint64 applicantUid = 0;
    qint64 targetUid = 0;
    QString status;
    qint64 createdAtMs = 0;
    QString explanation;
    qint64 updatedAtMs = 0;
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

// 会话形式与服务端thread/type一致：当前支持Private/Group文本，私聊另支持图片和文件。
// 群recvId=0且不承载附件；保留公共容器与私聊接口，不把群身份映射成好友UID。
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
    // 服务端确认时写入的创建时间（毫秒）。旧协议没有该字段时为 0。
    qint64 GetCreatedAtMs() const;
    int GetStatus() const;

    void SetMessageId(qint64 messageId);
    void SetThreadId(qint64 threadId);
    // 服务端确认后补充创建时间，保持基类成员仍由自身接口维护。
    void SetCreatedAtMs(qint64 createdAtMs);
    void SetStatus(int status);

private:
    QString _uniqueId;
    qint64 _messageId = 0;
    qint64 _threadId = 0;
    ChatFormType _formType = ChatFormType::Private;
    ChatMsgType _messageType = ChatMsgType::Text;
    QString _content;
    qint64 _senderId = 0;
    qint64 _createdAtMs = 0;
    int _status = 0;
};

// Private/Group文本公共模型：UUID构造用于本账号发送前的pending消息；
// 正式元数据构造用于ACK/notify/history持久化与渲染，正式messageId是去重标识。
class TextChatData : public ChatDataBase {
public:
    // msgId为客户端UUID，content为文本，fromUid/toUid为发送/接收UID；
    // formType区分private/group（群toUid=0），UUID不是群消息正式去重键。
    TextChatData(QString msgId, QString content, int fromUid, int toUid,
                 ChatFormType formType = ChatFormType::Private);
    // messageId为正式去重/历史游标ID，uniqueId为客户端UUID，threadId为会话ID，
    // content为文本，senderId为实际发送UID，recvId私聊为对端/群为0，status为服务端状态，
    // createdAtMs为服务端毫秒时间，formType区分独立群/私聊渲染，群不解释私聊已读状态。
    TextChatData(qint64 messageId, QString uniqueId, qint64 threadId,
                 QString content, qint64 senderId, qint64 recvId,
                 int status = 0, qint64 createdAtMs = 0,
                 ChatFormType formType = ChatFormType::Private);

    // 兼容层：旧文本收发链路直接访问这些字段；新代码优先使用 ChatDataBase getter。
    QString _msg_id;
    QString _msg_content;
    int _from_uid = 0;
    int _to_uid = 0;
    QString _sender_nick;
};

// 图片聊天协议的单项元数据。resourceId 是 ResourceServer 的 upload_id，而不是
// resource_url 或服务端文件路径；图片字节只能经 ResourceClient 上传、下载。
struct ImageChatData {
    // 服务端持久化后的 ID。实时 1034/1035 和离线 1030 都会携带它们，用于避免
    // 同一张图片在“实时通知 + 登录补同步”交错时被界面重复追加。
    qint64 messageId = 0;
    qint64 threadId = 0;
    QString msgId;
    QString resourceId;
    QString name;
    QString mimeType;
    qint64 fileSize = 0;
    int width = 0;
    int height = 0;
    int fromUid = 0;
    int toUid = 0;
    int deliveryState = 1; // 1034 的已保存/实时投递状态；1037/1039 再提升 UI 状态。
};

// 作用：描述一条私聊文件消息的元数据。文件不包含图片宽高；只有用户选择保存路径后
// 客户端才会通过 ResourceClient 请求文件分片。
struct FileChatData {
    qint64 messageId = 0;   // ChatServer 持久化后的消息 ID
    qint64 threadId = 0;    // ChatServer 返回的私聊会话 ID
    QString msgId;          // 客户端发送前生成的 UUID
    QString resourceId;     // ResourceServer upload_id；不包含本机路径或下载 URL
    QString name;           // 原始文件名，仅作为展示和保存对话框默认名称
    QString mimeType;       // MIME 元数据，不参与文件内容的自动打开
    qint64 fileSize = 0;    // 服务端确认的文件字节数
    int fromUid = 0;
    int toUid = 0;
    qint64 createdAtMs = 0;  // 服务端确认的创建时间；历史/1041 回包使用
    int serverStatus = 0;    // 服务端 status：0=未读，1=已读，2=撤回
    bool peerDisplayed = false; // 历史回包中的展示确认；不等同于已读
    int deliveryState = 1;  // 1041/1042 的投递阶段
};

// 好友认证的好友资料与附加消息必须作为一个整体交给界面：先把好友加入 UserMgr，
// 再用 textmsgs 中的 thread_id 创建本地正式会话，避免消息先到而找不到对端资料。
struct FriendAuthResult {
    std::shared_ptr<FriendInfo> friendInfo;
    QList<std::shared_ptr<TextChatData>> textMessages;
};

#endif // USERDATA_H
