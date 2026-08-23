#include "searchlist.h"
#include "adduseritem.h"
#include "findsuccessdlg.h"
#include <QLineEdit>
#include <QDebug>
#include <QJsonObject>
#include <QJsonDocument>
#include <tcpmgr.h>
#include <global.h>


SearchList::SearchList(QWidget *parent)
    : QListWidget(parent),_send_pending(false)
{
    this->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 安装事件过滤器：悬浮时显示滚动条、滚轮自定义滚动
    this->viewport()->installEventFilter(this);

    // 添加搜索列表条目，比如添加好友
    addTipItem();

    // 连接条目点击的信号和槽
    connect(this, &QListWidget::itemClicked, this, &SearchList::slot_item_clicked);
}

void SearchList::SetSearchEdit(QWidget *edit)
{
    _search_edit = edit;
}

void SearchList::waitpending(bool flag)
{

}

void SearchList::CloseFindDlg()
{
    if(_find_dlg){
        _find_dlg->hide();
        _find_dlg = nullptr;
    }
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

void SearchList::slot_item_clicked(QListWidgetItem *item)
{
    // 获取自定义widget对象
    QWidget *widget = this->itemWidget(item);
    if(!widget){
        qDebug() << "slot item clicked widget is nullptr";
        return;
    }

    // 对自定义widget进行操作，将item转化为ListItemBase
    ListItemBase *customItem = qobject_cast<ListItemBase*>(widget);
    if(!customItem){
        qDebug() << "slot item clicked widget is nullptr";
        return;
    }

    // 获取item类型
    auto itemType = customItem->GetItemType();
    if(itemType == ListItemType::INVALID_ITEM){
        return;
    }

    if(itemType == ListItemType::ADD_USER_TIP_ITEM){
        /*if(_send_pending){
            return;
        }
        waitpending(true);
        auto *edit = qobject_cast<QLineEdit*>(_search_edit);
        auto uid_str = edit->text();
        // 发送请求给server
        QJsonObject jsonObj;
        jsonObj["uid"] = uid_str;
        QJsonDocument doc(jsonObj);
        QString jsonString = doc.toJson(QJsonDocument::Indented);
        // 发送tcp请求给chat_server
        emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_SEARCH_USER_REQ, jsonString);
        */
        _find_dlg = std::make_shared<FindSuccessDlg>(this);
        _find_dlg->show();
        //std::dynamic_pointer_cast<FindSuccessDlg>(_find_dlg)->setSearchInfo(si);
        return; // 已经弹窗了，别走到下面的 CloseFindDlg 把它关掉
    }

    // 没有对应的类型
    // 清除弹出框
    CloseFindDlg();
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
