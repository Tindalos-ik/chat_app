#ifndef TIMERBTN_H
#define TIMERBTN_H

#include <QPushButton>
#include <QTimer>

//定时按钮，按钮上显示倒计时，倒计时结束后才可以继续点击
//给注册界面的获取验证码弄这个功能，写完之后去ui，选择按钮->右键类->提升为

class TimerBtn : public QPushButton
{
    Q_OBJECT //这个宏必须添加
public:
    TimerBtn(QWidget* parent = nullptr); //构造函数要和基类参数一致，去源代码看就行

    ~TimerBtn();

    //处理鼠标释放事件
    void mouseReleaseEvent(QMouseEvent* e) override; //override显式声明重写基类虚函数

private:
    QTimer* _timer; // 计时器
    int _counter; //倒计时时间
};

#endif // TIMERBTN_H
