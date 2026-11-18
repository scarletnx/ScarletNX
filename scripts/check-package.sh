#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Runs a packaged ScarletNX as a user would, in an empty folder of a machine without the checkout it was built from:
# the AppImage, or bin/scarletnx(.exe) of `cmake --install` (the Windows zip). CI runs it at the end of the jobs that
# make the packages (.github/workflows/build.yml), where nothing of the build stands in for what an installed copy
# lacks: the AppImage in a container of its own, without the build's tools, checkout and tree, the cross ones on the
# runner, which has neither the build container's checkout nor its tree, and the zip in Git Bash, without MSYS2 on
# PATH. An installed copy never takes the checkout of the tree it was built in (snx_checkout).
#
#   scripts/check-package.sh dist/ScarletNX-linux-x86_64.AppImage
#   scripts/check-package.sh stage/bin/scarletnx.exe
set -euo pipefail

if [ $# -ne 1 ] || [ ! -f "$1" ]; then
    echo "usage: $0 PACKAGED_SCARLETNX" >&2
    exit 2
fi
runtime=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
scripts=$(cd "$(dirname "$0")" && pwd)
exe=
case $runtime in
    *.exe) exe=.exe ;;
    # CI runners have no FUSE to mount it
    *.AppImage) export APPIMAGE_EXTRACT_AND_RUN=1 ;;
esac
# An AppImage of another architecture than this machine's runs through qemu-user, by its absolute path: the kernel's
# binfmt handler never takes an AppImage, since its runtime's ELF header carries the AppImage magic ("AI\x02" at byte 8)
# where qemu's mask wants zeros. The plain ELF programs it starts go through that handler, which has to be registered;
# the games it makes are copies of it, which need one more blind to that padding (the CI's cross check registers it)
launch=()
if [ -z "$exe" ]; then
    # e_machine in the byte order EI_DATA names: 1 little-endian, 2 big-endian (PowerPC64 BE's)
    set -- $(od -An -tu1 -j5 -N1 "$runtime") $(od -An -tu1 -j18 -N2 "$runtime")
    case $1:$(($1 == 2 ? $2 * 256 + $3 : $3 * 256 + $2)) in
        1:62) arch=x86_64 ;;
        1:183) arch=aarch64 ;;
        1:21) arch=ppc64le ;;
        2:21) arch=ppc64 ;;
        1:243) arch=riscv64 ;;
        1:258) arch=loongarch64 ;;
        *) arch= ;;
    esac
    if [ -z "$arch" ]; then
        echo "$runtime is not an x86-64, AArch64, PowerPC64, RISC-V 64 or LoongArch64 program" >&2
        exit 2
    fi
    if [ "$arch" != "$(uname -m)" ]; then
        launch=("$(command -v "qemu-$arch")") || {
            echo "$runtime is a $arch program: qemu-$arch runs it here, and it is not on PATH" >&2
            exit 2
        }
    fi
fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
# An installed copy keeps recompiled/ and prod.keys in the user's data folder, wherever it was started from, and a game's
# settings, log and caches (work/ too) in the XDG config, state and cache folders on Linux, in config/, state/ and
# cache/ of the data folder elsewhere: point them into this run's
export XDG_DATA_HOME="$work/data" XDG_CONFIG_HOME="$work/config" XDG_STATE_HOME="$work/state" XDG_CACHE_HOME="$work/cache"
data=$XDG_DATA_HOME/scarletnx
config=$XDG_CONFIG_HOME/scarletnx/recompiled
state=$XDG_STATE_HOME/scarletnx/recompiled
if [ -n "$exe" ]; then
    APPDATA=$(cygpath -w "$work/data")
    export APPDATA
    config=$data/config/recompiled
    state=$data/state/recompiled
fi

failed=0
pass() { echo "ok: $1"; }
fail() {
    echo "FAILED: $1" >&2
    shift
    printf '%s\n' "$@" | sed 's/^/    /' >&2
    failed=1
}
# the command's output, whatever its exit status; the status in $status
run() {
    status=0
    out=$(timeout 300 "$@" 2>&1) || status=$?
}

# tests/nx_runtime_smoke.py's homebrew: an NRO whose code prints a line through svcOutputDebugString and exits
put() { printf '%b' "$3" | dd of="$1" bs=1 seek=$(($2)) conv=notrunc status=none; }
word() { put "$1" "$2" "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' $(($3 & 255)) $(($3 >> 8 & 255)) $(($3 >> 16 & 255)) $(($3 >> 24)))"; }
head -c 512 /dev/zero >smoke.nro
word smoke.nro 0x00 0x14000020 # B +0x80, over the header
put smoke.nro 0x10 NRO0
word smoke.nro 0x18 0x200
for field in 0x20:0 0x24:0x100 0x28:0x100 0x2c:0x80 0x30:0x180 0x34:0x80; do # text, rodata, data
    word smoke.nro "${field%:*}" "${field#*:}"
