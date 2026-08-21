#include "MessageTextEdit.h"
#include <QDebug>
#include <QMessageBox>
#include <QUrl>
#include <QDir>
#include <QUuid>
#include <QVariant>


MessageTextEdit::MessageTextEdit(QWidget *parent)
    : QTextEdit(parent)
{
    //this->setStyleSheet("border: none;");
    // 输入框最高 60px：内容多了内部自己滚，不把窗口撑爆
    this->setMaximumHeight(60);

    // textEditChanged 的连接被注释掉：目前不需要随输入实时处理，
    // 将来要做"输入变化 → 更新发送按钮状态"时再打开
    //    connect(this,SIGNAL(textChanged()),this,SLOT(textEditChanged()));

}

MessageTextEdit::~MessageTextEdit()
{

}

// 核心方法：把编辑框里的"富文本"解析成有序的 MsgInfo 列表，供外层发送
QVector<MsgInfo> MessageTextEdit::getMsgList()
{
    mGetMsgList.clear(); // 清空上一次的结果

    // 文档的纯文本：图片/文件在纯文本里只是一个"替换符" QChar::ObjectReplacementCharacter
    QString doc = this->document()->toPlainText();
    QString text="";   // 正在累积的文本段落
    int indexUrl = 0;  // 游标：mMsgList 里已经匹配到第几个
    int count = mMsgList.size(); // 非文本内容总数（图片+文件）

    for(int index=0; index<doc.size(); index++)
    {
        // 遇到替换符 = 这里原本插入了一个图片/文件
        if(doc[index]==QChar::ObjectReplacementCharacter)
        {
            // 先把替换符前面的文字段落收尾
            if(!text.isEmpty())
            {
                QPixmap pix;
                insertMsgList(mGetMsgList,"text",text,pix);
                text.clear();
            }
            // 替换符在纯文本里的出现顺序 == mMsgList 的插入顺序，直接按顺序配对。
            // 不要用 toHtml().contains(url) 匹配：路径含中文/空格时 HTML 会转义 url，
            // 匹配不上会把图片/文件消息丢掉（这就是"发送不了图片"的坑）
            if(indexUrl<count)
            {
                mGetMsgList.append(mMsgList[indexUrl]);
                indexUrl++;
            }
        }
        else
        {
            // 普通字符，累积到当前文本段落
            text.append(doc[index]);
        }
    }
    // 收尾：文档末尾可能还剩一段文字
    if(!text.isEmpty())
    {
        QPixmap pix;
        insertMsgList(mGetMsgList,"text",text,pix);
        text.clear();
    }
    // 发送完就清空：mMsgList 和编辑框内容都清掉，等待下一次输入
    mMsgList.clear();
    this->clear();
    return mGetMsgList;
}

// 拖拽进入：只接受"从外部拖进来的"；从本控件内部拖出（选中文字拖走）忽略
void MessageTextEdit::dragEnterEvent(QDragEnterEvent *event)
{
    if(event->source()==this)
        event->ignore();
    else
        event->accept();
}

// 拖放落下：把 mime 里的文件路径解析出来并插入编辑框
void MessageTextEdit::dropEvent(QDropEvent *event)
{
    insertFromMimeData(event->mimeData());
    event->accept();
}

// 回车发送：不带 Shift 的回车 = 发送；带 Shift 的回车 = 换行（走默认行为）
void MessageTextEdit::keyPressEvent(QKeyEvent *e)
{
    if((e->key()==Qt::Key_Enter||e->key()==Qt::Key_Return)&& !(e->modifiers() & Qt::ShiftModifier))
    {
        emit send();
        return;
    }
    QTextEdit::keyPressEvent(e);
}

// 外部直接传入本地路径列表（不一定是拖拽），逐条按类型插入
void MessageTextEdit::insertFileFromUrl(const QStringList &urls)
{
    if(urls.isEmpty())
        return;

    foreach (QString url, urls){
        if(isImage(url))
            insertImages(url);
        else
            insertTextFile(url);
    }
}

