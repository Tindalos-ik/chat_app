#include "usermgr.h"

UserMgr::~UserMgr()
{

}

void UserMgr::SetName(QString name)
{
    _name = name;
}

void UserMgr::SetUid(int uid)
{
    _uid = uid;
}

void UserMgr::SetToken(QString token)
{
    _token = token;
}

QString UserMgr::GetName()
{
    return _name;
}

int UserMgr::GetUid()
{
    return _uid;
}

std::vector<std::shared_ptr<ApplyInfo> > UserMgr::GetApplyList()
{
    return _apply_list;
}

void UserMgr::AddApplyList(std::shared_ptr<ApplyInfo> apply)
{
    _apply_list.push_back(apply);
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



UserMgr::UserMgr() {

}
