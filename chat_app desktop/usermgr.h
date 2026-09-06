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
    void SetName(QString name);
    void SetUid(int uid);
    void SetToken(QString token);
    QString GetName();
    int GetUid();
    // 好友申请在登录会话期间缓存，页面创建时可恢复这些记录。
    std::vector<std::shared_ptr<ApplyInfo>> GetApplyList();
    void AddApplyList(std::shared_ptr<ApplyInfo> apply);
    // 以申请方 UID 去重，避免同一 TCP 通知重复显示。
    bool AlreadyApply(int uid);
private:
    UserMgr();
    QString _name;
    QString _token;
    int _uid;
    std::vector<std::shared_ptr<ApplyInfo>> _apply_list;

};

#endif // USERMGR_H
