# Qt 客户端知识点梳理（chat_app desktop）

> 对应目录：`chat_app desktop/`（Qt 6.8 + CMake，客户端只有这一个 Qt 工程）。
> 本文从代码出发，梳理 Qt 的核心机制、每个类的设计意图，以及"为什么这样设计、有什么好处"。
>
> 网络编程（HTTP + TCP）部分见 [Qt网络编程.md](Qt网络编程.md)

---

## 0. 客户端结构

```text
main.cpp（启动装配：QSS、配置、窗口图标、MainWindow）
  │
  └─ MainWindow（QMainWindow）
       └─ QStackedWidget（页面容器，只显示其中一个页面）
            ├─ LoginDialog      登录
            ├─ RegisterDialog   注册
            └─ ResetDialog      重置密码
                    │  发 HTTP 请求 / 收 HTTP 回包
                    ▼
              HttpMgr（单例）──── QNetworkAccessManager
                    │
                    ▼
              TcpMgr（单例）──── QTcpSocket（与 ChatServer 长连接）
```

整个客户端分成两层：

- **UI 层**（三个 Dialog）：只负责用户输入校验、发请求、展示结果；
- **网络层**（HttpMgr / TcpMgr）：只负责发请求、收数据、把结果分发出去。

两层之间**只通过信号和槽通信**，UI 不认识网络层的内部实现，网络层也不认识任何一个对话框。这是本项目最核心的设计思想。

---

## 1. 文件清单与职责

| 文件 | 职责 | 涉及的知识点 |
| --- | --- | --- |
| `main.cpp` | 程序入口 | QApplication、全局 QSS、QSettings 读配置、资源图标 |
| `mainwindow.h/cpp` | 主窗口、页面切换 | QStackedWidget、父子对象、信号→槽 |
| `logindialog.h/cpp` | 登录页 | 表单校验、HTTP 登录、发起 TCP 连接 |
| `registerdialog.h/cpp` | 注册页 | 邮箱正则、验证码、注册成功延时返回 |
| `resetdialog.h/cpp` | 重置密码页 | 与注册页几乎同构的第三份"模板" |
| `global.h/cpp` | 公共定义 | 协议枚举、错误码、repolish / xorString |
| `singleton.h` | 单例模板基类 | 模板 + 静态多态 + std::call_once |
| `httpmgr.h/cpp` | HTTP 管理器 | 单例、异步请求、信号分发 |
| `tcpmgr.h/cpp` | TCP 管理器 | 粘包/半包解析、handler 注册表 |
| `clickedlabel.h/cpp` | 可点击图片标签 | 事件重写、属性状态机、自定义信号 |
| `timerbtn.h/cpp` | 倒计时按钮 | 事件重写 + QTimer |
| `*.ui` | 界面布局 | Qt Designer、uic 生成 ui_*.h、控件提升 |
| `resource.qrc` | 资源打包 | `:/` 前缀、CMAKE_AUTORCC |
| `style/stylesheet.qss` | 全局样式 | 属性选择器、动态换肤 |
| `config.ini` | 运行配置 | 由 CMake 复制到构建目录，QSettings 读取 |

---

## 2. Qt 核心机制

### 2.1 元对象系统与 Q_OBJECT

Qt 在标准 C++ 之上增加了一套**元对象系统（Meta-Object System）**，给 C++ 类扩展了信号槽、属性、反射等能力。凡是需要信号槽的类，必须：

1. 继承 `QObject`（**且必须是第一个基类**，moc 会依赖这一点）；
2. 在类体内写 `Q_OBJECT` 宏；
3. 构建时经过 **moc** 预处理（CMake 里由 `qt_standard_project_setup()` / AUTOMOC 自动完成）。

本项目中的 `HttpMgr`、`TcpMgr`、`ClickedLabel`、`TimerBtn`、三个 Dialog、MainWindow 都写了 `Q_OBJECT`。

```cpp
class HttpMgr : public QObject, public Singleton<HttpMgr>,
                public std::enable_shared_from_this<HttpMgr>
{
    Q_OBJECT
    ...
};
```

**为什么这样设计**：C++ 本身没有信号槽，Qt 用 moc 在编译期扫描带 `Q_OBJECT` 的类，生成元信息代码（metaobject、信号函数体、`qt_static_metacall` 等）。宏 + 工具链的配合让"信号槽"用起来像语言特性，同时保持源码可读性。

### 2.2 信号与槽

信号槽本质是**观察者模式**：对象状态变化时发出信号，事先 connect 的槽函数被执行。信号和槽都是普通成员函数，`emit` 只是一个空宏（提示性），真正的分发由 moc 生成的代码完成。

本项目用了三种 connect 写法：

```cpp
// 1) 传统写法：按钮 clicked → 自定义槽（这里实际是信号连信号，见 4.3）
connect(ui->reg_btn, &QPushButton::clicked, this, &LoginDialog::switchRegister);

// 2) lambda 槽：HTTP 回包处理（异步回调内常用）
connect(reply, &QNetworkReply::finished, [self, reply, req_id, mod]{
    ...
    emit self->sig_http_finish(req_id, res, ErrorCodes::SUCCESS, mod);
});

// 3) 自定义信号 → 自定义槽
connect(HttpMgr::GetInstance().get(), &HttpMgr::sig_login_mod_finish,
        this, &LoginDialog::slot_login_mod_finish);
```

**参数匹配规则**：槽函数的参数个数 ≤ 信号的参数个数，且从前往后类型一致，多余的信号参数被丢弃。例如 `slot_http_finish(ReqId, QString, ErrorCodes, Modules)` 与 `sig_http_finish(ReqId, QString, ErrorCodes, Modules)` 完全一致；而 `ClickedLabel::Clicked(void)` 连接的 lambda 无参数。

**为什么这样设计**：

