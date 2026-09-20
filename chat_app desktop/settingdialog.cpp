#include "settingdialog.h"
#include "ui_settingdialog.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QIODevice>
#include <QMessageBox>
#include "global.h"
#include <QDebug>
#include <QJsonObject>
#include <QJsonDocument>
#include <QCoreApplication>
#include <QSettings>
#include <QShowEvent>
#include "avatarutil.h"
#include "resourceclient.h"
#include "tcpmgr.h"
#include "usermgr.h"

namespace {
constexpr qint64 kUploadChunkSize = 1024;
}

SettingDialog::SettingDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::SettingDialog)
{
    ui->setupUi(this);

    auto resourceClient = ResourceClient::GetInstance();
    connect(resourceClient.get(), &ResourceClient::sig_con_success, this, [this](bool success) {
        if (!_submitting || !_waiting_resource_connection) {
            return;
        }
        _waiting_resource_connection = false;
        if (!success) {
            failSubmission(tr("连接资源服务器失败。"));
            return;
        }
        requestUploadSync(true);
    });
    connect(resourceClient.get(), &ResourceClient::sig_net_error, this,
            [this](const QString &message) {
        if (_submitting) {
            failSubmission(message);
        }
    });
    connect(resourceClient.get(), &ResourceClient::sig_upload_error, this,
            [this](const QString &message) {
        if (_submitting) {
            failSubmission(message);
        }
    });
    connect(resourceClient.get(), &ResourceClient::sig_file_sync, this,
            [this](qint64 offset, qint64 total, bool completed,
                   const QString &uploadId, const QString &resourceUrl) {
        if (!_submitting) {
            return;
        }
        if (uploadId != _upload_id || total != _file_size || offset < 0 || offset > total) {
            failSubmission(tr("资源服务器返回的上传任务信息不匹配。"));
            return;
        }
        _has_upload_task = true;
        // 断点续传可能从非零位置开始，进度必须以服务端确认值初始化。
        ui->upload_progress->setVisible(true);
        ui->upload_progress->setValue(total > 0 ? static_cast<int>(offset * 100 / total) : 100);
        if (completed) {
            finishResourceUpload(resourceUrl);
            return;
        }
        _upload_active = true;
        sendNextChunk(offset);
    });
    connect(resourceClient.get(), &ResourceClient::sig_upload_progress, this,
            [this](qint64 offset, qint64 total, bool completed, const QString &resourceUrl) {
        if (!_submitting) {
            return;
        }
        if (total != _file_size || offset < 0 || offset > total) {
            failSubmission(tr("资源服务器返回了无效的上传进度。"));
            return;
        }
        const int percent = total > 0 ? static_cast<int>(offset * 100 / total) : 100;
        ui->upload_progress->setVisible(true);
        ui->upload_progress->setValue(percent);
        ui->submit_btn->setText(tr("上传中..."));
        if (completed) {
            finishResourceUpload(resourceUrl);
        } else {
            sendNextChunk(offset);
        }
    });

    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_update_profile_result, this,
            [this](int error) {
        if (!_submitting) {
            return;
        }
        if (error != ErrorCodes::SUCCESS) {
            failSubmission(tr("保存个人资料失败，错误码：%1").arg(error));
            return;
        }

        // 只有 ChatServer 明确返回成功才提交本地状态，失败时 UI/UserMgr 保持原资料。
        UserMgr::GetInstance()->UpdateProfile(_pending_nick, _pending_desc, _pending_icon);
        ui->nickname_edit->setText(_pending_nick);
        ui->signature_edit->setPlainText(_pending_desc);
        AvatarUtil::SetRoundAvatar(ui->avatar_lb, UserMgr::GetInstance()->GetUid(), _pending_icon);
        _file_path.clear();
        _file_md5.clear();
        _upload_id.clear();
        _file_size = 0;
        _upload_active = false;
        _has_upload_task = false;
        ui->avatar_hint->setText(tr("头像与个人资料已更新"));
        setSubmitting(false);
        QMessageBox::information(this, tr("保存成功"), tr("个人资料已更新。"));
    });

    refreshFromUser();
}

SettingDialog::~SettingDialog()
{
    delete ui;
}

void SettingDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    if (!_submitting) {
        refreshFromUser();
    }
}

