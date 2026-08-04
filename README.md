# chat_app 全栈聊天室

一个基于 C++ 的分布式聊天室项目，包含 Qt 桌面客户端和一套微服务架构的服务端：

- **GateServer**（C++/Beast）：HTTP 网关，处理注册、登录、验证码请求；
- **StatusServer**（C++/gRPC）：状态服务，登录时做负载均衡、签发/校验 token；
- **ChatServer**（C++/Boost.Asio）：TCP 长连接服务，负责用户会话、消息收发；
- **VarifyServer**（Node.js/gRPC）：邮箱验证码服务；
- **客户端**（Qt 6）：登录/注册/聊天界面。

---

## 系统架构

```text
客户端(Qt)
   │ HTTP(8080)
   ▼
GateServer ──gRPC(50051)──► VarifyServer(Node.js) ──► Redis + SMTP邮箱
   │  gRPC(50052)
   ▼
StatusServer ──► 选择 ChatServer(8090/8091) + 签发 token
   ▲
   │ gRPC(50052) 校验 token
ChatServer(8090 / 8091) ──► Redis / MySQL
```

| 服务 | 语言 | 端口 | 说明 |
| --- | --- | --- | --- |
| VarifyServer | Node.js | 50051 | 生成验证码、发邮件 |
| StatusServer | C++ | 50052 | 负载均衡、token 签发与校验 |
| ChatServer | C++ | 8090 / 8091 | TCP 长连接（可多实例） |
| GateServer | C++ | 8080 | HTTP 网关 |
| 客户端 | Qt 6 | - | 登录、聊天界面 |

---

## 目录结构

```text
chat_app/
├── CmakeLists.txt            # 顶层 CMake（聚合三个 C++ 服务）
├── CMakePresets.json         # CMake 预设（VS2022 + vcpkg 工具链）
├── GateServer/               # HTTP 网关（C++）
├── StatusServer/             # 状态服务（C++/gRPC）
├── ChatServer/               # 聊天服务器（C++/Asio）
├── VarifyServer/             # 验证码服务（Node.js/gRPC）
├── chat_app desktop/         # Qt 客户端
└── note/                     # 学习文档（TCP/连接池/gRPC/登录/部署）
```

> `proto_gen/`、`build/`、`node_modules/` 等均为生成/构建产物，已通过 `.gitignore` 排除，不参与版本管理。

---

## 环境要求

| 软件 | 版本要求 | 用途 |
| --- | --- | --- |
| Windows | 10/11 | 开发环境 |
| Visual Studio 2022 | 需勾选“使用 C++ 的桌面开发” | 编译 C++ 服务 |
| CMake | ≥ 3.21 | 构建系统 |
| vcpkg | 最新 | C++ 依赖管理 |
| Boost | 1.91（本项目路径 `C:/local/boost_1_91_0`） | Asio、property_tree |
| MySQL | 8.x | 用户/好友数据 |
| Redis | 任意较新版本 | token、验证码、在线状态缓存 |
| Node.js | ≥ 18 | VarifyServer |
| Qt | 6.x（客户端用 6.8） | Qt 客户端 |

---

## Windows 环境安装

### 1. Visual Studio 2022

安装时勾选工作负载：**使用 C++ 的桌面开发**（含 MSVC 编译器和 CMake 工具）。

### 2. CMake

VS2022 自带 CMake；也可以单独安装，确保 `cmake --version` ≥ 3.21。

### 3. vcpkg + 依赖库

```powershell
git clone https://github.com/microsoft/vcpkg.git D:/cppsoft/vcpkg
cd D:/cppsoft/vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg install grpc protobuf redis-plus-plus mysqlcppconnx --triplet x64-windows
```

安装的库：

- `grpc` / `protobuf`：服务间 RPC（CMake target：`gRPC::grpc++`、`protobuf::libprotobuf`）；
- `redis-plus-plus`：Redis C++ 客户端（CMake target：`redis++::redis++`，依赖 hiredis）；
- `mysqlcppconnx`：MySQL Connector/C++ X DevAPI（CMake target：`mysqlcppconnx`）。

### 4. Boost

下载 Boost（项目使用 1.91），解压到 `C:/local/boost_1_91_0`（asio、property_tree 等是 header-only，无需编译）。

> 如果安装到其他路径，请同步修改三个服务 CMakeLists.txt 里的 `target_include_directories` 中的 `"C:/local/boost_1_91_0"`。

### 5. MySQL / Redis

```powershell
# MySQL 8.x：安装后创建数据库 chat_app_db（见下方“数据库初始化”）
# Redis：Windows 版启动后默认 6379
```

### 6. Node.js（VarifyServer）

