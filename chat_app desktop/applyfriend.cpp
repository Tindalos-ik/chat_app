#include "applyfriend.h"
#include "ui_applyfriend.h"
#include "clickedoncelabel.h"
#include "friendlabel.h"
#include <QScrollBar>
#include <QFontMetrics>
#include <QJsonObject>
#include <QJsonDocument>
#include <QDebug>
#include <algorithm>

namespace {
// 推荐标签排版参数（放匿名命名空间，避免污染全局）
const int kTipOffset = 5;          // 推荐标签之间的水平间距
const int kMinLabelEdLen = 40;     // 输入框最短占位长度，用于判断是否换行
const QString kAddPrefix = QStringLiteral("添加标签 "); // 动态提示文字的前缀
}

ApplyFriend::ApplyFriend(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::ApplyFriend)
    , _label_point(2, 6)
    , _parent(parent)
{
    ui->setupUi(this);

    // 无边框 + 模态，和 FindSuccessDlg 风格一致
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    setObjectName("ApplyFriend");
    setModal(true);

    // 各输入框占位提示
    ui->name_ed->setPlaceholderText(QStringLiteral("您好，我是..."));
    ui->back_ed->setPlaceholderText(QStringLiteral("设置备注名"));
    ui->lb_ed->setPlaceholderText(QStringLiteral("搜索、添加标签"));

    // 标签输入框：限长 10，先挪到角落，由 addLabel/resetLabels 动态摆放
    ui->lb_ed->setMaxLength(10);
    ui->lb_ed->setFixedHeight(20);
    ui->lb_ed->move(2, 2);
    ui->input_tip_wid->hide(); // "添加标签 xxx"提示默认隐藏

    _tip_cur_point = QPoint(5, 5);

    // 预设推荐标签
    _tip_data = { "同学", "家人", "菜鸟教程", "C++ Primer", "Rust 程序设计",
                  "父与子学Python", "nodejs开发指南", "go 语言开发指南",
                  "游戏伙伴", "金融投资", "微信读书", "拼多多拼友" };

    // 输入框回车 -> 把当前文字添加为好友标签
    connect(ui->lb_ed, &QLineEdit::returnPressed, this, &ApplyFriend::slot_label_enter);
    // 输入内容变化 -> 动态更新"添加标签 xxx"提示
    connect(ui->lb_ed, &QLineEdit::textChanged, this, &ApplyFriend::slot_label_text_change);
    // 编辑结束（失焦）-> 隐藏提示
    connect(ui->lb_ed, &QLineEdit::editingFinished, this, &ApplyFriend::slot_label_edit_finished);

    // 点击"添加标签 xxx"提示 -> 直接添加
    connect(ui->tip_lb, &ClickedOnceLabel::clicked, this, &ApplyFriend::slot_add_friend_label_by_tip);
    // 点击"更多" -> 展开全部推荐标签
    connect(ui->more_lb, &ClickedOnceLabel::clicked, this, &ApplyFriend::slot_show_more_label);
    // 确认 / 取消
    connect(ui->sure_btn, &QPushButton::clicked, this, &ApplyFriend::slot_apply_sure);
    connect(ui->cancel_btn, &QPushButton::clicked, this, &ApplyFriend::slot_apply_cancel);

    // 滚动条：平时隐藏，悬浮滚动区时显示
    ui->scrollArea->horizontalScrollBar()->setHidden(true);
    ui->scrollArea->verticalScrollBar()->setHidden(true);
    ui->scrollArea->installEventFilter(this);

    // 排版推荐标签
    InitTipLbs();
}

ApplyFriend::~ApplyFriend()
{
    delete ui;
}

