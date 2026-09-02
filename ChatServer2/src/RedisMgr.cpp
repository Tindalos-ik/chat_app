#include "RedisMgr.h"
#include <iostream>
#include <string.h>
#include "ConfigMgr.h"

void RedisMgr::Close() 
{
    if (_connectionPool) {
        _connectionPool->Close();
    }
}

RedisMgr::RedisMgr()
{
    auto& config = ConfigMgr::Inst();
    std::string host = config["Redis"]["host"];
    std::string port = config["Redis"]["port"];
    std::string passwd = config["Redis"]["passwd"];
    _connectionPool = std::move(std::unique_ptr<RedisConPool>(new RedisConPool(5, host.c_str(), std::stoi(port.c_str()), passwd.c_str())));
}

RedisMgr::~RedisMgr()//RedisMgr析构先于Pool调用析构，所以需要手动调用Pool的Close
{
    Close();
}

bool RedisMgr::Get(const std::string &key, std::string &value)
{
    auto connect = _connectionPool->getConnection(); //从连接池中获取连接
    if (connect == nullptr) {
        std::cout << "[ GET " << key << " ] failed: no connection" << std::endl;
        return false;
    }
    
    //执行redis命令get，并且将结果进行强制转换，本来返回的是void*
    redisReply* reply = (redisReply*)redisCommand(connect, "GET %s", key.c_str()); 
    if(reply == NULL){
        std::cout << "[ GET " << key << " ] failed" << std::endl;
        freeReplyObject(reply); //释放内存
        _connectionPool->returnConnection(connect); //归还连接
        return false;
    }
    //返回值类型不为字符串，则返回false
    if(reply->type != REDIS_REPLY_STRING){
        std::cout << "[ GET " << key << " ] failed" << std::endl;
        freeReplyObject(reply); 
        _connectionPool->returnConnection(connect);
        return false;
    }

    value = reply->str;
    freeReplyObject(reply);

    std::cout << "[ GET " << key << " ] success" << std::endl;
    _connectionPool->returnConnection(connect);
    return true;
}

//set 值是字符串
bool RedisMgr::Set(const std::string &key, const std::string &value)
{
    auto connect = _connectionPool->getConnection();
    if (connect == nullptr) {
        std::cout << "[ SET " << key << " " << value << " ] failed: no connection" << std::endl;
        return false;
    }
    
    redisReply* reply = (redisReply*)redisCommand(connect, "SET %s %s", key.c_str(), value.c_str());
    if(reply == NULL){
        std::cout << "[ SET " << key << " " << value << " ] failed" << std::endl;
        freeReplyObject(reply);
        _connectionPool->returnConnection(connect);
        return false;
    }

    if(reply->type != REDIS_REPLY_STATUS || (strcmp(reply->str, "OK") != 0 && strcmp(reply->str, "ok") != 0)){
        std::cout << "[ SET " << key << " " << value << " ] failed" << std::endl;
        freeReplyObject(reply);
        _connectionPool->returnConnection(connect);
        return false;
    }

    freeReplyObject(reply);
    std::cout << "[ SET " << key << " " << value << " ] success" << std::endl;
    _connectionPool->returnConnection(connect);
    return true;
}

//密码认证
bool RedisMgr::Auth(const std::string &password)
{
    auto connect = _connectionPool->getConnection();
    if (connect == nullptr) {
        std::cout << "[ AUTH " << password << " ] failed: no connection" << std::endl;
        return false;
    }
    
    redisReply* reply = (redisReply*)redisCommand(connect, "AUTH %s", password.c_str());
    if(reply == NULL){
        std::cout << "[ AUTH " << password << " ] failed" << std::endl;
        freeReplyObject(reply);
        _connectionPool->returnConnection(connect);
        return false;
    }
    freeReplyObject(reply);
    std::cout << "[ AUTH " << password << " ] success" << std::endl;
    _connectionPool->returnConnection(connect);
    return true;
}

//redis中一个键可以对应一个消息队列，使用这种push出来的就是存消息队列
bool RedisMgr::LPush(const std::string &key, const std::string &value)
{
    auto connect = _connectionPool->getConnection();
    if (connect == nullptr) {
        std::cout << "[ LPUSH " << key << " " << value << " ] failed: no connection" << std::endl;
        return false;
    }
    
    redisReply* reply = (redisReply*)redisCommand(connect, "LPUSH %s %s", key.c_str(), value.c_str());

    if(reply == NULL){
        std::cout << "[ LPUSH " << key << " " << value << " ] failed" << std::endl;
        freeReplyObject(reply);
        _connectionPool->returnConnection(connect);
        return false;
    }

    if(reply->type != REDIS_REPLY_INTEGER || reply->integer <= 0){
        std::cout << "[ LPUSH " << key << " " << value << " ] failed" << std::endl;
        freeReplyObject(reply);
        _connectionPool->returnConnection(connect);
        return false;
    }

    freeReplyObject(reply);
    std::cout << "[ LPUSH " << key << " " << value << " ] success" << std::endl;
    _connectionPool->returnConnection(connect);
    return true;
}

