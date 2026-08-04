#ifndef HTTPMGR_H
#define HTTPMGR_H

/******************************************************************************
 *
 * @file       httpmgr.h
 * @brief      实现一个http管理类，负责发包和通知回包
 *
 * http管理类一般是单例模式
 * 1，可以共享网络连接池
 * 2，统一的请求管理
 * 3，全局配置共享
 *
 *
 * CRTP【奇异递归模板模式】是实现静态多态(编译时多态)的一种设计模式，我们下面要实现的类就用了这个
 *
 * @author     klein
 *****************************************************************************/

#include "singleton.h"
#include "global.h"
#include <QString>
#include <QUrl>
/* QObject 是 Qt 框架中最核心的基类，几乎所有 Qt 类都直接或间接继承自它。
 * 它提供了 Qt 的核心机制，比如信号和槽，对象树，元对象系统，和事件系统
 * 我们的httpmgr继承这个QObject可以实现上述机制*/
#include <QObject>

/*使用Qt的网络模块，需要在cmake中添加一些东西
# 在 find_package 中添加 Network 组件
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

在 target_link_libraries 中链接它
target_link_libraries(your_target_name PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
)*/
#include <QNetworkAccessManager>
/* 网络通信中，与服务器交换数据，json是主流格式
 * QJsonObject用于创建json对象
 * QJsonDocument用于序列化对象->字符串，与反序列化*/
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QByteArray>

//派生类将自己作为模板参数传递给基类，这就是编译时多态的体现
class HttpMgr:public QObject, public Singleton<HttpMgr>,
                public std::enable_shared_from_this<HttpMgr>
{
    Q_OBJECT //宏展开后会生成大量代码，从而启动各种核心机制，比如信号和槽之类的

public:
    //这个析构要设置为公有的，因为是智能指针负责的析构，要让他可以访问
    ~HttpMgr();

    /* 发送http请求的函数，需要
     * 用到请求的路由，也就是url，
     * 请求的数据(json或者protobuf序列化)，
     * 请求的id
     * 发出请求的模块*/
    void PostHttpReq(QUrl url, QJsonObject json, ReqId req_id, Modules mod);  //这两个数据去global里面定义

private:
    /* 将基类声明为友元，这样才可以_instance = std::shared_ptr<T>(new T)
     * 因为new是需要访问T的构造函数的*/
    friend class Singleton;
    HttpMgr(); //单例模式，构造函数自然要设置为私有的
    QNetworkAccessManager _manager; //Qt原生的网络管理者

private slots:
    //http发送完毕后的槽函数
    //槽函数参数数量要<=信号数量，并且参数顺序要保持一致
    void slot_http_finish(ReqId id, QString res, ErrorCodes err, Modules mod);


signals:
    //当一个http发送完毕，会发送一个信号通知其他模块
    void sig_http_finish(ReqId id, QString res, ErrorCodes err, Modules mod);
    //注册模块http完成的信号，让注册模块去接收的，所以去register里面完成一下
    void sig_reg_mod_finish(ReqId id, QString res, ErrorCodes err);

    void sig_login_mod_finish(ReqId id, QString res, ErrorCodes err);

    void sig_reset_mod_finish(ReqId id, QString res, ErrorCodes err);

};

#endif // HTTPMGR_H
