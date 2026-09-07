#include "userdata.h"

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
    _msg_id(msg_id), _msg_content(msg_content), _from_uid(from_uid), _to_uid(to_uid)
{

}
