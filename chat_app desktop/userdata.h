#ifndef USERDATA_H
#define USERDATA_H

#include <QString>
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

#endif // USERDATA_H
