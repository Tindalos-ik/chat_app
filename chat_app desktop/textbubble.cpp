#include "textbubble.h"
#include <QTextBlock>
#include <QTextDocument>
#include <QFontMetricsF>

TextBubble::TextBubble(ChatRole role, const QString &text, BubbleFrame *parent)
    :BubbleFrame(role, parent)
{
    m_pTextEdit = new QTextEdit();
    m_pTextEdit->setReadOnly(true);
    m_pTextEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_pTextEdit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_pTextEdit->installEventFilter(this);
    QFont font("Microsoft YaHei");
    font.setPointSize(12);
    m_pTextEdit->setFont(font);
    setPlainText(text);
    setWidget(m_pTextEdit);
    initStyleSheet();
}

bool TextBubble::eventFilter(QObject *watched, QEvent *event)
{
    if(m_pTextEdit == watched && event->type() == QEvent::Paint){
        adjustTextHeight();
    }
    return BubbleFrame::eventFilter(watched, event);
}

void TextBubble::adjustTextHeight()
{
    qreal doc_margin = m_pTextEdit->document()->documentMargin(); // 字体到边框的距离默认为4
    QTextDocument *doc = m_pTextEdit->document();
    qreal text_height = 0;
    // 把每一段的高度相加 = 文本高
    for(QTextBlock it = doc->begin(); it != doc->end(); it = it.next()){
        QTextLayout *pLayout = it.layout();
        QRectF text_rect = pLayout->boundingRect();
        text_height += text_rect.height();
    }
    int vMargin = this->layout()->contentsMargins().top();
    // 设置气泡高度 文本高 + 文本边界 + TextEdit 边框到气泡边框的距离
    setFixedHeight(text_height + doc_margin*2 + vMargin*2);
}

void TextBubble::setPlainText(const QString &text)
{
    m_pTextEdit->setPlainText(text);
    // 找到段落中最大高度
    qreal doc_margin = m_pTextEdit->document()->documentMargin();
    int margin_left = this->layout()->contentsMargins().left();
    int margin_right = this->layout()->contentsMargins().right();
    QFontMetricsF fm(m_pTextEdit->font());
    QTextDocument *doc = m_pTextEdit->document();
    int max_width = 0;
    // 遍历每一段找到最宽的那一段
    for(QTextBlock it = doc->begin(); it != doc->end() ; it = it.next()){
        int texW = int(fm.horizontalAdvance(it.text()));
        max_width = max_width < texW ? texW : max_width;
    }
    // 设置气泡最大宽度；+12 是余量：QTextEdit 的 frame/边距会让实际可用宽度比算出来的略小，
    // 不加余量会出现短单词贴边折行（"hello"在 hell 处换行）
    setMaximumWidth(max_width + doc_margin*2 + (margin_left + margin_right) + 12);
}

void TextBubble::initStyleSheet()
{
    m_pTextEdit->setStyleSheet("QTextEdit{background:transparent;border:none}");
}


