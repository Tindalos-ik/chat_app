#ifndef APPLYFRIEND_H
#define APPLYFRIEND_H

#include <QDialog>
#include <QMap>
#include <QPoint>
#include <memory>
#include <vector>
#include "clickedlabel.h"
#include "friendlabel.h"
#include "userdata.h"

namespace Ui {
class ApplyFriend;
}

// 添加好友申请弹窗（在 FindSuccessDlg 里点"添加到通讯录"后弹出）
// 功能：
//   1. 验证消息（name_ed）、备注名（back_ed）
//   2. 添加标签：在 lb_ed 输入后回车生成 FriendLabel 胶囊，可点叉删除；
//      上方一排推荐标签（tip）点击可一键添加
//   3. 确认（sure_btn）/ 取消（cancel_btn）
class ApplyFriend : public QDialog
{
    Q_OBJECT

public:
    explicit ApplyFriend(QWidget *parent = nullptr);
    ~ApplyFriend();

    // 初始化推荐标签（tip）：把预设标签文字排成一行行可点击的标签
    void InitTipLbs();

    // 排版一个推荐标签：按 text_width 判断是否需要换行，
    // next_point（引用）返回下一个标签应放置的位置
    void AddTipLbs(ClickedLabel *label, QPoint cur_point,
                   QPoint &next_point, int text_width, int text_height);

    // 事件过滤器：拦截 lb_ed 的输入/回车/失焦，用来生成或删除好友标签
    bool eventFilter(QObject *obj, QEvent *event) override;

    // 设置搜索到的用户信息（由 FindSuccessDlg 调用，确认时拼申请数据用）
    void SetSearchInfo(std::shared_ptr<SearchInfo> si);

public slots:
    // 点"更多"：把折叠隐藏的推荐标签展开显示
    void slot_show_more_label();

    // 推荐标签被点中（进入选中态）：显示"加为好友标签"的提示
    void slot_label_enter();

    // 好友标签点关闭叉：从展示列表移除该标签
    void slot_remove_friend_label(const QString &text);

    // 点击推荐标签：选中=添加好友标签，取消选中=移除好友标签
    // 用 sender() 取被点的标签（ClickedLabel::Clicked 信号不带参数）
    void slot_change_friend_label_by_tip();

    // lb_ed 文字变化：动态过滤推荐标签提示
    void slot_label_text_change(const QString &text);

    // lb_ed 编辑完成（回车）：把当前输入的文字固化为好友标签
    void slot_label_edit_finished();

    // 点击"添加标签 xxx"提示：直接把该文字添加为好友标签
    void slot_add_friend_label_by_tip(const QString &text);

    // 点"确认"：拼好友申请数据并发送（TODO：等后端协议）
    void slot_apply_sure();

    // 点"取消"：关闭弹窗
    void slot_apply_cancel();

private:
    // 重置所有标签的布局（增删标签后整体换行重排）
    void resetLabels();

    // 把一个文字添加为好友标签：创建 FriendLabel 放进 lb_list 并连接关闭信号
    void addLabel(const QString &name);

    // 在推荐标签栏（lb_list）追加一个标签并标绿；不存在则新建
    void appendTipLb(const QString &text);

    Ui::ApplyFriend *ui;

    // 推荐标签（tip）：文字 -> 标签控件，_tip_data 记录预设标签顺序
    QMap<QString, ClickedLabel*> _add_labels;
    std::vector<QString> _add_label_keys;
    std::vector<QString> _tip_data; // 预设的推荐标签文字
    QPoint _label_point;            // 推荐标签排版时的当前坐标
    QPoint _tip_cur_point;          // 推荐标签当前排版位置

    // 已添加的好友标签：文字 -> FriendLabel 胶囊
    QMap<QString, FriendLabel*> _friend_labels;
    std::vector<QString> _friend_label_keys;

    std::shared_ptr<SearchInfo> _si; // 搜索到的用户信息
    QWidget *_parent;                // 记录父窗口，后续弹下一级窗口用
    bool _b_show_all_tips = false;   // 推荐标签是否已全部展开（"更多"/"折叠"切换）
};

#endif // APPLYFRIEND_H
