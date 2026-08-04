#include "HttpConnection.h"


/*
启动服务器，在浏览器输入`http://localhost:8080/get_test`
会看到服务器回包 receive get_test req
如果我们输入带参数的url请求`http://localhost:8080/get_test?key1=value1&key2=value2`
会收到服务器反馈 url not found
所以对于get请求带参数的情况我们要实现参数解析，我们可以自己实现简单的url解析函数
*/

//将字符转换为16进制
unsigned char ToHex(unsigned char ch){
    return ch > 9 ? (ch + 55) : (ch + 48);
}

//将16进制转换为字符
unsigned char FromHex(unsigned char ch){
    unsigned char y;
    if(ch >= 'A' && ch <= 'Z') y = ch - 'A' + 10;
    else if(ch >= 'a' && ch <= 'z') y = ch - 'a' + 10;
    else if(ch >= '0' && ch <= '9') y = ch - '0';
    else assert(0);
    return y;
}

//url编码，这一块上网搜一下，有很多实现，这里只是简单实现一下
std::string UrlEncode(const std::string& str)
{
    std::string strTemp = "";
    size_t length = str.length();
    for (size_t i = 0; i < length; i++)
    {
        //判断是否仅有数字和字母构成
        if (isalnum((unsigned char)str[i]) ||
            (str[i] == '-') ||
            (str[i] == '_') ||
            (str[i] == '.') ||
            (str[i] == '~'))
            strTemp += str[i];
        else if (str[i] == ' ') //为空字符
            strTemp += "+";
        else
        {
            //其他字符需要提前加%并且高四位和低四位分别转为16进制
            strTemp += '%';
            strTemp += ToHex((unsigned char)str[i] >> 4);
            strTemp += ToHex((unsigned char)str[i] & 0x0F);
        }
    }
    return strTemp;
}

//url解码
std::string UrlDecode(const std::string& str)
{
    std::string strTemp = "";
    size_t length = str.length();
    for (size_t i = 0; i < length; i++)
    {
        //还原+为空
        if (str[i] == '+') strTemp += ' ';
        //遇到%将后面的两个字符从16进制转为char再拼接
        else if (str[i] == '%')
        {
            assert(i + 2 < length);
            unsigned char high = FromHex((unsigned char)str[++i]);
            unsigned char low = FromHex((unsigned char)str[++i]);
            strTemp += high * 16 + low;
        }
        else strTemp += str[i];
    }
    return strTemp;
}

//get请求参数解析
void HttpConnection::PreParseGetParams(){
    // 提取 URI   get_test?key1=value1&key2=value2
    auto uri = _request.target();
    // 查找查询字符串的开始位置（即 '?' 的位置）  
    auto query_pos = uri.find('?');
    if (query_pos == std::string::npos) {
        _get_url = uri;
        return;
    }

    _get_url = uri.substr(0, query_pos); //左闭右开
    std::string query_string = uri.substr(query_pos + 1);
    std::string key;
    std::string value;
    size_t pos = 0;
    while ((pos = query_string.find('&')) != std::string::npos) {
        auto pair = query_string.substr(0, pos);
        size_t eq_pos = pair.find('='); //再找=截开
        if (eq_pos != std::string::npos) {
            key = UrlDecode(pair.substr(0, eq_pos)); // 解码键名
            value = UrlDecode(pair.substr(eq_pos + 1));
            _get_params[key] = value;
        }
        query_string.erase(0, pos + 1); //删除已经解析的参数对
    }
    // 处理最后一个参数对（如果没有 & 分隔符）  
    if (!query_string.empty()) {
        size_t eq_pos = query_string.find('=');
        if (eq_pos != std::string::npos) {
            key = UrlDecode(query_string.substr(0, eq_pos));
            value = UrlDecode(query_string.substr(eq_pos + 1));
            _get_params[key] = value;
        }
    }
}


HttpConnection::HttpConnection(net::io_context &ioc):_socket(ioc),deadline_(ioc)
{

}


void HttpConnection::Start()
{
    auto self = shared_from_this();
    //异步读取http请求
    //_request是解析后的http请求，_buffer是临时缓冲区
    http::async_read(_socket,_buffer,_request,[self](beast::error_code ec,std::size_t  bytes_transferred){
        try{
             //error_code中重载了bool()运算符
            if(ec){
                std::cout << "http read error:" << ec.message() << std::endl;
                return;
            }
            boost::ignore_unused(bytes_transferred); //忽略未使用的参数
            self->HandleReq();
            self->CheckDeadline(); //处理完请求后，检查是否超时
        }catch(std::exception& e){
            std::cout << "http read exception:" << e.what() << std::endl;
        }
    });
}


void HttpConnection::CheckDeadline()
{
    auto self = shared_from_this();
    deadline_.async_wait([self](beast::error_code ec) {
        // 如果定时器被取消，ec 是 operation_aborted
        if(ec == net::error::operation_aborted) {
            // 定时器被主动取消，说明请求已正常完成
            // 不需要做任何处理
            return;
        }
        
        if(!ec) {
            // 超时！关闭连接
            std::cerr << "Connection timeout, closing...\n";
            self->_socket.close(ec);
        }
    });
}

void HttpConnection::WriteResponse()
{
    auto self = shared_from_this();
    _response.content_length(_response.body().size()); //设置响应体长度
    http::async_write(_socket,_response,[self](beast::error_code ec,std::size_t bytes_transferred){
        self->_socket.shutdown(tcp::socket::shutdown_send,ec); //关闭发送端
        self->deadline_.cancel(); //取消超时定时器

    });
}

void HttpConnection::HandleReq()
{
    //设置版本
    _response.version(_request.version());
    _response.keep_alive(false); //不需要维持长连接，处理完这个请求就关闭连接
    //处理get请求
    if(_request.method() == http::verb::get){
        //传入请求，返回响应
        //处理get请求参数
        PreParseGetParams();
        //_request.target()是请求的url，后面传入当前对象的智能指针，用于异步处理
        //返回true表示找到了对应路由并处理成功，返回false表示处理失败
        auto self = shared_from_this();
        //bool success = LogicSystem::GetInstance()->HandleGet(_request.target(), self);
        bool success = LogicSystem::GetInstance()->HandleGet(_get_url, self);
        if(!success){
            _response.result(http::status::not_found); //设置状态码为404
            _response.set(http::field::content_type, "text/plain"); //告诉服务器返回的是纯文本
            beast::ostream(_response.body()) << "url Not Found\r\n"; //向响应体中写入内容
            WriteResponse(); //发送响应
            return;
        }
         _response.result(http::status::ok);
        _response.set(http::field::server, "GateServer");
        WriteResponse();
    }
    if(_request.method() == http::verb::post){
        //处理post请求
        PreParseGetParams();
        auto self = shared_from_this();
        bool success = LogicSystem::GetInstance()->HandlePost(_request.target(), self);
        if(!success){
            _response.result(http::status::not_found);
            _response.set(http::field::content_type, "text/plain");
            beast::ostream(_response.body()) << "url Not Found\r\n"; 
            WriteResponse();
            return;
        }
        _response.result(http::status::ok);
        _response.set(http::field::server, "GateServer");
        WriteResponse();
    }
}
