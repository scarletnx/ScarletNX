#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# A PowerPC64 sysroot for the linux-ppc64* presets (docs/hosts/ppc64.md), made without root and without running the target's
# programs: mmdebstrap downloads the packages below with their dependencies and only unpacks them.
#
#   scripts/ppc64-sysroot.sh ARCH DIR [--desktop]
#   scripts/ppc64-sysroot.sh musl DIR ROOTFS.tar.*
#
# ARCH is ppc64el (Debian trixie, linux-ppc64le-cross) or ppc64 (big-endian ELFv1, Debian ports' unstable,
# linux-ppc64-cross; needs debian-ports-archive-keyring). musl unpacks a big-endian ELFv2 root file system with LLVM
# installed (Chimera's or Adélie's, linux-ppc64-musl-cross). --desktop adds the X11, Wayland and audio headers SDL3's
# source build needs for a window and sound; without them (CI) it builds the headless backends only. Then
# `export SNX_SYSROOT=DIR` and configure with the preset.
set -eu
[ $# -ge 2 ] || { sed -n '4,13p' "$0"; exit 2; }
arch=$1 dir=$2
shift 2
desktop=
[ "${1:-}" = --desktop ] && desktop=1

# the libraries the runtime links, and LLVM's (llvm-config names its system libraries: zstd, zlib, ffi, xml2);
# unicorn for the A64 differential tests; Vulkan's loader and lavapipe, Mesa's CPU driver, to run a game under qemu
packages="libc6-dev libstdc++-14-dev libgcc-14-dev llvm-dev libzstd-dev zlib1g-dev libffi-dev libxml2-dev
libunicorn-dev libvulkan1 mesa-vulkan-drivers"
if [ -n "$desktop" ]; then
    packages="$packages libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev
libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libpulse-dev libasound2-dev libpipewire-0.3-dev"
fi
include=$(echo $packages | tr ' ' ,)

# mmdebstrap writes a tarball: in unshare mode it works as a mapped root, which can't create folders in the user's
# own home; the tarball is written as the user, and unpacked below as the user too
mkdir -p "$dir"
tarball=$(realpath "$dir").tar
# Its staging folder: /var/tmp, on disk, not /tmp, often a small tmpfs (a sysroot unpacks to about 2 GB). It can't be
# in the home folder, which the mapped root may not enter
TMPDIR=${SNX_SYSROOT_TMP:-/var/tmp}
export TMPDIR
case $arch in
    ppc64el)
        mmdebstrap --mode=auto --variant=extract --architectures=ppc64el --include="$include" \
            trixie "$tarball" http://deb.debian.org/debian
        ;;
    ppc64)
        mmdebstrap --mode=auto --variant=extract --architectures=ppc64 --include="$include" \
            --keyring=/usr/share/keyrings/debian-ports-archive-keyring.gpg \
            unstable "$tarball" http://deb.debian.org/debian-ports
        ;;
    musl)
        tarball=${1:?musl needs the root file system tarball}
        ;;
    *)
        echo "unknown ARCH $arch: ppc64el, ppc64 or musl" >&2
        exit 2
        ;;
esac

# no device nodes or owners: a sysroot is only read
tar -xf "$tarball" -C "$dir" --no-same-owner --no-same-permissions --exclude='./dev/*'
[ "$arch" = musl ] || rm -f "$tarball"
# Debian's merged /usr: its packages ship /usr/lib only, and the links at the root (/lib64/ld64.so.2, the loader) come
# from setup scripts that an extract never runs
for d in bin sbin lib lib64; do
    [ -d "$dir/usr/$d" ] && [ ! -e "$dir/$d" ] && ln -s "usr/$d" "$dir/$d"
done

# Absolute links inside the sysroot (libm.so -> /lib/...) would point into the build machine's own files: make each
# relative to the link's folder, so the sysroot works wherever it is
find "$dir" -type l | while read -r link; do
    target=$(readlink "$link")
    case $target in
        /*)
            from=$(dirname "${link#"$dir"}")
            rel=$(realpath -m --relative-to="$from" "$target")
            ln -sfn "$rel" "$link"
            ;;
    esac
done
echo "sysroot ready: export SNX_SYSROOT=$(realpath "$dir")"
