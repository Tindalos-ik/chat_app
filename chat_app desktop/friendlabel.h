#ifndef FRIENDLABEL_H
#define FRIENDLABEL_H

#include <QFrame>
#include <QString>
namespace Ui {
class FriendLabel;
}

// 好友标签（添加好友申请时填写的"标签"小胶囊）
// 外形：浅绿圆角胶囊 + 标签文字 + 右上角关闭叉
// 用法（ApplyFriend 里）：
//   auto *label = new FriendLabel(this);
//   label->SetText("同事");                     // 设置标签文字
//   connect(label, &FriendLabel::sig_close, this,
//           [this](const QString &text){ ... }); // 点关闭叉时收到通知并删除自己
class FriendLabel : public QFrame
{
    Q_OBJECT

public:
    explicit FriendLabel(QWidget *parent = nullptr);
    ~FriendLabel();

    void SetText(const QString &text); // 设置标签文字，并按文字重新计算胶囊尺寸
    int Width() const;
    int Height() const;
    QString Text() const;

public slots:
    void slot_close();                 // 点关闭叉：发 sig_close，由外部负责删除自己

signals:
    void sig_close(const QString &text); // 关闭信号，参数是被关闭的标签文字

private:
    Ui::FriendLabel *ui;
    QString _text;  // 标签文字
    int _width;     // 文字排版后的宽度（记录供外部排列标签时使用）
    int _height;    // 标签高度（固定 43，和 ui 一致）
};

#endif // FRIENDLABEL_H
