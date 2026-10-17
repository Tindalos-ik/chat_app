#ifndef SEARCHLIST_H
#define SEARCHLIST_H

#include <QListWidget>
#include <QPointer>
#include <memory>
#include "userdata.h"

class QLineEdit;
class QLabel;
class QPushButton;
class GroupAvatarTransfer;
class QJsonObject;

// 搜索面板只负责用户查找、群号搜索及内嵌创建/申请表单，不修改群权限或缓存。
// SetSearchEdit 绑定共享输入框；ResetSearch 在导航、关闭和账号刷新时撤销所有界面关联。
// 群请求按 requestId、账号及登录代次匹配；仅显式成功操作通过 sig_enter_group 请求导航。
class SearchList : public QListWidget
{
    Q_OBJECT
public:
    explicit SearchList(QWidget *parent = nullptr);
    void SetSearchEdit(QWidget *edit);
    void SetAvatarTransfer(GroupAvatarTransfer *transfer);
    void ResetSearch();
signals:
    // 仅传值类型会话 ID，关闭搜索时不借用结果卡片中的群对象。
    void sig_enter_group(qint64 threadId);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    enum class Mode { None, User, Group, Create };
    void BuildUi();
    void Activate(Mode mode);
    void SearchGroup();
    void Submit();
    void UpdateCard();
    void SetStatus(const QString &text, bool error = false);
    void ResizeCard();
    bool IsCurrentSession() const;
    void CaptureSession();
    void HandleSearch(const QString &requestId, int error, const GroupInfo &group);
    void HandleMutation(const QString &requestId, int requestType, int error,
                        const QJsonObject &response);
    void ShowUserResult(std::shared_ptr<SearchInfo> &info);

    QPointer<QLineEdit> _searchEdit;
    QPointer<GroupAvatarTransfer> _avatarTransfer;
    QWidget *_card = nullptr;
    QListWidgetItem *_cardItem = nullptr;
    QLabel *_heading = nullptr;
    QLabel *_avatar = nullptr;
    QLabel *_name = nullptr;
    QLabel *_number = nullptr;
    QLabel *_status = nullptr;
    QLineEdit *_formEdit = nullptr;
    QPushButton *_submit = nullptr;
    Mode _mode = Mode::None;
    GroupInfo _group;
    GroupInfo _confirmedGroup;
    QString _requestId;
    int _requestType = 0;
    int _accountUid = 0;
    quint64 _loginGeneration = 0;
    qint64 _searchedThread = 0;
    bool _applicationPending = false;
    bool _userWirePending = false;
    quint64 _userWireGeneration = 0;
    bool _userResultWanted = false;
};
#endif // SEARCHLIST_H
