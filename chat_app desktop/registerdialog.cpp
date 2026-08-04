#include "registerdialog.h"
#include "ui_registerdialog.h"
#include "global.h"
#include "httpmgr.h"
#include <QTimer>
#include <QEventLoop>
#include <QLabel>
#include "clickedlabel.h"

RegisterDialog::RegisterDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::RegisterDialog)
{
    ui->setupUi(this);

    //connect(ui->sure_btn,&QPushButton::clicked,this,&RegisterDialog::switchLogin);

    ui->pwd_edit->setEchoMode(QLineEdit::Password);
    ui->confirm_edit->setEchoMode(QLineEdit::Password);

    //设置err_tip的两个属性，才好实现样式刷新的逻辑
    ui->err_tip->setProperty("state","normal"); //给控件定义一个属性state，normal是属性值
    repolish(ui->err_tip); //repolish这个函数在

    connect(HttpMgr::GetInstance().get(), &HttpMgr::sig_reg_mod_finish,
            this, &RegisterDialog::slot_reg_mod_finish);

    initHttpHandlers();

    //鼠标滑上去变成小手
    ui->pass_visible->setCursor(Qt::PointingHandCursor);
    ui->confirm_visible->setCursor(Qt::PointingHandCursor);

    ui->pass_visible->SetState("unvisible","unvisible_hover","","visible","visible_hover","");
    ui->confirm_visible->SetState("unvisible","unvisible_hover","","visible","visible_hover","");

    //连接小眼睛的信号和槽
    connect(ui->pass_visible,&ClickedLabel::Clicked,[this]{
        auto state = ui->pass_visible->GetCurState();
        if(state == ClickLbState::Selected){
            ui->pwd_edit->setEchoMode(QLineEdit::Normal);
        }else{
            ui->pwd_edit->setEchoMode(QLineEdit::Password);
        }
    });

    connect(ui->confirm_visible,&ClickedLabel::Clicked,[this]{
        auto state = ui->confirm_visible->GetCurState();
        if(state == ClickLbState::Selected){
            ui->confirm_edit->setEchoMode(QLineEdit::Normal);
        }else{
            ui->confirm_edit->setEchoMode(QLineEdit::Password);
        }
    });

}

RegisterDialog::~RegisterDialog()
{
    delete ui;
}

//利用qt creator的转到槽功能更高效的书写槽函数
void RegisterDialog::on_get_code_clicked()
{
    //验证邮箱的地址正则表达式
    auto email = ui->email_edit->text();
    //邮箱地址的正则表达式，这个上网搜，或者问ai即可
    QRegularExpression regex(R"((\w+)(\.|_)?(\w*)@(\w+)(\.(\w+))+)");
    bool match = regex.match(email).hasMatch(); // 执行正则表达式匹配
    if(match){
        //发送http请求获取验证码
        QJsonObject json_obj;
        json_obj["email"] = email;
        HttpMgr::GetInstance()->PostHttpReq(QUrl(gate_url_prefix + "/get_varifycode"),
                                            json_obj,ReqId::ID_GET_VERIFY_CODE,Modules::REGISTERMOD);
        showtip(tr("已成功发送验证码"),"normal");
    }
    else{
        showtip(tr("邮箱地址不正确"),"err");
    }
}


void RegisterDialog::slot_reg_mod_finish(ReqId id, QString res, ErrorCodes err)
{
    /*反序列化流程
、   *字节流->json字符串->json对象
     第一步在 QString res = reply->readAll(); 已经完成，readAll返回的就是json字符串 */

    //解析json字符串， res转换为QByteArray
    //这个QJsonDocument可以理解为.json文件
    //res是json格式的字符串
    //toUtf8 转化为utf8编码的字节数组，也就是01串
    //fromJson解析字节数组，生成json文档
    QJsonDocument jsonDoc = QJsonDocument::fromJson(res.toUtf8());
    if(jsonDoc.isNull()){
        //转换失败
        showtip(tr("json解析失败"),"err");
        return;
    }
    //json解析错误
    if(!jsonDoc.isObject()){
        showtip(tr("json解析失败"),"err");
        return;
    }
    //将json文档转成json对象，给到回调函数处理
    _handlers[id](jsonDoc.object());
    return;
}

