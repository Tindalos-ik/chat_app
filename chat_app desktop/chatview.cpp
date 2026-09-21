#include "chatview.h"
#include <QStyleOption>
#include <QPainter>
#include <QScrollBar>
#include <QEvent>

// 构造函数：搭出"外层容器 → 滚动区域 → 内容纸 → 垂直布局 + 底部弹簧"的骨架
ChatView::ChatView(QWidget *parent): QWidget(parent),isAppended(false)
{
    // 1. 外层垂直布局：ChatView 自己只放一个 QScrollArea
    QVBoxLayout* pMainLayout = new QVBoxLayout();
    this->setLayout(pMainLayout);
    // 边距全归零（左、上、右、下），让滚动区域占满整个 ChatView
    pMainLayout->setContentsMargins(0, 0, 0, 0);

    // 2. 滚动区域：负责"内容超高时滚动"，聊天记录都装在里面
    m_pScrollArea = new QScrollArea();
    m_pScrollArea->setObjectName("chat_area"); // 起名字，方便 QSS 用 #chat_area 选择器设样式
    pMainLayout->addWidget(m_pScrollArea);

    // 3. 内容 widget：一张"纸"，气泡按顺序排在这张纸上
    QWidget *w = new QWidget(this);
    w->setObjectName("chat_bg");
    w->setAutoFillBackground(true); // 让 QSS 背景色能真正画出来

    // 4. 内容布局 + "弹簧"
    //    addWidget(new QWidget(), 100000) 先加一个空 widget（sizeHint 0×0，拉伸因子 100000）：
    //    - 它本身不占空间，但会吞掉布局里所有"剩余空间"；
    //    - 弹簧虽然先加进来，但气泡总是 insertWidget(count()-1) 插在它前面，
    //      所以它永远被顶到最底下，气泡保持自然高度挤在顶部；
    //    - 对气泡误用 addWidget() 会插到弹簧后面（底部死区），气泡看不见。
    QVBoxLayout *pHLayout_1 = new QVBoxLayout();
    pHLayout_1->setContentsMargins(0, 0, 0, 0); // 气泡贴满内容区，去掉样式默认边距
    pHLayout_1->addWidget(new QWidget(), 100000);
    w->setLayout(pHLayout_1);
    m_pScrollArea->setWidget(w); // 把"纸"装进滚动区域
    m_pScrollArea->setWidgetResizable(true); // "纸"跟随视口拉伸：气泡撑满宽度、弹簧才吃得到剩余空间

    // 5. 滚动条：默认隐藏，鼠标悬浮时由 eventFilter 显示
    m_pScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff); // 默认不显示
    QScrollBar *pVScrollBar = m_pScrollArea->verticalScrollBar();
    connect(pVScrollBar, &QScrollBar::rangeChanged, this, &ChatView::onVScrollBarMoved); // 尾插后自动滚到底
    connect(pVScrollBar, &QScrollBar::valueChanged, this, [this, pVScrollBar](int value) {
        // rangeChanged 时滚动条值有可能暂时仍为 0；尾插和头插期间都不是用户的
        // “翻到最顶”操作，必须忽略，防止首次渲染或批量插入递归触发加载。
        if (!isAppended && !_isPrepending && pVScrollBar->maximum() > pVScrollBar->minimum()
            && value <= pVScrollBar->minimum()) {
            emit sig_reach_top();
        }
    });

    // 把垂直滚动条从"并排"改成"叠在右侧"：悬浮出现时不会挤占消息宽度
    QHBoxLayout *pHLayout_2 = new QHBoxLayout();
    pHLayout_2->addWidget(pVScrollBar, 0, Qt::AlignRight); // 伸缩因子 0 = 固定大小，靠右
    pHLayout_2->setContentsMargins(0, 0, 0, 0);
    m_pScrollArea->setLayout(pHLayout_2);    // 用这个布局重新摆放滚动条
    pVScrollBar->setHidden(true);            // 初始隐藏
    // 事件过滤器必须装到 viewport 上：鼠标悬浮时 Enter/Leave 是发给 viewport 的，
    // 装到 m_pScrollArea 自己身上永远收不到（这就是之前滚动条不显示的原因）
    m_pScrollArea->viewport()->installEventFilter(this);

    initStyleSheet(); // 滚动条/背景的 QSS 初始化
}