// 插入图片：超过 120x80 按比例缩小（宽图按宽、高图按高），插入文档并记入 mMsgList
void MessageTextEdit::insertImages(const QString &url)
{
    QImage image(url);
    // 按比例缩放图片
    if(image.width()>120||image.height()>80)
    {
        if(image.width()>image.height())
        {
            image =  image.scaledToWidth(120,Qt::SmoothTransformation);
        }
        else
            image = image.scaledToHeight(80,Qt::SmoothTransformation);
    }
    QTextCursor cursor = this->textCursor();
    // QTextDocument *document = this->document();
    // document->addResource(QTextDocument::ImageResource, QUrl(url), QVariant(image));
    // 在光标处插入图片；url 作为资源标识，之后 HTML 里会出现它，getMsgList 靠它匹配回这条消息
    cursor.insertImage(image,url);

    insertMsgList(mMsgList,"image",url,QPixmap::fromImage(image));
}

// 插入文件：非图片一律按文件处理
void MessageTextEdit::insertTextFile(const QString &url)
{
    QFileInfo fileInfo(url);
    if(fileInfo.isDir())
    {
        QMessageBox::information(this,"提示","只允许拖拽单个文件!");
        return;
    }

    if(fileInfo.size()>100*1024*1024)
    {
        QMessageBox::information(this,"提示","发送的文件大小不能大于100M");
        return;
    }

    // 把"系统图标+文件名+大小"画成一张卡片图，插入编辑框（视觉上就是一个文件卡片）
    QPixmap pix = getFileIconPixmap(url);
    QTextCursor cursor = this->textCursor();
    cursor.insertImage(pix.toImage(),url);
    insertMsgList(mMsgList,"file",url,pix);
}

// 重写 mime 插入判定：图片、文件、普通文本都允许插入
bool MessageTextEdit::canInsertFromMimeData(const QMimeData *source) const
{
    return source->hasImage() || source->hasUrls() || source->hasText();
}