void ApplyFriend::InitTipLbs()
{
    // 从第一行开始排，最多排两行；排不下的等点"更多"再展开
    int lines = 1;
    for (const QString &tip : _tip_data) {
        auto *lb = new ClickedLabel(ui->lb_list);
        lb->SetState("normal", "hover", "pressed",
                     "selected_normal", "selected_hover", "selected_pressed");
        lb->setObjectName("tipslb");
        lb->setText(tip);
        lb->adjustSize(); // 让标签宽度跟随文字（含 QSS padding），避免文字被截断
        // 点击推荐标签：选中 -> 加好友标签，取消选中 -> 移除
        connect(lb, &ClickedLabel::Clicked, this, &ApplyFriend::slot_change_friend_label_by_tip);

        QFontMetrics fontMetrics(lb->font());
        int textWidth = fontMetrics.horizontalAdvance(lb->text());
        int textHeight = fontMetrics.height();

        // 当前行放不下 -> 换行
        if (_tip_cur_point.x() + textWidth + kTipOffset > ui->lb_list->width()) {
            lines++;
            if (lines > 2) { // 超过两行：先不展示
                delete lb;
                return;
            }
            _tip_cur_point.setX(kTipOffset);
            _tip_cur_point.setY(_tip_cur_point.y() + textHeight + 15);
        }

        QPoint nextPoint = _tip_cur_point;
        AddTipLbs(lb, _tip_cur_point, nextPoint, textWidth, textHeight);
        _tip_cur_point = nextPoint;
    }
}

void ApplyFriend::AddTipLbs(ClickedLabel *label, QPoint curPoint,
                            QPoint &nextPoint, int textWidth, int textHeight)
{
    Q_UNUSED(textHeight);
    label->move(curPoint);                       // 摆到当前位置
    label->show();
    _add_labels.insert(label->text(), label);    // 记录：文字 -> 控件
    _add_label_keys.push_back(label->text());    // 记录顺序
    nextPoint.setX(label->pos().x() + textWidth + 15); // 下一个标签的 x
    nextPoint.setY(label->pos().y());
}

bool ApplyFriend::eventFilter(QObject *obj, QEvent *event)
{
    // 悬浮滚动区时显示滚动条，移开隐藏
    if (obj == ui->scrollArea) {
        if (event->type() == QEvent::Enter) {
            ui->scrollArea->verticalScrollBar()->setHidden(false);
        } else if (event->type() == QEvent::Leave) {
            ui->scrollArea->verticalScrollBar()->setHidden(true);
        }
    }
    return QDialog::eventFilter(obj, event);
}

void ApplyFriend::SetSearchInfo(std::shared_ptr<SearchInfo> si)
{
    _si = si;
    // 备注名默认填对方的昵称
    if (si) {
        ui->back_ed->setText(si->_name);
    }
    // TODO: UserMgr 还没有 GetName()，等补上后验证消息默认带上自己的名字
}

