#include "LogicSystem.h"
#include "json.h"
#include "json-forwards.h"
#include "VarifyGrpcClient.h"
#include "RedisMgr.h"
#include "MysqlMgr.h"
#include "StatusGrpcClient.h"

LogicSystem::LogicSystem()
{
    RegGet("/get_test", [](std::shared_ptr<HttpConnection> connection){
        beast::ostream(connection->_response.body()) << "receive get_test req";
        int i = 0;
        for(auto& elem : connection->_get_params){
            i++;
            beast::ostream(connection->_response.body()) << "param " << i << " key is " << elem.first ;
            beast::ostream(connection->_response.body()) << " param " << i << " value is " << elem.second;
        }
    });
    //实现获取验证码的逻辑
    RegPost("/get_varifycode", [](std::shared_ptr<HttpConnection> connection){
        //获取post请求的body，从buffer转成string
        auto body_str = boost::beast::buffers_to_string(connection->_request.body().data());
        std::cout<< "receive post body is " << body_str << std::endl;
        connection->_response.set(http::field::content_type, "text/json"); //设置返回类型为json
        //json解析，将body_str解析成json对象
        Json::Value root; //返回的json对象
        Json::Value src_root; //解析出来的json对象
        Json::CharReaderBuilder reader; //解析器
        std::string errs;
    
        //将字符串转换为流
        std::istringstream ss(body_str);
        bool parse_success = Json::parseFromStream(reader, ss, &src_root, &errs);
    
        if(!parse_success){
            std::cout << "Failed to parse JSON data" << std::endl;
            std::cout << errs << std::endl;
            root["error"] = ErrorCode::Error_Json;
            std::string jsonstr = root.toStyledString();
            beast::ostream(connection->_response.body()) << jsonstr;
            return true;
        }

        if(!src_root.isMember("email")){
            std::cout << "email not found" << std::endl;
            std::cout << errs << std::endl;
            root["error"] = ErrorCode::Error_Json;
            std::string jsonstr = root.toStyledString();
            beast::ostream(connection->_response.body()) << jsonstr;
            return true;
        }

        auto email = src_root["email"].asString();
        GetVarifyRsp rsp = VarifyGrpcClient::GetInstance()->GetVarifyCode(email); //调用grpc客户端调用获取验证码的远程服务
        std::cout << "email is " << email << std::endl;
        root["error"] = rsp.error();
        root["email"] = src_root["email"];
        //tcp面向字节流，要把json对象转成字符串才能发送
        std::string jsonstr = root.toStyledString(); //转化为标准字符串
        beast::ostream(connection->_response.body()) << jsonstr; //发送给客户端
        return true;
    });

    //实现注册的逻辑
    RegPost("/user_register", [](std::shared_ptr<HttpConnection> connection){
        //获取post请求的body，从buffer转成string
        auto body_str = boost::beast::buffers_to_string(connection->_request.body().data());
        std::cout<< "receive post body is " << body_str << std::endl;
        connection->_response.set(http::field::content_type, "text/json"); //设置返回类型为json
        //json解析，将body_str解析成json对象
        Json::Value root; //返回的json对象
        Json::Value src_root; //解析出来的json对象
        Json::CharReaderBuilder reader; //解析器
        std::string errs;
    
        //将字符串转换为流
        std::istringstream ss(body_str);
        bool parse_success = Json::parseFromStream(reader, ss, &src_root, &errs);
    
        if(!parse_success){
            std::cout << "Failed to parse JSON data" << std::endl;
            std::cout << errs << std::endl;
            root["error"] = ErrorCode::Error_Json;
            std::string jsonstr = root.toStyledString();
            beast::ostream(connection->_response.body()) << jsonstr;
            return true;
        }

        //先看邮箱是否已经注册
        bool checkemail = MysqlMgr::GetInstance()->Checkemail(src_root["email"].asString());
        if(checkemail){
            //邮箱已经注册
            std::cout << "email has been registered" << std::endl;
            root["error"] = ErrorCode::Error_EmailRegistered;
            std::string jsonstr = root.toStyledString();
            beast::ostream(connection->_response.body()) << jsonstr;
            return true;
        }

        //再看用户名是否已经注册
        bool checkuser = MysqlMgr::GetInstance()->Checkuser(src_root["user"].asString());
        if(checkuser){
            //用户名已经注册
            std::cout << "user has been registered" << std::endl;
            root["error"] = ErrorCode::Error_UserExist;
            std::string jsonstr = root.toStyledString();
            beast::ostream(connection->_response.body()) << jsonstr;
            return true;
        }
        
        //先看redis中email的验证码和发来的验证码是否匹配
        std::string email = src_root["email"].asString();
        std::string varifycode = src_root["varifycode"].asString();
        std::string redis_varifycode;
        std::string email_prefix = "code_";
        
        bool b_get_varify = RedisMgr::GetInstance()->Get(email_prefix+email, redis_varifycode);
        if(!b_get_varify){
            //验证码无效，可能是压根没发，也可能是过期了，归结为无效
            std::cout << "get redis varify code expired" << std::endl;
            root["error"] = ErrorCode::Error_VarifyCodeExpired;
            std::string jsonstr = root.toStyledString();
            beast::ostream(connection->_response.body()) << jsonstr;
            return true;
        }

        if(redis_varifycode != varifycode){
            //验证码错误
            std::cout << "varify code error" << std::endl;
            root["error"] = ErrorCode::Error_VarifyCode;
            std::string jsonstr = root.toStyledString();
            beast::ostream(connection->_response.body()) << jsonstr;
            return true;
        }


        root["error"] = ErrorCode::Success;
        root["email"] = src_root["email"];
        root["user"] = src_root["user"];
        root["passwd"] = src_root["passwd"];
        root["confirm"] = src_root["confirm"];
        root["varifycode"] = src_root["varifycode"];
        std::string jsonstr = root.toStyledString();
        beast::ostream(connection->_response.body()) << jsonstr;

        //写入mysql数据库
        MysqlMgr::GetInstance()->RegUser(root["user"].asString(), root["email"].asString(), root["passwd"].asString());

        return true;
    });


    //实现登录的逻辑
    RegPost("/user_login", [](std::shared_ptr<HttpConnection> connection){
        //获取post请求的body，从buffer转成string
        auto body_str = boost::beast::buffers_to_string(connection->_request.body().data());
        std::cout<< "receive post body is " << body_str << std::endl;
        connection->_response.set(http::field::content_type, "text/json"); //设置返回类型为json
        //json解析，将body_str解析成json对象
        Json::Value root; //返回的json对象
        Json::Value src_root; //解析出来的json对象
        Json::CharReaderBuilder reader; //解析器
        std::string errs;
    
        //将字符串转换为流
        std::istringstream ss(body_str);
        bool parse_success = Json::parseFromStream(reader, ss, &src_root, &errs);
    
        if(!parse_success){
            std::cout << "Failed to parse JSON data" << std::endl;
            std::cout << errs << std::endl;
            root["error"] = ErrorCode::Error_Json;
            std::string jsonstr = root.toStyledString();
            beast::ostream(connection->_response.body()) << jsonstr;
            return true;
        }

        std::string user = src_root["user"].asString();
        std::string passwd = src_root["passwd"].asString();
        //先看mysql中是否有这个用户
        bool checkuser = MysqlMgr::GetInstance()->Checkuser(user);
        if(!checkuser){
            //用户不存在
            std::cout << "user not exist" << std::endl;
            root["error"] = ErrorCode::Error_UserNoExist;
            std::string jsonstr = root.toStyledString();
            beast::ostream(connection->_response.body()) << jsonstr;
            return true;
        }

        //再看密码是否正确
        UserInfo user_info;
        bool checkpasswd = MysqlMgr::GetInstance()->CheckPwd(user, passwd, user_info);
        if(!checkpasswd){
            //密码错误
            std::cout << "password error" << std::endl;
            root["error"] = ErrorCode::Error_Password;
            std::string jsonstr = root.toStyledString();
            beast::ostream(connection->_response.body()) << jsonstr;
            return true;
        }

        //查询StatusServer找到合适的链接，这个过程中会生成token
        auto reply = StatusGrpcClient::GetInstance()->GetChatServer(user_info.uid);
        if(reply.error()){
            std::cout << "get chat server error" << std::endl;
            root["error"] = ErrorCode::RPCFaild;
            std::string jsonstr = root.toStyledString();
            beast::ostream(connection->_response.body()) << jsonstr;
            return true;
        }

        std::cout << "succeed to load userinfo uid is " << user_info.uid << std::endl;
        root["error"] = ErrorCode::Success;
        root["uid"] = user_info.uid;
        root["email"] = user_info.email;
        root["token"] = reply.token(); //token就是用户和服务器通信的凭证
        root["host"] = reply.host();
        root["port"] = reply.port();
        std::string jsonstr = root.toStyledString();
        beast::ostream(connection->_response.body()) << jsonstr;
        return true;
    
    });

    RegPost("/user_resetpassword",[](std::shared_ptr<HttpConnection> connection){
        //获取post请求的body，从buffer转成string
        auto body_str = boost::beast::buffers_to_string(connection->_request.body().data());
        std::cout<< "receive post body is " << body_str << std::endl;
        connection->_response.set(http::field::content_type, "text/json"); //设置返回类型为json
        //json解析，将body_str解析成json对象
        Json::Value root; //返回的json对象
        Json::Value src_root; //解析出来的json对象
        Json::CharReaderBuilder reader; //解析器
        std::string errs;
    
        //将字符串转换为流
        std::istringstream ss(body_str);
        bool parse_success = Json::parseFromStream(reader, ss, &src_root, &errs);
    
        if(!parse_success){
            std::cout << "Failed to parse JSON data" << std::endl;
            std::cout << errs << std::endl;
            root["error"] = ErrorCode::Error_Json;
            std::string jsonstr = root.toStyledString();
            beast::ostream(connection->_response.body()) << jsonstr;
            return true;
        }

        auto user = src_root["user"].asString();
        auto email = src_root["email"].asString();
        auto newpasswd = src_root["passwd"].asString();
        auto varifycode = src_root["varifycode"].asString();

        //首先检查有没有这个用户
        bool checkuser = MysqlMgr::GetInstance()->Checkuser(user);
        
        if(!checkuser){
            //用户不存在
            std::cout << "user not exist" << std::endl;
            root["error"] = ErrorCode::Error_UserNoExist;
            std::string jsonstr = root.toStyledString();
            beast::ostream(connection->_response.body()) << jsonstr;
            return true;
        }

        //再检查用户名和邮箱是否匹配
        bool ismatch = MysqlMgr::GetInstance()->isMatch(user,email);
        if(!ismatch){
            std::cout << "user and email not match" << std::endl;
            root["error"] = ErrorCode::Error_UserNotMatchEamil;
            std::string jsonstr = root.toStyledString();
            beast::ostream(connection->_response.body()) << jsonstr;
            return true;
        }

        std::string email_prefix = "code_";
        std::string redis_varifycode = "";
        //然后检查验证码是否正确
        bool b_get_varify = RedisMgr::GetInstance()->Get(email_prefix+email, redis_varifycode);
        if(!b_get_varify){
            //验证码无效，可能是压根没发，也可能是过期了，归结为无效
            std::cout << "get redis varify code expired" << std::endl;
            root["error"] = ErrorCode::Error_VarifyCodeExpired;
            std::string jsonstr = root.toStyledString();
            beast::ostream(connection->_response.body()) << jsonstr;
            return true;
        }

        if(redis_varifycode != varifycode){
            //验证码错误
            std::cout << "varify code error" << std::endl;
            root["error"] = ErrorCode::Error_VarifyCode;
            std::string jsonstr = root.toStyledString();
            beast::ostream(connection->_response.body()) << jsonstr;
            return true;
        }

        //修改密码
        bool resetpwd = MysqlMgr::GetInstance()->UpdatePwd(user,newpasswd);
        std::cout << "reset password success" << std::endl;
        
        //修改密码成功返回
        root["error"] = ErrorCode::Success;
        std::string jsonstr = root.toStyledString();
        beast::ostream(connection->_response.body()) << jsonstr;
        return true;
    });
}

bool LogicSystem::HandleGet(std::string path, std::shared_ptr<HttpConnection> con)
{
    if(_get_handlers.find(path) == _get_handlers.end()){
        //没有这个路由
        return false;
    }
    _get_handlers[path](con); //con是传进去的参数
    return true;

}

//加入路由和对应的处理函数，注册get请求
void LogicSystem::RegGet(std::string url, HttpHandler handler)
{
    _get_handlers.insert(std::make_pair(url, handler));
}


bool LogicSystem::HandlePost(std::string path, std::shared_ptr<HttpConnection> con)
{
    if(_post_handlers.find(path) == _post_handlers.end()){
        return false;
    }
    _post_handlers[path](con);
    return true;
}

void LogicSystem::RegPost(std::string url, HttpHandler handler)
{
    _post_handlers.insert(std::make_pair(url, handler));
}
