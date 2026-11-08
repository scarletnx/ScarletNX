#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Shows that a change for big-endian hosts leaves a little-endian host's code as it was (docs/hosts/ppc64.md): it builds
# BASE and the working tree with the default preset, then compares
#   - the disassembly of every object of the project (not of the third-party packages), and
#   - the code the recompiler makes for an AArch64 program, through each build's scarletnx-gnuabi.
# Run it on an x86-64 or AArch64 machine. It prints each object whose code differs: every one must be a file the change
# meant to change, and the commit says why.
#
#   scripts/le-unchanged.sh [--gnuabi-only] [BASE]     (BASE: default the merge base with origin/main)
set -euo pipefail

nx=ON
targets=()
if [ "${1:-}" = --gnuabi-only ]; then # the GNU ABI runtime and the recompiler under it, not the tests
    nx=OFF
    targets=(--target scarletnx-gnuabi) # the library, its command line in it: a target both trees have
    shift
fi
root=$(git rev-parse --show-toplevel)
base=${1:-$(git -C "$root" merge-base HEAD origin/main)}
work="$root/.cache/le-unchanged"
mkdir -p "$work"

# BASE as a worktree of its own, refreshed to the commit asked for
if [ -d "$work/src" ]; then
    git -C "$work/src" checkout -q --detach "$base"
else
    git -C "$root" worktree add -q --detach "$work/src" "$base"
fi

build() { # build SOURCE DIR
    (cd "$1" && cmake --preset default -B "$2" -DSNX_BUILD_NX=$nx -DSNX_BUILD_GNUABI=ON -DSNX_DEPLOY=OFF \
        -DCPM_SOURCE_CACHE="$root/.cache/cpm" -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache >"$2.configure.log" 2>&1) || {
        tail -20 "$2.configure.log"
        exit 1
    }
    cmake --build "$2" "${targets[@]}" >"$2.build.log" 2>&1 || {
        grep -m 20 -E 'error|FAILED' "$2.build.log"
        exit 1
    }
}
# a build folder per mode, and the base's per commit: a folder another configuration built keeps objects this one
# doesn't make, which would show as changes
short=$(git -C "$root" rev-parse --short "$base")
mode=$([ $nx = ON ] && echo all || echo gnuabi)
basedir="$work/base-$mode-$short" headdir="$work/head-$mode"
echo "building $short and the working tree"
build "$work/src" "$basedir"
build "$root" "$headdir"

# the project's objects: everything under the build tree but the packages CPM fetched (in _deps, or compiled from the
# CPM cache, whose path differs between the two source trees)
objects() { (cd "$1" && find . -name '*.o' -not -path './_deps/*' -not -path '*/cpm/*' | sort); }
code() { llvm-objdump -d --no-show-raw-insn --no-leading-addr "$1" 2>/dev/null | tail -n +3; }
differ=0
while read -r o; do
    if [ ! -f "$basedir/$o" ]; then
        echo "new:     $o"
        continue
    fi
    if ! cmp -s <(code "$basedir/$o") <(code "$headdir/$o"); then
        echo "changed: $o"
        differ=1
    fi
done < <(objects "$headdir")
comm -23 <(objects "$basedir") <(objects "$headdir") | sed 's/^/gone:    /'

# the recompiler's output: one freestanding AArch64 program, compiled by each build into a cache of its own
fixture="$work/cpu.elf"
clang --target=aarch64-linux-gnu -fuse-ld=lld -static -O2 -ffreestanding -fno-builtin -fno-stack-protector \
    -fno-pie -no-pie -nostdlib -march=armv8-a -Wl,-e,_start -I "$root/tests/gnuabi" \
    "$root/tests/gnuabi/start.S" "$root/tests/gnuabi/cpu.c" "$root/tests/gnuabi/boundary.S" -o "$fixture"
texts() { # the .text of each module in a cache, by hash, sorted: cache names carry the build's runtime library hash
    find "$1" -type f | while read -r m; do
        llvm-objcopy -O binary --only-section=.text "$m" /dev/stdout 2>/dev/null | sha256sum | cut -d' ' -f1
    done | sort
}
run() { # run BUILD NAME: the fixture through a build's scarletnx-gnuabi, with a cache of its own
    rm -rf "$work/cache-$2"
    # stdout and the exit status: stderr has notes about where the build is (a data folder beside the checkout)
    "$1/scarletnx-gnuabi" --jit-cache="$work/cache-$2" "$fixture" >"$work/run-$2.out" 2>/dev/null
    echo "exit $?" >>"$work/run-$2.out"
}
run "$basedir" base
run "$headdir" head
if ! cmp -s "$work/run-base.out" "$work/run-head.out"; then
    echo "changed: the program's output"
    differ=1
fi
if ! cmp -s <(texts "$work/cache-base") <(texts "$work/cache-head"); then
    echo "changed: the recompiled program's code"
    differ=1
fi
[ $differ = 0 ] && echo "little-endian code unchanged"
exit 0
