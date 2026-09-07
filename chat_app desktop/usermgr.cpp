#include "usermgr.h"

UserMgr::~UserMgr() = default;

void UserMgr::SetUserInfo(std::shared_ptr<UserInfo> userinfo)
{
    _userinfo = userinfo ? std::move(userinfo) : std::make_shared<UserInfo>();
}

std::shared_ptr<UserInfo> UserMgr::GetUserInfo()
{
    return _userinfo;
}

void UserMgr::SetName(QString name)
{
    _userinfo->_name = std::move(name);
}

void UserMgr::SetUid(int uid)
{
    _userinfo->_uid = uid;
}

void UserMgr::SetToken(QString token)
{
    _token = std::move(token);
}

QString UserMgr::GetName()
{
    return _userinfo->_name;
}

QString UserMgr::GetIcon()
{
    return _userinfo->_icon;
}

int UserMgr::GetUid()
{
    return _userinfo->_uid;
}

std::vector<std::shared_ptr<ApplyInfo> > UserMgr::GetApplyList()
{
    return _apply_list;
}

std::vector<std::shared_ptr<UserInfo>> UserMgr::GetFriendList()
{
    return _friend_list;
}

void UserMgr::SetApplyList(std::vector<std::shared_ptr<ApplyInfo>> applylist)
{
    _apply_list = std::move(applylist);
}

void UserMgr::SetFriendList(std::vector<std::shared_ptr<UserInfo>> friendlist)
{
    _friend_list = std::move(friendlist);
}

void UserMgr::AddApplyList(std::shared_ptr<ApplyInfo> apply)
{
    _apply_list.push_back(apply);
}

void UserMgr::AddFriendList(std::shared_ptr<UserInfo> newfriend)
{
    _friend_list.push_back(newfriend);
}

bool UserMgr::AlreadyApply(int uid)
{
    for(auto apply_info : _apply_list){
        if(apply_info->_uid == uid){
            return true;
        }
    }
    return false;
}

UserMgr::UserMgr()
    : _userinfo(std::make_shared<UserInfo>())
{
}
