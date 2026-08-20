#ifndef CHATVIEW_H
#define CHATVIEW_H
#include <QScrollArea>
#include <QVBoxLayout>
#include <QTimer>
#include <QWheelEvent>
#include <QDebug>

// 手写聊天布局：在 .ui 里把普通 QWidget 提升成 ChatView 就能用
//
// 整体结构（"滚动区域里放一张纸"）：
//   ChatView(QWidget)
//     └─ 外层 QVBoxLayout pMainLayout
//          └─ QScrollArea m_pScrollArea        ← 视口，内容超高时靠它滚动
//               └─ 内容 widget w（一张"纸"）
//                    └─ 内容 QVBoxLayout
//                         ├─ 气泡1 ...（insertWidget 插进来的）
//                         ├─ 气泡2 ...
//                         └─ 弹簧（addWidget 先加的空 widget，永远留在最后）
//
// 关键：弹簧是"第一个"加进内容布局的，但气泡一律用 insertWidget(count()-1)
// 插到它前面，所以弹簧永远被顶到最底部、气泡永远在它上方；
// 对气泡绝不能误用 addWidget()（会插到弹簧后面，变成底部死区看不到）。
//
// 为什么用 QScrollArea + QVBoxLayout 而不是 QListWidget：
// 聊天消息是"内容流"——气泡高度不定（文字换行）、左右对齐、要自动滚底/头插补偿，
// 这些 QListWidget 的固定行高模型都做不好；layout 方式每个气泡是一个普通 widget，
// 高度由内容自动决定，窗口缩放时也会自动重排。
class ChatView : public QWidget
{
    Q_OBJECT
public:
    ChatView(QWidget* parent = nullptr);

    // 三种插入方式（气泡就是一个 widget，左右对齐由气泡内部布局决定）：
    //  - appendChatItem  尾插：新消息到来（自己发的/对方发的），插到底部并自动滚到底
    //  - prependChatItem 头插：往上翻加载更早的历史消息，插到最上面并补偿滚动位置
    //  - insertChatItem  中间插：撤回提示、时间分隔条之类，插到指定气泡前面

    void appendChatItem(QWidget *item); // 尾插
    void prependChatItem(QWidget *item); // 头插
    void insertChatItem(QWidget *before, QWidget* item); //中间插

protected:
    // 事件过滤器：已装到 m_pScrollArea->viewport() 上（鼠标实际悬浮在 viewport），
    // 做"悬浮显示滚动条/离开隐藏"
    bool eventFilter(QObject *watched, QEvent *event)override;
    // 自定义 QWidget 子类必须重画 QStyle，否则 QSS 样式表对它不生效（Qt 固定套路）
    void paintEvent(QPaintEvent *event)override;

private:
    void initStyleSheet(); // 初始化样式表（滚动条、聊天背景 QSS），目前为空

private:
    // 滚动条范围变化时的槽（已由 rangeChanged 连过来）：尾插新消息后自动滚到底部
    // isAppended 防抖：只在新尾插后的 500ms 内响应，避免干扰用户手动滚动
    void onVScrollBarMoved(int min, int max);

private:
    QScrollArea *m_pScrollArea; // 滚动区域：真正的视口，气泡都加在它内部内容 widget 的布局上
    bool isAppended;            // 防抖标志：尾插后 500ms 内只自动滚底一次（配合 onVScrollBarMoved）
};

#endif // CHATVIEW_H
