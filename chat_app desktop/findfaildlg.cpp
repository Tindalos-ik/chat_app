#include "findfaildlg.h"
#include "ui_findfaildlg.h"

FindFailDlg::FindFailDlg(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::FindFailDlg)
{
    ui->setupUi(this);
    setWindowTitle("查找失败");
    // 和 FindSuccessDlg 风格一致：无边框 + 模态
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    setModal(true);
}

FindFailDlg::~FindFailDlg()
{
    delete ui;
}

void FindFailDlg::on_fail_sure_btn_clicked()
{
    // 用完即毁：隐藏后 deleteLater 自毁，SearchList 不需要跟踪弹窗
    this->hide();
    this->deleteLater();
}
