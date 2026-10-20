#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Checks the AppImage scripts/make-appimage.sh made: a copy named after a game looks for its module next to itself and
# where --game says, and the launcher's programs are in it. An install of the Linux runtime alone runs an AArch64
# program instead. qemu-$ARCH runs the AppImage where the machine cannot: the kernel's binfmt handler never takes one
# (its runtime carries the AppImage magic where qemu's mask wants zeros).
set -euo pipefail

ARCH=${ARCH:-$(uname -m)}
OUTPATH=${OUTPATH:-./dist}
OUTNAME=${OUTNAME:-ScarletNX-linux-$ARCH.AppImage}
APPDIR=${APPDIR:-$PWD/AppDir}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

launch=()
if command -v "qemu-$ARCH" >/dev/null; then
    launch=("$(command -v "qemu-$ARCH")")
fi

probe=$tmp/probe
mkdir -p "$probe"
cp "$OUTPATH/$OUTNAME" "$probe/Probe.AppImage"

# The Linux runtime alone: it runs an AArch64 program, which prints its line and exits with 42
if [ ! -e "$APPDIR/bin/scarletnx" ]; then
    "$(dirname "$0")/aarch64-smoke.sh" "$probe/smoke"
    status=0
    out=$(cd "$probe" && APPIMAGE_EXTRACT_AND_RUN=1 "${launch[@]}" ./Probe.AppImage ./smoke 2>&1) || status=$?
    if [ "$status" != 42 ] || ! grep -qx 'gnuabi smoke' <<<"$out"; then
        echo "The AppImage ran an AArch64 program to exit status $status, not 42 with its line. It printed:" >&2
        echo "$out" >&2
        exit 1
    fi
    echo "AppImage checked: it runs Linux AArch64 programs"
    exit 0
fi

# Without a game the runtime says which module it looked for. Under another name, it must look next to the AppImage
# for that name's module, which shows the libraries load and the name and folder come from $APPIMAGE. With --game it
# must look for the module that names
check() { # expected module, then the AppImage's arguments
    local want=$1 out
    shift
    out=$(cd "$probe" && APPIMAGE_EXTRACT_AND_RUN=1 "${launch[@]}" ./Probe.AppImage "$@" 2>&1 || true)
    if ! grep -qF "$want" <<<"$out"; then
        echo "The AppImage did not look for $want. It printed:" >&2
        echo "$out" >&2
        exit 1
    fi
}
# -H: started bare, it would open the launcher instead
check "$probe/Probe_code.so" -H
check "$probe/games/Other_code.so" --game="$probe/games/Other"
# Started bare it opens the launcher, which needs everything it runs beside it (the launcher itself can't be run
# here: without a display SDL may fall back to an offscreen one and keep it open)
for program in scarletnx-3ds scarletnx-gnuabi; do
    if [ ! -e "$APPDIR/bin/$program" ]; then
        echo "The AppImage has no bin/$program, which the launcher runs" >&2
        exit 1
    fi
done
echo "AppImage checked: it looks for its game module next to itself or where --game says, and has the launcher"