done
word smoke.nro 0x80 0x10000400 # ADR X0, the line in rodata
word smoke.nro 0x84 0xd2800221 # MOV X1, its 17 bytes
word smoke.nro 0x88 0xd40004e1 # SVC 0x27, OutputDebugString
word smoke.nro 0x8c 0xd40000e1 # SVC 7, ExitProcess
word smoke.nro 0x90 0xd4200000 # BRK
put smoke.nro 0x100 'NX runtime smoke\n'

# A package of the Linux runtime alone (a big-endian host's) runs Linux AArch64 programs, and nothing else here
run "${launch[@]}" "$runtime" --help
if grep -q '^Usage: scarletnx-gnuabi' <<<"$out"; then
    if [ "$status" -eq 0 ]; then pass "scarletnx-gnuabi --help"; else fail "scarletnx-gnuabi --help exited $status" "$out"; fi
    "$scripts/aarch64-smoke.sh" smoke
    run "${launch[@]}" "$runtime" ./smoke
    if [ "$status" -eq 42 ] && grep -qx 'gnuabi smoke' <<<"$out"; then
        pass "it runs an AArch64 program"
    else
        fail "the AArch64 program exited $status, not 42 with its line" "$out"
    fi
    exit $failed
fi

for mode in "" -x -r; do
    run "${launch[@]}" "$runtime" ${mode:+"$mode"} --help
    if [ "$status" -eq 0 ] && grep -q '^Usage: scarletnx' <<<"$out"; then
        pass "scarletnx${mode:+ $mode} --help"
    else
        fail "scarletnx${mode:+ $mode} --help exited $status" "$out"
    fi
done

# No --outdir: an installed copy has no checkout, so its games go in the data folder, each in a folder of its own
game=$data/recompiled/Smoke
run "${launch[@]}" "$runtime" -r smoke.nro Smoke --jobs 2
if [ "$status" -eq 0 ] && [ -f "$game/Smoke$exe" ] && [ -f "$game/Smoke.abi" ]; then
    pass "scarletnx -r smoke.nro made $game/Smoke$exe"
else
    fail "scarletnx -r smoke.nro exited $status, without $game/Smoke$exe and its .abi" "$out"
fi
if [ -f "$config/Smoke/Smoke.ini" ] && [ ! -e "$game/Smoke.ini" ]; then
    pass "its settings are $config/Smoke/Smoke.ini"
else
    fail "scarletnx -r smoke.nro did not write its settings in $config/Smoke only" "$out"
fi

# The runtime left in the game's folder must start on its own (on Windows, with the DLLs it needs), find the game's
# code by its name and take its ABI. The banner comes after that and before the GPU, which CI runners lack
if [ -f "$game/Smoke$exe" ]; then
    run env -C "$game" "./Smoke$exe" -H
    if grep -q '^ScarletNX - Smoke' <<<"$out"; then
        pass "$game/Smoke$exe started the game (exit $status: $(tail -n 1 <<<"$out"))"
    else
        fail "$game/Smoke$exe exited $status before the game's banner" "$out"
    fi
    if grep -q 'ScarletNX - Smoke' "$state/Smoke/Smoke.log" 2>/dev/null && [ ! -e "$game/Smoke.log" ]; then
        pass "its log is $state/Smoke/Smoke.log"
    else
        fail "the game did not log in $state/Smoke only" "$out"
    fi
fi

# A dump on its own: recompile, then play, each a child of the first. Another name, which Windows would not tell
# from Smoke by case
cp smoke.nro play.nro
run "${launch[@]}" "$runtime" play.nro -H
if grep -q '^ScarletNX - play' <<<"$out"; then
    pass "scarletnx play.nro recompiled and started the game (exit $status)"
else
    fail "scarletnx play.nro exited $status before the game's banner" "$out"
fi

# A PFS0 without files: only hactool, reading it in the driver, can tell there is no program in it
printf 'PFS0\0\0\0\0\0\0\0\0\0\0\0\0' >empty.nsp
run "${launch[@]}" "$runtime" -x empty.nsp
if grep -q 'no base Program NCA' <<<"$out"; then
    pass "scarletnx -x empty.nsp read it with hactool"
else
    fail "scarletnx -x empty.nsp exited $status before hactool read it" "$out"
fi

if [ "$failed" -ne 0 ]; then
    echo "$runtime does not work as installed" >&2
    exit 1
fi
echo "$runtime works as installed"
