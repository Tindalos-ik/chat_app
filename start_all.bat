@rem 使用 Docker Compose 启动数据库，并从 CMake 产物目录启动 Windows 后端。
@echo off
setlocal EnableExtensions

set "ROOT=%~dp0"
set "BUILD=%ROOT%build"
if not "%~1"=="" set "BUILD=%~f1"
set "ENV_FILE=%LOCALAPPDATA%\chat_app\docker\.env"
set "COMPOSE_FILE=%ROOT%deploy\compose.yaml"

if not exist "%ENV_FILE%" (
    echo [ERROR] Missing Compose environment file: "%ENV_FILE%"
    exit /b 1
)
if not exist "%ROOT%VarifyServer\config.json" (
    echo [ERROR] Configure VarifyServer\config.json before starting.
    exit /b 1
)
if not exist "%ROOT%VarifyServer\node_modules\" (
    echo [ERROR] Run npm.cmd ci in VarifyServer before starting.
    exit /b 1
)
where npm.cmd >nul 2>&1 || (
    echo [ERROR] npm.cmd was not found on PATH.
    exit /b 1
)
for %%S in (StatusServer ResourceServer ChatServer1 ChatServer2 GateServer) do (
    if not exist "%BUILD%\%%S\Debug\%%S.exe" (
        echo [ERROR] Missing Debug executable: "%BUILD%\%%S\Debug\%%S.exe"
        goto :failure
    )
    if not exist "%BUILD%\%%S\Debug\config.ini" (
        echo [ERROR] Missing runtime configuration: "%BUILD%\%%S\Debug\config.ini"
        goto :failure
    )
)
if defined VCPKG_ROOT set "PATH=%VCPKG_ROOT%\installed\x64-windows\debug\bin;%VCPKG_ROOT%\installed\x64-windows\bin;%PATH%"

docker compose --env-file "%ENV_FILE%" -f "%COMPOSE_FILE%" up -d --wait --wait-timeout 180
if errorlevel 1 (
    echo [ERROR] MySQL and Redis did not become healthy.
    exit /b 1
)

start "VarifyServer" /D "%ROOT%VarifyServer" cmd.exe /k npm.cmd run serve || exit /b 1
for %%S in (StatusServer ResourceServer ChatServer1 ChatServer2 GateServer) do (
    start "%%S" /D "%BUILD%\%%S\Debug" cmd.exe /k %%S.exe || goto :failure
)
echo Backend windows opened. Start ZLMediaKit separately if video or audio is enabled.
exit /b 0

:failure
exit /b 1
