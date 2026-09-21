#include "userdata.h"

#include <utility>

SearchInfo::SearchInfo(int uid, QString name, QString nick, QString desc, int sex)
    : _uid(uid)
    , _name(name)
    , _nick(nick)
    , _desc(desc)
    , _sex(sex)
{
}

AddFriendApply::AddFriendApply(int fromuid, QString name, QString nick, QString desc, int sex, QString icon)
    : _fromuid(fromuid)
    , _name(name)
    , _nick(nick)
    , _desc(desc)
    , _sex(sex)
    , _icon(icon)
{

}

ApplyInfo::ApplyInfo(int uid, QString name, QString desc, QString icon, QString nick, int sex, int status)
    :_uid(uid),_name(name),_desc(desc),
    _icon(icon),_nick(nick),_sex(sex),_status(status){}

ApplyInfo::ApplyInfo(std::shared_ptr<AddFriendApply> addinfo)
    :_uid(addinfo->_fromuid),_name(addinfo->_name),
    _desc(addinfo->_desc),_icon(addinfo->_icon),
    _nick(addinfo->_nick),_sex(addinfo->_sex),
    _status(0)
{}

void ApplyInfo::SetIcon(QString head){
    _icon = head;
}

FriendInfo::FriendInfo(int uid, QString name, QString nick, QString desc,
                       QString icon, QString bakname, int sex)
    :_uid(uid), _name(name), _nick(nick), _desc(desc), _icon(icon), _bakname(bakname), _sex(sex)
{

}

UserInfo::UserInfo(int uid, QString name, QString nick, QString desc, int sex, QString icon):
    _uid(uid), _name(name), _nick(nick), _desc(desc), _sex(sex), _icon(icon)
{

}

TextChatData::TextChatData(QString msg_id, QString msg_content, int from_uid, int to_uid):
    ChatDataBase(msg_id, 0, ChatFormType::Private, ChatMsgType::Text, msg_content, from_uid),
    _msg_id(msg_id), _msg_content(msg_content), _from_uid(from_uid), _to_uid(to_uid)
{

}

ChatDataBase::ChatDataBase(qint64 messageId, qint64 threadId, ChatFormType formType,
                           ChatMsgType messageType, QString content, qint64 senderId)
    : _messageId(messageId)
    , _threadId(threadId)
    , _formType(formType)
    , _messageType(messageType)
    , _content(std::move(content))
    , _senderId(senderId)
{
}

ChatDataBase::ChatDataBase(QString uniqueId, qint64 threadId, ChatFormType formType,
                           ChatMsgType messageType, QString content, qint64 senderId)
    : _uniqueId(std::move(uniqueId))
    , _threadId(threadId)
    , _formType(formType)
    , _messageType(messageType)
    , _content(std::move(content))
    , _senderId(senderId)
{
}

qint64 ChatDataBase::GetMessageId() const { return _messageId; }
qint64 ChatDataBase::GetThreadId() const { return _threadId; }
ChatFormType ChatDataBase::GetFormType() const { return _formType; }
ChatMsgType ChatDataBase::GetMsgType() const { return _messageType; }
const QString &ChatDataBase::GetContent() const { return _content; }
qint64 ChatDataBase::GetSendUid() const { return _senderId; }
const QString &ChatDataBase::GetUniqueId() const { return _uniqueId; }
qint64 ChatDataBase::GetCreatedAtMs() const { return _createdAtMs; }
int ChatDataBase::GetStatus() const { return _status; }
void ChatDataBase::SetMessageId(qint64 messageId) { _messageId = messageId; }
void ChatDataBase::SetThreadId(qint64 threadId) { _threadId = threadId; }
void ChatDataBase::SetStatus(int status) { _status = status; }

TextChatData::TextChatData(qint64 messageId, QString uniqueId, qint64 threadId,
                           QString content, qint64 senderId, qint64 recvId, int status,
                           qint64 createdAtMs)
    : ChatDataBase(std::move(uniqueId), threadId, ChatFormType::Private,
                   ChatMsgType::Text, content, senderId)
    , _msg_id(GetUniqueId())
    , _msg_content(std::move(content))
    , _from_uid(static_cast<int>(senderId))
    , _to_uid(static_cast<int>(recvId))
{
    SetMessageId(messageId);
    SetStatus(status);
    _createdAtMs = createdAtMs;
}
