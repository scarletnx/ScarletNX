#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
: "${CC:=clang}"
: "${LLVM_LIBRARY:=$(llvm-config --libdir 2>/dev/null)/libLLVM.so}"
python3 "$root/tests/llvm_perf/test_nzcv.py" "$work/nzcv.c"
"$CC" -std=gnu23 -O2 -I "$root/src" "$work/nzcv.c" "$LLVM_LIBRARY" -o "$work/nzcv"
"$work/nzcv"
python3 "$root/tests/llvm_perf/test_rotate.py" "$work/rotate.c"
"$CC" -std=gnu23 -O2 "$work/rotate.c" "$LLVM_LIBRARY" -o "$work/rotate"
"$work/rotate"
python3 "$root/tests/llvm_perf/test_profile.py" "$work/profile.c"
"$CC" -std=gnu23 -O2 "$work/profile.c" "$LLVM_LIBRARY" -o "$work/profile"
"$work/profile" "$root/tests/llvm_perf/profile.txt"
