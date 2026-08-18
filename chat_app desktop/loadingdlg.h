#ifndef LOADINGDLG_H
#define LOADINGDLG_H

#include <QDialog>

namespace Ui {
class LoadingDlg;
}

// 加载中的半透明遮罩弹窗：盖在父窗口上，中间转圈 + 提示文字
class LoadingDlg : public QDialog
{
    Q_OBJECT

public:
    explicit LoadingDlg(QWidget *parent = nullptr, QString tip = "Loading...");
    ~LoadingDlg();

private:
    Ui::LoadingDlg *ui;
};

#endif // LOADINGDLG_H
