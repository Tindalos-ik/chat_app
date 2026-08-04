#include "timerbtn.h"
#include <QMouseEvent>
#include <QDebug>

//将parent传递给基类
TimerBtn::TimerBtn(QWidget * parent):QPushButton(parent),_counter(10) {
    _timer = new QTimer(this);

    //定时器倒计时结束之后，就恢复可用
    connect(_timer,&QTimer::timeout,[this]{
        _counter--;
        if(_counter <= 0){
            _timer->stop();
            _counter = 10;
            this->setText("获取");
            this->setEnabled(true);
            return;
        }
        this->setText(QString::number(_counter));
    });
}

TimerBtn::~TimerBtn()
{
    _timer->stop(); //先调用自己的再去调用成员的，所以提前关闭一下更安全
}


void TimerBtn::mouseReleaseEvent(QMouseEvent *e)
{
    if(e->button() == Qt::LeftButton) {
        //处理鼠标左键释放事件
        qDebug() << "mybutton was released!" << Qt::endl;
        this->setEnabled(false);
        this->setText(QString::number(_counter));
        _timer->start(1000); //一秒为一次间隔，也就是总共倒计时10s
        emit clicked(); //发送点击信号
    }

    //调用基类的mouseReleaseEvent确保正常的事件处理，如点击效果
    QPushButton::mouseReleaseEvent(e);
}
