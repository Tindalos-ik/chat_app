#ifndef SEARCHLIST_H
#define SEARCHLIST_H

#include <QListWidget>
#include <QEvent>
#include <QWheelEvent>
#include <QScrollBar>
#include <memory>
#include "findsuccessdlg.h"

// 搜索列表：顶部固定一个"查找用户"提示项（AddUserItem）
// 点击提示项 = 用搜索框内容发起添加好友请求（TCP 搜索协议未实现，先发信号占位）
class SearchList : public QListWidget
{
    Q_OBJECT

public:
    explicit SearchList(QWidget *parent = nullptr);

    void SetSearchEdit(QWidget *edit); // 记录搜索框，点击提示项时取里面的文字

    void waitpending(bool flag);

    void CloseFindDlg();


protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void addTipItem(); // 搜索列表添加条目

    QWidget *_search_edit = nullptr;
    QListWidgetItem *_tipItem = nullptr;

    std::shared_ptr<FindSuccessDlg> _find_dlg;

    bool _send_pending;

public slots:
    void slot_item_clicked(QListWidgetItem* item);

};

#endif // SEARCHLIST_H
