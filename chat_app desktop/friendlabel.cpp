#include "friendlabel.h"
#include "ui_friendlabel.h"
#include "clickedlabel.h"
#include <QPainter>
#include <QFontMetrics>

FriendLabel::FriendLabel(QWidget *parent)
    : QFrame(parent)
    , ui(new Ui::FriendLabel)
    , _width(0)
    , _height(0)
{
    ui->setupUi(this);

    connect(ui->close_lb, &ClickedLabel::Clicked, this, &FriendLabel::slot_close);

    // 用 ui 里的默认文字先把尺寸算好，避免刚创建时尺寸是 0
    SetText(ui->tip_lb->text());
}

FriendLabel::~FriendLabel()
{
    delete ui;
}

void FriendLabel::SetText(const QString &text)
{
    _text = text;
    ui->tip_lb->setText(text); // 显示文字

    // 用字体度量计算文字实际宽度：
    // 文字宽度 + 右侧关闭叉区域(20) + 左右留白(16)，让胶囊刚好包住内容
    QFontMetrics metrics(ui->tip_lb->font());
    _width = metrics.horizontalAdvance(text) + 20 + 16;
    _height = 20; // 和 ApplyFriend 里的标签输入框（lb_ed）高度保持一致

    // 按算好的尺寸固定自身，外部布局（ApplyFriend 的 lb_list）就能直接摆放
    setFixedSize(_width, _height);
}

void FriendLabel::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    // 用代码画浅绿圆角胶囊背景：
    // QFrame 的 QSS background 渲染不稳定（可能出现背景不显示/错位），
    // 直接 paint 最可靠，文字和关闭叉由子控件正常绘制
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#daf6e7"));
    // 圆角取高度一半：无论胶囊多高都保持"全圆角药丸"造型
    painter.drawRoundedRect(rect(), height() / 2, height() / 2);
}

int FriendLabel::Width() const
{
    return _width;
}

int FriendLabel::Height() const
{
    return _height;
}

QString FriendLabel::Text() const
{
    return _text;
}

void FriendLabel::slot_close()
{
    // 只负责发信号，真正的删除动作（从 lb_list 移除、调整布局）由外部处理
    emit sig_close(_text);
}
