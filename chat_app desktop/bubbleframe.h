#ifndef BUBBLEFRAME_H
#define BUBBLEFRAME_H
#include <QFrame>
#include <QHBoxLayout>
#include <QPaintEvent>
#include "global.h"

// 这个类用于设计那个气泡的形状 ， QFrame相当于有边框的QWidget
// 消息分为 文本，图片，文件，这个类作为基类

class BubbleFrame : public QFrame
{
    Q_OBJECT
public:
    explicit BubbleFrame(ChatRole role, QWidget *parent = nullptr);
    void setMargin(int margin);
    void setWidget(QWidget *w);
private:
    void paintEvent(QPaintEvent *e); // 画气泡框的
private:
    QHBoxLayout *m_pHLayout;
    ChatRole m_role;
    int m_margin;
};

#endif // BUBBLEFRAME_H
