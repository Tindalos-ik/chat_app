#include "findsuccessdlg.h"
#include "ui_findsuccessdlg.h"
#include "avatarutil.h"

FindSuccessDlg::FindSuccessDlg(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::FindSuccessDlg)
{
    ui->setupUi(this);
    // 设置对话框标题
    setWindowTitle("添加");
    // 隐藏对话框标题栏
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    this->setModal(true); // 设置为模态对话框

    _parent = parent;
}

FindSuccessDlg::~FindSuccessDlg()
{
    delete ui;
}

void FindSuccessDlg::setSearchInfo(std::shared_ptr<SearchInfo> si)
{
    if (!si) {
        return;
    }
    ui->name_lb->setText(si->_name);
    AvatarUtil::SetRoundAvatar(ui->head_lb, si->_uid, {});
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

