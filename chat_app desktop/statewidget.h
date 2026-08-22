#ifndef STATEWIDGET_H
#define STATEWIDGET_H
#include <QWidget>
#include <QLabel>
#include "global.h"
#include <QEvent>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QPaintEvent>

// 侧边栏按钮功能
// 多状态按钮：normal / hover / press / select / select_hover / select_press
// 外观由 stylesheet.qss 的属性选择器（StateWidget#xxx[state='...']）控制

class StateWidget : public QWidget
{
    Q_OBJECT
public:
    explicit StateWidget(QWidget *parent = nullptr);

    void SetState(QString normal="", QString hover="", QString press="",
                  QString select="", QString select_hover="", QString select_press="");

    ClickLbState GetCurState();
    void ClearState();

    void SetSelected(bool bselected);
    void AddRedPoint();
    void ShowRedPoint(bool show=true);

protected:
    void paintEvent(QPaintEvent* event) override;

    virtual void mousePressEvent(QMouseEvent *ev) override;
    virtual void mouseReleaseEvent(QMouseEvent *ev) override;
    virtual void enterEvent(QEnterEvent* event) override; // Qt6 的 enterEvent 参数是 QEnterEvent*
    virtual void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent *event) override;       // 让红点始终钉在右上角

private:
    void setState(const QString &stateName); // 设置 state 属性并刷新样式

    QString _normal;
    QString _normal_hover;
    QString _normal_press;

    QString _selected;
    QString _selected_hover;
    QString _selected_press;

    ClickLbState _curstate;
    QLabel * _red_point;

signals:
    void clicked(void);
};

#endif // STATEWIDGET_H
