#ifndef CHATDIALOG_H
#define CHATDIALOG_H

#include <QDialog>

namespace Ui {
class ChatDialog;
}

class ChatDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ChatDialog(QWidget *parent = nullptr);
    ~ChatDialog();

private:
    Ui::ChatDialog *ui;
    int _cur_mode = 0;   // 0=聊天列表 1=好友列表，搜索清空后回到当前模式

    bool _b_loading;
};

#endif // CHATDIALOG_H