- **类型安全**：编译期检查信号槽签名，比回调函数指针更安全；
- **松耦合**：发送方不知道接收方是谁、有几个接收方，新增订阅者不用改发送方代码；
- **连接方式可配置**：同线程默认直接调用，跨线程自动用队列连接（事件循环中转），也可以显式指定 `Qt::QueuedConnection`；
- **支持多对多**：一个信号接多个槽、多个信号接一个槽、信号接信号，组合能力强。

#### 高并发下为什么更安全

这是信号槽相对"直接调用函数/回调"最重要的优势之一，核心在 Qt 的**连接类型（Connection Type）**和**线程亲和性**。

先看直接函数调用在高并发下要自己解决的三件事：

* 跨线程调用共享对象 → 手动加 mutex；
* 异步回调到达时对象可能已销毁 → 悬垂指针崩溃；
* 回调可能重入 → 自己的状态被半路打断。

信号槽把这几个问题内置解决了：

**① 跨线程自动改用队列连接**

connect 默认的 `AutoConnection` 会判断发送者和接收者是否在同一线程：

* 同线程 → `DirectConnection`，直接调用（快，但语义和普通函数一样）；
* 不同线程 → `QueuedConnection`，把"这次调用"连同参数打包成 `QMetaCallEvent`，投递到接收者所在线程的事件循环，等接收线程空闲时再执行槽函数。

效果：**接收者的槽永远在自己线程里执行，它的状态不会被别的线程同时触碰**，天然串行，不需要为接收者加锁。

```text
线程A：emit sig(...)
   │ 打包成事件，投递到接收者线程的事件队列
   ▼
线程B（接收者所属线程）：事件循环取出事件 → 执行槽函数
```

最常见的应用就是"工作线程把结果发给 GUI 线程"：槽在 GUI 线程执行，直接更新界面，不用加锁。

**② 排队效果**

多个线程同时 emit 同一个信号时，事件逐个进入队列、逐个被处理，槽函数不会互相打断。本项目 TcpMgr 里那句注释说的就是这个：

```cpp
// 为什么不直接调用tcpmgr发给服务器呢？在高并发的情况下，实现一个类似排队的效果
TcpMgr::GetInstance()->sig_send_data(ReqId::ID_CHAT_LOGIN, jsonString);
```

将来把连接显式改成 `QueuedConnection`，请求就会排进事件循环按序执行，且**调用方代码一行都不用改**——这就是"连接方式可配置"带来的安全余量。

**③ 接收者销毁自动断开**

connect 的第三个参数（接收者 context）是 QObject 指针，Qt 会在它销毁时自动移除相关连接。跨线程异步场景下，即使槽还没执行、接收者先被销毁，也不会出现"回调时对象已死"的悬垂调用。

**④ 避免重入**

`DirectConnection` 是同步调用，槽执行到一半可能又被同一条路径触发（重入）。`QueuedConnection` 把调用串行化进事件循环，一个槽执行期间不会被同一个信号重入打断，状态更可控。

注意事项（不能神化）：

* `QueuedConnection` 有开销：参数要复制进事件队列（类型需要可拷贝），高频小信号用队列连接不如直连快；
* 它保证"槽在接收者线程里串行执行"，但不保证两个对象**共享同一份可变数据**时的安全——共享状态该加锁还得加锁；
* 同线程默认是 `DirectConnection`，所以"并发安全"这个优势主要针对跨线程场景。

回到本项目：目前 HttpMgr / TcpMgr 和所有界面都住在主线程，连接全是直连；将来如果把网络收发挪到工作线程，信号槽会自动切到队列连接，业务代码不用动——这就是"高并发下更安全"的工程价值。

### 2.3 事件系统：重写事件 vs 信号槽

Qt 有两套"通知机制"：

| | 事件（event） | 信号槽 |
| --- | --- | --- |
| 谁发的 | 系统/框架（鼠标、键盘、定时器等） | 任意对象主动 emit |
| 怎么处理 | 重写 `xxxEvent` 虚函数 | connect 连接槽函数 |
| 传递 | 可被 accept/ignore，沿父对象链传播 | 无传播，点对点 |
| 典型用途 | 底层行为定制 | 业务逻辑解耦 |

本项目 `ClickedLabel` 重写 `mousePressEvent` / `enterEvent` / `leaveEvent` 定制鼠标交互，`TimerBtn` 重写 `mouseReleaseEvent` 定制点击行为。它们都把"处理完的事件"转换成**业务信号**（`Clicked`、`clicked`）再抛出去，使用方只需要 connect，不需要关心事件细节。

注意：重写事件函数时，最后要调用**基类版本**（如 `QLabel::mousePressEvent(ev)`），保证默认行为（如点击效果、事件继续传播）不被破坏。

#### 事件过滤器（Event Filter）

事件过滤器是 Qt 事件系统的第三层干预手段：**在事件到达目标控件之前，安排一个"哨兵"先检查它**。

```cpp
目标对象->installEventFilter(哨兵对象);              // 安装过滤器
bool 哨兵对象::eventFilter(QObject *watched, QEvent *event); // 每个事件先过这里
```

事件的处理顺序：

```text
事件产生
  ↓
事件过滤器 eventFilter()      ← 最先看到事件（可拦截）
  ↓
目标对象的 event()             ← 分发
  ↓
具体处理函数（mousePressEvent / wheelEvent ...）
```

`eventFilter` 返回 `true` = 事件被吞掉，不再往下传；返回 `false` = 放行，走正常流程。

**和"重写事件函数"的区别**：重写是"我自己处理自己的事件"（在目标类内部）；事件过滤器是"第三方插队旁观/拦截别人的事件"，**不用修改目标类的代码**，还能用同一个过滤器监控多个对象（在 `eventFilter` 里按 `watched` 分流）。

**本项目/参考项目实例：ChatUserList（仿微信的聊天列表）**

