#ifndef CONUSERLIST_H
#define CONUSERLIST_H

#include <QObject>
#include <QListWidget>
#include <QWheelEvent>
#include <QScrollEvent>
#include <QDebug>

class ConUserList:public QListWidget
{
    Q_OBJECT
public:
    ConUserList(QWidget* parent = nullptr);

protected:
    bool eventFilter(QObject *watched, QEvent *event)override;

signals:
    void sig_loading_con_user();
};

#endif // CONUSERLIST_H
