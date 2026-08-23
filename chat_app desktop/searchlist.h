#ifndef SEARCHLIST_H
#define SEARCHLIST_H

#include <QListWidget>
#include <QEvent>
#include <QWheelEvent>
#include <QScrollBar>

// 搜索列表：顶部固定一个"查找用户"提示项（AddUserItem）
// 点击提示项 = 用搜索框内容发起添加好友请求（TCP 搜索协议未实现，先发信号占位）
class SearchList : public QListWidget
{
    Q_OBJECT

public:
    explicit SearchList(QWidget *parent = nullptr);

    void SetSearchEdit(QWidget *edit); // 记录搜索框，点击提示项时取里面的文字

signals:
    void sig_add_friend_clicked(const QString &searchText);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void addTipItem(); // 顶部加"查找用户"提示项

    QWidget *_search_edit = nullptr;
    QListWidgetItem *_tipItem = nullptr;
};

#endif // SEARCHLIST_H