```cpp
ChatUserList::ChatUserList(QWidget *parent)
    : QListWidget(parent)
{
    this->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 装到 viewport() 而不是 this：悬浮/滚轮事件是发给 viewport 的
    this->viewport()->installEventFilter(this);
}

bool ChatUserList::eventFilter(QObject *watched, QEvent *event)
{
    // ① 鼠标进入/离开：显示/隐藏滚动条（仿微信的"悬浮才出现滚动条"）
    if (watched == this->viewport()) {
        if (event->type() == QEvent::Enter) {
            this->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        } else if (event->type() == QEvent::Leave) {
            this->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        }
    }

    // ② 滚轮事件：自定义滚动步长 + 滚到底部触发"加载更多"
    if (watched == this->viewport() && event->type() == QEvent::Wheel) {
        QWheelEvent *wheelEvent = static_cast<QWheelEvent*>(event);
        int numSteps = (wheelEvent->angleDelta().y() / 8) / 15;
        this->verticalScrollBar()->setValue(
            this->verticalScrollBar()->value() - numSteps);

        QScrollBar *scrollBar = this->verticalScrollBar();
        int maxScrollValue = scrollBar->maximum();
        int currentValue = scrollBar->value();
        // 有滚动余地且到达/接近底部（留 10px 余量）才触发，避免内容不满一屏时误触发
        if (maxScrollValue > 0 && maxScrollValue - currentValue <= 10) {
            emit sig_loading_chat_user();     // 让外层去加载下一页联系人
        }
        return true;  // 返回 true：拦截默认滚轮行为，避免滚两次
    }

    // ③ 其他事件放行，走正常流程（漏了这行会吞掉所有事件！）
    return QListWidget::eventFilter(watched, event);
}
```

要点：

* **为什么装到 `viewport()` 而不是 `this`**：QListWidget 的可视区域是内部子控件 viewport，滚轮事件发给它；
* **`return true` = 拦截**：滚轮被接管后阻止默认行为，避免"自己滚一遍 + 默认再滚一遍"；
* **`return 基类::eventFilter(...)` = 放行**：不关心的事件交给基类，**漏了会吞掉所有事件导致控件失灵**；
* **过滤器里可以发信号**：滚动到底部 `emit sig_loading_chat_user()`，列表控件不关心数据从哪来，实现"旁观者"解耦；
* **滚动条"悬浮才显示"**：默认 `AlwaysOff` 隐藏，鼠标进入列表可视区（`QEvent::Enter`）才切 `AsNeeded`、离开（`QEvent::Leave`）再切回 `AlwaysOff`。这个方案有个经典坑：**判断离开时不能写成 `else`**——viewport 上任何非 Enter 事件（鼠标移动、滚轮等）都会把滚动条藏回去，表现为"滚动条一闪而过/永远看不到"，必须用 `else if (event->type() == QEvent::Leave)` 精确匹配。

**接收侧：ChatDialog 把信号接上**。列表控件只负责发信号，真正"补数据"的是 ChatDialog：

```cpp
// 构造函数里连接：列表滚到底 → 加载更多
connect(ui->session_list, &ChatUserList::sig_loading_chat_user,
        this, &ChatDialog::slot_loading_chat_user);

void ChatDialog::slot_loading_chat_user()
{
    if (_b_loading) return;      // 防抖：加载期间忽略重复触发
    _b_loading = true;
    // 追加一批示例会话；真实场景改为从网络/数据库拉取
    for (int i = 0; i < 20; ++i) { addChatUserList(...); }
    _b_loading = false;
}
```

**防抖放在哪**：加载"更多"是高频触发点，防抖标志放在接收侧（ChatDialog）而不是发送侧（ChatUserList）更合理——同一个列表可能有多个接收者，发送者不该替接收者决定"能不能加载"。

#### 加载遮罩 LoadingDlg

"加载更多"期间给用户一个视觉反馈，llfc 单独抽了一个 `LoadingDlg`（`QDialog` 子类 + `loadingdlg.ui`），chat_app 的 `loadingdlg.ui` 已按同样结构建好，类代码（.h/.cpp）自行实现：

* 界面结构：`status_lb`（提示文字，如"正在加载聊天列表..."）+ `loading_lb`（200×200 的 GIF 动画，`QMovie` 播放 `:/res/loading.gif`），上下 spacer 让内容垂直居中；
* 关键窗口属性：`Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint` + `Qt::WA_TranslucentBackground`（无边框、置顶、背景透明），`setFixedSize(parent->size())` + `move(parent->mapToGlobal(0,0))` 覆盖整个父窗口；
* 使用方式：`new LoadingDlg(this, tip)` → `setModal(true)` → `show()`，用完 `deleteLater()`。模态遮罩挡住底层鼠标滚轮，配合 `_b_loading` 防抖双保险，避免加载期间重复触发"加载更多"；
* 为什么要单独建类而不是内联：它是"加载中"这个横切关注点的通用组件，聊天列表、搜索列表等多处复用，且挂 .ui 才符合"一个界面一个 Designer 类"的约定。

**什么时候用过滤器 vs 重写事件**：

| 场景 | 选择 | 原因 |
| --- | --- | --- |
| 拦的是**别人**的事件（如 viewport、子控件） | 事件过滤器 | 不能改 Qt 源码，装过滤器最直接 |
| 事件是**自己**的、只为自己服务 | 重写事件函数 | 更直观、性能更好 |
| 想监听多个对象统一处理 | 事件过滤器 | 一个 `eventFilter` 按 `watched` 分流 |
| 临时监听（如弹窗期间） | 事件过滤器 | 用完 `removeEventFilter` 卸载 |

### 2.4 对象树与内存管理

QObject 可以指定 parent，形成**对象树**：父对象析构时自动 delete 所有子对象。本项目大量使用这个机制：

```cpp
_stacked_widget = new QStackedWidget(this);        // 挂在 MainWindow 下
_login_dlg = new LoginDialog(this);                // 对话框挂在主窗口下
_timer = new QTimer(this);                         // 计时器挂在按钮下
```

