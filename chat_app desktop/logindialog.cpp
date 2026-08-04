#include "logindialog.h"
#include "ui_logindialog.h"
#include <QJsonObject>
#include "httpmgr.h"
#include "clickedlabel.h"
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>
#include "logindialog.h"
#include "tcpmgr.h"

LoginDialog::LoginDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::LoginDialog)
{
    ui->setupUi(this);

    initHead();

    //当注册按钮被点击，就向主窗口发送切换注册界面的信号，去到mainwindow.cpp完成逻辑实现
    connect(ui->reg_btn,&QPushButton::clicked,this,&LoginDialog::switchRegister);

    ui->pwd_edit->setEchoMode(QLineEdit::Password);

    //绑定信号和槽，当httpmgr接收到回包之后执行槽函数
    connect(HttpMgr::GetInstance().get(),&HttpMgr::sig_login_mod_finish,
            this,&LoginDialog::slot_login_mod_finish);

    initHttpHandlers();

    ui->forget_pwd_label->SetState("normal","hover","","selected","selected_hover","");

    connect(ui->forget_pwd_label,&ClickedLabel::Clicked,[this]{
        qDebug() << "switchRet" ;
        emit switchReset();
    }); //通知Mainwindow切屏

    //连接服务器
    connect(this, &LoginDialog::sig_connect_tcp, TcpMgr::GetInstance().get(), &TcpMgr::slot_tcp_connect);

    //处理与服务器连接的结果
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_con_success, this, &LoginDialog::slot_tcp_con_success);

    //处理tcpmgr发来的登录失败信号
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_login_failed, this, &LoginDialog::slot_login_failed);

}

LoginDialog::~LoginDialog()
{
    delete ui;
}

void LoginDialog::showtip(QString str,QString state)
{
    ui->err_tip->setText(str);
    ui->err_tip->setProperty("state",state);
    repolish(ui->err_tip);
}

void LoginDialog::initHead()
{
    //加载图片
    QPixmap originalPixmap(":/res/head_2.jpg");
    //设置图片自动缩放
    qDebug() << originalPixmap.size() << ui->head_label->size();
    originalPixmap = originalPixmap.scaled(ui->head_label->size(),
                    Qt::KeepAspectRatio,Qt::SmoothTransformation);

    //创建一个和原始图片同样大小的pixmap，用于绘制圆角图片
    QPixmap roundedPixmap(originalPixmap.size());
    roundedPixmap.fill(Qt::transparent); //用透明色填充

    QPainter painter(&roundedPixmap);
    painter.setRenderHint(QPainter::Antialiasing); //设置抗锯齿，使圆角更平滑
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    //使用QPainterPath设置圆角
    QPainterPath path;
    path.addRoundedRect(0,0,originalPixmap.width(),originalPixmap.height(),10,10);
    painter.setClipPath(path);

    //将原始图片绘制在roundedPixmap上
    painter.drawPixmap(0,0,originalPixmap);

    //设置绘制好的圆角图片到QLabel上
    ui->head_label->setPixmap(roundedPixmap);
}

void LoginDialog::on_login_btn_clicked()
{
    //仿造前面注册界面确认按钮
    if(ui->user_edit->text() == ""){
        showtip(tr("用户名不能为空"),"err");
        return;
    }

    if(ui->pwd_edit->text() == ""){
        showtip(tr("密码不能为空"),"err");
        return;
    }

    //为了减少Mysql的查询压力，我们可以在这里多加一点密码的判断
    if(!checkPassValid()) return;

    enabledBtn(false);


    //发送http请求注册用户
    QJsonObject json_obj;
    json_obj["user"] = ui->user_edit->text();
    json_obj["passwd"] = xorString(ui->pwd_edit->text());
    HttpMgr::GetInstance()->PostHttpReq(QUrl(gate_url_prefix+"/user_login"),json_obj,
                                        ReqId::ID_LOGIN,Modules::LOGINMOD);


}


void LoginDialog::slot_login_mod_finish(ReqId id, QString res, ErrorCodes err)
{
    enabledBtn(true);

    QJsonDocument jsonDoc = QJsonDocument::fromJson(res.toUtf8());
    if(jsonDoc.isNull() || !jsonDoc.isObject()){
        //转换失败，或者json解析失败
        showtip(tr("json解析失败"),"err");
        return;
    }

    //将json文档转成json对象，给到回调函数处理
    _handlers[id](jsonDoc.object());

}

void LoginDialog::slot_login_failed(int err)
{
    if(err != ErrorCodes::SUCCESS){
        showtip(tr("登录失败"),"err");
        return;
    }
    showtip(tr("登录成功"),"normal");
    ui->pwd_edit->clear();
    ui->user_edit->clear();
    return;
}

void LoginDialog::slot_tcp_con_success(bool success)
{
    showtip(tr("连接聊天服务器成功...正在登录中..."),"err");
    QJsonObject json_obj;
    json_obj["uid"] = _uid;
    json_obj["token"] = _token;

    QJsonDocument doc(json_obj);
    QString jsonString = doc.toJson(QJsonDocument::Indented);

    //发送tcp请求给chatserver
    TcpMgr::GetInstance()->sig_send_data(ReqId::ID_CHAT_LOGIN, jsonString);
}


void LoginDialog::initHttpHandlers()
{
    //处理登录回包
    _handlers.insert(ReqId::ID_LOGIN,[this](const QJsonObject& json_obj){
        int err = json_obj["error"].toInt();

        if(err == ErrorCodes::Error_Password){
            showtip(tr("密码错误"),"err");
            return;
        }

        if(err == ErrorCodes::Error_UserNoExist){
            showtip(tr("用户名不存在"),"err");
            return;
        }

        if(err != ErrorCodes::SUCCESS){
            showtip(tr("未知错误"),"err");
            return;
        }

        QString user = json_obj["user"].toString();
        showtip(tr("用户验证通过，正在连接服务器..."),"normal");

        //发送信号通知tcpMgr发送长链接，网关那边会调用statuserver给出tcp服务器的地址
        ServerInfo si;
        si.Uid = json_obj["uid"].toInt();
        si.Host = json_obj["host"].toString();
        si.Port = json_obj["port"].toString();
        si.Token = json_obj["token"].toString();

        _uid = si.Uid;
        _token = si.Token;

        emit sig_connect_tcp(si); //发给tcpmgr
    });

}

bool LoginDialog::checkPassValid()
{
    auto pass = ui->pwd_edit->text();

    if(pass.length() < 6 || pass.length()>15){
        //提示长度不准确
        showtip(tr("密码长度应为6-15"),"err");
        return false;
    }

    // 创建一个正则表达式对象，按照上述密码要求
    // 这个正则表达式解释：
    // ^[a-zA-Z0-9!@#$%^&*]{6,15}$ 密码长度至少6，可以是字母、数字和特定的特殊字符
    QRegularExpression regExp("^[a-zA-Z0-9!@#$%^&*]{6,15}$");
    bool match = regExp.match(pass).hasMatch();
    if(!match){
        //提示字符非法
        showtip(tr("不能包含非法字符"),"err");
        return false;;
    }
    return true;
}

void LoginDialog::enabledBtn(bool enabled)
{
    ui->login_btn->setEnabled(enabled);
}