// 选择文件
void SettingDialog::on_select_avatar_btn_clicked()
{
    // 使用 Qt 原生文件对话框；只限图片可以上传
    const QString filePath = QFileDialog::getOpenFileName(
        this,
        tr("选择要上传的图片"),
        QDir::homePath(),          // 当前目录
        tr("图片文件 (*.png *.jpg *.jpeg)"));

    // 用户取消选择时保留界面中原有的文件信息
    if(filePath.isEmpty()){
        return;
    }

    // 切换文件后，旧文件对应的暂停任务不能继续使用。
    _upload_active = false;
    _has_upload_task = false;

    // 先更新无需读取文件内容即可获得的路径、文件名和精确字节数。
    const QFileInfo fileInfo(filePath);

    QFile file(fileInfo.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("选择头像失败"), tr("无法读取所选文件。"));
        return;
    }

    if (fileInfo.size() <= 0) {
        QMessageBox::warning(this, tr("选择头像失败"), tr("头像文件不能为空。"));
        return;
    }

    // 按 1 MB 分块计算 MD5，避免大文件被一次性读入内存。
    QCryptographicHash md5(QCryptographicHash::Md5);
    constexpr qint64 chunkSize = 1024 * 1024;
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(chunkSize);

        // 空数据可能代表读取失败；正常到达文件末尾由 atEnd() 处理。
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
            qDebug() << "读取文件失败" << Qt::endl;
            return;
        }
        md5.addData(chunk);
    }

    // MD5 以常见的 32 位小写十六进制字符串显示。
    _file_md5 = QString::fromLatin1(md5.result().toHex());
    _file_path = fileInfo.absoluteFilePath();
    _file_size = fileInfo.size();
    _upload_id = QStringLiteral("%1_%2_%3").arg(_file_md5).arg(_file_size).arg(kUploadChunkSize);
    AvatarUtil::SetRoundAvatar(ui->avatar_lb, UserMgr::GetInstance()->GetUid(), _file_path);
    ui->avatar_hint->setText(tr("已选择：%1").arg(fileInfo.fileName()));
    ui->upload_progress->setValue(0);
    ui->upload_progress->setVisible(true);
}


void SettingDialog::on_cancel_btn_clicked()
{
    if (_submitting) {
        return;
    }
    emit sig_setting_cancel();
}


void SettingDialog::on_submit_btn_clicked()
{
    if (_submitting) {
        return;
    }

    const auto userInfo = UserMgr::GetInstance()->GetUserInfo();
    if (!userInfo || userInfo->_uid <= 0) {
        QMessageBox::warning(this, tr("保存失败"), tr("当前用户信息无效，请重新登录。"));
        return;
    }
    if (!TcpMgr::GetInstance()->IsConnected()) {
        QMessageBox::warning(this, tr("保存失败"), tr("未连接聊天服务器，请重新登录后再试。"));
        return;
    }

    _pending_nick = ui->nickname_edit->text().trimmed();
    _pending_desc = ui->signature_edit->toPlainText().trimmed();
    _pending_icon = userInfo->_icon;
    if (_pending_nick.isEmpty()) {
        QMessageBox::warning(this, tr("保存失败"), tr("昵称不能为空。"));
        return;
    }
    // ChatServer 最终写入 varchar(255)；按 UTF-8 字节提前拦截，避免先上传头像
    // 再因为签名过长导致资料请求失败。
    if (_pending_nick.toUtf8().size() > 96 || _pending_desc.toUtf8().size() > 255) {
        QMessageBox::warning(this, tr("保存失败"), tr("昵称或个性签名过长。"));
        return;
    }

    setSubmitting(true, _file_path.isEmpty() ? tr("保存中...") : tr("准备上传..."));
    if (_file_path.isEmpty()) {
        ui->upload_progress->setVisible(false);
        sendProfileUpdate(_pending_icon);
        return;
    }

    auto resourceClient = ResourceClient::GetInstance();
    if (resourceClient->IsConnected()) {
        requestUploadSync(true);
        return;
    }

    const QString configPath = QDir::toNativeSeparators(
        QCoreApplication::applicationDirPath() + QDir::separator() + QStringLiteral("config.ini"));
    QSettings settings(configPath, QSettings::IniFormat);
    const QString host = settings.value(QStringLiteral("ResourceServer/host")).toString();
    const QString port = settings.value(QStringLiteral("ResourceServer/port")).toString();
    if (host.isEmpty() || port.toUShort() == 0) {
        failSubmission(tr("ResourceServer 地址未配置或端口无效。"));
        return;
    }
    _waiting_resource_connection = true;
    resourceClient->slot_tcp_connect(host, port);
}

