@echo off
chcp 65001 >nul
setlocal
rem ============================================================
rem  chat_app 后端一键启动脚本（Redis + 5 个服务 = 6 个 cmd 窗口）
rem
rem  前置条件：
rem    1. 先编译（顶层 CMake 会构建 GateServer / StatusServer / ChatServer1 / ChatServer2 四个 C++ 服务）：
rem       "D:\Qt\qt\Tools\CMake_64\bin\cmake.exe" --preset windows-vcpkg
rem       "D:\Qt\qt\Tools\CMake_64\bin\cmake.exe" --build --preset debug
rem    2. MySQL / Redis 已启动
rem    3. VarifyServer 依赖已安装：cd VarifyServer && npm install
rem
rem  启动顺序：MySQL(手动) → Redis → VarifyServer → StatusServer → ChatServer1 → ChatServer2 → GateServer
rem ============================================================

set "ROOT=D:\myproject\chat_app"
set "REDIS=D:\cppsoft\Redis-x64-5.0.14.1"
rem 想用 Release 版就把下面改成 Release
set "CFG=Debug"
set "STATUS=%ROOT%\build\StatusServer\%CFG%"
set "CHAT1=%ROOT%\build\ChatServer1\%CFG%"
set "CHAT2=%ROOT%\build\ChatServer2\%CFG%"
set "GATE=%ROOT%\build\GateServer\%CFG%"

echo [1/6] 启动 Redis...
if not exist "%REDIS%\redis-server.exe" (
    echo    [错误] 找不到 redis-server.exe，请检查路径：%REDIS%
    pause
    exit /b 1
)
start "Redis" cmd /k "cd /d %REDIS% && redis-server.exe redis.windows.conf"

echo [2/6] 启动 VarifyServer...
if not exist "%ROOT%\VarifyServer\package.json" (
    echo    [错误] 找不到 VarifyServer\package.json，请确认项目完整
    pause
    exit /b 1
)
if not exist "%ROOT%\VarifyServer\node_modules" (
    echo    [提示] VarifyServer 还没装依赖，先执行：cd %ROOT%\VarifyServer ^&^& npm install
)
start "VarifyServer" cmd /k "cd /d %ROOT%\VarifyServer && npm run serve"

echo [3/6] 启动 StatusServer...
if not exist "%STATUS%\StatusServer.exe" (
    echo    [错误] 找不到 StatusServer.exe，请先编译！
    pause
    exit /b 1
)
start "StatusServer" cmd /k "cd /d %STATUS% && StatusServer.exe"

echo [4/6] 启动 ChatServer1（端口 8090 / rpc 50055）...
if not exist "%CHAT1%\ChatServer1.exe" (
    echo    [错误] 找不到 ChatServer1.exe，请先编译！
    pause
    exit /b 1
)
start "ChatServer1" cmd /k "cd /d %CHAT1% && ChatServer1.exe"

echo [5/6] 启动 ChatServer2（端口 8091 / rpc 50056）...
if not exist "%CHAT2%\ChatServer2.exe" (
    echo    [错误] 找不到 ChatServer2.exe，请先编译！
    pause
    exit /b 1
)
start "ChatServer2" cmd /k "cd /d %CHAT2% && ChatServer2.exe"

echo [6/6] 启动 GateServer...
if not exist "%GATE%\GateServer.exe" (
    echo    [错误] 找不到 GateServer.exe，请先编译！
    pause
    exit /b 1
)
start "GateServer" cmd /k "cd /d %GATE% && GateServer.exe"

echo.
echo 全部启动完成（Redis + 五个服务），关闭对应 cmd 窗口即可停止。
pause
