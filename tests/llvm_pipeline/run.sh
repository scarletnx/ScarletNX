#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
: "${CC:=clang}"
: "${LLVM_LIBRARY:=$(llvm-config --libdir 2>/dev/null)/libLLVM.so}"
if [[ ! -f "$LLVM_LIBRARY" ]]; then echo "shared LLVM library required: $LLVM_LIBRARY" >&2; exit 2; fi
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
python3 create_emit_test.py "$build/emit_test.c"
"$CC" -std=gnu23 -O2 "$build/emit_test.c" "$LLVM_LIBRARY" -o "$build/emit_test"
"$CC" -std=gnu23 -O2 bitcode_view_test.c "$LLVM_LIBRARY" -o "$build/bitcode_view_test"
"$build/bitcode_view_test"
"$build/emit_test"
"$CC" -std=gnu23 -O2 -D_POSIX_C_SOURCE=200809L passbench.c "$LLVM_LIBRARY" -o "$build/passbench"
"$build/passbench" fixture.ll 'default<O2>' -1 "$build/default.o" > "$build/default.report"
"$build/passbench" fixture.ll 'default<O2>' 100 "$build/bounded.o" > "$build/bounded.report"
"$CC" -shared "$build/default.o" -o "$build/default.so"
"$CC" -shared "$build/bounded.o" -o "$build/bounded.so"
python3 compare.py "$build/default.so" "$build/bounded.so" "$build/default.report" "$build/bounded.report"