// 粘贴/拖入的统一入口：优先处理图片数据，其次处理文件，最后普通文本交给 QTextEdit
void MessageTextEdit::insertFromMimeData(const QMimeData *source)
{
    // 1. 剪贴板里是图片数据（截图、复制网页图片）：存到临时文件后按图片插入
    if (source->hasImage()) {
        QImage image = qvariant_cast<QImage>(source->imageData());
        if (!image.isNull()) {
            const QString tmpFile = QDir::temp().filePath(
                QStringLiteral("chat_paste_%1.png")
                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
            if (image.save(tmpFile)) {
                insertImages(tmpFile);
            }
        }
        return;
    }

    // 2. 拖拽/复制文件：优先用 urls() 拿真实路径（比解析 text 更可靠）
    QStringList urls;
    if (source->hasUrls()) {
        const auto fileUrls = source->urls();
        for (const QUrl &url : fileUrls) {
            if (url.isLocalFile()) {
                urls.append(url.toLocalFile());
            }
        }
    }
    if (urls.isEmpty()) {
        urls = getUrl(source->text()); // 兜底：解析 file:/// 文本
    }

    if (!urls.isEmpty()) {
        for (const QString &url : urls) {
            if (isImage(url)) {
                insertImages(url);
            } else {
                insertTextFile(url);
            }
        }
        return;
    }

    // 3. 普通文本粘贴等：交给 QTextEdit 默认处理
    QTextEdit::insertFromMimeData(source);
}

// 判断文件是不是图片：按扩展名匹配常见图片格式（大小写不敏感）
bool MessageTextEdit::isImage(QString url)
{
    QString imageFormat = "bmp,jpg,png,tif,gif,pcx,tga,exif,fpx,svg,psd,cdr,pcd,dxf,ufo,eps,ai,raw,wmf,webp";
    QStringList imageFormatList = imageFormat.split(",");
    QFileInfo fileInfo(url);
    QString suffix = fileInfo.suffix();
    if(imageFormatList.contains(suffix,Qt::CaseInsensitive)){
        return true;
    }
    return false;
}

// 组装一条 MsgInfo 并追加到列表（文本/图片/文件统一走这里）
void MessageTextEdit::insertMsgList(QVector<MsgInfo> &list, QString flag, QString text, QPixmap pix)
{
    MsgInfo msg;
    msg.msgFlag=flag;  // "text" / "image" / "file"
    msg.content = text; // 文本内容 或 图片/文件的 url
    msg.pixmap = pix;  // 图片/文件预览图（文本时为空）
    list.append(msg);
}

// 从 mime 的纯文本里解析本地路径：拖拽时 text 形如 "file:///C:/a.png\nfile:///D:/b.txt"
QStringList MessageTextEdit::getUrl(QString text)
{
    QStringList urls;
    if(text.isEmpty()) return urls;

    QStringList list = text.split("\n");
    foreach (QString url, list) {
        if(!url.isEmpty()){
            QStringList str = url.split("///");
            if(str.size()>=2) // 去掉 "file:///" 前缀，拿到真实路径
                urls.append(str.at(1));
        }
    }
    return urls;
}

// 生成文件卡片预览图：左边系统文件图标，右边文件名 + 大小
QPixmap MessageTextEdit::getFileIconPixmap(const QString &url)
{
    QFileIconProvider provder;
    QFileInfo fileinfo(url);
    QIcon icon = provder.icon(fileinfo);

    QString strFileSize = getFileSize(fileinfo.size());
    //qDebug() << "FileSize=" << fileinfo.size();

    QFont font(QString("宋体"),10,QFont::Normal,false);
    QFontMetrics fontMetrics(font);
    QSize textSize = fontMetrics.size(Qt::TextSingleLine, fileinfo.fileName()); // 文件名占宽

    QSize FileSize = fontMetrics.size(Qt::TextSingleLine, strFileSize);         // 大小文字占宽
    int maxWidth = textSize.width() > FileSize.width() ? textSize.width() :FileSize.width();
    QPixmap pix(50 + maxWidth + 10, 50); // 宽 = 图标50 + 文字最大宽 + 间距；高固定 50
    pix.fill();

    QPainter painter;
    // painter.setRenderHint(QPainter::Antialiasing, true);
    //painter.setFont(font);
    painter.begin(&pix);
    // 左 50x50 画系统文件图标
    QRect rect(0, 0, 50, 50);
    painter.drawPixmap(rect, icon.pixmap(40,40));
    painter.setPen(Qt::black);
    // 右上画文件名
    QRect rectText(50+10, 3, textSize.width(), textSize.height());
    painter.drawText(rectText, fileinfo.fileName());
    // 右下画文件大小
    QRect rectFile(50+10, textSize.height()+5, FileSize.width(), FileSize.height());
    painter.drawText(rectFile, strFileSize);
    painter.end();
    return pix;
}

// 字节数转可读字符串（B / KB / MB / GB，保留两位小数）
QString MessageTextEdit::getFileSize(qint64 size)
{
    QString Unit;
    double num;
    if(size < 1024){
        num = size;
        Unit = "B";
    }
    else if(size < 1024 * 1224){
        num = size / 1024.0;
        Unit = "KB";
    }
    else if(size <  1024 * 1024 * 1024){
        num = size / 1024.0 / 1024.0;
        Unit = "MB";
    }
    else{
        num = size / 1024.0 / 1024.0/ 1024.0;
        Unit = "GB";
    }
    return QString::number(num,'f',2) + " " + Unit;
}

// 预留槽：目前没有 connect，将来可用于"输入变化 → 更新发送按钮状态"
void MessageTextEdit::textEditChanged()
{
    //qDebug() << "text changed!" << endl;
}