void SettingDialog::requestUploadSync(bool start_upload_after_sync)
{
    Q_UNUSED(start_upload_after_sync);
    if (!_submitting || _file_path.isEmpty()) {
        return;
    }
    QJsonObject request;
    request["upload_id"] = _upload_id;
    request["md5"] = _file_md5;
    request["name"] = QFileInfo(_file_path).fileName();
    request["total_size"] = static_cast<double>(_file_size);
    request["chunk_size"] = static_cast<double>(kUploadChunkSize);
    ui->submit_btn->setText(tr("检查上传进度..."));
    ResourceClient::GetInstance()->sendMsg(ResourceReqId::ID_SYNC_FILE_REQ,
        QJsonDocument(request).toJson(QJsonDocument::Compact));
}

void SettingDialog::sendNextChunk(qint64 confirmed_offset)
{
    // 每次仅从服务端确认位置发送一个分片；下一个分片由成功回包驱动，避免假进度。
    QFile file(_file_path);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(confirmed_offset)) {
        failSubmission(tr("读取头像文件失败。"));
        return;
    }
    const QByteArray chunk = file.read(kUploadChunkSize);
    if (chunk.isEmpty() && confirmed_offset < _file_size) {
        failSubmission(tr("读取头像分片失败。"));
        return;
    }

    QJsonObject request;
    request["upload_id"] = _upload_id;
    request["md5"] = _file_md5;
    request["name"] = QFileInfo(_file_path).fileName();
    request["total_size"] = static_cast<double>(_file_size);
    request["offset"] = static_cast<double>(confirmed_offset);
    request["is_last"] = confirmed_offset + chunk.size() == _file_size;
    request["data"] = QString::fromLatin1(chunk.toBase64());
    ResourceClient::GetInstance()->sendMsg(ResourceReqId::ID_UPLOAD_FILE_REQ,
        QJsonDocument(request).toJson(QJsonDocument::Compact));
}

void SettingDialog::finishResourceUpload(const QString &resource_url)
{
    _upload_active = false;
    ui->upload_progress->setVisible(true);
    ui->upload_progress->setValue(100);
    const QString url = resource_url.trimmed();
    // 当前客户端约定完成回包字段名为 resource_url；缺失时不能把 upload_id 或本地路径
    // 冒充可访问地址写进用户资料。
    if (url.isEmpty()) {
        failSubmission(tr("头像已上传，但资源服务器未返回 resource_url。"));
        return;
    }
    sendProfileUpdate(url);
}

void SettingDialog::sendProfileUpdate(const QString &avatar_url)
{
    if (!TcpMgr::GetInstance()->IsConnected()) {
        failSubmission(tr("聊天服务器连接已断开，无法保存个人资料。"));
        return;
    }
    if (avatar_url.toUtf8().size() > 255) {
        failSubmission(tr("资源地址过长，无法保存到用户资料。"));
        return;
    }
    _pending_icon = avatar_url;
    QJsonObject request;
    request["uid"] = UserMgr::GetInstance()->GetUid();
    request["nick"] = _pending_nick;
    request["desc"] = _pending_desc;
    request["icon"] = _pending_icon;
    ui->submit_btn->setText(tr("保存中..."));
    emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_UPDATE_USER_PROFILE_REQ,
        QJsonDocument(request).toJson(QJsonDocument::Compact));
}

void SettingDialog::setSubmitting(bool submitting, const QString &button_text)
{
    _submitting = submitting;
    ui->submit_btn->setEnabled(!submitting);
    ui->cancel_btn->setEnabled(!submitting);
    ui->select_avatar_btn->setEnabled(!submitting);
    ui->nickname_edit->setEnabled(!submitting);
    ui->signature_edit->setEnabled(!submitting);
    ui->submit_btn->setText(submitting ? button_text : tr("提交"));
}

void SettingDialog::failSubmission(const QString &message)
{
    _upload_active = false;
    _waiting_resource_connection = false;
    setSubmitting(false);
    QMessageBox::warning(this, tr("保存失败"), message);
}

void SettingDialog::refreshFromUser()
{
    const auto userInfo = UserMgr::GetInstance()->GetUserInfo();
    if (!userInfo) {
        return;
    }
    ui->username_edit->setText(userInfo->_name);
    ui->nickname_edit->setText(userInfo->_nick);
    ui->signature_edit->setPlainText(userInfo->_desc);
    AvatarUtil::SetRoundAvatar(ui->avatar_lb, userInfo->_uid, userInfo->_icon);

}

