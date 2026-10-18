#include "mainwindow.h"
#include "tcpmgr.h"
#include "localchatstoragemgr.h"
#include "videotestcontroller.h"

#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QFont>
int main(int argc, char *argv[])
{
    // Chromium在QApplication初始化时读取scheme属性，提前注册才能让媒体页使用稳定安全origin。
    VideoTestController::RegisterScheme();
    QApplication a(argc, argv);
    a.setStyle(QStringLiteral("Fusion"));
    a.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));

    // 所有页面共用一套配色与控件状态；UTF-8 保留样式中的中文字体名。
    QFile qss(":/style/stylesheet.qss");

    if( qss.open(QFile::ReadOnly))
    {
        qDebug("open success");
        QString style = QString::fromUtf8(qss.readAll());
        a.setStyleSheet(style);
        qss.close();
    }else{
        qDebug("Open failed");
    }

    //读取配置
    QString filename = "config.ini";
    QString app_path = QCoreApplication::applicationDirPath(); //获取应用执行目录
    //linux中直接 / 就是文件路径，但是windows不行
    QString config_path = QDir::toNativeSeparators(app_path + QDir::separator() + filename);
    //使用QSetting读取配置，Qt原生
    QSettings settings(config_path,QSettings::IniFormat);
    QString gate_host = settings.value("GateServer/host").toString();
    QString gate_port = settings.value("GateServer/port").toString();
    gate_url_prefix = "http://" + gate_host + ":" + gate_port;
    qDebug() << "gate_server : " << gate_url_prefix << Qt::endl;


    //改变运行时候左上角图标
    a.setWindowIcon(QIcon(":/res/chat_app.png"));
    MainWindow w;
    w.show();

    // aboutToQuit 仍在 QApplication 生命周期内发出。先停止心跳并断开 socket 回调，
    // 避免窗口关闭使事件循环退出后，QTimer/QTcpSocket 的回调重入正在析构的 UI。
    QObject::connect(&a, &QCoreApplication::aboutToQuit, [] {
        TcpMgr::GetInstance()->PrepareForShutdown();
    });
    const int exitCode = QCoreApplication::exec();

    // 此处 QApplication 仍在作用域中，先释放 TcpMgr 才能安全销毁其心跳定时器。
    // 若等到静态单例析构阶段，QApplication 可能已经销毁，会触发 Qt6Core 崩溃。
    TcpMgr::DestroyInstance();
    // QSqlDatabase 的连接也应在 QApplication 仍存活时显式移除，避免 Qt 全局析构顺序不确定。
    LocalChatStorageMgr::DestroyInstance();
    return exitCode;
}
