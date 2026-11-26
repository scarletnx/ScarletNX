#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Makes dist/ScarletNX-linux-<arch>.AppImage from the runtime installed in /usr, with pkgforge's quick-sharun
# (https://github.com/pkgforge-dev/Anylinux-AppImages): it bundles the runtime with every library it loads, SDL3, the
# Vulkan loader and Mesa's Vulkan drivers included. CI runs it in pkgforge's Arch container of the architecture, after
# `cmake --install` into /usr (.github/workflows/build.yml), and quick-sharun has to be on PATH.
#
# Anylinux-AppImages is this packaging's reference: its defaults (quick-sharun's, its FAQ.md) come before a tool, a
# download or a step of our own.
#
# A copy named after a game works like the bare runtime: games/MyGame.AppImage (or games/MyGame) loads
# games/MyGame_code.so, since the runtime takes its name and folder from $APPIMAGE.
#
# An install of the Linux runtime alone (scarletnx without the Switch driver, scarletnx-switch, and scarletnx-gnuabi a
# link to it) makes an AppImage of it: it runs Linux
# AArch64 programs (`ScarletNX-linux-ppc64.AppImage ./program`), and its checks run one.
#
# The cross builds bundle in the target's container (SKIP_APPIMAGE=1) and make the AppImage on the build machine
# (MAKE_APPIMAGE_ONLY=1), whose appimagetool compresses natively and takes the target's runtime from APPIMAGE_ARCH.
set -euo pipefail

ARCH=${ARCH:-$(uname -m)}
VERSION=${VERSION:-$(git -C "$(dirname "$0")/.." describe --always --dirty 2>/dev/null || echo unknown)}
export ARCH VERSION
# The architecture inside the AppImage: appimagetool picks its runtime by it, and quick-sharun its downloads
export APPIMAGE_ARCH=$ARCH
export OUTPATH=./dist
export OUTNAME=ScarletNX-linux-$ARCH.AppImage
# The AppImage's program: the driver, else the Linux runtime when it is all the install has
bin=/usr/bin
main=scarletnx
if [ ! -e "$bin/scarletnx-switch" ] && [ -e "$bin/scarletnx-gnuabi" ]; then
    main=scarletnx-gnuabi
fi
# quick-sharun copies ICON to .DirIcon as well
export ICON=/usr/share/icons/hicolor/256x256/apps/wimsy.png
export DESKTOP=/usr/share/applications/$main.desktop
# AppImageUpdate finds new builds through the .zsync file next to the AppImage in the release this names
UPINFO=$("$(dirname "$0")/appimage-update-info.sh" "$ARCH")
export UPINFO
# The runtime exits at once without a game module, so tracing it for dlopened libraries finds nothing: name them
export STRACE_MODE=0 DEPLOY_SDL=1 DEPLOY_OPENGL=1
# mkdwarfs's default preset (zstd:level=22) is much slower; this keeps its
# block size and is about 15% larger
export DWARFS_COMP="zstd:level=7 -S26 -B2"
# appimagetool adds a small SquashFS image with the icon and desktop entry, so thumbnailers that only read
# SquashFS show the DwarFS AppImage (appimagetool 0.7.0 or newer; older ones ignore it)
export SQUASHFS_METADATA_SIDECAR=1

# The AppDir is bundled: appimagetool makes it, with this machine's compressor and the target's runtime
if [ "${MAKE_APPIMAGE_ONLY:-0}" = 1 ]; then
    quick-sharun --make-appimage
    exit 0
fi

# The runtime stays the AppImage's program (the desktop entry's Exec), so a copy named after a game runs that game.
# Started bare it opens its launcher, which runs Switch games with it and the other drivers from beside it
# (scarletnx-3ds, scarletnx-vita, scarletnx-agb, scarletnx-nds, scarletnx-gnuabi). The Linux runtime alone has no launcher.
if [ "$main" = scarletnx ]; then
    quick-sharun "$bin/scarletnx" "$bin/scarletnx-switch" "$bin/scarletnx-3ds" "$bin/scarletnx-vita" \
        "$bin/scarletnx-agb" "$bin/scarletnx-nds" "$bin/scarletnx-gnuabi"
else
    quick-sharun "$bin/scarletnx" "$bin/scarletnx-gnuabi"
fi

# The licenses travel with the AppImage: the runtime's and those of the libraries built into it (`cmake --install` put
# them in /usr/share/licenses/scarletnx), and those of the Arch packages quick-sharun took the bundled libraries from,
# with a list of those packages, their versions and licenses
appdir=${APPDIR:-$PWD/AppDir}
if [ ! -d "$appdir/lib" ]; then
    echo "quick-sharun left no $appdir/lib: where it puts the bundled libraries has changed" >&2
    exit 1
fi
# each driver the launcher runs, a name of scarletnx's (SNX_MULTICALL): sharun has to start it under its own name
if [ "$main" = scarletnx ]; then
    for program in scarletnx-3ds scarletnx-vita scarletnx-agb scarletnx-nds scarletnx-gnuabi; do
        out=$("$appdir/bin/$program" --help 2>&1 || true)
        if ! grep -q "^Usage: $program" <<<"$out"; then
            echo "The AppDir's bin/$program --help is not $program's. It printed:" >&2
            echo "$out" >&2
            exit 1
        fi
    done
fi
if [ "$(head -c 8 "$appdir/.DirIcon" | od -An -tx1 | tr -d ' \n')" != 89504e470d0a1a0a ]; then
    echo "$appdir/.DirIcon is not a PNG: quick-sharun no longer takes ICON for it" >&2
    exit 1
fi

# and the vector next to it, the best the desktop entry's Icon= can name
cp /usr/share/icons/hicolor/scalable/apps/wimsy.svg "$appdir/"
mkdir -p "$appdir/share/licenses"
cp -r /usr/share/licenses/scarletnx "$appdir/share/licenses/"
mapfile -t packages < <(cd "$appdir/lib" && find . -name '*.so*' -type f | while read -r lib; do
    pacman -Qqo "/usr/lib/${lib#./}" 2>/dev/null || true
done | sort -u)
for pkg in "${packages[@]}"; do
    pacman -Qi "$pkg" | grep -E '^(Name|Version|Licenses) '
    echo
    if [ -d "/usr/share/licenses/$pkg" ]; then
        cp -r "/usr/share/licenses/$pkg" "$appdir/share/licenses/"
    fi
done >"$appdir/share/licenses/bundled-packages.txt"

# The build machine makes the AppImage of the AppDir this left
if [ "${SKIP_APPIMAGE:-0}" != 1 ]; then
    quick-sharun --make-appimage
    if [ "${SKIP_CHECKS:-0}" != 1 ]; then
        "$(dirname "$0")/check-appimage.sh"
    fi
fi
