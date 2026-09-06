#include "applyfrienditem.h"
#include "ui_applyfrienditem.h"
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>

namespace {
void setRoundPixmap(QLabel *label, const QString &path)
{
    QPixmap original(path);
    if (original.isNull()) {
        label->clear();
        return;
    }
    original = original.scaled(label->size(), Qt::KeepAspectRatioByExpanding,
                               Qt::SmoothTransformation);
    QPixmap rounded(label->size());
    rounded.fill(Qt::transparent);
    QPainter painter(&rounded);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addEllipse(rounded.rect());
    painter.setClipPath(clip);
    painter.drawPixmap(0, 0, original, (original.width() - rounded.width()) / 2,
                       (original.height() - rounded.height()) / 2,
                       rounded.width(), rounded.height());
    label->setPixmap(rounded);
}
}

ApplyFriendItem::ApplyFriendItem(QWidget *parent)
    : QWidget(parent), ui(new Ui::ApplyFriendItem)
{
    ui->setupUi(this);
    // 申请记录使用按钮直接触发认证弹窗，普通历史记录仍显示状态文本。
    _add_btn = new QPushButton(QStringLiteral("添加"), this);
    _add_btn->setCursor(Qt::PointingHandCursor);
    _add_btn->setFixedSize(64, 30);
    _add_btn->setStyleSheet(QStringLiteral(
        "QPushButton { background:#07c160; color:white; border:none; border-radius:4px; }"
        "QPushButton:hover { background:#06ad56; } QPushButton:pressed { background:#059a4c; }"));
    ui->root_layout->addWidget(_add_btn);
    _add_btn->hide();
    connect(_add_btn, &QPushButton::clicked, this, [this] {
        if (_apply_info) {
            emit sig_auth_friend(_apply_info);
        }
    });
}

ApplyFriendItem::~ApplyFriendItem() { delete ui; }

QSize ApplyFriendItem::sizeHint() const { return QSize(260, 60); }

void ApplyFriendItem::SetInfo(std::shared_ptr<ApplyInfo> applyInfo)
{
    if (!applyInfo) return;
    // ApplyInfo 的 status 约定：0 表示待处理，1 表示已经同意。
    _apply_info = std::move(applyInfo);
    ui->user_name_lb->setText(_apply_info->_name);
    setRoundPixmap(ui->icon_lb, _apply_info->_icon);
    ShowAddBtn(_apply_info->_status == 0);
}

void ApplyFriendItem::ShowAddBtn(bool show)
{
    // 按钮隐藏后不可再次提交，避免重复发送认证请求。
    _add_btn->setVisible(show);
    ui->status_lb->setVisible(!show);
    if (!show) {
        ui->status_lb->setText(QStringLiteral("已添加"));
        ui->status_lb->setStyleSheet(QStringLiteral("color:#999999;"));
        if (_apply_info) _apply_info->_status = 1;
    }
}

int ApplyFriendItem::GetUid() const { return _apply_info ? _apply_info->_uid : 0; }
