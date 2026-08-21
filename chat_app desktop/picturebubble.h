#ifndef PICTUREBUBBLE_H
#define PICTUREBUBBLE_H
#include "bubbleframe.h"
#include <QHBoxLayout>
#include <QPixmap>

// 图片 只需要实现构造就行了

class PictureBubble : public BubbleFrame
{
public:
    PictureBubble(ChatRole role, const QPixmap& picture, BubbleFrame *parent = nullptr);
};

#endif // PICTUREBUBBLE_H