好处：**不用手动管理子对象生命周期**，只要顶层窗口被销毁，整棵对象树自动回收，避免内存泄漏。`QNetworkReply` 这类异步对象不能立即 delete，用 `reply->deleteLater()` 在事件循环中安全释放（避免"对象正在处理事件时被销毁"）。

### 2.5 属性系统与 QSS 动态换肤

QSS（Qt 样式表）支持**属性选择器**：

```css
QLabel#err_tip[state="normal"] { color: green; }
QLabel#err_tip[state="err"]    { color: red; }
ClickedLabel#pass_visible[state='unvisible'] { border-image: url(:/res/unvisible.png); }
```

代码里用 `setProperty("state", ...)` 动态改属性：

```cpp
ui->err_tip->setText(str);
ui->err_tip->setProperty("state", state);
repolish(ui->err_tip);   // 关键：刷新样式
```

`repolish` 定义在 global.cpp：

```cpp
std::function<void(QWidget*)> repolish = [](QWidget* w){
    w->style()->unpolish(w);   // 清理旧的样式设置
    w->style()->polish(w);     // 重新应用样式设置（重新匹配 QSS 属性选择器）
};
```

**为什么必须 repolish**：控件创建时样式已经应用过一次，之后只改动态属性不会自动重算样式，必须 unpolish + polish 强制"重新走一遍样式匹配流程"。这是 Qt 里"属性状态机 + QSS"的标准套路。

这里用 `std::function` 定义工具函数而不是普通函数，好处是可以在 `.cpp` 里用 lambda 实现、将来还能整体替换实现，声明放在 `global.h` 里只暴露签名。

### 2.6 资源系统（qrc）

`resource.qrc` 把所有图片、QSS 打包进可执行文件，运行时用 `:/` 前缀访问：

```cpp
QFile qss(":/style/stylesheet.qss");
QPixmap originalPixmap(":/res/head_2.jpg");
```

**为什么这样设计**：资源随程序打包，**不依赖外部文件路径**，部署时不会出现"图片找不到"；也方便跨平台。CMake 里需要显式开启 `set(CMAKE_AUTORCC ON)`（`qt_standard_project_setup()` 只默认开 AUTOMOC 和 AUTOUIC）。

### 2.7 UI 文件与控件提升

`.ui` 文件是 XML 描述的界面，由 uic 生成 `ui_xxx.h`，代码里通过 `ui->setupUi(this)` 实例化界面。界面与逻辑分离：

- 调整布局、加控件，在 Designer 里可视化完成，不用手写布局代码；
- 代码只关心 `ui->控件名` 的引用和业务逻辑。

自定义控件（`ClickedLabel`、`TimerBtn`）通过 Designer 的**提升（Promote）**功能替换标准控件：在设计器里放一个 QLabel/QPushButton，然后提升为自定义类。前提是自定义类构造函数签名与基类兼容（`TimerBtn(QWidget* parent = nullptr)`），并且包含 `Q_OBJECT`。

---

## 3. 核心类设计逐一分析

### 3.1 global.h —— 把"协议"集中放在一处

```cpp
enum ReqId { ID_GET_VERIFY_CODE = 1001, ID_REG_USER = 1002, ID_LOGIN = 1003,
             ID_RESETPASSWORD = 1004, ID_CHAT_LOGIN, ID_CHAT_LOGIN_RSP };
enum Modules { REGISTERMOD = 0, LOGINMOD = 1, RESETMOD = 2 };
enum ErrorCodes { SUCCESS = 0, ERR_JSON = 1001, ... TokenInvalid = 1012 };
struct ServerInfo { QString Host, Port, Token; int Uid; };
```

设计意图：

- **请求 ID、模块、错误码是前后端约定的协议**，集中定义成强类型枚举，避免散落的魔法数字，改协议只改一处；
- `ReqId` 同时用于 HTTP 请求和 TCP 消息，一套 ID 贯穿两层；
- `ServerInfo` 把"登录成功后要连哪个聊天服务器"的数据聚合成一个结构体，信号参数更清晰；
- `extern gate_url_prefix` 在 main.cpp 里根据 config.ini 初始化，所有页面共享网关地址。

### 3.2 Singleton\<T\> —— 单例模板基类

```cpp
template <typename T>
class Singleton {
protected:
    Singleton() = default;
    Singleton(const Singleton&) = delete;
    Singleton& operator=(const Singleton&) = delete;
    static std::shared_ptr<T> _instance;
    static std::once_flag _once_flag;
public:
    static std::shared_ptr<T> GetInstance() {
        std::call_once(_once_flag, []() {
            _instance = std::shared_ptr<T>(new T());
        });
        return _instance;
    }
};
template <typename T> std::shared_ptr<T> Singleton<T>::_instance = nullptr;
template <typename T> std::once_flag Singleton<T>::_once_flag;
```

**为什么这样设计**（每个细节都有原因）：

| 设计点 | 原因 / 好处 |
| --- | --- |
| 模板基类 | 单例逻辑只写一份，`HttpMgr`、`TcpMgr` 直接继承复用；后面任何新管理器（如 UserMgr）零成本获得单例能力 |
| 构造函数 protected | 子类可以构造基类；外部不能 `new`，保证唯一性 |
| 拷贝构造/赋值 delete | 禁止复制，杜绝"第二个实例" |
| `std::call_once` + `once_flag` | **线程安全的懒汉式**：第一次调用才创建，多线程同时调用也只创建一次（C++11 保证） |
| 返回 `std::shared_ptr` | 实例生命周期由引用计数管理，`GetInstance()` 返回值持有引用时单例不会被提前销毁 |
| 配合 `enable_shared_from_this` | 异步回调（HTTP/TCP）期间对象必须存活，`shared_from_this()` 让回调闭包持有自身引用，避免"回调时对象已被销毁"的悬垂问题 |
| `friend class Singleton;` | 基类里 `new T()` 需要访问子类私有的构造函数，声明友元才能编译 |

