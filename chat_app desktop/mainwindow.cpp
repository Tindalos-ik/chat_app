#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "tcpmgr.h"

// 切换到注册界面的槽函数
void MainWindow::SlotSwitchReg()
{
    // 使用堆叠窗口的setCurrentWidget方法切换到注册界面
    // 这个方法会自动隐藏当前界面，显示目标界面，不会删除任何组件
    _stacked_widget->setCurrentWidget(_reg_dlg);
}

// 切换到登录界面的槽函数实现
void MainWindow::SlotSwitchLogin()
{
    // 使用堆叠窗口的setCurrentWidget方法切换回登录界面
    _stacked_widget->setCurrentWidget(_login_dlg);
    resize(300, 500);
}

void MainWindow::SlotSwitchReset()
{
    _stacked_widget->setCurrentWidget(_reset_dlg);
}

void MainWindow::SlotSwitchChat()
{
    _stacked_widget->setCurrentWidget(_chat_dlg);
    resize(1000, 750);
}

// 主窗口构造函数
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow){       // 创建UI对象
    ui->setupUi(this);              // 初始化UI界面（加载.ui文件内容）

    //创建堆叠窗口作为中央部件
    // QStackedWidget：可以管理多个页面，每次只显示其中一个
    _stacked_widget = new QStackedWidget(this);
    setCentralWidget(_stacked_widget);


    //父对象设置为this（主窗口），这些对话框会在主窗口销毁时自动释放
    _login_dlg = new LoginDialog(this);
    _reg_dlg = new RegisterDialog(this);
    _reset_dlg = new ResetDialog(this);
    _chat_dlg = new ChatDialog(this);

    // addWidget()方法会将部件添加到堆叠窗口的管理中
    // 添加后，堆叠窗口会给每个部件分配一个索引（从0开始）
    _stacked_widget->addWidget(_login_dlg);  // 登录对话框索引为0
    _stacked_widget->addWidget(_reg_dlg);    // 注册对话框索引为1
    _stacked_widget->addWidget(_reset_dlg);
    _stacked_widget->addWidget(_chat_dlg);

    // setCurrentWidget()方法会显示指定的部件，隐藏其他所有部件
    // 这里设置登录对话框为初始显示界面
    _stacked_widget->setCurrentWidget(_login_dlg);

    // 当登录对话框发出switchRegister信号时，执行切换到注册界面的槽函数
    connect(_login_dlg, &LoginDialog::switchRegister, this, &MainWindow::SlotSwitchReg);

    // 当注册对话框发出switchLogin信号时，执行切换到登录界面的槽函数
    connect(_reg_dlg, &RegisterDialog::switchLogin, this, &MainWindow::SlotSwitchLogin);

    connect(_login_dlg,&LoginDialog::switchReset,this,&MainWindow::SlotSwitchReset);

    connect(_reset_dlg,&ResetDialog::switchLogin,this,&MainWindow::SlotSwitchLogin);

    //当接收到登录成功信息的时候，切换聊天界面
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_switch_chatdlg, this, &MainWindow::SlotSwitchChat);

    emit TcpMgr::GetInstance()->sig_switch_chatdlg();

}

// 主窗口析构函数
MainWindow::~MainWindow()
{
    delete ui;
}
