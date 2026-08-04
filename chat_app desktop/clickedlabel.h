#ifndef CLICKEDLABEL_H
#define CLICKEDLABEL_H

#include <QLabel>
#include "global.h"4
#include <QEvent>
#include <QEnterEvent>
#include <QMouseEvent>

//实现一个可以点击的label，label可能没有这个鼠标点击事件，那就往上找widgets一定有
//label从此具有 是否悬浮 + 是否点击 两个状态

class ClickedLabel : public QLabel
{
    Q_OBJECT //要用到信号的槽，一定要包含这个宏
public:
    ClickedLabel(QWidget* parent = nullptr);

    virtual void mousePressEvent(QMouseEvent* ev) override; //设置选中状态和未选中状态，即睁眼和闭眼
    virtual void enterEvent(QEnterEvent* event) override;  //鼠标进入控件区域，显示高亮啥的
    virtual void leaveEvent(QEvent* event) override;

    void SetState(QString normal,QString hover,QString press,QString select,QString select_hover,QString select_press);

    ClickLbState GetCurState();

private:
    QString _normal;
    QString _normal_hover;
    QString _normal_press;

    QString _selected;
    QString _selected_hover;
    QString _selected_press;

    ClickLbState _curstate;

signals:
    void Clicked(void);


};

#endif // CLICKEDLABEL_H
