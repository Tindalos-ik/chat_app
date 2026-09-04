#include "findsuccessdlg.h"
#include "ui_findsuccessdlg.h"
#include <QDir>

FindSuccessDlg::FindSuccessDlg(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::FindSuccessDlg)
{
    ui->setupUi(this);
    // 设置对话框标题
    setWindowTitle("添加");
    // 隐藏对话框标题栏
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    // 获取当前应用程序路径，读取对应用户信息
    // 所以需要把static给复制到应用程序执行路径
    QString app_path = QCoreApplication::applicationDirPath();
    QString pix_path = QDir::toNativeSeparators(app_path +
                            QDir::separator() + "static" + QDir::separator() + "head_1.jpg");
    // 头像放在static文件夹中，这个头像后续是通过服务器传过来的

    QPixmap head_pix(pix_path);
    head_pix = head_pix.scaled(ui->head_lb->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    ui->head_lb->setPixmap(head_pix);
    this->setModal(true); // 设置为模态对话框

    _parent = parent;
}

FindSuccessDlg::~FindSuccessDlg()
{
    delete ui;
}

void FindSuccessDlg::setSearchInfo(std::shared_ptr<SearchInfo> si)
{
    ui->name_lb->setText(si->_name);
    _si = si;
}

void FindSuccessDlg::on_add_friend_btn_clicked()
{
    // 每次点击都新建一个 ApplyFriend：
    // 它确认/取消后会 deleteLater 自毁，不能复用同一个实例
    auto *applyFriendDlg = new ApplyFriend(_parent);
    applyFriendDlg->SetSearchInfo(_si); // 把搜索到的用户信息传给申请界面
    applyFriendDlg->setModal(true);
    applyFriendDlg->show();

    // 自己也用完即毁：SearchList 不需要持有弹窗成员
    this->hide();
    this->deleteLater();
}

