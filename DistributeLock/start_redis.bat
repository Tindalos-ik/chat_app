@rem 使用项目的 Docker Compose 启动 Redis，供独立锁测试使用。
@echo off
setlocal EnableExtensions

set "ENV_FILE=%LOCALAPPDATA%\chat_app\docker\.env"
if not exist "%ENV_FILE%" (
    echo [ERROR] Missing Compose environment file: "%ENV_FILE%"
    exit /b 1
)

docker compose --env-file "%ENV_FILE%" -f "%~dp0..\deploy\compose.yaml" up -d --wait --wait-timeout 60 redis
exit /b %errorlevel%
