@echo off
setlocal EnableExtensions

rem ============================================================
rem Redis launcher for DistributeLock/test.cpp.
rem This uses the same Redis installation and redis.windows.conf as start_all.bat.
rem The config must listen on 127.0.0.1:6379 and use requirepass 123456.
rem This file is intentionally ASCII-only: cmd.exe can run it correctly even when
rem the system code page is not UTF-8.
rem ============================================================

set "REDIS=D:\cppsoft\Redis-x64-5.0.14.1"
set "REDIS_HOST=127.0.0.1"
set "REDIS_PORT=6379"
set "REDIS_PASSWORD=123456"

if not exist "%REDIS%\redis-server.exe" (
    echo [ERROR] redis-server.exe was not found: %REDIS%\redis-server.exe
    echo Update the REDIS variable in this script to match your installation.
    goto :failure
)

if not exist "%REDIS%\redis.windows.conf" (
    echo [ERROR] Redis config was not found: %REDIS%\redis.windows.conf
    goto :failure
)

rem Only an exact PONG means that Redis is healthy and the password is correct.
rem redis-cli can return exit code 0 for an error reply such as MISCONF, so do not
rem use the exit code alone as a health check.
call :ping_redis
if not errorlevel 1 (
    echo Redis is already healthy at %REDIS_HOST%:%REDIS_PORT%.
    goto :success
)

rem Do not launch another server when a non-healthy process already owns the port.
netstat -ano | findstr /r /c:":%REDIS_PORT% .*LISTENING" >nul
if not errorlevel 1 (
    echo [ERROR] Port %REDIS_PORT% is occupied, but Redis PING did not return PONG.
    echo Check the existing Redis password, log output, and RDB persistence error.
    goto :failure
)

echo Starting Redis at %REDIS_HOST%:%REDIS_PORT% with password %REDIS_PASSWORD%...
start "Redis for DistributeLock" cmd /k "cd /d %REDIS% && redis-server.exe redis.windows.conf"

rem Wait at most ten seconds for server startup and AUTH + PING to succeed.
for /l %%i in (1,1,10) do (
    timeout /t 1 /nobreak >nul
    call :ping_redis
    if not errorlevel 1 (
        echo Redis is ready. You can now run distribute_lock_test.exe.
        goto :success
    )
)

echo [ERROR] Redis did not become healthy in 10 seconds.
echo Check redis.windows.conf: port must be 6379 and requirepass must be 123456.
goto :failure

:ping_redis
set "PING_REPLY="
for /f "delims=" %%p in ('"%REDIS%\redis-cli.exe" -h %REDIS_HOST% -p %REDIS_PORT% -a %REDIS_PASSWORD% ping 2^>nul') do set "PING_REPLY=%%p"
if /i "%PING_REPLY%"=="PONG" exit /b 0
exit /b 1

:success
echo.
echo Redis is ready.
if /i "%~1"=="--no-pause" exit /b 0
pause
exit /b 0

:failure
echo.
echo Redis is not ready. This window stays open so you can read the error above.
if /i "%~1"=="--no-pause" exit /b 1
pause
exit /b 1
