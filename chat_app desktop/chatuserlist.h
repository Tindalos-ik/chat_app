#ifndef CHATUSERLIST_H
#define CHATUSERLIST_H
#include <QObject>
#include <QListWidget>
#include <QWheelEvent>
#include <QScrollEvent>
#include <QDebug>

// 自定义控件
class ChatUserList : public QListWidget
{
    Q_OBJECT
public:
    ChatUserList(QWidget* parent = nullptr);

protected:
    // 重写事件过滤器
    bool eventFilter(QObject *watched, QEvent *event)override;

signals:
    void sig_loading_chat_user();

};

#endif // CHATUSERLIST_H
