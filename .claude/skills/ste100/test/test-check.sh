#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The test of ste100/check.py: each rule in fixtures/flagged.md, nothing in fixtures/clean.md, the exit codes, and
# the hook for an edit, a prompt and a report.
cd "$(dirname "$0")" || exit 1
CHECK=${CHECK:-../check.py}
fail=0
got=$(python3 "$CHECK" fixtures/flagged.md 2>/dev/null)
[ $? -eq 1 ] || { echo "FAIL: flagged.md must exit 1"; fail=1; }
for rule in passive tense length semicolon contraction word ing paragraph; do
    printf '%s\n' "$got" | grep -q ": $rule: " || { echo "FAIL: no $rule rule break in flagged.md"; fail=1; }
done
out=$(python3 "$CHECK" fixtures/clean.md 2>&1) || { echo "FAIL: clean.md has rule breaks:"; echo "$out"; fail=1; }
python3 "$CHECK" fixtures/no-such-file.md >/dev/null 2>&1
[ $? -eq 2 ] || { echo "FAIL: a path that the checker cannot read must exit 2"; fail=1; }
# A prompt to a subagent: the hook stops a bad one (exit 2) and lets a good one go (exit 0)
printf '%s' '{"hook_event_name":"PreToolUse","tool_name":"Agent","tool_input":{"prompt":"The file is read by the agent."}}' |
    python3 "$CHECK" --hook 2>/dev/null
[ $? -eq 2 ] || { echo "FAIL: the hook must stop a prompt in the passive voice"; fail=1; }
printf '%s' '{"hook_event_name":"PreToolUse","tool_name":"Agent","tool_input":{"prompt":"Read the file."}}' |
    python3 "$CHECK" --hook 2>/dev/null || { echo "FAIL: the hook must let a good prompt go"; fail=1; }
# An edit to a Claude file: only the changed lines count
dir=$(mktemp -d) && mkdir -p "$dir/.claude" && printf 'The file is read.\n\nOld text.\n' > "$dir/.claude/a.md"
printf '{"hook_event_name":"PreToolUse","tool_name":"Edit","tool_input":{"file_path":"%s","old_string":"Old text.","new_string":"New text."}}' "$dir/.claude/a.md" |
    python3 "$CHECK" --hook 2>/dev/null || { echo "FAIL: an edit must not stop for a line that it does not change"; fail=1; }
printf '{"hook_event_name":"PreToolUse","tool_name":"Edit","tool_input":{"file_path":"%s","old_string":"Old text.","new_string":"It is done."}}' "$dir/.claude/a.md" |
    python3 "$CHECK" --hook 2>/dev/null
[ $? -eq 2 ] || { echo "FAIL: the hook must stop an edit that adds a rule break"; fail=1; }
rm -rf "$dir"
# A report: the hook asks for it again with a block decision. printf's %s takes the agent's ID as an argument, because
# bash 3.2, the sh of macOS, drops $$ that stands between quotes inside $( ). The hook counts the rewrites of a report
# in TMPDIR/snx-ste100, so the test gives it a folder of its own: a count left by an earlier run would pass the third
dir=$(mktemp -d)
r=$(printf '{"hook_event_name":"SubagentStop","session_id":"t","agent_id":"%s","last_assistant_message":"It is done."}' "t$$" |
    TMPDIR=$dir python3 "$CHECK" --hook)
rm -rf "$dir"
printf '%s' "$r" | grep -q '"decision": "block"' || { echo "FAIL: the hook must block a report with a rule break"; fail=1; }
[ $fail -eq 0 ] && echo "check.py: each rule in flagged.md, none in clean.md, exit codes and hooks: passed"
exit $fail
