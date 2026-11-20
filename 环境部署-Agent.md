# 环境部署（Agent）

按顺序执行。适用于收到恢复依赖、构建或运行项目的任务。用户只要求更新文档时，不执行安装、启动或数据初始化。共享的版本、Compose 定义和启动命令集中在 [环境部署.md](环境部署.md)。

## 1. 检查工具，安装缺项

从仓库根目录打开 PowerShell。检查 Git、CMake、Node.js、npm、VS 2022 x64 C++ 工具链和 Docker Compose；按[安装 Windows 工具](环境部署.md#1-安装-windows-工具)补齐。仅在需要运行服务时确认 Docker Desktop 已启动且服务端使用 Linux 容器；如安装器要求重启，让用户完成后再继续。

```powershell
Get-Command git, cmake, node, npm.cmd, docker
cmake --version
node --version
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
& $vswhere -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
docker compose version
```

设置 `$qt` 指向 Qt 6.8.3 MSVC 2022 x64 kit。检查 `lib/cmake/Qt6WebEngineWidgets/Qt6WebEngineWidgetsConfig.cmake`、`lib/cmake/Qt6WebChannel/Qt6WebChannelConfig.cmake`、`bin/QtWebEngineProcess.exe` 和 `plugins/sqldrivers/qsqlite.dll` 均存在。已有 kit 缺少模块时使用原安装目录的 `MaintenanceTool.exe` 添加模块。

全新安装可用下列命令。执行前需要用户已接受 Qt 的许可证和开源义务，且当前 Windows 用户已通过 Qt 安装器登录。若登录或授权缺失，请让用户通过 Qt 安装器完成；不要索取或输出 Qt 密码。

```powershell
$qtRoot = 'C:\Qt'
$installer = Join-Path $env:TEMP 'qt-online-installer-windows-x64-online.exe'
Invoke-WebRequest -UseBasicParsing 'https://download.qt.io/official_releases/online_installers/qt-online-installer-windows-x64-online.exe' -OutFile $installer
$qtInstall = Start-Process -FilePath $installer -ArgumentList @(
    '--root', ('"' + $qtRoot + '"'), '--accept-licenses', '--accept-obligations',
    '--default-answer', '--confirm-command', '--auto-answer', 'telemetry-question=No',
    'install', 'qt.qt6.683.win64_msvc2022_64',
    'qt.qt6.683.addons.qtwebchannel.win64_msvc2022_64',
    'qt.qt6.683.addons.qtpositioning.win64_msvc2022_64',
    'extensions.qtwebengine.683.win64_msvc2022_64'
) -Wait -PassThru
if ($qtInstall.ExitCode -ne 0) { throw "Qt 安装失败：$($qtInstall.ExitCode)" }
Remove-Item -LiteralPath $installer
$qt = Join-Path $qtRoot '6.8.3\msvc2022_64'
```

## 2. 恢复依赖，构建产物

1. 执行[安装 C++ 依赖](环境部署.md#2-安装-c-依赖)。已有独立 vcpkg 目录且版本匹配时，从 `vcpkg install` 一步继续；版本不同则选择新的 `VCPKG_ROOT`，保留原目录。构建和启动所用的 PowerShell 都要设置该变量。
2. 进入 `VarifyServer/`，执行 `npm.cmd ci` 后返回仓库根目录。准备运行验证码服务时，若 `config.json` 不存在，从 `config.example.json` 复制。保留当前 `email` 配置；新部署缺少真实 SMTP 授权码时，让用户在本地填写。仍可先完成构建。
3. 执行[构建服务和桌面客户端](环境部署.md#5-构建服务和桌面客户端)的命令，将 `$qt` 设为第 1 节确认的目录。发现旧 CMake 缓存来自其他工具链时，保留原构建目录，使用 `cmake --preset windows-vcpkg -B build_recovered "-DCMAKE_PREFIX_PATH=$qt"` 和 `cmake --build build_recovered --config Debug`，后续服务路径也用 `build_recovered`。客户端同理可用新的构建目录，并把 `static/` 复制到实际的 `Release/` 目录。
4. 确认五个 C++ 服务、客户端的 `.exe` 和相邻 `config.ini` 都已生成。仅要求恢复依赖或构建时，到此停止。

## 3. 启动 Compose 并设置运行配置

1. 按[用 Docker Compose 启动 MySQL 和 Redis](环境部署.md#3-用-docker-compose-启动-mysql-和-redis)准备仓库外的 `.env`，并使用仓库内的 [deploy/compose.yaml](deploy/compose.yaml)。如有端口冲突，先修改 `.env` 中的宿主机端口，再执行 `docker compose @composeArgs up -d --wait`。Compose 未就绪时执行 `docker compose @composeArgs logs mysql redis`。
2. 按[配置业务服务](环境部署.md#4-配置业务服务)表格填写**产物目录**中的 INI：五个服务的 `[Mysql]` 和四个服务的 `[Redis]` 使用同一个 Compose 项目的端口与凭据，两台 ChatServer 默认关闭 `[VideoTest]`；有群头像需求时，为两台 ChatServer 与 ResourceServer 设置相同的私密 `service_key`。桌面客户端的 `[GateServer]` 与 `[ResourceServer]` 使用回环地址。只修改这些字段；不在聊天、命令输出或版本库中公开密码。重新配置或构建若覆盖了运行文件，需要重新应用设置。
3. 修改 `VarifyServer/config.json` 中的 `redis.host`、`redis.port` 和 `redis.passwd`。端口写 JSON 数字，密码写字符串。保留 `mysql` 对象；保留用户已经填写的 SMTP 配置。

## 4. 启动并观察实际结果

- SMTP 配置齐全时，按[启动和检查](环境部署.md#6-启动和检查)运行 `start_all.bat`，如使用 `build_recovered` 则将它作为参数传入。SMTP 配置缺失时，执行同节的分别启动命令，跳过 `npm.cmd run serve` 那一行。需要音视频时，先按[本机音视频](环境部署.md#7-可选恢复本机音视频)启动媒体服务，为两个 ChatServer 打开 `[VideoTest]`，然后启动服务。
- 使用 `docker compose @composeArgs ps` 确认 MySQL 与 Redis 为 `healthy`。请求 `curl.exe --fail http://127.0.0.1:8080/get_test`，观察网关 HTTP 返回；确认客户端实际打开登录窗口。服务退出时在其可执行文件目录前台运行，查看报错。
- 需要验证注册时使用已获授权的邮箱；需要验证聊天时，用真实账号登录并发送消息。缺少邮箱或账号时，说明未检查的链路，不把端口连通当作业务成功。
- 用户要求停止时，仅关闭本次启动的应用窗口，并执行 `docker compose @composeArgs stop`。保留数据卷和私有配置。