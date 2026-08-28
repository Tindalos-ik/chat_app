#ifndef AUTHENFRIEND_H
#define AUTHENFRIEND_H

#include <QDialog>

namespace Ui {
class AuthenFriend;
}

// 同意好友申请弹窗（在"新的朋友"页点"好友申请"状态的条目后弹出）
// 内容：申请者头像/名字 + 验证消息 + 备注名输入 + 同意/拒绝按钮
class AuthenFriend : public QDialog
{
    Q_OBJECT

public:
    explicit AuthenFriend(QWidget *parent = nullptr);
    ~AuthenFriend();

    // 设置申请者信息：名字、头像、验证消息
    void SetApplyInfo(const QString &name, const QString &icon, const QString &msg);

signals:
    // 点击"同意"后发出，携带申请者名字（页面据此把条目状态改成"已添加"）
    void sig_auth_agreed(const QString &name);

private slots:
    void on_sure_btn_clicked();   // 同意
    void on_cancel_btn_clicked(); // 拒绝

private:
    Ui::AuthenFriend *ui;
    QString _name; // 申请者名字
    QString _icon; // 申请者头像
};

#endif // AUTHENFRIEND_H
