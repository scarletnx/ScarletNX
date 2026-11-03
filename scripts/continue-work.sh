#!/usr/bin/env bash
# Continue the ScarletNX HDR/upscaling session until docs/REMAINING.md is done.
#
# The watcher runs the same OpenCode session again after each answer. It stops when
# docs/.work-complete exists, or when docs/.work-stop exists. Set OPENCODE_CONTINUE_MAX
# to bound the number of answers, and OPENCODE_CONTINUE_SESSION to another session.
set -u

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
session="${OPENCODE_CONTINUE_SESSION:-ses_e5ca286c7ffejD29JYJSJjpxwQ}"
sentinel="$root/docs/.work-complete"
stop="$root/docs/.work-stop"
log="${OPENCODE_CONTINUE_LOG:-$root/docs/continue-work.log}"
max="${OPENCODE_CONTINUE_MAX:-200}"
delay="${OPENCODE_CONTINUE_DELAY:-15}"
exec 9>"$root/docs/.continue.lock"
flock -n 9 || exit 0

prompt='Continue the ScarletNX work. Read CLAUDE.md and docs/REMAINING.md first. Work through the checklist in order. Do not stop while a box is unchecked. Build, run the tests, and commit each verified step with an SSH signature. Never push. When every box is checked and the full verification passed, update the demo folder and create docs/.work-complete with the date and commit hash. If a task cannot be finished, write the mechanism and the next step in docs/issues/ and leave the box unchecked, then continue with the next box. Do not ask the user anything.'

mkdir -p "$(dirname -- "$log")"
: >"$log"
for i in $(seq 1 "$max"); do
    if [ -e "$sentinel" ]; then
        printf 'continue-work: done, %s exists\n' "$sentinel" >>"$log"
        exit 0
    fi
    if [ -e "$stop" ]; then
        printf 'continue-work: stopped by %s\n' "$stop" >>"$log"
        exit 0
    fi
    printf '\n===== continue-work run %s at %s =====\n' "$i" "$(date -Is)" >>"$log"
    (cd "$root" && opencode run --session "$session" --auto "$prompt") >>"$log" 2>&1
    printf 'continue-work: run %s exited %s\n' "$i" "$?" >>"$log"
    sleep "$delay"
done
printf 'continue-work: %s runs reached, leaving the rest for a person\n' "$max" >>"$log"
exit 1
