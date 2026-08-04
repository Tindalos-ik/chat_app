#ifndef LOGINDIALOG_H
#define LOGINDIALOG_H

#include <QDialog>
#include "global.h"

namespace Ui {
class LoginDialog;
}

class LoginDialog : public QDialog
{
    Q_OBJECT

signals:
    //在注册按钮被点击的时候发送切换注册界面的信号，该信号会发送给主窗口
    void switchRegister();

    void sig_connect_tcp(const ServerInfo& si);

    void switchReset();

public:
    explicit LoginDialog(QWidget *parent = nullptr);
    ~LoginDialog();

private slots:
    void on_login_btn_clicked();

    void slot_login_mod_finish(ReqId id, QString res, ErrorCodes err);

    void slot_login_failed(int);

    void slot_tcp_con_success(bool);

private:
    void initHttpHandlers();

    Ui::LoginDialog *ui;

    void showtip(QString str,QString state);

    QMap<ReqId,std::function<void(const QJsonObject&)>> _handlers; //处理http回包逻辑

    void initHead();

    bool checkPassValid();

    void enabledBtn(bool enabled); //登录按钮不让一直点击

    int _uid;
    QString _token;
};

#endif // LOGINDIALOG_H
