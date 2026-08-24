#ifndef CLICKEDONCELABEL_H
#define CLICKEDONCELABEL_H

#include <QLabel>
#include <QMouseEvent>

// 一次性点击标签：鼠标左键松开时发 clicked(text) 信号
// 和 ClickedLabel 的区别：ClickedLabel 带 normal/selected 状态切换；
// 这个只负责"点一下发信号"，用于 ApplyFriend 的提示标签 / 更多按钮
class ClickedOnceLabel : public QLabel
{
    Q_OBJECT

public:
    explicit ClickedOnceLabel(QWidget *parent = nullptr);

protected:
    void mouseReleaseEvent(QMouseEvent *ev) override;

signals:
    void clicked(const QString &text); // 携带标签文字
};

#endif // CLICKEDONCELABEL_H