注意：模板的静态成员必须在头文件里定义（否则每个编译单元各有一份导致链接错误），所以 singleton.h 全部内联在头文件中。

### 3.3 HttpMgr —— HTTP 管理器

```cpp
class HttpMgr : public QObject, public Singleton<HttpMgr>,
                public std::enable_shared_from_this<HttpMgr>
{
    Q_OBJECT
public:
    void PostHttpReq(QUrl url, QJsonObject json, ReqId req_id, Modules mod);
private:
    friend class Singleton;
    HttpMgr();
    QNetworkAccessManager _manager;
private slots:
    void slot_http_finish(ReqId id, QString res, ErrorCodes err, Modules mod);
signals:
    void sig_http_finish(ReqId id, QString res, ErrorCodes err, Modules mod);
    void sig_reg_mod_finish(ReqId id, QString res, ErrorCodes err);
    void sig_login_mod_finish(ReqId id, QString res, ErrorCodes err);
    void sig_reset_mod_finish(ReqId id, QString res, ErrorCodes err);
};
```

**为什么做成单例**：

1. 一个 `QNetworkAccessManager` 全局复用，内部连接池、Cookie、代理配置等共享；
2. 所有页面统一入口，不用每个对话框都 new 一个 manager；
3. 回包统一在一个类里处理，便于统一加日志、加密、统计。

**请求流程**（PostHttpReq）：

```cpp
QByteArray data = QJsonDocument(json).toJson();      // QJsonObject → 字节流
QNetworkRequest request(url);
request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
request.setHeader(QNetworkRequest::ContentLengthHeader, QByteArray::number(data.length()));

auto self = shared_from_this();                      // 回调期间保持存活
QNetworkReply* reply = _manager.post(request, data);
connect(reply, &QNetworkReply::finished, [self, reply, req_id, mod]{
    if (reply->error() != QNetworkReply::NoError) {
        emit self->sig_http_finish(req_id, "", ErrorCodes::ERR_NETWORK, mod);
    } else {
        QString res = reply->readAll();
        emit self->sig_http_finish(req_id, res, ErrorCodes::SUCCESS, mod);
    }
    reply->deleteLater();
});
```

设计亮点：

- **网络错误归一化**：网络失败统一转成 `ErrorCodes::ERR_NETWORK`，上层只认业务错误码，不关心底层细节；
- **异步 + 信号**：不阻塞 UI 线程，回包通过信号广播出去；
- **两级分发**：`sig_http_finish`（带模块）→ `slot_http_finish` 按 `Modules` 分流 → 发出 `sig_reg/login/reset_mod_finish`（不带模块，因为模块已经确定）。这样每个对话框只订阅自己关心的信号，**加新模块 = 加一个信号 + 一个 if 分支**，符合开闭原则。

### 3.4 TcpMgr —— TCP 管理器与粘包处理

```cpp
class TcpMgr : public QObject, public Singleton<TcpMgr>,
               public std::enable_shared_from_this<TcpMgr>
{
    Q_OBJECT
private:
    TcpMgr();
    void initHandlers();
    QMap<ReqId, std::function<void(ReqId, int, QByteArray)>> _handler;
    QTcpSocket _socket;
    QString _host;
    uint16_t _port;
    QByteArray _buffer;      // 接收缓冲区（TCP 是字节流，可能粘包/半包）
    bool _b_recy_pending;    // 是否正在等待一个完整数据包
    quint16 _message_id;
    quint16 _message_len;
public slots:
    void slot_tcp_connect(ServerInfo);
    void slot_send_data(ReqId reqId, QString data);
signals:
    void sig_con_success(bool);
    void sig_send_data(ReqId, QString);
    void sig_switch_chatdlg();
    void sig_login_failed(int);
};
```

**粘包/半包处理**（TCP 面向字节流，一次 readAll 可能包含多个包或半个包）：

```cpp
connect(&_socket, &QTcpSocket::readyRead, [this]{
    _buffer.append(_socket.readAll());       // 先全部缓存
    QDataStream stream(&_buffer, QIODevice::ReadOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    // 1. 不够一个消息头（id + len，各 2 字节）就等下一次
    if (_buffer.size() < sizeof(quint16) * 2) return;
    stream >> _message_id >> _message_len;
    _buffer = _buffer.mid(sizeof(quint16) * 2);
    // 2. 消息体没到齐就等（_b_recy_pending 语义）
    if (_buffer.size() < _message_len) return;
    QByteArray body = _buffer.mid(0, _message_len);
    _buffer = _buffer.mid(_message_len);
    // 3. 按消息 ID 查 handler 表分发
    auto iter = _handler.find(ReqId(_message_id));
    if (iter != _handler.end()) iter.value()(ReqId(_message_id), _message_len, body);
});
```

知识点：

- **消息头定长**（消息 ID + 消息长度），是解决 TCP 粘包最常用的"头部约定长度"方案；
- `QDataStream` 默认**大端字节序**，与网络字节序一致；发送端又显式 `setByteOrder(QDataStream::BigEndian)`，收端 `setVersion` 保证格式一致，这是跨平台/跨语言通信的常识（收发必须统一字节序和格式版本）；
- `quint16` 等类型保证跨平台固定宽度，避免 int 在 32/64 位平台宽度不一致；
- **handler 注册表**（`QMap<ReqId, std::function<...>>`）替代一长串 if/else：消息类型多了以后，新增消息 = 注册一个 handler，发送方/分发逻辑都不动。

**为什么用信号触发发送**：`LoginDialog` 里这样发数据——

```cpp
TcpMgr::GetInstance()->sig_send_data(ReqId::ID_CHAT_LOGIN, jsonString);
```

