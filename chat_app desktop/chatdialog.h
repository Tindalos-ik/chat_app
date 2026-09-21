#ifndef CHATDIALOG_H
#define CHATDIALOG_H

#include <QDialog>
#include <QHash>
#include <QListWidget>
#include <QPointer>
#include <QVector>
#include "statewidget.h"
#include "userdata.h"

namespace Ui {
class ChatDialog;
}

class SettingDialog;
class ChatItemBase;
struct LocalChatMessage;

class ChatDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ChatDialog(QWidget *parent = nullptr);
    ~ChatDialog();

    // 登录完成后刷新当前账号显示；ChatDialog 本身早于登录界面创建。
    void UpdateUserTitle();
    void RefreshLoginData();
    void initChatUserList();
    void initConUserList();

    void handleGlobalMousePress(QMouseEvent* mouseEvent);

private slots:
    void slot_loading_chat_user();   // 聊天列表滚到底部时加载更多
    void slot_loading_con_user();
    void slot_send_message(); // 发送消息
    void slot_side_chat();      // 侧边栏：聊天
    void slot_side_contact();   // 侧边栏：联系人
    void slot_side_setting();   // 侧边栏：打开个人设置界面
    void slot_hide_setting();   // 关闭个人设置界面
    void slot_apply_friend(std::shared_ptr<AddFriendApply>& apply_info); // 添加好友申请
    void slot_auth_friend(std::shared_ptr<FriendAuthResult>& authResult); // 好友列表增加好友并保存附加消息
    void slot_create_private_chat(int uid, int otherUid, qint64 threadId); // 1028 回包建立正式本地会话
    void slot_text_chat(std::shared_ptr<TextChatData>& message); // 显示当前会话收到的文本
    void slot_local_chat_synced(qint64 threadId); // SQLite 增量同步完成后刷新会话摘要
    void slot_load_older_local_messages(); // 聊天窗口到顶部后读取 SQLite 上一页
    void slot_text_chat_send_result(const QString &messageId, bool success);

protected:
    // 重写事件过滤器实现根据鼠标位置判断是否隐藏搜索框恢复聊天界面
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    Ui::ChatDialog *ui;
    int _cur_mode = 0;   // 0=聊天列表 1=好友列表，搜索清空后回到当前模式
    QVector<StateWidget*> _lb_list;  // 侧边栏按钮组，保证一次只高亮一个
    SettingDialog *_setting_page = nullptr;

    bool _b_loading = false;       // 防抖标志：加载期间忽略重复触发
    int _loaded_chat_count = 0;   // 已加载的聊天会话条数（示例数据计数）
    int _loaded_con_count = 0;

    void addChatUserWid(QListWidget *list, const std::shared_ptr<UserInfo> &userInfo,
                        const QString &msg, const QString &time, bool red,
                        qint64 threadId = 0); // 添加聊天用户
    void addConUserWid(QListWidget *list, int uid, const QString &name, const QString &icon); //添加好友

    void AddLBGroup(StateWidget *lb);            // 把侧边栏按钮加入互斥组
    void ClearLabelState(StateWidget *lb);       // 清除除 lb 之外所有按钮的选中态
    void SetCurrentChatUser(const std::shared_ptr<UserInfo> &chatUser, qint64 threadId = 0);
    void AppendReceivedTextMessage(const std::shared_ptr<TextChatData> &message,
                                   const std::shared_ptr<UserInfo> &sender);
    // 把 SQLite 读取出的已确认消息渲染为气泡；调用方已保证消息属于当前会话。
    void AppendStoredTextMessage(const LocalChatMessage &message,
                                 const std::shared_ptr<UserInfo> &friendInfo);
    ChatItemBase *CreateStoredTextChatItem(const LocalChatMessage &message,
                                           const std::shared_ptr<UserInfo> &friendInfo);
    void LoadOlderLocalMessages();
    void UpdateChatSessionPreview(const std::shared_ptr<UserInfo> &userInfo,
                                  const QString &message, bool unread);
    void SaveFriendAuthMessages(const std::shared_ptr<UserInfo> &friendInfo,
                                const QList<std::shared_ptr<TextChatData>> &messages);

    std::shared_ptr<UserInfo> _current_chatuser;
    qint64 _current_thread_id = 0;
    qint64 _oldest_local_message_id = 0;
    bool _has_more_local_history = false;
    bool _loading_older_local_history = false;
    // UUID -> 乐观展示的消息行。QPointer 会在切换会话或重绘删除气泡后自动置空。
    QHash<QString, QPointer<ChatItemBase>> _pending_text_items;
    QHash<int, QVector<std::shared_ptr<TextChatData>>> _unread_text_messages;

signals:
    void sig_append_send_chat_msg(std::shared_ptr<TextChatData>&);
};

#endif // CHATDIALOG_H
