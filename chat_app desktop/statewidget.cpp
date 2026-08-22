#include "statewidget.h"
#include <QPainter>
#include <QStyle>
#include <QStyleOption>

StateWidget::StateWidget(QWidget *parent):
    QWidget(parent), _curstate(ClickLbState::Normal)
{
    setCursor(Qt::PointingHandCursor); // 设置光标类型为小手手
    // 添加红点
    AddRedPoint();
}

void StateWidget::AddRedPoint()
{
    _red_point = new QLabel(this);
    _red_point->setObjectName("red_point");
    _red_point->setFixedSize(18, 18);
    _red_point->setVisible(false); // 默认不显示，需要时 ShowRedPoint
    _red_point->move(width() - _red_point->width() - 2, 2); // 先放到右上角，resize 时再校正
}

void StateWidget::SetState(QString normal, QString hover, QString press,
                           QString select, QString select_hover, QString select_press)
{
    _normal = normal;
    _normal_hover = hover;
    _normal_press = press;

    _selected = select;
    _selected_hover = select_hover;
    _selected_press = select_press;

    setState(_normal);
}

ClickLbState StateWidget::GetCurState()
{
    return _curstate;
}

void StateWidget::ClearState()
{
    _curstate = ClickLbState::Normal;
    setState(_normal);
}

void StateWidget::SetSelected(bool bselected)
{
    if (bselected) {
        _curstate = ClickLbState::Selected;
        setState(_selected);
        return;
    }

    ClearState();
}

void StateWidget::setState(const QString &stateName)
{
    setProperty("state", stateName);
    repolish(this); // 刷新样式表，让 QSS 属性选择器生效
    update();       // 强制重绘
}

void StateWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    // 自定义 QWidget 必须自己走一遍样式绘制，QSS 的 border-image 才会生效
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

void StateWidget::mousePressEvent(QMouseEvent *ev)
{
    if (ev->button() == Qt::LeftButton) {
        if (_curstate == ClickLbState::Normal) {
            // 未选中时按下：立刻进入选中态，显示"选中+按下"样式
            _curstate = ClickLbState::Selected;
            setState(_selected_press);
        } else {
            // 已选中的按钮再按：保持选中，显示"选中+按下"
            setState(_selected_press);
        }
        return;
    }
    // 其他按键走默认处理
    QWidget::mousePressEvent(ev);
}

void StateWidget::mouseReleaseEvent(QMouseEvent *ev)
{
    if (ev->button() == Qt::LeftButton) {
        // 松开：显示"选中+悬停"，并通知外部切换页面
        setState(_selected_hover);
        emit clicked();
        return;
    }
    QWidget::mouseReleaseEvent(ev);
}

void StateWidget::enterEvent(QEnterEvent *event)
{
    QWidget::enterEvent(event);
    if (_curstate == ClickLbState::Normal) {
        setState(_normal_hover);
    } else {
        setState(_selected_hover);
    }
}

void StateWidget::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    if (_curstate == ClickLbState::Normal) {
        setState(_normal);
    } else {
        setState(_selected);
    }
}

void StateWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (_red_point) {
        _red_point->move(width() - _red_point->width() - 2, 2); // 钉在右上角
    }
}

void StateWidget::ShowRedPoint(bool show)
{
    if (_red_point) {
        _red_point->setVisible(show);
    }
}
