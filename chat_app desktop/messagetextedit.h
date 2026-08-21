#ifndef MESSAGETEXTEDIT_H
#define MESSAGETEXTEDIT_H

#include <QObject>
#include <QTextEdit>
#include <QMouseEvent>
#include <QApplication>
#include <QDrag>
#include <QMimeData>
#include <QMimeType>
#include <QFileInfo>
#include <QFileIconProvider>
#include <QPainter>
#include <QVector>
#include "global.h"


// 消息输入框：在 QTextEdit 基础上扩展三类能力
//  1. 混合内容编辑：文字、图片、文件图标可以混排在一个编辑框里
//     - 图片/文件以"替换符"（QChar::ObjectReplacementCharacter，U+FFFC）插入文档，
//       真实内容（url / 缩放后的图）按插入顺序记在 mMsgList 里；
//     - 发送时 getMsgList() 遍历文档，把文字段落和替换符对应的 MsgInfo
//       按原顺序拼成 QVector<MsgInfo> 返回。
//  2. 拖拽/粘贴图片和文件：重写 dragEnterEvent / dropEvent / insertFromMimeData，
//     图片按比例缩放后插入，文件生成"图标+文件名+大小"的卡片图再插入。
//  3. 回车发送：Enter/Return（不带 Shift）→ emit send()；Shift+Enter 才是换行。
class MessageTextEdit : public QTextEdit
{
    Q_OBJECT
public:
    explicit MessageTextEdit(QWidget *parent = nullptr);

    ~MessageTextEdit();

    QVector<MsgInfo> getMsgList();                    // 解析文档，按顺序取出 文本/图片/文件 消息

    void insertFileFromUrl(const QStringList &urls);  // 从本地路径列表插入 图片/文件
signals:
    void send(); // 回车时发出，外层连接后执行"发送"

protected:
    void dragEnterEvent(QDragEnterEvent *event); // 允许把外部文件拖进来
    void dropEvent(QDropEvent *event);           // 拖放落下 → 走 insertFromMimeData
    void keyPressEvent(QKeyEvent *e);            // 拦截回车：无 Shift 回车 = 发送

private:
    void insertImages(const QString &url);                         // 插入图片（超 120x80 按比例缩放）
    void insertTextFile(const QString &url);                       // 插入文件（图标+文件名+大小拼成卡片图）
    bool canInsertFromMimeData(const QMimeData *source) const;     // 粘贴/拖入的 mime 是否允许插入
    void insertFromMimeData(const QMimeData *source);              // 把 mime 里的文件路径插入编辑框

private:
    bool isImage(QString url);                                                  // 按扩展名判断是否为图片
    void insertMsgList(QVector<MsgInfo> &list,QString flag, QString text, QPixmap pix); // 往列表里记一条消息

    QStringList getUrl(QString text);                  // 从 mime 文本里解析 file:/// 路径
    QPixmap getFileIconPixmap(const QString &url);     // 生成"文件图标+名称+大小"的预览图
    QString getFileSize(qint64 size);                  // 字节数转 B/KB/MB/GB 字符串

private slots:
    void textEditChanged(); // 预留：文本变化时的处理（目前没连接）

private:
    QVector<MsgInfo> mMsgList;    // 已插入的非文本内容（图片/文件），顺序与文档里的替换符一一对应
    QVector<MsgInfo> mGetMsgList; // getMsgList 的返回缓冲：按文档顺序拼好的完整消息列表
};

#endif // MESSAGETEXTEDIT_H
