#pragma once
#ifndef DATA_H
#define DATA_H

#include <string>

struct UserInfo {
    std::string user;
    std::string email;
    std::string passwd;
    int uid = 0;
    std::string nick;
    std::string desc;
    int sex = 0;
    std::string icon;
};

struct ApplyInfo {
    int _uid;
    std::string _user;
    std::string _desc;
    std::string _icon;
    std::string _nick;
};

#endif // DATA_H