void ApplyFriend::slot_show_more_label()
{
    // 已展开：点"折叠"收回两行
    if (_b_show_all_tips) {
        _b_show_all_tips = false;

        // 折叠：不重建标签，超出两行的隐藏，其余挪回原来的两行位置
        ui->lb_list->setFixedWidth(287); // 还原列表宽度
        _tip_cur_point = QPoint(5, 5);

        int lines = 1;
        for (const QString &key : _add_label_keys) {
            auto *lb = _add_labels.value(key);
            QFontMetrics fontMetrics(lb->font());
            int textWidth = fontMetrics.horizontalAdvance(lb->text());
            int textHeight = fontMetrics.height();

            if (_tip_cur_point.x() + textWidth + kTipOffset > ui->lb_list->width()) {
                lines++;
                _tip_cur_point.setX(kTipOffset);
                _tip_cur_point.setY(_tip_cur_point.y() + textHeight + 15);
            }
            if (lines > 2) {
                lb->hide(); // 超过两行：隐藏而不是删除，展开时还能直接 show 回来
                continue;
            }
            lb->move(_tip_cur_point); // 挪回原来的位置
            lb->show();
            _tip_cur_point.setX(_tip_cur_point.x() + textWidth + 15);
        }

        ui->lb_list->setFixedHeight(50); // 还原两行高度

        // "折叠"按钮还原成下拉箭头（清掉内联样式，QSS 的 border-image 重新生效）
        ui->more_lb->setText(QString());
        ui->more_lb->setStyleSheet(QString());
        return;
    }

    // 点"更多"：加宽列表，把所有推荐标签重新排出来（更多按钮原地变成"折叠"）
    _b_show_all_tips = true;
    ui->lb_list->setFixedWidth(325);
    _tip_cur_point = QPoint(5, 5);

    QPoint nextPoint = _tip_cur_point;
    int textWidth = 0;
    int textHeight = 0;

    // 1) 重排已展示的标签
    for (const QString &key : _add_label_keys) {
        auto *addedLb = _add_labels.value(key);
        QFontMetrics fontMetrics(addedLb->font());
        textWidth = fontMetrics.horizontalAdvance(addedLb->text());
        textHeight = fontMetrics.height();

        if (_tip_cur_point.x() + textWidth + kTipOffset > ui->lb_list->width()) {
            _tip_cur_point.setX(kTipOffset);
            _tip_cur_point.setY(_tip_cur_point.y() + textHeight + 15);
        }
        addedLb->move(_tip_cur_point);
        addedLb->show(); // 折叠时被隐藏的标签要重新显示出来
        nextPoint.setX(addedLb->pos().x() + textWidth + 15);
        nextPoint.setY(_tip_cur_point.y());
        _tip_cur_point = nextPoint;
    }

    // 2) 补上之前被折叠的标签
    for (const QString &tip : _tip_data) {
        if (_add_labels.contains(tip)) {
            continue;
        }
        auto *lb = new ClickedLabel(ui->lb_list);
        lb->SetState("normal", "hover", "pressed",
                     "selected_normal", "selected_hover", "selected_pressed");
        lb->setObjectName("tipslb");
        lb->setText(tip);
        lb->adjustSize(); // 展开"更多"时新补的标签同样要按文字调整宽度
        connect(lb, &ClickedLabel::Clicked, this, &ApplyFriend::slot_change_friend_label_by_tip);

        QFontMetrics fontMetrics(lb->font());
        textWidth = fontMetrics.horizontalAdvance(lb->text());
        textHeight = fontMetrics.height();

        if (_tip_cur_point.x() + textWidth + kTipOffset > ui->lb_list->width()) {
            _tip_cur_point.setX(kTipOffset);
            _tip_cur_point.setY(_tip_cur_point.y() + textHeight + 15);
        }
        nextPoint = _tip_cur_point;
        AddTipLbs(lb, _tip_cur_point, nextPoint, textWidth, textHeight);
        _tip_cur_point = nextPoint;
    }

    // 3) 按最后一行高度调整 lb_list，避免标签被截断
    ui->lb_list->setFixedHeight(nextPoint.y() + textHeight + kTipOffset);
    ui->more_lb->setText(QStringLiteral("折叠"));
    ui->more_lb->setStyleSheet(QStringLiteral("border-image: none; font-size: 12px; color: #48bf56;"));
}

void ApplyFriend::slot_label_enter()
{
    if (ui->lb_ed->text().isEmpty()) {
        return;
    }
    const QString text = ui->lb_ed->text();
    addLabel(text);              // 生成好友标签胶囊
    ui->input_tip_wid->hide();   // 隐藏"添加标签"提示

    // 记录进推荐标签数据（没出现过就追加）
    if (std::find(_tip_data.begin(), _tip_data.end(), text) == _tip_data.end()) {
        _tip_data.push_back(text);
    }
    appendTipLb(text);           // 推荐标签栏同步标绿
}

void ApplyFriend::slot_label_text_change(const QString &text)
{
    if (text.isEmpty()) {
        ui->tip_lb->setText(QString());
        ui->input_tip_wid->hide();
        return;
    }
    // 输入的是预设标签 -> 提示直接显示标签名；否则显示"添加标签 xxx"
    auto it = std::find(_tip_data.begin(), _tip_data.end(), text);
    if (it == _tip_data.end()) {
        ui->tip_lb->setText(kAddPrefix + text);
    } else {
        ui->tip_lb->setText(text);
    }
    ui->input_tip_wid->show();
}

