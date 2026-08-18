#include "loadingdlg.h"
#include "ui_loadingdlg.h"
#include <QMovie>
#include <QPoint>

LoadingDlg::LoadingDlg(QWidget *parent, QString tip)
    : QDialog(parent)
    , ui(new Ui::LoadingDlg)
{
    ui->setupUi(this);

    // 无边框、置顶、背景透明：做成盖在父窗口上的遮罩
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint
                   | Qt::WindowSystemMenuHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);

    // 覆盖父窗口整个面积（没有父窗口时保持 ui 里的默认尺寸）
    if (parent) {
        setFixedSize(parent->size());
        move(parent->mapToGlobal(QPoint(0, 0)));
    }

    // 转圈动画
    auto *movie = new QMovie(this); // 挂在this有对象树统一回收
    movie->setFileName(":/res/loading.gif");
    movie->setScaledSize(ui->loading_lb->size());
    ui->loading_lb->setMovie(movie);
    movie->start();

    ui->status_lb->setText(tip);
}

LoadingDlg::~LoadingDlg()
{
    delete ui;
}
