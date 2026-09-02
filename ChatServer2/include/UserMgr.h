#pragma once
#ifndef USERMGR_H
#define USERMGR_H

#include "singleton.h"
#include <unordered_map>
#include <memory>
#include <mutex>

class CSession;

/*
    服务器的用户管理类
    UserMgr 维护一张“用户 uid → TCP 会话 CSession”的内存映射。
    用户登录 ChatServer 成功后，把当前会话保存进去；
    其他用户发来好友申请或聊天消息时，就可以根据目标用户的 uid 找到对应会话，并通过 CSession::Send() 推送给客户端。
*/
class UserMgr : public Singleton<UserMgr>
{
    friend class Singleton<UserMgr>;
public:
    ~UserMgr();
    // 根据uid获取一个session
    std::shared_ptr<CSession> GetSession(int uid);
    void SetUserSession(int uid, std::shared_ptr<CSession> session);
    void RmvUserSession(int uid);

private:
    UserMgr();
    std::unordered_map<int , std::shared_ptr<CSession>> _uid_to_session;
    std::mutex _session_mutex;

};

#endif // USERMGR_H