void ApplyFriend::slot_label_edit_finished()
{
    // 编辑结束（回车/失焦）：隐藏"添加标签"提示
    ui->input_tip_wid->hide();
}

void ApplyFriend::slot_add_friend_label_by_tip(const QString &text)
{
    // 去掉"添加标签 "前缀，得到真正的标签文字
    QString realText = text;
    if (realText.startsWith(kAddPrefix)) {
        realText = realText.mid(kAddPrefix.length());
    }
    addLabel(realText);

    if (std::find(_tip_data.begin(), _tip_data.end(), realText) == _tip_data.end()) {
        _tip_data.push_back(realText);
    }
    appendTipLb(realText);
}

void ApplyFriend::slot_change_friend_label_by_tip()
{
    // ClickedLabel::Clicked 不带参数，用 sender() 取回被点击的标签
    auto *lb = qobject_cast<ClickedLabel*>(sender());
    if (lb == nullptr) {
        return;
    }
    const QString text = lb->text();
    if (lb->GetCurState() == ClickLbState::Selected) {
        addLabel(text);                   // 选中 -> 添加好友标签
    } else {
        slot_remove_friend_label(text);   // 取消选中 -> 移除好友标签
    }
}

void ApplyFriend::slot_remove_friend_label(const QString &text)
{
    // 从记录里删除好友标签控件并重排
    _label_point.setX(2);
    _label_point.setY(6);

    auto it = _friend_labels.find(text);
    if (it == _friend_labels.end()) {
        return;
    }
    auto keyIt = std::find(_friend_label_keys.begin(), _friend_label_keys.end(), text);
    if (keyIt != _friend_label_keys.end()) {
        _friend_label_keys.erase(keyIt);
    }
    // 注意：这里正在处理 sig_close 信号，sender（FriendLabel）还在发信号，
    // 直接 delete 会连带删掉正在执行 mousePressEvent 的 close_lb，导致崩溃。
    // deleteLater 等事件循环再删，保证信号链路安全。
    it.value()->deleteLater();
    _friend_labels.erase(it);

    resetLabels();

    // 对应的推荐标签恢复灰色（未选中）
    auto addIt = _add_labels.find(text);
    if (addIt != _add_labels.end()) {
        addIt.value()->ResetNormalState();
    }
}

void ApplyFriend::slot_apply_sure()
{
    // 拼好友申请数据（TODO: 后端协议就绪后通过 TcpMgr 发送）
    QJsonObject jsonObj;
    QString name = ui->name_ed->text();
    if (name.isEmpty()) {
        name = ui->name_ed->placeholderText();
    }
    jsonObj["applyname"] = name;

    QString bakname = ui->back_ed->text();
    if (bakname.isEmpty()) {
        bakname = ui->back_ed->placeholderText();
    }
    jsonObj["bakname"] = bakname;

    if (_si) {
        jsonObj["touid"] = _si->_uid;
    }

    QJsonDocument doc(jsonObj);
    QString jsonData = doc.toJson(QJsonDocument::Compact);
    qDebug() << "apply friend request:" << jsonData;

    // TODO: TcpMgr::GetInstance()->sig_send_data(ReqId::ID_ADD_FRIEND_REQ, jsonData);
    //       ID_ADD_FRIEND_REQ 待加入 global.h 的 ReqId

    hide();
    deleteLater();
}

void ApplyFriend::slot_apply_cancel()
{
    hide();
    deleteLater();
}

