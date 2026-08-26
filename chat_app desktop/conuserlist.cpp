#include "conuserlist.h"
#include "conuserwid.h"
#include "listitembase.h"
#include <QScrollBar>
#include <QLabel>
#include <QVBoxLayout>

ConUserList::ConUserList(QWidget *parent)
    : QListWidget(parent)
    , _add_friend_item(nullptr)
    , _grpupitem(nullptr)
{
    Q_UNUSED(parent);
    // 默认隐藏滚动条：鼠标悬浮到列表区域时再显示
    this->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 安装事件过滤器，用于自定义滚动条显隐和滚轮步长
    this->viewport()->installEventFilter(this);

    // 顶部加入口项和分组标题
    addContactUserList();

    // 点击条目 -> 按类型分发信号
    connect(this, &QListWidget::itemClicked, this, &ConUserList::slot_item_clicked);
}

void ConUserList::addContactUserList()
{
    // 1) "新的朋友"入口：点击切到好友申请页
    _add_friend_item = new ConUserWid;
    _add_friend_item->setObjectName("new_friend_item");
    _add_friend_item->SetUserName(QStringLiteral("新的朋友"));
    _add_friend_item->SetHeadIcon(":/res/add_friend.png");
    _add_friend_item->SetItemType(ListItemType::APPLY_FRIEND_ITEM);

    auto *addItem = new QListWidgetItem;
    addItem->setSizeHint(_add_friend_item->sizeHint());
    this->addItem(addItem);
    this->setItemWidget(addItem, _add_friend_item);

    // 2) "联系人"分组标题：不可选中，点击时 qobject_cast<ListItemBase*> 失败会被忽略
    auto *groupWid = new QWidget;
    groupWid->setObjectName("group_tip");
    auto *groupLb = new QLabel(QStringLiteral("联系人"), groupWid);
    groupLb->setObjectName("group_tip_lb");
    auto *groupLayout = new QVBoxLayout(groupWid);
    groupLayout->setContentsMargins(20, 0, 0, 0);
    groupLayout->addWidget(groupLb);

    _grpupitem = new QListWidgetItem;
    _grpupitem->setSizeHint(QSize(250, 25));
    _grpupitem->setFlags(_grpupitem->flags() & ~Qt::ItemIsSelectable);
    this->addItem(_grpupitem);
    this->setItemWidget(_grpupitem, groupWid);

    // 好友本体由 ChatDialog::addConUserList() 在构造时追加到列表尾部
}

void ConUserList::ShowRedPoint(bool bshow)
{
    // 有新好友申请时，把"新的朋友"入口的红点亮出来
    if (_add_friend_item) {
        _add_friend_item->ShowRedPoint(bshow);
    }
}

bool ConUserList::eventFilter(QObject *watched, QEvent *event)
{
    // 鼠标进入/离开列表可视区：悬浮时显示滚动条，移开时隐藏
    if (watched == this->viewport()) {
        if (event->type() == QEvent::Enter) {
            this->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        } else if (event->type() == QEvent::Leave) {
            this->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        }
    }

    // 滚轮事件：自己控制滚动幅度；滚到底部时触发加载更多联系人
    if (watched == this->viewport() && event->type() == QEvent::Wheel) {
        auto *wheelEvent = static_cast<QWheelEvent*>(event);
        int numDegrees = wheelEvent->angleDelta().y() / 8;
        int numSteps = numDegrees / 15;
        this->verticalScrollBar()->setValue(this->verticalScrollBar()->value() - numSteps);

        QScrollBar *scrollbar = this->verticalScrollBar();
        int maxScrollValue = scrollbar->maximum();
        int currentValue = scrollbar->value();

        // 有滚动余地且接近底部（留 5px 余量）才加载更多，避免频繁触发
        if (maxScrollValue > 0 && maxScrollValue - currentValue <= 5) {
            qDebug() << "load more contact user";
            emit sig_loading_con_user(); // 通知 ChatDialog 追加联系人
        }

        return true; // 停止事件传递
    }

    return QListWidget::eventFilter(watched, event);
}

void ConUserList::slot_item_clicked(QListWidgetItem *item)
{
    QWidget *widget = this->itemWidget(item);
    if (widget == nullptr) {
        return;
    }

    // 分组标题等不是 ListItemBase 的条目直接忽略
    auto *baseItem = qobject_cast<ListItemBase*>(widget);
    if (baseItem == nullptr) {
        return;
    }

    ListItemType itemType = baseItem->GetItemType();
    if (itemType == ListItemType::INVALID_ITEM || itemType == ListItemType::GRUOP_TIP_ITEM) {
        return; // 不可点击条目
    }

    if (itemType == ListItemType::APPLY_FRIEND_ITEM) {
        // "新的朋友"：切到好友申请界面
        qDebug() << "apply friend item clicked";
        emit sig_switch_apply_friend_page();
        return;
    }

    if (itemType == ListItemType::CONTACT_USER_ITEM) {
        // 点联系人：带上被点的好友项，切到好友信息界面
        qDebug() << "contact user item clicked";
        auto *wid = qobject_cast<ConUserWid*>(baseItem);
        if (wid) {
            emit sig_switch_friend_info_page(wid);
        }
        return;
    }
}
