#include "chatitembase.h"
#include <QFont>
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
    m_pIconLabel->setScaledContents(true); // 允许伸缩
    m_pIconLabel->setFixedSize(40,40);

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
        // 放在网格布局第零行第一列，占用一行一列
        pGlayout->addWidget(m_pNameLabel, 0,1,1,1);
        pGlayout->addWidget(m_pIconLabel, 0,2,2,1, Qt::AlignTop);
        pGlayout->addItem(pSpacer, 1,0,1,1);
        pGlayout->addWidget(m_pBubble, 1,1,1,1);
        pGlayout->setColumnStretch(0,2); // 第零行占40%比例
        pGlayout->setColumnStretch(1,3); // 第一行占60%比例
    }else{
        m_pNameLabel->setContentsMargins(8,0,0,0);
        m_pNameLabel->setAlignment(Qt::AlignLeft);
        pGlayout->addWidget(m_pNameLabel, 0,0,2,1);
        pGlayout->addWidget(m_pIconLabel, 0,1,1,1, Qt::AlignTop);
        pGlayout->addWidget(m_pBubble, 1,1,1,1);
        pGlayout->addItem(pSpacer, 1,2,1,1);
        pGlayout->setColumnStretch(1,3);
        pGlayout->setColumnStretch(2,2);
    }

    this->setLayout(pGlayout);

}

void ChatItemBase::setUserName(const QString &name)
{
    m_pNameLabel->setText(name);
}

void ChatItemBase::setUserIcon(const QPixmap &icon)
{
    m_pIconLabel->setPixmap(icon);
}

void ChatItemBase::setWidget(QWidget *w)
{
    QGridLayout *pGlayout = (qobject_cast<QGridLayout*>(this->layout()));
    pGlayout->replaceWidget(m_pBubble, w);
    delete m_pBubble;
    m_pBubble = w;
}