代码注释的原话是："为什么不直接调用 tcpmgr 发给服务器呢？在高并发的情况下，实现一个类似排队的效果，用 tcpmgr 的信号和槽机制。" 解读：

- 直接调用 `slot_send_data` 也能工作，但**调用方就绑死了具体对象和具体实现**；
- 通过信号，发送方只表达"我要发这条消息"的意图，消息何时发、怎么发由 TcpMgr 决定；
- 如果将来改用队列连接（QueuedConnection），发送请求会被排进事件循环按序处理，天然形成"排队"，且**不需要改调用方任何代码**；
- 信号是公开成员函数（宏展开为 public），所以可以外部 emit，这是本项目特意利用的一点（更规范的做法是封装一个发送方法）。

### 3.5 MainWindow + QStackedWidget —— 页面切换

```cpp
_stacked_widget = new QStackedWidget(this);
setCentralWidget(_stacked_widget);

_login_dlg = new LoginDialog(this);
_reg_dlg  = new RegisterDialog(this);
_reset_dlg = new ResetDialog(this);
_stacked_widget->addWidget(_login_dlg);
_stacked_widget->addWidget(_reg_dlg);
_stacked_widget->addWidget(_reset_dlg);
_stacked_widget->setCurrentWidget(_login_dlg);

connect(_login_dlg, &LoginDialog::switchRegister, this, &MainWindow::SlotSwitchReg);
connect(_reg_dlg, &RegisterDialog::switchLogin, this, &MainWindow::SlotSwitchLogin);
connect(_login_dlg, &LoginDialog::switchReset, this, &MainWindow::SlotSwitchReset);
connect(_reset_dlg, &ResetDialog::switchLogin, this, &MainWindow::SlotSwitchLogin);
```

**为什么用 QStackedWidget 而不是每次 new/delete 对话框**：

- 三个页面常驻内存，**切换只换显示，不销毁重建**，输入状态（如记住的用户名）天然保留；
- 切换成本极低（隐藏/显示），不会反复创建 UI 导致卡顿和内存碎片；
- 页面都挂在 `this`（MainWindow）下，由对象树统一管理生命周期。

**为什么切换逻辑集中在 MainWindow**：对话框只负责发"我想切到注册页"这类信号，**不关心谁响应、怎么响应**。响应者由 MainWindow 这个"装配方"决定，这就是控制反转的雏形——新增页面时，对话框代码不用改，只在 MainWindow 加一个槽和一条 connect。

### 3.6 三个 Dialog —— UI 与业务分离、命令分发

三个对话框结构高度一致，以 RegisterDialog 为例：

```cpp
private slots:
    void on_get_code_clicked();                 // 按钮命名约定：on_控件名_信号名
    void slot_reg_mod_finish(ReqId, QString, ErrorCodes);
    void on_sure_btn_clicked();
private:
    QMap<ReqId, std::function<void(const QJsonObject&)>> _handlers;  // 回包分发表
```

设计要点：

- **on_xxx_clicked 命名约定**：`on_` + 对象名 + `_` + 信号名，Qt 会自动连接（无需手动 connect），这是 Designer 生成代码时常用的快捷方式；
- **每个对话框自包含**：校验（checkPassValid）、请求（PostHttpReq）、回包处理（_handlers）都在自己类里，页面之间零依赖；
- **回包按 ReqId 查表分发**：`_handlers.insert(ReqId::ID_GET_VERIFY_CODE, [this](const QJsonObject& obj){...})`，把"收到什么消息干什么事"集中注册，避免散落一地的 if/switch，新增回包类型只加一行；
- **showtip + repolish 套路**：统一错误/成功提示样式，靠属性状态机切换颜色（见 2.5）；
- **密码异或混淆**（xorString）：客户端传输前做一次异或，服务器解密；简单防明文，注释也说明复杂场景应换加密库；
- **QEventLoop + QTimer::singleShot 延时 3 秒**：注册成功提示 3 秒后自动回登录页，是"等待但不 sleep 卡死事件循环"的一种方式（代价是 UI 仍被阻塞，见第 6 节改进点）。

**代价与权衡**：三个对话框复制了大量相似代码（校验、showtip、connect），这是"页面完全独立、改动互不影响"换来的。更优雅的做法是抽一个 `BaseDialog` 基类，把公共逻辑下沉——这是本项目留给读者的自然演进方向。

### 3.7 ClickedLabel —— 可点击图片标签

```cpp
class ClickedLabel : public QLabel {
    Q_OBJECT
public:
    virtual void mousePressEvent(QMouseEvent* ev) override;
    virtual void enterEvent(QEnterEvent* event) override;
    virtual void leaveEvent(QEvent* event) override;
    void SetState(QString normal, QString hover, QString press,
                  QString select, QString select_hover, QString select_press);
signals:
    void Clicked(void);
};
```

**为什么继承 QLabel 而不是 QPushButton**：这个控件的视觉本质是"一张会切换的图片"（小眼睛、忘记密码文字），QLabel 更贴合；交互能力（点击、悬浮）用事件重写补上。这也说明 Qt 里"没有现成控件时优先继承最接近的控件，而不是拼凑"。

状态机：

- 两个大状态：`Normal`（闭眼/未选中）、`Selected`（睁眼/已选中）；
- 每个状态又有 3 个视觉子状态：normal / hover / press；
- `SetState` 一次性传入 6 张图片的状态名，控件内部按"当前状态 + 鼠标事件"切换 `state` 属性，再 repolish 让 QSS 匹配对应的 `border-image`。

**为什么把"事件"转成"信号"**：鼠标按下、进入、离开都是底层事件，业务层只关心"被点击了"。控件内部处理后发出 `Clicked` 信号，使用方 connect 即可：

```cpp
connect(ui->pass_visible, &ClickedLabel::Clicked, [this]{
    auto state = ui->pass_visible->GetCurState();
    ui->pwd_edit->setEchoMode(state == ClickLbState::Selected
                              ? QLineEdit::Normal : QLineEdit::Password);
});
```

