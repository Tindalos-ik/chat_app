#include "searchlist.h"
#include "adduseritem.h"
#include <QLineEdit>

SearchList::SearchList(QWidget *parent)
    : QListWidget(parent)
{
    this->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 安装事件过滤器：悬浮时显示滚动条、滚轮自定义滚动
    this->viewport()->installEventFilter(this);

    // 顶部先放一个"查找用户"提示项
    addTipItem();

    // 点击提示项 -> 把搜索框文字发出去（协议就绪后在这里发 TCP 请求）
    connect(this, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (item != _tipItem) {
            return;
        }
        QString searchText;
        if (auto *edit = qobject_cast<QLineEdit*>(_search_edit)) {
            searchText = edit->text();
        }
        emit sig_add_friend_clicked(searchText);
    });
}

void SearchList::SetSearchEdit(QWidget *edit)
{
    _search_edit = edit;
}

void SearchList::addTipItem()
{
    // 顶部 10px 分隔条：不可选中，纯占位
    auto *invalid_item = new QWidget;
    invalid_item->setObjectName("invalid_item");
    auto *item_tmp = new QListWidgetItem;
    item_tmp->setSizeHint(QSize(250, 10));
    item_tmp->setFlags(item_tmp->flags() & ~Qt::ItemIsSelectable);
    this->addItem(item_tmp);
    this->setItemWidget(item_tmp, invalid_item);

    // "查找用户"提示项
    auto *tip = new AddUserItem;
    _tipItem = new QListWidgetItem;
    _tipItem->setSizeHint(tip->sizeHint());
    this->addItem(_tipItem);
    this->setItemWidget(_tipItem, tip);
}

bool SearchList::eventFilter(QObject *watched, QEvent *event)
{
    // 鼠标进入/离开列表可视区：悬浮时显示滚动条
    if (watched == this->viewport()) {
        if (event->type() == QEvent::Enter) {
            this->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        } else if (event->type() == QEvent::Leave) {
            this->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        }
    }

    // 滚轮事件：自己控制滚动幅度
    if (watched == this->viewport() && event->type() == QEvent::Wheel) {
        auto *wheelEvent = static_cast<QWheelEvent*>(event);
        int numDegrees = wheelEvent->angleDelta().y() / 8;
        int numSteps = numDegrees / 15;
        this->verticalScrollBar()->setValue(this->verticalScrollBar()->value() - numSteps);

        return true;
    }

    return QListWidget::eventFilter(watched, event);
}
