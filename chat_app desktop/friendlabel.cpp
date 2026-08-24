#include "friendlabel.h"
#include "ui_friendlabel.h"
#include "clickedlabel.h"
#include <QFontMetrics>

FriendLabel::FriendLabel(QWidget *parent)
    : QFrame(parent)
    , ui(new Ui::FriendLabel)
    , _width(0)
    , _height(0)
{
    ui->setupUi(this);

    // QFrame 默认不绘制样式表背景，加上这个属性后 #FriendLabel 的
    // 浅绿底色 + 圆角才会真正渲染出来
    setAttribute(Qt::WA_StyledBackground, true);
    setFrameShape(QFrame::NoFrame); // 样式表已经做了圆角背景，不需要默认边框

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
    // 文字宽度 + 右侧关闭叉区域(25) + 左右留白(20)，让胶囊刚好包住内容
    QFontMetrics metrics(ui->tip_lb->font());
    _width = metrics.horizontalAdvance(text) + 25 + 20;
    _height = 43;

    // 按算好的尺寸固定自身，外部布局（ApplyFriend 的 lb_list）就能直接摆放
    setFixedSize(_width, _height);
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
