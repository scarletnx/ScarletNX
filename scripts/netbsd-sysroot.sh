#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# A NetBSD sysroot for the netbsd-x86_64-cross preset (docs/hosts/netbsd.md), made without root from the release's sets and
# pkgsrc's binary packages, each checked against the SHA512 list it is published with.
#
#   scripts/netbsd-sysroot.sh DIR
#
# The C library and the rest of the system from base, its headers, crt files and libgcc from comp (base has none),
# and from pkgsrc LLVM (the LLVM backend, with zstd and libxml2, which LLVM's CMake package asks for) and SDL3.
# NETBSD_RELEASE (11.1), NETBSD_PKGSRC (11.0_2026Q3) and NETBSD_MIRROR (https://cdn.netbsd.org/pub) pick others.
# Then `export SNX_SYSROOT=DIR` and configure with the preset.
set -eu
[ $# -eq 1 ] || { sed -n '4,12p' "$0"; exit 2; }
dir=$1
release=${NETBSD_RELEASE:-11.1}
pkgsrc=${NETBSD_PKGSRC:-11.0_2026Q3}
mirror=${NETBSD_MIRROR:-https://cdn.netbsd.org/pub}
sets=$mirror/NetBSD/NetBSD-$release/amd64/binary/sets
packages=$mirror/pkgsrc/packages/NetBSD/x86_64/$pkgsrc/All
# the packages, by name: the newest version of each is taken from the SHA512 list
names="llvm zstd libxml2 SDL3"

mkdir -p "$dir/.download" "$dir/usr/pkg"
dl=$dir/.download
fetch() {
    [ -s "$dl/$2" ] || curl -fsSL --retry 3 -o "$dl/$2" "$1/$2"
}
# check FILE against LIST, whose lines are `SHA512 (PATH) = HASH`, PATH being FILE or All/FILE
check() {
    want=$(grep -F "($3$2) = " "$1" | sed 's/.* = //')
    [ -n "$want" ] || { echo "$2 is not in $1" >&2; exit 1; }
    [ "$(sha512sum "$dl/$2" | cut -d' ' -f1)" = "$want" ] || { echo "$2 does not match $1" >&2; exit 1; }
}

fetch "$sets" SHA512
for set in base comp; do
    fetch "$sets" $set.tar.xz
    check "$dl/SHA512" $set.tar.xz ""
done
# base whole but its device nodes; from comp, what compiling and linking against it takes
tar -xJf "$dl/base.tar.xz" -C "$dir" --no-same-owner --exclude='./dev/*' --exclude='./var/*'
tar -xJf "$dl/comp.tar.xz" -C "$dir" --no-same-owner --wildcards './usr/include/*' './usr/lib/*.o' \
    './usr/lib/libgcc*.a' './usr/lib/pkgconfig/*'

fetch "$packages/.." SHA512.bz2
bzip2 -dc "$dl/SHA512.bz2" > "$dl/SHA512.pkgsrc"
for name in $names; do
    # names are followed by a version that starts with a digit: llvm-21.1.8nb1, not llvm-libs-...
    file=$(sed -n "s|^SHA512 (All/\\($name-[0-9][^)]*\\.tgz\\)) = .*|\\1|p" "$dl/SHA512.pkgsrc" | sort -V | tail -1)
    [ -n "$file" ] || { echo "no $name package in pkgsrc $pkgsrc" >&2; exit 1; }
    fetch "$packages" "$file"
    check "$dl/SHA512.pkgsrc" "$file" All/
    # a package's files are relative to its prefix, /usr/pkg; the +FILES are its metadata
    tar -xJf "$dl/$file" -C "$dir/usr/pkg" --no-same-owner --warning=no-unknown-keyword --exclude='+*'
    echo "$file"
done
echo "sysroot: $dir (NetBSD $release, pkgsrc $pkgsrc)"
