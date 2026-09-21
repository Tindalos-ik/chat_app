#include "chatitembase.h"
#include "avatarutil.h"
#include <QDebug>
#include <QFont>
#include <QPixmap>
#include <QSpacerItem>

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

    QGridLayout *pGlayout = new QGridLayout();
    pGlayout->setVerticalSpacing(3);
    pGlayout->setHorizontalSpacing(3); // 布局内组件垂直间距和水平间距都设置一下
    pGlayout->setContentsMargins(3,3,3,3);

    // 设置最小高度40，宽度20的可伸长的弹簧，把消息挤到左边或者右边
    QSpacerItem *pSpacer = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

    if(m_role == ChatRole::Self){
        m_pNameLabel->setContentsMargins(0,0,8,0);
        m_pNameLabel->setAlignment(Qt::AlignRight);
        // 自己消息：弹簧 | 状态图标 | 气泡 | 头像。状态图标在气泡左端，失败时
        // 不会挤压头像，也不会改变气泡内文字的宽度。
        pGlayout->addWidget(m_pNameLabel, 0,2,1,1);
        pGlayout->addWidget(m_pIconLabel, 0,3,2,1, Qt::AlignTop);
        pGlayout->addItem(pSpacer, 1,0,1,1);
        pGlayout->addWidget(m_pSendStatusLabel, 1,1,1,1, Qt::AlignRight | Qt::AlignVCenter);
        pGlayout->addWidget(m_pBubble, 1,2,1,1, Qt::AlignRight | Qt::AlignTop);
        pGlayout->setColumnStretch(0,2); // 第零行占40%比例
        pGlayout->setColumnStretch(2,3); // 气泡列占60%比例
    }else{
        m_pNameLabel->setContentsMargins(8,0,0,0);
        m_pNameLabel->setAlignment(Qt::AlignLeft);
        // 对方消息与自己消息镜像：左头像、气泡、状态图标、右侧弹簧。
        pGlayout->addWidget(m_pIconLabel, 0,0,2,1, Qt::AlignTop);
        pGlayout->addWidget(m_pNameLabel, 0,1,1,1);
        pGlayout->addWidget(m_pBubble, 1,1,1,1, Qt::AlignLeft | Qt::AlignTop);
        pGlayout->addWidget(m_pSendStatusLabel, 1,2,1,1, Qt::AlignLeft | Qt::AlignVCenter);
        pGlayout->addItem(pSpacer, 1,3,1,1);
        pGlayout->setColumnStretch(1,3);
        pGlayout->setColumnStretch(3,2);
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
    QGridLayout *pGlayout = (qobject_cast<QGridLayout*>(this->layout()));
    pGlayout->replaceWidget(m_pBubble, w);
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
