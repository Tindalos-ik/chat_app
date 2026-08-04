#include "httpmgr.h"


HttpMgr::HttpMgr(){
    //连接信号和槽，在接收到回包后通知对应模块
    connect(this, &HttpMgr::sig_http_finish, this, &HttpMgr::slot_http_finish);
}

void HttpMgr::PostHttpReq(QUrl url, QJsonObject json, ReqId req_id, Modules mod)
{
    /*发送需要序列化
     *QJsonObject->json字符串->字节流*/
    //将json对象转换为字节数组，也就是字节流
    QByteArray data = QJsonDocument(json).toJson();
    //配置HTTP请求头
    QNetworkRequest request(url); //创建一个http请求对象，并且指定url
    //告知服务器请求体的数据格式是json
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    //告诉服务器请求体的内容长度
    request.setHeader(QNetworkRequest::ContentLengthHeader, QByteArray::number(data.length()));

    /*设置完后，大概就是这样
     * POST /api/register HTTP/1.1
     * Host: example.com
     * Content-Type: application/json
     * Content-Length: 27

    {"username":"alice","age":25}*/


    auto self = shared_from_this(); //使用智能指针，确保异步回调的过程中对象不会被销毁
    //提交请求，并且返回reply
    QNetworkReply* reply = _manager.post(request, data); //通过网络发送字节流
    //注意这里的资源要自己回收，使用deleteLater
    //使用lambda作为槽函数的时候，一般没有显式接收者
    connect(reply,&QNetworkReply::finished,[self, reply, req_id, mod]{
        //处理错误情况
        if(reply->error() != QNetworkReply::NoError){
            qDebug() << reply->errorString();
            //发送信号通知完成
            emit self->sig_http_finish(req_id, "", ErrorCodes::ERR_NETWORK, mod);
            reply->deleteLater(); // 回收reply
            return;
        }
        //无错误
        QString res = reply->readAll(); //返回的就是json格式的字符串
        //发送信号通知完成
        emit self->sig_http_finish(req_id, res, ErrorCodes::SUCCESS, mod);
        reply->deleteLater();
        return;
    });
}

void HttpMgr::slot_http_finish(ReqId id, QString res, ErrorCodes err, Modules mod)
{
    if(mod == Modules::REGISTERMOD){
        //发送信号通知指定模块，http响应结束
        emit sig_reg_mod_finish(id, res, err);
    }

    if(mod == Modules::LOGINMOD){
        emit sig_login_mod_finish(id,res,err);
    }

    if(mod == Modules::RESETMOD){
        emit sig_reset_mod_finish(id,res,err);
    }
}



HttpMgr::~HttpMgr(){

}
