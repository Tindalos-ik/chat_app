#ifndef USERDATA_H
#define USERDATA_H

#include <QString>
#include <memory>

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

struct ApplyInfo{
    ApplyInfo(std::shared_ptr<AddFriendApply>& apply);

};

#endif // USERDATA_H
