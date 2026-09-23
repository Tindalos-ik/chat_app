#include "chatitembase.h"
#include "avatarutil.h"
#include <QDebug>
#include <QFont>
#include <QPixmap>
#include <QSpacerItem>
#include <QStringList>

ChatItemBase::ChatItemBase(ChatRole role, QWidget* parent)
    :QWidget(parent), m_role(role)
{
    m_pNameLabel = new QLabel();
    m_pNameLabel->setObjectName("chat_user_name");
    QFont font("Microsoft YaHei"); // 设置字体
    font.setPointSize(9);
    m_pNameLabel->setFont(font);
    m_pNameLabel->setFixedHeight(20);

    m_pIconLabel = new QLabel();
    m_pIconLabel->setFixedSize(40,40);

    // 气泡状态图标属于整条消息，而不是 TextBubble 本身：图片、文件等消息以后也
    // 可以复用同一位置。自己消息显示在气泡左侧，对方消息镜像放在气泡右侧。
    m_pSendStatusLabel = new QLabel();
    m_pSendStatusLabel->setFixedSize(18, 18);
    m_pSendStatusLabel->setAlignment(Qt::AlignCenter);
    m_pSendStatusLabel->setToolTip(QStringLiteral("消息发送失败"));
    m_pSendStatusLabel->hide();

    m_pBubble = new QWidget();
    m_pBubbleRowLayout = new QHBoxLayout();
    m_pBubbleRowLayout->setContentsMargins(0, 0, 0, 0);
    m_pBubbleRowLayout->setSpacing(3);
    auto *bubbleRow = new QWidget();
    bubbleRow->setLayout(m_pBubbleRowLayout);

    QGridLayout *pGlayout = new QGridLayout();
    pGlayout->setVerticalSpacing(3);
    pGlayout->setHorizontalSpacing(3); // 布局内组件垂直间距和水平间距都设置一下
    pGlayout->setContentsMargins(3,3,3,3);

    // 设置最小高度40，宽度20的可伸长的弹簧，把消息挤到左边或者右边
    QSpacerItem *pSpacer = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

    if(m_role == ChatRole::Self){
        m_pNameLabel->setContentsMargins(0,0,8,0);
        m_pNameLabel->setAlignment(Qt::AlignRight);
        // 状态图标和气泡先组成一个不可拆分的内容行，再由外层弹簧整体推到头像左侧。
        // 因此短文本、长用户名、图片和文件卡片都会保持“状态图标贴着气泡”。
        m_pBubbleRowLayout->addWidget(m_pSendStatusLabel, 0, Qt::AlignVCenter);
        m_pBubbleRowLayout->addWidget(m_pBubble, 0, Qt::AlignTop);
        pGlayout->addWidget(m_pNameLabel, 0,1,1,1);
        pGlayout->addWidget(m_pIconLabel, 0,2,2,1, Qt::AlignTop);
        pGlayout->addItem(pSpacer, 1,0,1,1);
        pGlayout->addWidget(bubbleRow, 1,1,1,1, Qt::AlignRight | Qt::AlignTop);
        pGlayout->setColumnStretch(0, 1);
        pGlayout->setColumnStretch(1, 0);
    }else{
        m_pNameLabel->setContentsMargins(8,0,0,0);
        m_pNameLabel->setAlignment(Qt::AlignLeft);
        // 对方消息使用镜像内容行；虽然正常接收消息不显示状态图标，失败提示仍与气泡相邻。
        m_pBubbleRowLayout->addWidget(m_pBubble, 0, Qt::AlignTop);
        m_pBubbleRowLayout->addWidget(m_pSendStatusLabel, 0, Qt::AlignVCenter);
        pGlayout->addWidget(m_pIconLabel, 0,0,2,1, Qt::AlignTop);
        pGlayout->addWidget(m_pNameLabel, 0,1,1,1);
        pGlayout->addWidget(bubbleRow, 1,1,1,1, Qt::AlignLeft | Qt::AlignTop);
        pGlayout->addItem(pSpacer, 1,2,1,1);
        pGlayout->setColumnStretch(1, 0);
        pGlayout->setColumnStretch(2, 1);
    }

    this->setLayout(pGlayout);

}

void ChatItemBase::setUserName(const QString &name)
{
    m_pNameLabel->setText(name);
}

void ChatItemBase::setUserAvatar(int uid, const QString &avatarPath)
{
    AvatarUtil::SetRoundAvatar(m_pIconLabel, uid, avatarPath);
}

void ChatItemBase::setWidget(QWidget *w)
{
    if (!w || !m_pBubbleRowLayout) {
        return;
    }
    QLayoutItem *oldItem = m_pBubbleRowLayout->replaceWidget(m_pBubble, w);
    delete oldItem;
    delete m_pBubble;
    m_pBubble = w;
}

void ChatItemBase::SetSendFailed(bool failed)
{
    if (!failed) {
        m_pSendStatusLabel->hide();
        return;
    }

    // 资源已在 resource.qrc 中注册。显式按标签尺寸缩放，避免高 DPI 或原图尺寸变化
    // 影响一整行消息的布局高度。
    const QPixmap icon(QStringLiteral(":/res/send_fail.png"));
    if (icon.isNull()) {
        qWarning() << "send failure icon resource is unavailable";
        return;
    }
    m_pSendStatusLabel->setPixmap(icon.scaled(m_pSendStatusLabel->size(),
                                               Qt::KeepAspectRatio,
                                               Qt::SmoothTransformation));
    m_pSendStatusLabel->show();
}

void ChatItemBase::SetDeliveryState(int state)
{
    if (m_role != ChatRole::Self || state < 1 || state > 4) {
        return;
    }
    // 这两个资源共用发送失败图标的位置，避免状态改变时让气泡横向跳动。服务端
    // status=1 或 1039 会传入 state=4；在此之前即使已实时投递或展示，仍显示
    // 未读图标，因为展示并不能证明对方读完了消息。
    static const QStringList labels = {
        QString(), QStringLiteral("已保存"), QStringLiteral("已送达"),
        QStringLiteral("已显示"), QStringLiteral("已读")
    };
    const QString resource = state == 4
        ? QStringLiteral(":/res/readed.png")
        : QStringLiteral(":/res/unread.png");
    const QPixmap icon(resource);
    if (icon.isNull()) {
        // 资源打包遗漏时仍展示文本，不让“对方已读”这个业务状态在 UI 中静默丢失。
        qWarning() << "delivery status icon resource is unavailable:" << resource;
        m_pSendStatusLabel->setPixmap(QPixmap());
        m_pSendStatusLabel->setText(labels.at(state));
        m_pSendStatusLabel->setStyleSheet(
            QStringLiteral("QLabel { color: #999999; font-size: 10px; }"));
        m_pSendStatusLabel->setMinimumWidth(36);
    } else {
        m_pSendStatusLabel->setText(QString());
        m_pSendStatusLabel->setPixmap(icon.scaled(m_pSendStatusLabel->size(),
                                                   Qt::KeepAspectRatio,
                                                   Qt::SmoothTransformation));
        m_pSendStatusLabel->setMinimumWidth(18);
    }
    m_pSendStatusLabel->setToolTip(labels.at(state));
    m_pSendStatusLabel->setFixedHeight(18);
    m_pSendStatusLabel->show();
}
