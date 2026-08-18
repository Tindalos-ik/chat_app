#include "chatuserlist.h"
#include <QScrollBar>

ChatUserList::ChatUserList(QWidget *parent):QListWidget(parent)
{
    Q_UNUSED(parent);
    // 默认隐藏滚动条：鼠标悬浮到列表区域时再显示
    this->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    //安装事件过滤器，用于自定义一些没有的功能
    this->viewport()->installEventFilter(this);

}

bool ChatUserList::eventFilter(QObject *watched, QEvent *event)
{
    // 鼠标进入/离开列表可视区：悬浮时显示滚动条，移开时隐藏
    // watched 就代表 list 的范围
    if (watched == this->viewport()) {
        if (event->type() == QEvent::Enter) {
            // 鼠标悬浮，显示滚动条（内容超出可视区时出现）
            this->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        } else if (event->type() == QEvent::Leave) {
            // 鼠标离开，隐藏滚动条
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

        // 有滚动余地且到达/接近底部时（留 10 像素余量）才触发加载更多
        if(maxScrollValue > 0 && maxScrollValue - currentValue <= 5){
            // 滚动到底部，加载新的联系人
            qDebug() << "load more chat user";
            // 发送信号通知聊天界面加载更多聊天内容
            emit sig_loading_chat_user();
        }

        return true; //停止事件传递
    }

    return QListWidget::eventFilter(watched, event);
}