//传进去key和value，执行完函数之后value存的就是弹出的值
bool RedisMgr::LPop(const std::string &key, std::string &value)
{
    auto connect = _connectionPool->getConnection();
    if (connect == nullptr) {
        std::cout << "[ LPOP " << key << " ] failed: no connection" << std::endl;
        return false;
    }
    
    redisReply* reply = (redisReply*)redisCommand(connect, "LPOP %s", key.c_str());

    if(reply == NULL || reply->type == REDIS_REPLY_NIL){ //NIL表示没有数据
        std::cout << "[ LPOP " << key << " ] failed" << std::endl;
        freeReplyObject(reply);  
        _connectionPool->returnConnection(connect);
        return false;
    }

    value = reply->str;
    std::cout << "[ LPOP " << key << " ] success" << std::endl;
    freeReplyObject(reply);
    _connectionPool->returnConnection(connect);
    return true;
}

bool RedisMgr::RPush(const std::string &key, const std::string &value)
{
    auto connect = _connectionPool->getConnection();
    if (connect == nullptr) {
        std::cout << "[ RPUSH " << key << " " << value << " ] failed: no connection" << std::endl;
        return false;
    }
    
    redisReply* reply = (redisReply*)redisCommand(connect, "RPUSH %s %s", key.c_str(), value.c_str());
    if(reply == NULL){
        std::cout << "[ RPUSH " << key << " " << value << " ] failed" << std::endl;
        freeReplyObject(reply);
        _connectionPool->returnConnection(connect);
        return false;
    }

    if(reply->type != REDIS_REPLY_INTEGER || reply->integer <= 0){
        std::cout << "[ RPUSH " << key << " " << value << " ] failed" << std::endl;
        freeReplyObject(reply);
        _connectionPool->returnConnection(connect);
        return false;
    }

    freeReplyObject(reply);
    std::cout << "[ RPUSH " << key << " " << value << " ] success" << std::endl;
    _connectionPool->returnConnection(connect);
    return true;
}

bool RedisMgr::RPop(const std::string &key, std::string &value)
{
    auto connect = _connectionPool->getConnection();
    if (connect == nullptr) {
        std::cout << "[ RPOP " << key << " ] failed: no connection" << std::endl;
        return false;
    }
    
    redisReply* reply = (redisReply*)redisCommand(connect, "RPOP %s", key.c_str());

    if(reply == NULL || reply->type == REDIS_REPLY_NIL){ //NIL表示没有数据
        std::cout << "[ RPOP " << key << " ] failed" << std::endl;
        freeReplyObject(reply);  
        _connectionPool->returnConnection(connect);
        return false;
    }

    value = reply->str;
    std::cout << "[ RPOP " << key << " ] success" << std::endl;
    freeReplyObject(reply);
    _connectionPool->returnConnection(connect);
    return true;
}

//哈希，数据类型是HASH
bool RedisMgr::HSet(const std::string &key, const std::string &hkey, const std::string &value) {
    auto connect = _connectionPool->getConnection();
    if (connect == nullptr) {
        std::cout << "Execut command [ HSet " << key << "  " << hkey << "  " << value << " ] failure ! no connection" << std::endl;
        return false;
    }
    
    redisReply* reply = (redisReply*)redisCommand(connect, "HSET %s %s %s", key.c_str(), hkey.c_str(), value.c_str());
    if (reply == nullptr || reply->type != REDIS_REPLY_INTEGER ) {
        std::cout << "Execut command [ HSet " << key << "  " << hkey <<"  " << value << " ] failure ! " << std::endl;
        freeReplyObject(reply);
        _connectionPool->returnConnection(connect);
        return false;
    }
    std::cout << "Execut command [ HSet " << key << "  " << hkey << "  " << value << " ] success ! " << std::endl;
    freeReplyObject(reply);
    _connectionPool->returnConnection(connect);
    return true;
}


bool RedisMgr::HSet(const char* key, const char* hkey, const char* hvalue, size_t hvaluelen)
{
    auto connect = _connectionPool->getConnection();
    if (connect == nullptr) {
        std::cout << "Execut command [ HSet " << key << "  " << hkey << "  " << hvalue << " ] failure ! no connection" << std::endl;
        return false;
    }
    
    const char* argv[4];
    size_t argvlen[4];
    argv[0] = "HSET";
    argvlen[0] = 4;
    argv[1] = key;
    argvlen[1] = strlen(key);
    argv[2] = hkey;
    argvlen[2] = strlen(hkey);
    argv[3] = hvalue;
    argvlen[3] = hvaluelen;
    //这里这样处理是为了存储'\0'
    redisReply* reply = (redisReply*)redisCommandArgv(connect, 4, argv, argvlen);
    if (reply == nullptr || reply->type != REDIS_REPLY_INTEGER) {
        std::cout << "Execut command [ HSet " << key << "  " << hkey << "  " << hvalue << " ] failure ! " << std::endl;
        freeReplyObject(reply);
        _connectionPool->returnConnection(connect);
        return false;
    }
    std::cout << "Execut command [ HSet " << key << "  " << hkey << "  " << hvalue << " ] success ! " << std::endl;
    freeReplyObject(reply);
    _connectionPool->returnConnection(connect);
    return true;
}

