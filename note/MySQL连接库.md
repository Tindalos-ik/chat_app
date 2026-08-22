# MySQL连接库知识点（X DevAPI / mysqlcppconnx）

[官方文档](https://dev.mysql.com/doc/dev/connector-cpp/latest/)

> 本项目实际使用的库：vcpkg 安装的 **mysql-connector-cpp（版本 9.7.0）**，CMake 链接目标 `mysqlcppconnx`，头文件 `<mysqlx/xdevapi.h>`。
>
> 对应代码：GateServer / StatusServer / ChatServer 三份完全相同的 `MysqlMgr.h/cpp`。
> 连接池的完整设计见 [连接池.md](连接池.md)，建表逻辑见 [数据库设计.md](数据库设计.md)。

这是 MySQL 官方的 C++ 连接器，和项目里另两个"第三方库"（Boost、gRPC）不一样——它不依赖任何其他库，装完 vcpkg 就能用。



### 项目里有两种 MySQL C++ API

连接器装一个包，但提供**两套完全不同的 API**：

| | 经典 API（mysqlcppconn） | X DevAPI（mysqlcppconnx，本项目用的） |
| --- | --- | --- |
| 底层协议 | 经典协议（MySQL Protocol） | **X Protocol** |
| 默认端口 | 3306 | **33060** |
| 头文件 | `<mysqlx/xapi.h>` | `<mysqlx/xdevapi.h>` |
| 连接类 | `sql::Connection` | `mysqlx::Session` |
| 执行语句 | `prepareStatement` + `setString` | `session->sql(...).bind(...).execute()` |
| 风格 | 类似 JDBC，啰嗦 | 链式调用，面向对象 |

你的 `config.ini` 里：

```
[Mysql]
host = 127.0.0.1
port = 33060      ← X Protocol 端口，不是 3306
```

**为什么用 X DevAPI 而不是经典 API**：

1. 链式写法简洁：`sql().bind().execute()` 一行串起来，不用一堆 `setString`；
2. 参数绑定是 API 自带的，天然防 SQL 注入；
3. 除了 SQL，还支持 NoSQL 风格的 Collection 操作（本项目没用，但库支持）；
4. 官方推荐的新接口，llfc 老项目用的经典 API，新版也逐步迁移到 X DevAPI。

> 连 3306 用经典 API，连 33060 用 X DevAPI



### 核心对象模型

X DevAPI 的对象层级，从上到下：

```text
mysqlx::Session（一次连接，相当于数据库会话）
  └─ Schema（数据库，相当于 create database 出来的库）
       ├─ Table（关系表）      → 增删改查
       └─ Collection（JSON集合）→ NoSQL 风格（本项目没用）
```

执行一条 SQL 时涉及的对象：

```text
session->sql("...")     → SqlStatement（一条预解析的 SQL）
        .bind(...)      → 绑定参数（返回值还是 SqlStatement，所以能链式）
        .execute()      → SqlResult（执行结果）
        .fetchOne()     → Row（一行数据）
        row[0]          → Value（一个单元格的值）
        value.get<T>()  → T（取成 C++ 类型）
```

| 类 | 职责 | 项目里出现的位置 |
| --- | --- | --- |
| `mysqlx::Session` | 一个数据库连接（X Protocol） | MySqlPool 池里的元素 |
| `SqlStatement` | 一条 SQL + 参数 | `con->sql(sql).bind(...)` |
| `SqlResult` | 执行结果（行集合/影响行数） | `execute()` 的返回值 |
| `Row` | 结果集里的一行 | `fetchOne()` 的返回值 |
| `Value` | 一个单元格，可以 `get<T>()` 转换 | `rows[0]` |



### 项目用法逐段拆解

#### 1. 创建连接：mysqlx::Session 构造

```cpp
std::unique_ptr<mysqlx::Session> MySqlPool::CreateSession() {
    // 构造函数参数：host, port, user, password, schema
    return std::make_unique<mysqlx::Session>(host_, std::stoi(port_), user_, pass_, schema_);
}
```

构造函数直接建立连接，五个参数就把"连哪台机器、用哪个账号、进哪个库"全说清楚了：

* `port_` 是 33060（X Protocol），所以 `std::stoi(port_)` 转成数字；
* 最后一个参数 `schema_` 是默认库，之后执行 SQL 不用写库名前缀；
* 注意这里用的是 `unique_ptr` 而不是裸指针——连接是独占资源，交给智能指针管理，不用手动 delete。

#### 2. 执行 SQL + 参数绑定

```cpp
bool MysqlMgr::RegUser(const std::string &name, const std::string &email, const std::string &password)
{
    auto con = pool_->GetConnection();   // 从池里拿连接
    Defer defer([&con, this]() { pool_->ReturnConnection(std::move(con)); }); // 离开作用域自动归还
    try {
        if (con == nullptr) return false; // 池关闭时拿不到连接，要判空

        std::string sql = "INSERT INTO user (name, email, pwd) VALUES (?, ?, ?)";
        auto result = con->sql(sql).bind(name).bind(email).bind(password).execute();
        return true;
    } catch (const std::exception &e) {
        std::cout << "Exception: " << e.what() << std::endl;
        return false;
    }
}
```

知识点：

**`?` 占位符 + `bind()` 绑定**：SQL 里用 `?` 留坑，然后按顺序 `bind(name).bind(email).bind(password)` 填值。这是**参数化查询**，值是作为数据传给服务器，而不是拼进 SQL 字符串——所以天然免疫 `' OR '1'='1` 这种注入。

> 顺带一提：X DevAPI 的 `bind()` 同时支持 `?` 位置参数和 `:name` 命名参数，本项目用的是 `?`。

**`con->sql(...)` 返回 SqlStatement，`execute()` 返回 SqlResult**：链式调用的秘密就是每一步都返回对象本身或下一个对象。

#### 3. 读取结果：fetchOne / Row / Value

```cpp
bool MysqlMgr::CheckPwd(const std::string &name, const std::string &pwd, UserInfo &userInfo)
{
    auto con = pool_->GetConnection();
    Defer defer([&con, this]() { pool_->ReturnConnection(std::move(con)); });
    try {
        std::string sql = "SELECT uid, name, email, pwd FROM user WHERE name = ?";
        auto result = con->sql(sql).bind(name).execute();
        auto rows = result.fetchOne();          // 取第一行

        if (!rows) return false;                // 没查到（Row 支持 bool 判断）

        std::string storedPassword = rows[3].get<std::string>();  // 第4列 pwd
        if (storedPassword != pwd) return false;

        userInfo.uid = rows[0].get<int>();      // 第1列 uid
        userInfo.user = rows[1].get<std::string>();
        userInfo.email = rows[2].get<std::string>();
        userInfo.passwd = storedPassword;
        return true;
    } catch (const std::exception &e) {
        ...
    }
}
```

知识点：

* **`fetchOne()` 取一行，`fetchAll()` 取全部**（返回行列表）。本项目查询都是"查一条用户"，所以只用 `fetchOne`；
* **`Row` 可以直接当 bool 用**：`Row` 内部实现了 `operator bool()`（等价于 `!isNull()`），所以 `if (!rows)` 就是"没查到"；
* **`row[i]` 按下标取列，`Value::get<T>()` 转类型**：`rows[0].get<int>()`、`rows[3].get<std::string>()`；
* **列下标从 0 开始，必须和 SELECT 的列顺序一一对应**：`SELECT uid, name, email, pwd` → 0=uid、1=name、2=email、3=pwd。改 SQL 时忘了同步下标，是这类代码最常见的 bug；
* `get<T>()` 类型不匹配会抛异常（比如拿字符串列调 `get<int>`），正好被 catch 兜住返回 false。

#### 4. 错误处理：异常机制

X DevAPI **不用返回码，出错就抛 C++ 异常**（`mysqlx::Error`，继承自 `std::exception`）。

项目里的统一套路：

```cpp
try {
    // 执行 SQL
} catch (const std::exception &e) {
    std::cout << "Exception: " << e.what() << std::endl;
    return false;
}
```

好处：不用每个调用都检查返回值；坏处：**忘了 catch，异常一路抛出去程序直接崩**。所以每个数据库方法外面都包一层 try/catch 是必须的。

#### 5. 连接池 + Defer RAII + 心跳检测

这是本项目 X DevAPI 用得最重的地方，完整设计见 [连接池.md](连接池.md)，这里只讲和库相关的三个点：

**为什么需要连接池**：`mysqlx::Session` 构造 = TCP 连接 + X Protocol 握手，一次几百微秒到几毫秒。每次请求都新建连接，高并发下开销巨大；而且 MySQL 服务端有 `wait_timeout`（默认 8 小时），**空闲连接会被服务端主动断开**（项目注释里写的"mysql 的连接长时间不用会自动断开，redis 不会"就是这个），所以池子必须能检测失效连接并重建。

**心跳检测 `SELECT 1`**：

```cpp
bool IsConnectionValid(mysqlx::Session* session) {
    if (!session) return false;
    try {
        session->sql("SELECT 1").execute();   // 能执行成功 = 连接还活着
        return true;
    } catch (const std::exception& e) {
        return false;
    }
}
```

`SELECT 1` 是最轻的探活语句。项目在**取出连接时**和**归还连接时**各检测一次，失效就 `CreateSession()` 重建。

**Defer 自动归还**：

```cpp
Defer defer([&con, this]() { pool_->ReturnConnection(std::move(con)); });
```

`Defer` 是项目在 const.h 里手写的 RAII 类（仿 Go 的 defer），出作用域自动执行归还逻辑。好处：**任何 return 分支都不会忘还连接**，而忘还连接会导致池被借空、其他线程永久阻塞在 `GetConnection()` 上。



### CMake / vcpkg 集成

安装（README 里写的是 `mysqlcppconnx`，**这是笔误**，vcpkg 官方端口名是 `mysql-connector-cpp`）：

```
vcpkg install mysql-connector-cpp --triplet x64-windows
```

这个包同时产出两个库：

```
lib/mysqlcppconnx.lib   ← X DevAPI（本项目链接这个）
lib/mysqlcppconn.lib    ← 经典 API
```

三个服务的 CMakeLists.txt 里都是：

```
target_link_libraries(your_target
    PRIVATE
        mysqlcppconnx    # 链接 mysql-c++ 库（X DevAPI）
)
```

头文件路径不用自己配：vcpkg 工具链（`D:/cppsoft/vcpkg/scripts/buildsystems/vcpkg.cmake`）会自动把 `include/mysqlx/xdevapi.h` 所在的目录加进编译搜索路径。



### 配置驱动

连接参数不写死在代码里，从 `config.ini` 读取（ConfigMgr 解析）：

```
[Mysql]
host = 127.0.0.1
user = root
port = 33060
passwd = 123456
schema = chat_app_db
poolsize = 10
```

```cpp
this->host_    = config["Mysql"]["host"];
this->port_    = config["Mysql"]["port"];
this->user_    = config["Mysql"]["user"];
this->pass_    = config["Mysql"]["passwd"];
this->schema_  = config["Mysql"]["schema"];
this->poolSize_ = std::stoi(config["Mysql"]["poolsize"]);
```

好处：换数据库、调并发（poolsize）都不用重新编译。注意 `poolsize` 是字符串，要 `std::stoi` 转数字。



### 面试/复习自测

* X DevAPI 和经典 Connector/C++ 有什么区别？端口各是多少？
* `session->sql().bind().execute()` 每步返回什么类型？
* `Row` 为什么能写 `if (rows)`？
* 为什么参数化查询能防 SQL 注入？
* MySQL 连接为什么会断？连接池怎么检测和重建？
* 为什么连接要自动归还？忘了归还会发生什么？
* `fetchOne()` 和 `fetchAll()` 什么区别？取空结果集怎么判断？
* X DevAPI 的列下标从几开始？经典 API 呢？
