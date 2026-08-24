#include "clickedlabel.h"


ClickedLabel::ClickedLabel(QWidget *parent) : QLabel (parent),_curstate(ClickLbState::Normal)
{
    this->setCursor(Qt::PointingHandCursor);
}

void ClickedLabel::mousePressEvent(QMouseEvent *ev)
{
    if(ev->button() == Qt::LeftButton){
        if(_curstate == ClickLbState::Normal){
            //鼠标左键点击
            qDebug() << "clicked,change to selected hover:" << _selected_hover;
            _curstate = ClickLbState::Selected;
            setProperty("state",_selected_hover);
            repolish(this); //刷新样式表
            update(); //更新一下，强制刷新
        }else{
            qDebug() << "clicked,change to normal hover" << _normal_hover;
            _curstate = ClickLbState::Normal;
            setProperty("state",_normal_hover);
            repolish(this);
            update();
        }
        emit Clicked(); //发送点击信号
    }
    //调用基类的事件保证正常事件处理
    QLabel::mousePressEvent(ev);
}

void ClickedLabel::enterEvent(QEnterEvent *event)
{
    if(_curstate == ClickLbState::Normal){
        qDebug() << "enter,change to normal hover:" << _normal_hover;
        setProperty("state",_normal_hover);
        repolish(this);
        update();
    }else{
        qDebug() << "enter,change to selected hover:" << _selected_hover;
        setProperty("state",_selected_hover);
        repolish(this);
        update();
    }
}

void ClickedLabel::leaveEvent(QEvent *event)
{
    if(_curstate == ClickLbState::Normal){
        qDebug() << "enter,change to normal:" << _normal;
        setProperty("state",_normal);
        repolish(this);
        update();
    }else{
        qDebug() << "enter,change to selected:" << _selected;
        setProperty("state",_selected);
        repolish(this);
        update();
    }
}

void ClickedLabel::SetState(QString normal, QString hover, QString press, QString select, QString select_hover, QString select_press)
{
    _normal = normal;
    _normal_hover = hover;
    _normal_press = press;

    _selected = select;
    _selected_hover = select_hover;
    _selected_press = select_press;

    setProperty("state",normal);
    repolish(this);
}

ClickLbState ClickedLabel::GetCurState()
{
    return _curstate;
}

bool ClickedLabel::SetCurState(ClickLbState state)
{
    _curstate = state;
    if (_curstate == ClickLbState::Normal) {
        setProperty("state", _normal);
    } else {
        setProperty("state", _selected);
    }
    repolish(this); // 刷新样式表，让 [state=...] 选择器重新生效
    update();
    return true;
}

void ClickedLabel::ResetNormalState()
{
    _curstate = ClickLbState::Normal;
    setProperty("state", _normal);
    repolish(this);
    update();
}
