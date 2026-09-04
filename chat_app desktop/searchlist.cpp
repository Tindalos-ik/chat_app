#include "searchlist.h"
#include "adduseritem.h"
#include "findsuccessdlg.h"
#include "findfaildlg.h"
#include <QLineEdit>
#include <QDebug>
#include <QJsonObject>
#include <QJsonDocument>
#include "tcpmgr.h"
#include "global.h"
#include <memory>


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

    // 当tcpmgr接收到后端回包，显示搜索结果
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_user_search, this, &SearchList::slot_show_search_result);

}

void SearchList::SetSearchEdit(QWidget *edit)
{
    _search_edit = edit;
}

void SearchList::waitpending(bool pending)
{
    // 加一个蒙版
    if(pending){
        _loadingDlg = new LoadingDlg();
        _loadingDlg->setModal(true);
        _loadingDlg->show();
        _send_pending = true;
    }else{
        _loadingDlg->hide();
        _loadingDlg->deleteLater();
        _send_pending = false;
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
        if(_send_pending){
            return;
        }
        waitpending(true); // 等待后台响应
        auto *edit = qobject_cast<QLineEdit*>(_search_edit);
        auto uid_str = edit->text(); // 搜索列表可以搜uid，也可以搜昵称，让服务器自己判断
        // 发送请求给server
        QJsonObject jsonObj;
        jsonObj["uid"] = uid_str;
        QJsonDocument doc(jsonObj);
        QByteArray jsonData = doc.toJson(QJsonDocument::Compact); // 压缩传回来，节省空间
        // 发送tcp请求给chat_server
        emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_SEARCH_USER_REQ, jsonData);
        return;
    }
}

void SearchList::slot_show_search_result(std::shared_ptr<SearchInfo> &si)
{
    waitpending(false);
    if(si == nullptr){
        // 查找失败（服务器返回错误 / 用户不存在）：显示失败提示，不走成功弹窗
        // 父对象 SearchList 持有，弹窗按钮点击后自己 deleteLater，无需成员跟踪
        auto *failDlg = new FindFailDlg(this);
        failDlg->setModal(true);
        failDlg->show();
        return;
    }
    // 此处有两种情况，一种是已经是自己好友了，一种是未添加好友

    // 不是好友
    // 父对象 SearchList 持有，用完（跳到申请好友/隐藏）自己 deleteLater
    auto *successDlg = new FindSuccessDlg(this);
    successDlg->setSearchInfo(si);
    successDlg->setModal(true);
    successDlg->show();
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
