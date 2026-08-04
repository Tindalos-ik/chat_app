#ifndef REGISTERDIALOG_H
#define REGISTERDIALOG_H

#include <QDialog>
#include <QString>
#include "global.h"


/*客户端如何增加post逻辑，打通前后端，实现获取验证码的逻辑？
 * 在点击获取槽函数里添加发送http的post请求即可 */

namespace Ui {
class RegisterDialog;
}

class RegisterDialog : public QDialog
{
    Q_OBJECT

signals:
    void switchLogin();

public:
    explicit RegisterDialog(QWidget *parent = nullptr);
    ~RegisterDialog();

private slots:
    void on_get_code_clicked();

    //接收到httpmgr发送的注册完成后的信号执行的槽函数，参数要和信号匹配
    void slot_reg_mod_finish(ReqId id, QString res, ErrorCodes err);

    void on_sure_btn_clicked();

    void on_cancel_btn_clicked();

private:
    void initHttpHandlers();

    Ui::RegisterDialog *ui;

    void showtip(QString str,QString state);

    QMap<ReqId,std::function<void(const QJsonObject&)>> _handlers;

    bool checkPassValid();
};

#endif // REGISTERDIALOG_H
