@echo off
chcp 65001 >nul
setlocal
rem Start Redis and all six backend services.
rem Build all C++ targets before use and run npm install in VarifyServer once.
rem The start command uses /D for the working directory and avoids nested commands.

set "ROOT=D:\myproject\chat_app"
set "REDIS=D:\cppsoft\Redis-x64-5.0.14.1"
rem Change Debug to Release when starting Release binaries.
set "CFG=Debug"
set "STATUS=%ROOT%\build\StatusServer\%CFG%"
set "CHAT1=%ROOT%\build\ChatServer1\%CFG%"
set "CHAT2=%ROOT%\build\ChatServer2\%CFG%"
set "RESOURCE=%ROOT%\build\ResourceServer\%CFG%"
set "GATE=%ROOT%\build\GateServer\%CFG%"

echo [1/7] Starting Redis...
if not exist "%REDIS%\redis-server.exe" (
    echo [ERROR] redis-server.exe was not found: %REDIS%
    pause
    exit /b 1
)
start "Redis" /D "%REDIS%" cmd /k redis-server.exe redis.windows.conf

echo [2/7] Starting VarifyServer...
if not exist "%ROOT%\VarifyServer\package.json" (
    echo [ERROR] VarifyServer\package.json was not found.
    pause
    exit /b 1
)
if not exist "%ROOT%\VarifyServer\node_modules" (
    echo [INFO] Run npm install in %ROOT%\VarifyServer before starting this service.
)
start "VarifyServer" /D "%ROOT%\VarifyServer" cmd /k npm run serve

echo [3/7] Starting StatusServer...
if not exist "%STATUS%\StatusServer.exe" (
    echo [ERROR] StatusServer.exe was not found. Build the project first.
    pause
    exit /b 1
)
start "StatusServer" /D "%STATUS%" cmd /k StatusServer.exe

echo [4/7] Starting ChatServer1 (TCP 8090, RPC 50055)...
if not exist "%CHAT1%\ChatServer1.exe" (
    echo [ERROR] ChatServer1.exe was not found. Build the project first.
    pause
    exit /b 1
)
start "ChatServer1" /D "%CHAT1%" cmd /k ChatServer1.exe

echo [5/7] Starting ChatServer2 (TCP 8091, RPC 50056)...
if not exist "%CHAT2%\ChatServer2.exe" (
    echo [ERROR] ChatServer2.exe was not found. Build the project first.
    pause
    exit /b 1
)
start "ChatServer2" /D "%CHAT2%" cmd /k ChatServer2.exe

rem ResourceServer owns the independent TCP upload endpoint on port 9090.
echo [6/7] Starting ResourceServer (TCP 9090)...
if not exist "%RESOURCE%\ResourceServer.exe" (
    echo [ERROR] ResourceServer.exe was not found. Build the project first.
    pause
    exit /b 1
)
start "ResourceServer" /D "%RESOURCE%" cmd /k ResourceServer.exe

echo [7/7] Starting GateServer...
if not exist "%GATE%\GateServer.exe" (
    echo [ERROR] GateServer.exe was not found. Build the project first.
    pause
    exit /b 1
)
start "GateServer" /D "%GATE%" cmd /k GateServer.exe

echo.
echo All services were started. Close each command window to stop its service.
pause
