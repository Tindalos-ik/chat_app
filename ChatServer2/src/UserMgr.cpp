#include "UserMgr.h"
#include "CSession.h"
#include "RedisMgr.h"
#include "data.h"

UserMgr::UserMgr(){
    
}

UserMgr::~UserMgr(){
    _uid_to_session.clear();
}


std::shared_ptr<CSession> UserMgr::GetSession(int uid){
    std::lock_guard<std::mutex> lock(_session_mutex);
    auto iter = _uid_to_session.find(uid);
    if(iter == _uid_to_session.end()) return nullptr;
    return iter->second;
}


void UserMgr::SetUserSession(int uid, std::shared_ptr<CSession> session){
    std::lock_guard<std::mutex> lock(_session_mutex);
    _uid_to_session[uid] = session;
}

void UserMgr::RmvUserSession(int uid, const std::string& session_id){
    std::lock_guard<std::mutex> lock(_session_mutex);
    auto iter = _uid_to_session.find(uid);
    if(iter == _uid_to_session.end()){
        return;
    }

    // 旧连接的延迟回调不能删除同 uid 后来建立的新连接。
    if(iter->second->GetSessionId() != session_id){
        return;
    }

    _uid_to_session.erase(iter);
}
