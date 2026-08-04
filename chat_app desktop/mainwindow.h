#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QStackedWidget>  // 包含堆叠窗口头文件
#include "logindialog.h"   // 登录对话框头文件
#include "registerdialog.h" // 注册对话框头文件
#include "resetdialog.h"

/******************************************************************************
 *
 * @file       mainwindow.h
 * @brief      主窗口
 * 切换界面的逻辑在这里实现，也就是说，这个类成员有很多对话框，对话框头文件里面负责发送信号，这个主窗口来实现界面切换
 *
 * @author     klein
 *****************************************************************************/

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    LoginDialog* getLoginDlg(){
        return _login_dlg;
    }

private slots:
    void SlotSwitchReg();    // 切换到注册界面的槽函数
    void SlotSwitchLogin();  // 切换到登录界面的槽函数
    void SlotSwitchReset();

private:
    Ui::MainWindow *ui;
    QStackedWidget *_stacked_widget;  // 堆叠窗口，用于管理多个界面
    LoginDialog *_login_dlg;          // 登录对话框指针
    RegisterDialog *_reg_dlg;         // 注册对话框指针
    ResetDialog *_reset_dlg;

};

#endif // MAINWINDOW_H