这是一个很好的**"自定义控件"教学案例**：继承 → 重写事件 → 属性状态机 → 自定义信号，四个步骤完整覆盖 Qt 控件定制的常见套路。

### 3.8 TimerBtn —— 倒计时按钮

```cpp
TimerBtn::TimerBtn(QWidget* parent) : QPushButton(parent), _counter(10) {
    _timer = new QTimer(this);          // 挂到 this 下，按钮销毁时自动释放
    connect(_timer, &QTimer::timeout, [this]{
        _counter--;
        if (_counter <= 0) { _timer->stop(); _counter = 10;
                             this->setText("获取"); this->setEnabled(true); return; }
        this->setText(QString::number(_counter));
    });
}

void TimerBtn::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        this->setEnabled(false);
        this->setText(QString::number(_counter));
        _timer->start(1000);            // 每秒一次，共 10 秒
        emit clicked();                 // 保持按钮语义
    }
    QPushButton::mouseReleaseEvent(e);  // 调用基类，保留默认行为
}
```

**为什么重写 mouseReleaseEvent 而不是 connect clicked 信号**：需求是"点击后立刻禁用并开始倒计时"。`clicked` 信号是在释放后才发出的，且如果只连槽函数，从"按钮禁用"到"倒计时开始"之间还要写额外逻辑；在事件函数里统一处理"禁用 + 改文本 + 启动定时器 + 发信号"更内聚。

知识点：

- `QTimer(this)` 挂在按钮下，**对象树自动管理**，析构只需停表（`_timer->stop()`）；
- 重写事件后主动 `emit clicked()`，让外部连到 `clicked` 的槽继续生效——自定义控件要保持与 Qt 惯例兼容；
- `QTimer::timeout` 是信号槽驱动定时任务的标准方式，回调里修改文本/使能状态是典型用法。

### 3.9 main.cpp —— 启动装配

```cpp
QApplication a(argc, argv);

QFile qss(":/style/stylesheet.qss");        // 全局 QSS 从资源加载
if (qss.open(QFile::ReadOnly)) { a.setStyleSheet(QLatin1String(qss.readAll())); }

QString app_path = QCoreApplication::applicationDirPath();   // 可执行文件目录
QString config_path = QDir::toNativeSeparators(app_path + QDir::separator() + "config.ini");
QSettings settings(config_path, QSettings::IniFormat);
gate_url_prefix = "http://" + settings.value("GateServer/host").toString()
                          + ":" + settings.value("GateServer/port").toString();

a.setWindowIcon(QIcon(":/res/chat_app.png"));
MainWindow w;
w.show();
return QCoreApplication::exec();            // 进入事件循环
```

知识点：

- 每个 Qt 程序**只有一个 QApplication**（生命周期先于所有窗口），它负责事件循环、全局设置；
- `QSettings + IniFormat` 读取 config.ini，把"服务器地址"等运行时配置与代码分离；config.ini 由 CMake `file(COPY ...)` 自动复制到构建目录；
- `QDir::toNativeSeparators` 统一 `/` 与 `\`，跨平台拼路径的必备细节；
- `exec()` 进入事件循环后，程序所有响应都靠事件驱动（点击、定时器、网络回包都是事件），事件循环退出程序才结束。

---

## 4. 信号与槽：本项目重点

### 4.1 登录全链路的信号流

```text
[LoginDialog] on_login_btn_clicked
      │ HttpMgr::PostHttpReq(...)          （直接调用，发请求）
      ▼
[HttpMgr] QNetworkReply::finished
      │ emit sig_http_finish(id, res, err, LOGINMOD)
      ▼
[HttpMgr] slot_http_finish（按模块分流）
      │ emit sig_login_mod_finish(id, res, err)
      ▼
[LoginDialog] slot_login_mod_finish
      │ _handlers[ID_LOGIN](json) → 解析 host/port/token
      │ emit sig_connect_tcp(ServerInfo)     （信号 → 槽）
      ▼
[TcpMgr] slot_tcp_connect → _socket.connectToHost
      │ QTcpSocket::connected
      ▼
[TcpMgr] emit sig_con_success(true)
      ▼
[LoginDialog] slot_tcp_con_success
      │ TcpMgr::sig_send_data(ID_CHAT_LOGIN, json)   （外部 emit 信号）
      ▼
[TcpMgr] slot_send_data → _socket.write(带头的报文)
      ▼
[ChatServer] 回 ID_CHAT_LOGIN_RSP
      ▼
[TcpMgr] readyRead → 粘包解析 → _handler[ID_CHAT_LOGIN_RSP]
      │ emit sig_login_failed(err) / sig_switch_chatdlg()
