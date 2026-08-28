#ifndef APPLYFRIENDPAGE_H
#define APPLYFRIENDPAGE_H

#include <QWidget>

namespace Ui {
class ApplyFriendPage;
}

// "新的朋友"页（联系人页点"新的朋友"入口后显示）
// 仿微信：一个列表，每条目最右端一个状态标签，取值：
//   "已添加" / "好友申请"（别人申请添加我，待我同意） / "等待对方同意"（我发出的申请）
class ApplyFriendPage : public QWidget
{
    Q_OBJECT

public:
    explicit ApplyFriendPage(QWidget *parent = nullptr);
    ~ApplyFriendPage();

    // 接口：添加一个"新的朋友"条目
    // status 取值："已添加" / "好友申请" / "等待对方同意"
    void AddFriendItem(const QString &name, const QString &icon, const QString &status);

private:
    // 测试数据（等后端申请/好友接口就绪后替换成真实数据）
    void LoadTestData();

    Ui::ApplyFriendPage *ui;
};

#endif // APPLYFRIENDPAGE_H
