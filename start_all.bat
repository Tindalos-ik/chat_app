@echo off
chcp 65001 >nul
setlocal
rem ============================================================
rem  chat_app 后端一键启动脚本（Redis + 4 个服务 = 5 个 cmd 窗口）
rem
rem  前置条件：
rem    1. 先编译（建议用 Qt 的 cmake）：
rem       "D:\Qt\qt\Tools\CMake_64\bin\cmake.exe" --preset windows-vcpkg
rem       "D:\Qt\qt\Tools\CMake_64\bin\cmake.exe" --build --preset debug
rem    2. MySQL / Redis 已启动
rem    3. VarifyServer 依赖已安装：cd VarifyServer && npm install
rem
rem  启动顺序：MySQL(手动) → Redis → VarifyServer → StatusServer → ChatServer → GateServer
rem ============================================================

set "ROOT=D:\myproject\chat_app"
set "REDIS=D:\cppsoft\Redis-x64-5.0.14.1"
rem 想用 Release 版就把下面改成 Release
set "CFG=Debug"
set "STATUS=%ROOT%\build\StatusServer\%CFG%"
set "CHAT=%ROOT%\build\ChatServer\%CFG%"
set "GATE=%ROOT%\build\GateServer\%CFG%"

echo [1/5] 启动 Redis...
if not exist "%REDIS%\redis-server.exe" (
    echo    [错误] 找不到 redis-server.exe，请检查路径：%REDIS%
    pause
    exit /b 1
)
start "Redis" cmd /k "cd /d %REDIS% && redis-server.exe redis.windows.conf"

echo [2/5] 启动 VarifyServer...
if not exist "%ROOT%\VarifyServer\package.json" (
    echo    [错误] 找不到 VarifyServer\package.json，请确认项目完整
    pause
    exit /b 1
)
if not exist "%ROOT%\VarifyServer\node_modules" (
    echo    [提示] VarifyServer 还没装依赖，先执行：cd %ROOT%\VarifyServer ^&^& npm install
)
start "VarifyServer" cmd /k "cd /d %ROOT%\VarifyServer && npm run serve"

echo [3/5] 启动 StatusServer...
if not exist "%STATUS%\StatusServer.exe" (
    echo    [错误] 找不到 StatusServer.exe，请先编译！
    pause
    exit /b 1
)
start "StatusServer" cmd /k "cd /d %STATUS% && StatusServer.exe"

echo [4/5] 启动 ChatServer...
if not exist "%CHAT%\ChatServer.exe" (
    echo    [错误] 找不到 ChatServer.exe，请先编译！
    pause
    exit /b 1
)
start "ChatServer" cmd /k "cd /d %CHAT% && ChatServer.exe"

echo [5/5] 启动 GateServer...
if not exist "%GATE%\GateServer.exe" (
    echo    [错误] 找不到 GateServer.exe，请先编译！
    pause
    exit /b 1
)
start "GateServer" cmd /k "cd /d %GATE% && GateServer.exe"

echo.
echo 全部启动完成（Redis + 四个服务），关闭对应 cmd 窗口即可停止。
pause
