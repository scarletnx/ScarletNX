#!/usr/bin/env bash
# Check whether the ScarletNX work session is still moving. If it has not been busy for
# 15 minutes, run the continuation watcher. The watcher holds docs/.continue.lock, so a
# second run never starts.
set -u

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
session="${OPENCODE_CONTINUE_SESSION:-ses_e5ca286c7ffejD29JYJSJjpxwQ}"
db="${OPENCODE_DB:-$HOME/.local/share/opencode/opencode.db}"
idle_ms="${OPENCODE_IDLE_MS:-900000}"
sentinel="$root/docs/.work-complete"
stop="$root/docs/.work-stop"

[ -e "$sentinel" ] && exit 0
[ -e "$stop" ] && exit 0

now="$(date +%s%3N)"
last="$(sqlite3 "$db" "select max(time_updated, coalesce(time_busy, 0), coalesce(time_idle, 0)) from session_v2 where id='$session';" 2>/dev/null)"
if [ -n "$last" ] && [ "$last" -gt 0 ] 2>/dev/null; then
    busy_for=$((now - last))
    if [ "$busy_for" -lt "$idle_ms" ]; then
        printf 'check-work: session is active, last change %s ms ago\n' "$busy_for"
        exit 0
    fi
    printf 'check-work: session idle %s ms, starting the watcher\n' "$busy_for"
else
    printf 'check-work: no session row, starting the watcher\n'
fi
exec "$root/scripts/continue-work.sh"
