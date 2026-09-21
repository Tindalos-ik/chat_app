#include "mainwindow.h"
#include "tcpmgr.h"
#include "localchatstoragemgr.h"

#include <QApplication>
#include <QDebug>
#include <QFile>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    //使用qss文件美化一下
    //在这之前要把这个文件添加进去资源文件，也就是qrc文件里面
    QFile qss(":/style/stylesheet.qss");

    if( qss.open(QFile::ReadOnly))
    {
        qDebug("open success");
        QString style = QLatin1String(qss.readAll());
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
