#ifndef APPLYFRIENDITEM_H
#define APPLYFRIENDITEM_H

#include <QWidget>

namespace Ui {
class ApplyFriendItem;
}

// "新的朋友"页的列表条目（仿微信）
// 结构：圆形头像 + 名字 + 右侧状态标签
// 状态取值："已添加" / "好友申请" / "等待对方同意"
class ApplyFriendItem : public QWidget
{
    Q_OBJECT

public:
    explicit ApplyFriendItem(QWidget *parent = nullptr);
    ~ApplyFriendItem();

    QSize sizeHint() const override; // 260x60，和其他列表项一致

    // 设置条目内容：名字、头像、右侧状态
    void SetInfo(const QString &name, const QString &icon, const QString &status);

private:
    Ui::ApplyFriendItem *ui;
};

#endif // APPLYFRIENDITEM_H
