#include "clickedoncelabel.h"

ClickedOnceLabel::ClickedOnceLabel(QWidget *parent)
    : QLabel(parent)
{
    setCursor(Qt::PointingHandCursor); // 小手光标，提示可点击
}

void ClickedOnceLabel::mouseReleaseEvent(QMouseEvent *ev)
{
    if (ev->button() == Qt::LeftButton) {
        emit clicked(this->text()); // 左键松开：带文字发信号
        return;
    }
    QLabel::mouseReleaseEvent(ev); // 其他按键走默认处理
}
