#ifndef USERMGR_H
#define USERMGR_H
#include <QObject>
#include <memory>
#include <singleton.h>
#include "userdata.h"

/*
 * 用户管理类，管理用户信息
 */

class UserMgr:public QObject, public Singleton<UserMgr>,
                public std::enable_shared_from_this<UserMgr>
{
    Q_OBJECT
public:
    friend class Singleton<UserMgr>;
    ~UserMgr();
    void SetUserInfo(std::shared_ptr<UserInfo> userinfo);
    std::shared_ptr<UserInfo> GetUserInfo();

    // Keep existing call sites working while user data is stored in UserInfo.
    void SetName(QString name);
    void SetUid(int uid);
    void SetToken(QString token);
    QString GetName();
    QString GetIcon();
    int GetUid();
    // 好友申请在登录会话期间缓存，页面创建时可恢复这些记录。
    std::vector<std::shared_ptr<ApplyInfo>> GetApplyList();
    std::vector<std::shared_ptr<UserInfo>> GetFriendList();
    void SetApplyList(std::vector<std::shared_ptr<ApplyInfo>>);
    void SetFriendList(std::vector<std::shared_ptr<UserInfo>>);
    void AddApplyList(std::shared_ptr<ApplyInfo> apply);
    void AddFriendList(std::shared_ptr<UserInfo> newfriend);
    // 以申请方 UID 去重，避免同一 TCP 通知重复显示。
    bool AlreadyApply(int uid);
private:
    UserMgr();
    std::shared_ptr<UserInfo> _userinfo;
    QString _token;
    std::vector<std::shared_ptr<ApplyInfo>> _apply_list; // 好友申请列表
    std::vector<std::shared_ptr<UserInfo>> _friend_list; //好友列表

};

#endif // USERMGR_H
