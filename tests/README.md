# 自动化测试

根目录 CMake 默认启用 `BUILD_TESTING`，已注册的测试目标随根目录默认构建一起编译。可从根目录运行 `run_tests.ps1`，逐项选择要运行的测试；传入 `-Tests tcp,message` 等参数可跳过交互提示。数据库用例只在显式提供隔离测试库配置时运行。各目标的覆盖范围、MySQL 环境变量和 CI 方式见 [测试笔记](../note/测试.md)。

## 连接容量测试

`connection_capacity_test` 是针对真实 ChatServer 的 TCP 集成测试，不是纯内存单元测试。它会一台一台地持续建立并保持空闲 TCP 连接，直到出现第一次建连失败，或每台都达到探测上限 `CHAT_CONNECTION_MAX`。

ChatServer 本身没有固定的“最大连接数”常量：`CServer` 会把每个接受的连接放进 `_sessions`，没有主动拒绝的代码。实际能连到多少受三方面限制：

1. 客户端（运行测试的这台机器）的本地 TCP 动态端口范围。Windows 默认是 `49152~65535`，只有约 16,384 个，单客户端最容易先撞上它。
2. 服务端的 socket 句柄、内存和网络栈。
3. 本机同时保持连接的场景下，两端进程的实际负载。

因此本项目默认把探测上限设为 20,000（超过默认动态端口数），单客户端跑通常会先因端口耗尽自动停下；如果两台都达到 20,000，说明客户端探测上限不够，需要调大 `CHAT_CONNECTION_MAX` 或从多台客户端同时压。

## 前置条件

1. 已构建项目和测试目标。
2. MySQL、Redis 和 `ChatServer1` 已按项目启动流程运行。
3. `ChatServer1` 和 `ChatServer2` 正在监听测试目标端口，默认分别为 `127.0.0.1:8090` 与 `127.0.0.1:8091`。

## 运行

先构建测试：

```powershell
cmake --preset windows-vcpkg
cmake --build --preset debug --target connection_capacity_test
```

使用默认参数运行（每台至少 100 条，最多逐步探测到 20,000 条；两台服务会交替建连并同时保持连接，遇到首次失败即停止）：

```powershell
ctest --test-dir build -L capacity --output-on-failure
```

收紧或放宽探测上限，例如最多建立 5,000 条并保持 3 秒：

```powershell
$env:CHAT_CONNECTION_MIN = 100
$env:CHAT_CONNECTION_MAX = 5000
$env:CHAT_CONNECTION_HOLD_MS = 3000
ctest --test-dir build -L capacity --output-on-failure
```

连接其他实例时设置主机和端口列表：

```powershell
$env:CHAT_CONNECTION_HOST = '127.0.0.1'
$env:CHAT_CONNECTION_PORTS = '8090,8091'
ctest --test-dir build -L capacity --output-on-failure
```

测试会分别输出两台 ChatServer 的成功连接数，以及第一次失败的 socket 错误码和含义：

- `WSAEADDRINUSE` / `WSAEADDRNOTAVAIL`：通常是**这台测试客户端**的动态端口耗尽（`netsh int ipv4 show dynamicport tcp` 可查看范围），不代表 ChatServer 到达上限；
- `WSAECONNREFUSED`：目标服务没监听、backlog 已满或防火墙拒绝；
- `WSAENOBUFS` / `WSAEMFILE`：进程句柄、内存或缓冲资源不足。

测试结束后会以 RST 方式关闭所有连接（`SO_LINGER` 置 0），避免留下大量 `TIME_WAIT` 占住端口导致无法连续复跑。若两台服务都没有启动，测试会跳过而不会把“连接被拒绝”误报为容量不足。