void ApplyFriend::resetLabels()
{
    // 所有好友标签按顺序重排，超出 gridWidget 宽度就换行
    auto maxWidth = ui->gridWidget->width();
    int labelHeight = 0;
    for (auto *label : _friend_labels) {
        if (_label_point.x() + label->width() > maxWidth) {
            _label_point.setY(_label_point.y() + label->height() + 6);
            _label_point.setX(2);
        }
        label->move(_label_point);
        label->show();
        _label_point.setX(_label_point.x() + label->width() + 2);
        labelHeight = label->height();
    }

    // 输入框跟在最后一个标签后面，放不下就换行
    if (_friend_labels.isEmpty()) {
        ui->lb_ed->move(_label_point);
        return;
    }
    if (_label_point.x() + kMinLabelEdLen > ui->gridWidget->width()) {
        ui->lb_ed->move(2, _label_point.y() + labelHeight + 6);
    } else {
        ui->lb_ed->move(_label_point);
    }
}

void ApplyFriend::addLabel(const QString &name)
{
    // 重复标签不添加，只清空输入框
    if (_friend_labels.contains(name)) {
        ui->lb_ed->clear();
        return;
    }

    auto *label = new FriendLabel(ui->gridWidget);
    label->SetText(name);
    label->setObjectName("FriendLabel");

    // 当前行放不下 -> 换行
    auto maxWidth = ui->gridWidget->width();
    if (_label_point.x() + label->width() > maxWidth) {
        _label_point.setY(_label_point.y() + label->height() + 6);
        _label_point.setX(2);
    }
    label->move(_label_point);
    label->raise(); // 确保胶囊盖在输入框上面（lb_ed 是先创建的兄弟控件）
    label->show();

    _friend_labels.insert(label->Text(), label); // 记录：文字 -> 胶囊
    _friend_label_keys.push_back(label->Text()); // 记录顺序
    connect(label, &FriendLabel::sig_close, this, &ApplyFriend::slot_remove_friend_label);

    _label_point.setX(_label_point.x() + label->width() + 2);

    // 输入框跟随标签；放不下就换行
    if (_label_point.x() + kMinLabelEdLen > ui->gridWidget->width()) {
        ui->lb_ed->move(2, _label_point.y() + label->height() + 2);
    } else {
        ui->lb_ed->move(_label_point);
    }

    ui->lb_ed->clear();

    // gridWidget 高度不够时撑高，避免标签被截断（按一行的高度算，别把区域撑太大）
    if (ui->gridWidget->height() < _label_point.y() + label->height() + 2) {
        ui->gridWidget->setFixedHeight(_label_point.y() + label->height() + 2);
    }
}

void ApplyFriend::appendTipLb(const QString &text)
{
    // 推荐标签栏已有 -> 直接标绿
    if (_add_labels.contains(text)) {
        _add_labels.value(text)->SetCurState(ClickLbState::Selected);
        return;
    }

    // 没有 -> 新建一个推荐标签并标绿
    auto *lb = new ClickedLabel(ui->lb_list);
    lb->SetState("normal", "hover", "pressed",
                 "selected_normal", "selected_hover", "selected_pressed");
    lb->setObjectName("tipslb");
    lb->setText(text);
    lb->adjustSize(); // 让标签宽度跟随文字（含 QSS padding），避免文字被截断
    connect(lb, &ClickedLabel::Clicked, this, &ApplyFriend::slot_change_friend_label_by_tip);

    QFontMetrics fontMetrics(lb->font());
    int textWidth = fontMetrics.horizontalAdvance(lb->text());
    int textHeight = fontMetrics.height();

    if (_tip_cur_point.x() + textWidth + kTipOffset + 3 > ui->lb_list->width()) {
        _tip_cur_point.setX(5);
        _tip_cur_point.setY(_tip_cur_point.y() + textHeight + 15);
    }

    QPoint nextPoint = _tip_cur_point;
    AddTipLbs(lb, _tip_cur_point, nextPoint, textWidth, textHeight);
    _tip_cur_point = nextPoint;

    // 列表高度不够时撑高 lb_list，避免标签被截断
    ui->lb_list->setFixedHeight(nextPoint.y() + textHeight + kTipOffset);
    lb->SetCurState(ClickLbState::Selected);
}