void ChatView::appendChatItem(QWidget *item)
{
    // 空指针保护：内容纸或内容布局不存在时直接返回，避免崩溃
    QWidget *content = m_pScrollArea ? m_pScrollArea->widget() : nullptr;
    if (content == nullptr) {
        return;
    }
    QVBoxLayout *v1 = qobject_cast<QVBoxLayout *>(content->layout());
    if (v1 == nullptr) {
        return;
    }
    v1->insertWidget(v1->count()-1, item); // 插到弹簧前面；注意不能 addWidget(item)
    isAppended = true; // 加入内容 会触发 rangeChanged，滚动条向下滑一下
}

void ChatView::ClearChatItems()
{
    QWidget *content = m_pScrollArea ? m_pScrollArea->widget() : nullptr;
    auto *layout = content ? qobject_cast<QVBoxLayout *>(content->layout()) : nullptr;
    if (layout == nullptr) {
        return;
    }

    // 最后一项是构造函数创建的弹簧，负责把消息顶到顶部，绝不能一起删除。
    while (layout->count() > 1) {
        QLayoutItem *item = layout->takeAt(0);
        if (item == nullptr) {
            continue;
        }
        delete item->widget();
        delete item;
    }
    isAppended = false;
}

void ChatView::prependChatItem(QWidget *item)
{
    if (item != nullptr) {
        prependChatItems({item});
    }
}

void ChatView::prependChatItems(const QList<QWidget *> &items)
{
    QWidget *content = m_pScrollArea ? m_pScrollArea->widget() : nullptr;
    auto *layout = content ? qobject_cast<QVBoxLayout *>(content->layout()) : nullptr;
    if (layout == nullptr || items.isEmpty()) {
        return;
    }

    QScrollBar *scrollBar = m_pScrollArea->verticalScrollBar();
    const int oldValue = scrollBar->value();
    const int oldMaximum = scrollBar->maximum();
    _isPrepending = true;

    // items 按时间正序传入。头插时要从末尾倒着插，最终页面仍是“旧 -> 新”。
    for (auto iter = items.crbegin(); iter != items.crend(); ++iter) {
        if (*iter != nullptr) {
            layout->insertWidget(0, *iter);
        }
    }
    layout->activate();

    // 布局和滚动范围会在本事件循环末尾才完全更新。以最大值的增量补偿当前位置，
    // 使用户原来正在看的那条消息保持在同一屏幕位置，而不是被新历史顶走。
    QTimer::singleShot(0, this, [this, oldValue, oldMaximum] {
        if (!m_pScrollArea) {
            return;
        }
        QScrollBar *scrollBar = m_pScrollArea->verticalScrollBar();
        const int insertedHeight = qMax(0, scrollBar->maximum() - oldMaximum);
        scrollBar->setValue(qMin(scrollBar->maximum(), oldValue + insertedHeight));
        _isPrepending = false;
    });
}

void ChatView::insertChatItem(QWidget *before, QWidget *item)
{
    // 实现思路 = 先取 before 在布局里的 index，再 insertWidget(index, item)
}

bool ChatView::eventFilter(QObject *watched, QEvent *event)
{
    if(event->type() == QEvent::Enter && watched == m_pScrollArea->viewport()){
        // 可以滚动就显示滚动条
        m_pScrollArea->verticalScrollBar()->setHidden(m_pScrollArea->verticalScrollBar()->maximum() == 0);
    }
    else if(event->type() == QEvent::Leave && watched == m_pScrollArea->viewport()){
        m_pScrollArea->verticalScrollBar()->setHidden(true);
    }
    return QWidget::eventFilter(watched, event);
}

// 公式化
void ChatView::paintEvent(QPaintEvent *event)
{
    QStyleOption opt;
    opt.initFrom(this); // 把当前控件状态（enable/hover/背景等）填进 opt
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this); // 用 QStyle 画这个 widget
}

void ChatView::initStyleSheet()
{

}

// 滚动条范围变化时触发：尾插新消息后自动滚到底部
void ChatView::onVScrollBarMoved(int min, int max)
{
    if(isAppended && !_isPrepending){ // 头插历史时绝不能沿用尾插的自动滚底标志
        QScrollBar *pVScrollBar = m_pScrollArea->verticalScrollBar();
        pVScrollBar->setSliderPosition(pVScrollBar->maximum()); // 滑到最底
        // 500ms 内 rangeChanged 可能多次触发，500ms 后重置标志
        QTimer::singleShot(500, [this]{
            isAppended = false;
        });
    }
}
