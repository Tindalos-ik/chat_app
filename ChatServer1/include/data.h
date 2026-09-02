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

#endif // DATA_H
