#include "resetdialog.h"
#include "ui_resetdialog.h"
#include "httpmgr.h"

ResetDialog::ResetDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::ResetDialog)
{
    ui->setupUi(this);

    ui->pass_visible->SetState("unvisible","unvisible_hover","","visible","visible_hover","");

    ui->newpwd_edit->setEchoMode(QLineEdit::Password);

    connect(ui->pass_visible,&ClickedLabel::Clicked,[this]{
        auto state = ui->pass_visible->GetCurState();
        if(state == ClickLbState::Selected){
            ui->newpwd_edit->setEchoMode(QLineEdit::Normal);
        }else{
            ui->newpwd_edit->setEchoMode(QLineEdit::Password);
        }
    });

    connect(HttpMgr::GetInstance().get(),&HttpMgr::sig_reset_mod_finish,
            this,&ResetDialog::slot_reset_mod_finish);

    initHttpHandlers();


}

void ResetDialog::showtip(QString str,QString state)
{
    ui->err_tip->setText(str);
    ui->err_tip->setProperty("state",state);
    repolish(ui->err_tip); //刷新
}

ResetDialog::~ResetDialog()
{
    delete ui;
}

void ResetDialog::on_return_btn_clicked()
{
    ui->email_edit->clear();
    ui->user_edit->clear();
    ui->newpwd_edit->clear();
    ui->varifycode_edit->clear();
    emit switchLogin();
}


void ResetDialog::on_reset_btn_clicked()
{
    if(ui->newpwd_edit->text() == ""){
        showtip(tr("新密码不能为空"),"err");
        return;
    }

    if(!checkPassValid()){
        return;
    }

    QJsonObject json_obj;
    json_obj["user"] = ui->user_edit->text();
    json_obj["email"] = ui->email_edit->text();
    json_obj["passwd"] = xorString(ui->newpwd_edit->text());
    json_obj["varifycode"] = ui->varifycode_edit->text();
    HttpMgr::GetInstance()->PostHttpReq(QUrl(gate_url_prefix+"/user_resetpassword"),json_obj,
                                        ReqId::ID_RESET_PWD,Modules::RESETMOD);
}

void ResetDialog::slot_reset_mod_finish(ReqId id, QString res, ErrorCodes err)
{
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


bool ResetDialog::checkPassValid()
{
    auto pass = ui->newpwd_edit->text();

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


void ResetDialog::on_get_code_clicked()
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
                                            json_obj,ReqId::ID_GET_VARIFY_CODE,Modules::RESETMOD);
        showtip(tr("已成功发送验证码"),"normal");
    }
    else{
        showtip(tr("邮箱地址不正确"),"err");
    }
}

void ResetDialog::initHttpHandlers()
{
    //注册获取验证码回包的逻辑
    _handlers.insert(ReqId::ID_GET_VARIFY_CODE, [this](const QJsonObject& jsonObj){
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

    //重置密码回包的逻辑
    _handlers.insert(ReqId::ID_RESET_PWD,[this](const QJsonObject& jsonObj){
        int error = jsonObj["error"].toInt();

        if(error == ErrorCodes::Error_UserNotMatchEamil){
            showtip(tr("用户名和邮箱不匹配"),"err");
            return;
        }

        if(error == ErrorCodes::Error_UserNoExist){
            showtip(tr("用户名不存在"),"err");
            return;
        }

        if(error == ErrorCodes::Error_VarifyCode){
            showtip(tr("验证码错误"),"err");
            return;
        }

        if(error == ErrorCodes::Error_VarifyCodeExpired){
            showtip(tr("验证码无效"),"err");
            return;
        }

        if(error != ErrorCodes::SUCCESS){
            showtip(tr("请求失败"),"err");
            return;
        }

        showtip(tr("修改密码成功，点击返回按钮返回登录界面"),"normal");
        ui->newpwd_edit->clear();
        return;
    });

}

