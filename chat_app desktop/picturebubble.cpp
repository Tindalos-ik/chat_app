#include "picturebubble.h"

#include <QDialog>
#include <QGuiApplication>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QVBoxLayout>

#include <functional>

#define PIC_MAX_WIDTH 200
#define PIC_MAX_HEIGHT 133

namespace {

// QLabel 默认不会发出点击信号。这个轻量控件不使用 Q_OBJECT，避免为单一点击动作
// 引入额外的 moc 文件；鼠标释放仍交还给 QLabel，保证普通焦点和样式行为不变。
class PreviewableImageLabel final : public QLabel
{
public:
    explicit PreviewableImageLabel(QWidget *parent = nullptr) : QLabel(parent) {}

    void SetClickHandler(std::function<void()> handler)
    {
        _clickHandler = std::move(handler);
        setCursor(Qt::PointingHandCursor);
        setToolTip(QStringLiteral("点击预览图片"));
    }

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())
            && _clickHandler) {
            _clickHandler();
        }
        QLabel::mouseReleaseEvent(event);
    }

private:
    std::function<void()> _clickHandler;
};

// 图片预览保留原始像素，超出窗口可见区域时交给滚动区域浏览；窗口初始尺寸限制为
// 当前屏幕可用区域的 80%，因此 4K 图片不会撑出屏幕，也不会在预览阶段二次压缩。
class ImagePreviewDialog final : public QDialog
{
public:
    explicit ImagePreviewDialog(const QPixmap &picture, QWidget *parent)
        : QDialog(parent)
    {
        setWindowTitle(QStringLiteral("图片预览"));
        setModal(true);
        setAttribute(Qt::WA_DeleteOnClose);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 12, 12, 12);

        auto *scrollArea = new QScrollArea(this);
        scrollArea->setWidgetResizable(false);
        scrollArea->setAlignment(Qt::AlignCenter);
        auto *imageLabel = new QLabel(scrollArea);
        imageLabel->setPixmap(picture);
        imageLabel->setFixedSize(picture.size());
        imageLabel->setAlignment(Qt::AlignCenter);
        scrollArea->setWidget(imageLabel);
        layout->addWidget(scrollArea, 1);

        auto *closeButton = new QPushButton(QStringLiteral("关闭"), this);
        closeButton->setFixedWidth(80);
        layout->addWidget(closeButton, 0, Qt::AlignHCenter);
        connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);

        const QScreen *screen = QGuiApplication::primaryScreen();
        const QSize available = screen ? screen->availableGeometry().size() : QSize(1000, 700);
        const QSize maximumSize = available * 0.8;
        // 为布局边距、滚动条和关闭按钮预留空间，避免刚好等于屏幕大小时产生额外溢出。
        const QSize chromeSize(60, 100);
        resize(qMin(picture.width() + chromeSize.width(), maximumSize.width()),
               qMin(picture.height() + chromeSize.height(), maximumSize.height()));
    }
};

void ShowImagePreview(const QPixmap &picture, QWidget *parent)
{
    if (picture.isNull()) {
        return;
    }
    auto *dialog = new ImagePreviewDialog(picture, parent);
    // open() 采用应用模态但不嵌套事件循环；聊天 TCP、图片下载和心跳仍可正常处理。
    dialog->open();
}

} // namespace

PictureBubble::PictureBubble(ChatRole role, const QPixmap &picture, BubbleFrame *parent):
    BubbleFrame(role, parent)
{
    // 把图片放在标签里面
    auto *lb = new PreviewableImageLabel(this);
    lb->setScaledContents(true); // 设置内容可伸缩
    QPixmap pix = picture.scaled(QSize(PIC_MAX_WIDTH, PIC_MAX_HEIGHT), Qt::KeepAspectRatio);
    lb->setPixmap(pix);
    // 气泡仅显示缩略图，但预览必须使用构造时传入的原图；这样接收方下载到的高分辨率
    // 缓存和发送方本地原图都可以放大查看，而不是把 200px 缩略图再放大。
    lb->SetClickHandler([picture, this] { ShowImagePreview(picture, window()); });
    this->setWidget(lb);

    int left_margin = this->layout()->contentsMargins().left();
    int right_margin = this->layout()->contentsMargins().right();
    int v_margin = this->layout()->contentsMargins().bottom();
    setFixedSize(pix.width() + left_margin + right_margin, pix.height() + v_margin*2);
}