std::string RedisMgr::HGet(const std::string &key, const std::string &hkey)
{
    auto connect = _connectionPool->getConnection();
    if (connect == nullptr) {
        std::cout << "Execut command [ HGet " << key << " "<< hkey <<"  ] failure ! no connection" << std::endl;
        return "";
    }
    
    const char* argv[3];
    size_t argvlen[3];
    argv[0] = "HGET";
    argvlen[0] = 4;
    argv[1] = key.c_str();
    argvlen[1] = key.length();
    argv[2] = hkey.c_str();
    argvlen[2] = hkey.length();
    redisReply* reply = (redisReply*)redisCommandArgv(connect, 3, argv, argvlen);
    if (reply == nullptr || reply->type == REDIS_REPLY_NIL) {
        freeReplyObject(reply);
        std::cout << "Execut command [ HGet " << key << " "<< hkey <<"  ] failure ! " << std::endl;
        _connectionPool->returnConnection(connect);
        return "";
    }

    std::string value = reply->str;
    freeReplyObject(reply);
    std::cout << "Execut command [ HGet " << key << " " << hkey << " ] success ! " << std::endl;
    _connectionPool->returnConnection(connect);
    return value;
}

bool RedisMgr::Del(const std::string &key)
{
    auto connect = _connectionPool->getConnection();
    if (connect == nullptr) {
        std::cout << "Execut command [ Del " << key << " ] failure ! no connection" << std::endl;
        return false;
    }
    
    redisReply* reply = (redisReply*)redisCommand(connect, "DEL %s", key.c_str());
    if (reply == nullptr || reply->type != REDIS_REPLY_INTEGER) {
        std::cout << "Execut command [ Del " << key <<  " ] failure ! " << std::endl;
        freeReplyObject(reply);
        _connectionPool->returnConnection(connect);
        return false;
    }
    std::cout << "Execut command [ Del " << key << " ] success ! " << std::endl;
    freeReplyObject(reply);
    _connectionPool->returnConnection(connect);
    return true;
}

bool RedisMgr::ExistsKey(const std::string &key)
{
    auto connect = _connectionPool->getConnection();
    if (connect == nullptr) {
        std::cout << "Not Found [ Key " << key << " ]  ! no connection" << std::endl;
        return false;
    }
    
    redisReply* reply = (redisReply*)redisCommand(connect, "exists %s", key.c_str());
    if (reply == nullptr || reply->type != REDIS_REPLY_INTEGER || reply->integer == 0) {
        std::cout << "Not Found [ Key " << key << " ]  ! " << std::endl;
        freeReplyObject(reply);
        _connectionPool->returnConnection(connect);
        return false;
    }
    std::cout << " Found [ Key " << key << " ] exists ! " << std::endl;
    freeReplyObject(reply);
    _connectionPool->returnConnection(connect);
    return true;
}

bool RedisMgr::HDel(const std::string& key, const std::string& field){
    auto connect = _connectionPool->getConnection();
    if (connect == nullptr) {
        std::cout << "Execut command [ HDel " << key << " " << field << " ] failure ! no connection" << std::endl;
        return false;
    }

    redisReply* reply = (redisReply*)redisCommand(connect, "HDEL %s %s", key.c_str(), field.c_str());
    if (reply == nullptr || reply->type != REDIS_REPLY_INTEGER) {
        std::cout << "Execut command [ HDel " << key << " " << field << " ] failure ! " << std::endl;
        freeReplyObject(reply);
        _connectionPool->returnConnection(connect);
        return false;
    }

    std::cout << "Execut command [ HDel " << key << " " << field << " ] success ! " << std::endl;
    freeReplyObject(reply);
    _connectionPool->returnConnection(connect);
    return true;
}

bool RedisMgr::HIncrBy(const std::string& key, const std::string& field, int incr){
    auto connect = _connectionPool->getConnection();
    if (connect == nullptr) {
        std::cout << "Execut command [ HIncrBy " << key << " " << field << " " << incr << " ] failure ! no connection" << std::endl;
        return false;
    }

    redisReply* reply = (redisReply*)redisCommand(connect, "HINCRBY %s %s %d", key.c_str(), field.c_str(), incr);
    if (reply == nullptr || reply->type != REDIS_REPLY_INTEGER) {
        std::cout << "Execut command [ HIncrBy " << key << " " << field << " " << incr << " ] failure ! " << std::endl;
        freeReplyObject(reply);
        _connectionPool->returnConnection(connect);
        return false;
    }

    std::cout << "Execut command [ HIncrBy " << key << " " << field << " " << incr << " ] success ! " << std::endl;
    freeReplyObject(reply);
    _connectionPool->returnConnection(connect);
    return true;
}
