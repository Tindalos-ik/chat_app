#ifndef APPLYFRIENDITEM_H
#define APPLYFRIENDITEM_H

#include <QWidget>
#include <memory>
#include "userdata.h"

namespace Ui { class ApplyFriendItem; }

// “新的朋友”列表条目：普通历史记录显示状态，待处理申请显示“添加”按钮。
class ApplyFriendItem : public QWidget
{
    Q_OBJECT
public:
    explicit ApplyFriendItem(QWidget *parent = nullptr);
    ~ApplyFriendItem();
    QSize sizeHint() const override;
    void SetInfo(std::shared_ptr<ApplyInfo> applyInfo);
    // 切换待处理/已处理状态，并同步 ApplyInfo::status。
    void ShowAddBtn(bool show);
    int GetUid() const;

signals:
    void sig_auth_friend(std::shared_ptr<ApplyInfo> applyInfo);

private:
    Ui::ApplyFriendItem *ui;
    std::shared_ptr<ApplyInfo> _apply_info;
    class QPushButton *_add_btn = nullptr;
};

#endif // APPLYFRIENDITEM_H
