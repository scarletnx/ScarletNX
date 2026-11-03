#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The target LLVM's llvm-config for a cross build (SNX_LLVM_CONFIG of the linux-*-cross presets, docs/PPC64.md). The
# sysroot's llvm-config is a program of the target: it runs under the qemu-user of its machine, and the paths it prints
# that are the target's own (/usr/...) get the sysroot in front, as the build machine reads them there.
set -eu
sysroot=${SNX_SYSROOT:?SNX_SYSROOT names the system root of the target (scripts/ppc64-sysroot.sh, or an Arch root)}
config=
for c in "$sysroot"/usr/lib/llvm-*/bin/llvm-config "$sysroot"/usr/bin/llvm-config; do
    [ -x "$c" ] && config=$c
done
[ -n "$config" ] || { echo "no llvm-config in $sysroot: install LLVM's development files there" >&2; exit 1; }
# the program's machine, from its ELF header: EI_CLASS (2 for 64-bit), EI_DATA (1 little-endian, 2 big-endian) and
# e_machine in that order
class=$(od -An -tu1 -j4 -N1 "$config" | tr -d ' ')
data=$(od -An -tu1 -j5 -N1 "$config" | tr -d ' ')
lo=$(od -An -tu1 -j18 -N1 "$config" | tr -d ' ')
hi=$(od -An -tu1 -j19 -N1 "$config" | tr -d ' ')
[ "$data" = 1 ] && machine=$((lo + 256 * hi)) || machine=$((256 * lo + hi))
case $machine:$data in
    21:1) qemu=qemu-ppc64le ;;
    21:2) qemu=qemu-ppc64 ;;
    243:1) qemu=qemu-riscv64 ;;
    258:1) qemu=qemu-loongarch64 ;;
    8:1) qemu=qemu-mips64el ;;
    8:2) qemu=qemu-mips64 ;;
    22:2) qemu=qemu-s390x ;;
    *) echo "$config is a program of machine $machine, which this script runs no qemu for" >&2; exit 1 ;;
esac
# MIPS's e_machine is the 32-bit machines' too, which the runtime doesn't build for
if [ "$machine" = 8 ] && [ "$class" != 2 ]; then
    echo "$config is a program of machine $machine, EI_CLASS $class (32-bit MIPS): the runtime builds for n64" >&2
    exit 1
fi
"$qemu" -L "$sysroot" "$config" "$@" | sed -E "s#(^|[ =]|-I|-L)/(usr|lib)/#\\1$sysroot/\\2/#g"
