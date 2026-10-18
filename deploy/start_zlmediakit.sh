#!/usr/bin/env bash
# Windows 写入心跳；窗口退出后管道 EOF，或 5 秒没有心跳，只停止本次启动的进程。
set -u

media_server=$1
config=$2
log_dir=$3

if [[ ! -x "$media_server" || ! -f "$config" ]]; then
    printf '[ERROR] MediaServer or configuration not found: %s ; %s\n' "$media_server" "$config" >&2
    exit 1
fi
mkdir -p -- "$log_dir" || exit 1

# 锁与配置绑定；重复启动不接管或停止先前窗口拥有的实例。
exec 9<"$config"
if ! flock -n 9; then
    printf '[ERROR] ZLMediaKit is already managed by another window: %s\n' "$config" >&2
    exit 1
fi

media_pid=
cleanup() {
    trap '' HUP INT TERM
    if [[ -n "$media_pid" ]] && kill -0 "$media_pid" 2>/dev/null; then
        printf '\n[INFO] Stopping ZLMediaKit (PID %s)...\n' "$media_pid"
        kill -TERM "$media_pid" 2>/dev/null || true
        wait "$media_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' HUP TERM

# 显式保留监护进程的 stdin；MediaServer 不读取心跳，也不继承配置锁。
"$media_server" -c "$config" -t 2 -l 2 --log-dir "$log_dir" </dev/null 9<&- &
media_pid=$!
printf '[INFO] ZLMediaKit PID %s; close this window to stop it.\n' "$media_pid"
while IFS= read -r -t 5 heartbeat; do
    if ! kill -0 "$media_pid" 2>/dev/null; then
        wait "$media_pid"
        result=$?
        media_pid=
        exit "$result"
    fi
done
