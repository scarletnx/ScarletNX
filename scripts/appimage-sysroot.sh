#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The libraries a cross-built AppImage links and bundles, installed into ROOT, an Arch root of the target's
# architecture (pkgforge's image of it, unpacked: .github/workflows/build.yml, appimage-cross). Run as root on an Arch
# machine. CI keeps the root this makes a week at a time, keyed by this file, so a change here makes it again.
#
#   scripts/appimage-sysroot.sh ROOT [--linux-only]
#
# The target's: gcc for the crt files, libgcc and libstdc++ Clang links the target's programs with, and with the Switch
# and 3DS runtimes the libraries they link and bundle and what the pinned SDL3's build needs, as Arch POWER's SDL3 is
# older than the pin (its X11, Wayland and audio backends). --linux-only, for the Linux runtime alone, is the GCC
# runtime only. They go in by the root's own pacman.conf (its architecture, repositories and mirrors, under the root)
# and its own keys. Install scriptlets are the target's programs, left out. Each pacman is tried 3 times, as mirrors
# drop a connection now and then.
set -eu
[ $# -ge 1 ] || { sed -n '4,15p' "$0"; exit 2; }
root=$1
libraries=gcc
[ "${2:-}" = --linux-only ] || libraries="$libraries libx11 mesa vulkan-icd-loader vulkan-radeon vulkan-swrast
    libxext libxcursor libxfixes libxi libxinerama libxrandr libxrender libxss libxtst libxkbcommon wayland
    wayland-protocols libdecor alsa-lib libpulse libpipewire hidapi libusb"
retry() { for try in 1 2 3; do "$@" && return; [ "$try" = 3 ] && return 1; sleep 5; done; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/pkg" # pacman's downloads, which it doesn't make a folder for
sed "s#/etc/pacman.d/#$root/etc/pacman.d/#g" "$root/etc/pacman.conf" >"$work/pacman.conf"
pacman-key --gpgdir "$root/etc/pacman.d/gnupg" --init
pacman-key --gpgdir "$root/etc/pacman.d/gnupg" --populate-from "$root/usr/share/pacman/keyrings" --populate
pm() {
    pacman --root "$root" --dbpath "$root/var/lib/pacman" --config "$work/pacman.conf" \
        --gpgdir "$root/etc/pacman.d/gnupg" --cachedir "$work/pkg" --noscriptlet "$@"
}
retry pm -Sy --noconfirm
# A port whose openssl is already 4 while some of its packages still link OpenSSL 3 refuses the upgrade unless its
# openssl-3 comes along; a port still on OpenSSL 3 has no such package
if pm -Si openssl-3 >/dev/null 2>&1; then libraries="$libraries openssl-3"; fi
retry pm -Su --noconfirm --needed $libraries
# without them CMake's first test link fails, its message deep in the configure's output
ls "$root"/usr/lib/gcc/*/*/crtbeginS.o >/dev/null 2>&1 ||
    { echo "the system root has no GCC runtime (crtbeginS.o, libgcc) for Clang to link with" >&2; exit 1; }
# what neither the build nor the packaging reads (it reads the package database and the licenses), which a cache of the
# root would carry
rm -rf "$root/usr/share/doc" "$root/usr/share/man" "$root/usr/share/info" "$root/usr/share/locale" \
    "$root/usr/share/i18n" "$root/usr/share/gtk-doc" "$root/var/cache/pacman/pkg"
