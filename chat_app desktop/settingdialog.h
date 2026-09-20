#ifndef SETTINGDIALOG_H
#define SETTINGDIALOG_H

#include <QDialog>

class QShowEvent;

namespace Ui {
class SettingDialog;
}

class SettingDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingDialog(QWidget *parent = nullptr);
    ~SettingDialog();

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void on_select_avatar_btn_clicked();

    void on_cancel_btn_clicked();

    void on_submit_btn_clicked();

private:
    // 先同步服务端进度，再从服务端确认的 offset 继续读取本地文件。
    void requestUploadSync(bool start_upload_after_sync);
    // 一次只发送一个分片。收到服务端确认回包后才发送下一片，断线时不会积压整文件。
    void sendNextChunk(qint64 confirmed_offset);
    // 资源上传完成后才把服务端资源地址交给 ChatServer，绝不发送本地文件路径。
    void sendProfileUpdate(const QString &avatar_url);
    void finishResourceUpload(const QString &resource_url);
    void setSubmitting(bool submitting, const QString &button_text = {});
    void failSubmission(const QString &message);
    void refreshFromUser();

    Ui::SettingDialog *ui;
    QString _file_path; // 文件完整路径
    QString _file_md5; // 用于断点续传校验
    QString _upload_id; // md5_文件大小_分片大小，重连后保持稳定
    qint64 _file_size = 0;
    bool _upload_active = false;
    bool _has_upload_task = false; // 已成功发起过任务，暂停后才允许点击“继续上传”
    // 提交期间锁住表单，保证 ResourceServer 回包不会串到另一份用户输入上。
    bool _submitting = false;
    bool _waiting_resource_connection = false;
    QString _pending_nick;
    QString _pending_desc;
    QString _pending_icon;

signals:
    void sig_tcp_connect(QString, QString);
    void sig_setting_cancel();
};

#endif // SETTINGDIALOG_H