void RegisterDialog::initHttpHandlers()
{
    //注册获取验证码回包的逻辑
    _handlers.insert(ReqId::ID_GET_VERIFY_CODE, [this](const QJsonObject& jsonObj){
        int error = jsonObj["error"].toInt(); //json对象有一个键是error，我们将它转化为int

        if(error == ErrorCodes::DuplicateRequest){
            showtip(tr("验证码已经发送，请勿重复请求"),"err");
            return;
        }

        if(error != ErrorCodes::SUCCESS){
            showtip(tr("服务异常"),"err");
            return;
        }


        auto email = jsonObj["email"].toString();
        showtip(tr("验证码已经发送到邮箱，请注意查收"),"normal");
        qDebug() << "email is" << email;
    });

    //注册用户回包逻辑
    _handlers.insert(ReqId::ID_REG_USER,[this](const QJsonObject& jsonObj){
        int error = jsonObj["error"].toInt();

        if(error == ErrorCodes::Error_VarifyCode){
            showtip(tr("验证码错误"),"err");
            return;
        }

        if(error == ErrorCodes::Error_VarifyCodeExpired){
            showtip(tr("验证码无效"),"err");
            return;
        }

        if(error == ErrorCodes::Error_EmailRegistered){
            showtip(tr("邮箱已经注册，请重新输入邮箱"),"err");
            return;
        }

        if(error == ErrorCodes::Error_UserExist){
            showtip(tr("用户名存在，请重新输入用户名"),"err");
            return;
        }

        if(error != ErrorCodes::SUCCESS){
            showtip(tr("参数错误，注册失败"),"err");
            return;
        }

        auto email = jsonObj["email"].toString();
        showtip(tr("用户注册成功，3s后返回登录界面"),"normal");

        // 非阻塞等待3秒
        QEventLoop loop;
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        loop.exec();

        //清空输入栏文字
        ui->email_edit->clear();
        ui->user_edit->clear();
        ui->confirm_edit->clear();
        ui->pwd_edit->clear();
        ui->verify_edit->clear();


        emit switchLogin(); //发送切换login的信号
        qDebug() << "email is" << email;
    });
}

void RegisterDialog::showtip(QString str,QString state)
{
    ui->err_tip->setText(str);
    ui->err_tip->setProperty("state",state);
    repolish(ui->err_tip); //刷新
}


//注册确认按钮的槽函数
void RegisterDialog::on_sure_btn_clicked()
{
    if(ui->user_edit->text() == ""){
        showtip(tr("用户名不能为空"),"err");
        return;
    }

    if(ui->email_edit->text() == ""){
        showtip(tr("邮箱不能为空"),"err");
        return;
    }

    if(ui->pwd_edit->text() == ""){
        showtip(tr("密码不能为空"),"err");
        return;
    }

    if(!checkPassValid()){
        return;
    }

    if(ui->confirm_edit->text() == ""){
        showtip(tr("确认密码不能为空"),"err");
        return;
    }

    if(ui->pwd_edit->text() != ui->confirm_edit->text()){
        showtip(tr("密码和确认密码不匹配"),"err");
        return;
    }

    if(ui->verify_edit->text() == ""){
        showtip(tr("验证码不能为空"),"err");
        return;
    }

    //发送http请求注册用户
    QJsonObject json_obj;
    json_obj["user"] = ui->user_edit->text();
    json_obj["email"] = ui->email_edit->text();
    json_obj["passwd"] = xorString(ui->pwd_edit->text()); //用上加密算法
    json_obj["confirm"] = xorString(ui->confirm_edit->text());
    json_obj["varifycode"] = ui->verify_edit->text();
    HttpMgr::GetInstance()->PostHttpReq(QUrl(gate_url_prefix+"/user_register"),json_obj,
                                        ReqId::ID_REG_USER,Modules::REGISTERMOD);



}

//点击取消会转到login界面，并且清空输入栏
void RegisterDialog::on_cancel_btn_clicked()
{
    ui->email_edit->clear();
    ui->user_edit->clear();
    ui->confirm_edit->clear();
    ui->pwd_edit->clear();
    ui->verify_edit->clear();


    emit switchLogin(); //发送切换login的信号
}


bool RegisterDialog::checkPassValid()
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