到 [nodejs.org](https://nodejs.org) 安装 LTS 版本，然后：

```powershell
cd VarifyServer
Copy-Item config.example.json config.json   # 首次：复制示例配置
notepad config.json                          # 填写你的 163 邮箱授权码
npm install
```

### 7. Qt（客户端）

客户端需要 Qt 6.8（MinGW 或 MSVC 套件）+ CMake，用 Qt Creator 打开 `chat_app desktop/CMakeLists.txt` 构建。

---

## 数据库初始化

```sql
CREATE DATABASE IF NOT EXISTS chat_app_db DEFAULT CHARACTER SET utf8mb4;
```

建表脚本可参考参考项目 `llfcchat/sql备份/llfc.sql`（把库名改为 `chat_app_db` 后导入）：

```powershell
mysql -uroot -p chat_app_db < llfc.sql
```

---

## 构建与运行

### 服务端构建（Windows）

项目根目录已配置好 CMake 预设（VS2022 + vcpkg 工具链）：

```powershell
cd D:\myproject\chat_app
cmake --preset windows-vcpkg     # 配置
cmake --build --preset debug     # 编译 Debug
```

生成的可执行文件：

```text
build\ChatServer\Debug\ChatServer.exe
build\GateServer\Debug\GateServer.exe
build\StatusServer\Debug\StatusServer.exe
```

### 启动顺序

**MySQL → Redis → VarifyServer → StatusServer → ChatServer → GateServer**

```powershell
# 1. VarifyServer
cd D:\myproject\chat_app\VarifyServer
npm run serve

# 2. StatusServer
cd D:\myproject\chat_app\build\StatusServer\Debug
.\StatusServer.exe

# 3. ChatServer（多实例就复制一份，改 config.ini 端口为 8091）
cd D:\myproject\chat_app\build\ChatServer\Debug
.\ChatServer.exe

# 4. GateServer
cd D:\myproject\chat_app\build\GateServer\Debug
.\GateServer.exe
```

> 必须在可执行文件所在目录启动（ConfigMgr 从“当前工作目录/config.ini”读取配置，config.ini 已由 CMake 自动拷贝到各 Debug 目录）。

### 验证

```powershell
curl http://127.0.0.1:8080/get_test
curl -X POST http://127.0.0.1:8080/user_login -H "Content-Type: application/json" `
  -d '{"email":"test@163.com","passwd":"123456"}'
```

### 客户端构建

用 Qt Creator 打开 `chat_app desktop/CMakeLists.txt`，选择 Qt 6.8 套件构建并运行；登录前把 `global.cpp` 里的 `gate_url_prefix` 改成 GateServer 地址（默认 `http://127.0.0.1:8080`）。

---

## 配置说明

各服务运行目录下的 `config.ini`（C++ 服务）和 `VarifyServer/config.json`（Node.js）保存连接信息：

```ini
[Mysql]
host = 127.0.0.1
port = 33060          ; X DevAPI 端口
user = root
passwd = 123456
schema = chat_app_db

[Redis]
host = 127.0.0.1
port = 6379
passwd = 123456

[StatusServer]
host = 127.0.0.1
port = 50052

[ChatServer1]
host = 127.0.0.1      ; 返回给客户端直连的地址，部署到服务器时改为公网 IP
port = 8090
name = ChatServer1
```

> `VarifyServer/config.json` 含邮箱授权码等敏感信息，已加入 `.gitignore`，克隆仓库后请复制 `config.example.json` 填写。

---

## 部署到 Linux 服务器

详见 [note/服务部署.md](note/服务部署.md)：包含依赖安装、CMakeLists 跨平台改造、systemd 守护、防火墙配置等。

---

## 文档索引（note/）

- [TCP服务器.md](note/TCP服务器.md)：Asio TCP 服务器搭建、粘包处理、发送/接收队列
- [连接池.md](note/连接池.md)：项目中 4 类连接池的模型与应用
- [grpc.md](note/grpc.md)：gRPC 在项目中的应用
- [login.md](note/login.md)：登录全链路解析
- [服务部署.md](note/服务部署.md)：Windows / Linux 部署指南

---

## 常见问题

| 问题 | 处理 |
| --- | --- |
| 编译报“找不到 gRPC/boost 头文件” | 确认 vcpkg 工具链路径、Boost 解压路径与 CMakeLists 一致 |
| 启动报 `Config file not found` | 在可执行文件所在目录启动 |
| 验证码收不到 | 检查 `VarifyServer/config.json` 的 163 邮箱授权码和 Redis |
| 登录后连不上聊天服务器 | StatusServer 配置里 ChatServer 的 host 填了 127.0.0.1，客户端在其他机器时改为可达 IP |
