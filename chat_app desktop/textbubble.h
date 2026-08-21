#ifndef TEXTBUBBLE_H
#define TEXTBUBBLE_H
#include <QTextEdit>
#include <QHBoxLayout>
#include "bubbleframe.h"

// 文本气泡

class TextBubble : public BubbleFrame
{
    Q_OBJECT
public:
    explicit TextBubble(ChatRole role, const QString &text, BubbleFrame* parent = nullptr);
protected:
    bool eventFilter(QObject* watched, QEvent* event);
private:
    // 调整高度
    void adjustTextHeight();
    void setPlainText(const QString &text);
    void initStyleSheet();
private:
    QTextEdit *m_pTextEdit;
};

#endif // TEXTBUBBLE_H
