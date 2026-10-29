#!/bin/sh
# Gate test of snx-code-quality/check.py: each rule found once in fixtures/flagged.c, nothing in fixtures/clean.c
# (near misses: a feature flag, two pointers, padded atomics, extern atomics, text in comments and strings, two
# owners, a const table), exit 0 with candidates and 2 on a path it cannot read. CHECK=<edited copy> runs a mutant.
cd "$(dirname "$0")" || exit 1
CHECK=${CHECK:-../check.py}
fail=0
got=$(python3 "$CHECK" fixtures/flagged.c fixtures/clean.c 2>/dev/null)
status=$?
if [ "$got" != "$(cat fixtures/expected.txt)" ]; then
    echo "FAIL: candidates differ"; printf '%s\n' "$got" | diff fixtures/expected.txt - ; fail=1
fi
[ $status -eq 0 ] || { echo "FAIL: exit $status with candidates, want 0"; fail=1; }
python3 "$CHECK" fixtures/no-such-file.c >/dev/null 2>&1
[ $? -eq 2 ] || { echo "FAIL: an unreadable path must exit 2"; fail=1; }
[ -z "$(python3 "$CHECK" fixtures/clean.c 2>/dev/null)" ] || { echo "FAIL: clean.c has candidates"; fail=1; }
[ $fail -eq 0 ] && echo "check.py: 8 candidates in flagged.c, none in clean.c, exit codes: passed"
exit $fail
