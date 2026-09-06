#ifndef APPLYFRIENDPAGE_H
#define APPLYFRIENDPAGE_H

#include <QWidget>
#include <QHash>
#include <memory>
#include "userdata.h"

namespace Ui { class ApplyFriendPage; }
class ApplyFriendItem;
class QLabel;

// “新的朋友”页面只维护申请记录；好友列表本身仍由联系人页面负责。
class ApplyFriendPage : public QWidget
{
    Q_OBJECT
public:
    explicit ApplyFriendPage(QWidget *parent = nullptr);
    ~ApplyFriendPage();

    // 接收 TCP 通知并将新的申请插入列表顶部。
    void AddNewApply(std::shared_ptr<AddFriendApply> apply);

private:
    void addApplyInfo(const std::shared_ptr<ApplyInfo> &applyInfo, bool prepend);
    void updateEmptyState();
    void showAuthDialog(ApplyFriendItem *item, const std::shared_ptr<ApplyInfo> &applyInfo);

    Ui::ApplyFriendPage *ui;
    QLabel *_empty_label = nullptr;
    QHash<int, ApplyFriendItem *> _apply_items;
};

#endif // APPLYFRIENDPAGE_H