```

这条链路里，**同一层之间的协作是普通函数调用（PostHttpReq），跨层协作全部是信号槽**。这就是本项目信号槽用得最漂亮的地方。

### 4.2 为什么用信号而不是直接调用

对照一个反面例子：如果 LoginDialog 直接持有 HttpMgr/TcpMgr 指针并调用槽函数，会得到：

- LoginDialog 依赖网络层的具体实现，两者无法独立修改/测试；
- 每次新页面要收数据，都要改网络层代码；
- 调用时机无法灵活控制（排队、延迟、跨线程都做不到）。

改用信号槽之后：

| 好处 | 在本项目中的体现 |
| --- | --- |
| 发送方/接收方完全解耦 | LoginDialog 不认识 TcpMgr 内部，TcpMgr 不认识 LoginDialog |
| 一个信号多个订阅者 | `sig_http_finish` 被内部槽接收后还能继续按模块广播 |
| 新增接收者不改发送方 | 新增一个页面想收登录结果，只需 connect 一次 |
| 连接方式可切换 | 同线程直连 / 跨线程队列连接，语义不变 |
| 天然支持异步 | 网络回包到达时间未知，信号恰好是"事件发生"的抽象 |
| 类型安全 | 参数签名编译期检查，信号槽不匹配直接编译报错 |

### 4.3 本项目 connect 写法盘点

| 连接 | 类型 | 作用 |
| --- | --- | --- |
| `clicked` → `switchRegister` | **信号连信号** | 按钮点击转发成 LoginDialog 自己的信号，MainWindow 再去接收 |
| `clicked` → lambda | 信号→lambda | 小眼睛切换密码可见性 |
| `QNetworkReply::finished` → lambda | 异步信号→lambda | HTTP 回包处理 |
| `HttpMgr::sig_xxx_mod_finish` → Dialog 槽 | 自定义信号→自定义槽 | 回包分发 |
| `LoginDialog::sig_connect_tcp` → `TcpMgr::slot_tcp_connect` | 跨对象解耦 | 登录成功发起 TCP |
| `TcpMgr::sig_send_data` → `TcpMgr::slot_send_data` | 信号→槽（同对象） | 外部只发信号，内部统一发送 |
| `QTimer::timeout` → lambda | 定时器信号 | 倒计时 |
| `ClickedLabel::Clicked` → lambda | 自定义信号 | 点击事件转业务 |

### 4.4 容易忽略的规则

- 槽参数个数 ≤ 信号参数个数，顺序必须从前往后一致；
- 重载信号要指定指针（如 `QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::errorOccurred)`）；
- 信号→信号也可以 connect，但最终必须有一条路径连到槽；
- lambda 槽捕获 `this`/裸指针时，**接收方生命周期要覆盖信号发出时刻**（本项目用 `shared_from_this` 兜底）；
- `connect` 的默认连接类型：同线程直连（直接调用槽），跨线程自动排队（通过事件循环）。

---

## 5. 设计模式汇总

| 模式 | 位置 | 解决什么问题 / 好处 |
| --- | --- | --- |
| 单例（懒汉 + 线程安全） | `Singleton<T>`，HttpMgr/TcpMgr | 全局唯一实例、统一入口、连接等资源复用 |
| 观察者 | 所有信号槽 | 模块解耦、一对多通知、异步回调 |
| 命令分发（注册表） | `_handlers` / `_handler`（QMap<ReqId, function>） | 按消息 ID 查表执行，避免 if/else 爆炸，新增类型零侵入 |
| 模板方法/静态多态（CRTP） | `Singleton<HttpMgr>` 继承写法 | 编译期绑定，派生类获得基类能力，无虚函数开销 |
| 控制反转（装配方） | MainWindow 统一 connect | 页面只发信号，谁响应由装配方决定，新增页面不动旧代码 |
| 状态机 + 属性 | ClickedLabel 的 6 状态 | 视觉状态与业务状态分离，QSS 驱动显示 |
| 对象树（组合） | QStackedWidget、QTimer(parent) | 父对象统一管理子对象生命周期 |
| 门面/管理类 | HttpMgr、TcpMgr | 把底层网络 API 包装成业务友好的信号接口，UI 不直接碰 QNetworkAccessManager/QTcpSocket |

---

## 6. 容易踩的坑与本项目可改进点

1. **忘写 `Q_OBJECT`**：信号槽编译报错或链接报"undefined reference to vtable"；凡是带 signals/slots 的类必须写。
2. **QObject 必须是第一个基类**：否则 moc 生成的代码无法正确工作（TcpMgr 注释里专门强调）。
3. **改动态属性忘了 repolish**：QSS 不刷新，表现为"状态变了但样式没变"。
4. **`slot_send_data` 里长度取的是 `data.size()`（QString 字符数）而非 `dataByte.size()`（UTF-8 字节数）**：中文字符时字节数 > 字符数，报文头长度会比实际体长小，可能导致服务器端粘包解析错位。这是本项目一个真实的隐患，可作为"编码规范"知识点记住。
5. **TcpMgr 的 ID_CHAT_LOGIN_RSP 处理中，失败分支 emit 完 `sig_login_failed` 后没有 return**，会继续走到 `emit sig_switch_chatdlg()`，属于"错误路径穿层"的 bug——教训：错误分支要么 return，要么用 if/else 保证互斥。
6. **`QEventLoop` 嵌套 exec 会阻塞 UI**（注册成功等 3 秒期间窗口无法交互），更好的做法是 `QTimer::singleShot(3000, this, &RegisterDialog::switchLogin)` 或配合状态标志。
7. **TimerBtn 调用基类 `mouseReleaseEvent` 后，基类本身也会发 `clicked`**，加上手动 `emit clicked()` 可能触发两次，实际使用注意去重。
8. **外部 emit 他人对象的信号**（`TcpMgr::GetInstance()->sig_send_data(...)`）虽然合法（信号是 public），但侵入性强，更规范的是 TcpMgr 提供 `SendData()` 公开方法内部 emit。

---

## 7. 总结

这个 Qt 客户端虽然只有十几个源文件，但设计非常典型，值得记住的几条主线：

1. **UI 与网络彻底分离**：页面只发信号，网络层只发信号，靠 connect 装配，模块各自可独立演进；
2. **单例 + 信号槽 + 注册表** 三件套：全局唯一的管理器、异步通知、按 ID 分发，是 Qt 中型客户端最实用的组合；
3. **每个 Qt 机制都有明确用途**：元对象系统支撑信号槽、对象树管内存、属性系统 + QSS 做换肤、资源系统打包部署、事件重写做控件定制；
4. **设计的代价也是知识点**：复制代码（三个 Dialog）、单例与 shared_ptr 的生命周期、事件与信号的双重触发，都值得在面试/学习时展开讨论。

推荐阅读顺序：先看 `global.h`（协议），再看 `singleton.h`（单例），然后 `HttpMgr` → `TcpMgr`（网络层），最后 `MainWindow` + 三个 Dialog（UI 层），配合本文第 4 节的信号流图走一遍登录流程，整个客户端就串起来了。
