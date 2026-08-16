#include "chatuserlist.h"
#include <QScrollBar>

ChatUserList::ChatUserList(QWidget *parent):QListWidget(parent)
{
    Q_UNUSED(parent);
    //关闭横向和纵向的滚动条
    this->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    //安装事件过滤器，用于自定义一些没有的功能
    this->viewport()->installEventFilter(this);

}

bool ChatUserList::eventFilter(QObject *watched, QEvent *event)
{
    // 检查事件是否是鼠标悬浮进入或离开
    if(watched == this->viewport()){
        if(event->type() == QEvent::Enter){
            // 鼠标悬浮，显示滚动条
            this->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        }else{
            this->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        }
    }

    // 检查事件是否由鼠标滚轮事件
    if(watched == this->viewport() && event->type() == QEvent::Wheel){
        QWheelEvent *wheelevent = static_cast<QWheelEvent*>(event);
        int numDegrees = wheelevent->angleDelta().y() / 8;
        int numSteps = numDegrees / 15; //计算滚轮步数

        // 设置滚动幅度
        this->verticalScrollBar()->setValue(this->verticalScrollBar()->value() - numSteps);

        //检查是否滚动到底部
        QScrollBar *scrollbar = this->verticalScrollBar();
        int maxScrollValue = scrollbar->maximum();
        int currentValue = scrollbar->value();

        if(maxScrollValue - currentValue <= 0){
            // 滚动到底部，加载新的联系人
            qDebug() << "load more chat user";
            // 发送信号通知聊天界面加载更多聊天内容
            emit sig_loading_chat_user();
        }

        return true; //停止事件传递
    }

    return QListWidget::eventFilter(watched, event);
}







