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
