#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# LLVM's libraries for a Linux build that links them statically, as the AppImages do (scripts/make-appimage.sh). Only
# the libraries the recompiler links (scripts/llvm-libraries), their headers and CMake package, and one code generator:
# the host's. A distribution's LLVM is one shared library with every target, most of what an AppImage would carry.
#
#   scripts/linux-llvm.sh ARCH PREFIX [SOURCE] [-- cmake args]
#
# ARCH is the machine the runtime runs on: x86_64, aarch64, ppc64le, riscv64, loongarch64, ppc64 (big-endian ELFv2,
# Arch POWER's), mips64 or mips64el (n64). Another than this machine's is cross-built with Clang against the system root
# SNX_SYSROOT, for the same CPU baseline and ABI as the linux-*-cross presets build the runtime (cmake/cross.cmake),
# and LLVM builds the tablegens it runs here in a tree of their own. SOURCE is an llvm-project folder or .tar.xz
# (default: the release named by LLVM_VERSION, downloaded). Configure ScarletNX with -DLLVM_DIR=PREFIX/lib/cmake/llvm
# afterwards, and -DSNX_LLVM_CONFIG= with a cross preset, which names the system root's llvm-config.
set -eu
[ $# -ge 2 ] || { sed -n '4,15p' "$0"; exit 2; }
arch=$1 prefix=$2
shift 2
source=
if [ $# -gt 0 ] && [ "$1" != -- ]; then source=$1; shift; fi
[ "${1:-}" = -- ] && shift
flags=
case $arch in
    x86_64) target=X86 triple=x86_64-linux-gnu ;;
    aarch64) target=AArch64 triple=aarch64-linux-gnu ;;
    ppc64le) target=PowerPC triple=powerpc64le-linux-gnu flags=-mcpu=power8 ;;
    ppc64) target=PowerPC triple=powerpc64-unknown-linux-gnu flags="-mcpu=power8 -mabi=elfv2" ;;
    riscv64) target=RISCV triple=riscv64-linux-gnu flags=-march=rv64gc ;;
    loongarch64) target=LoongArch triple=loongarch64-linux-gnu flags=-march=la64v1.0 ;;
    mips64) target=Mips triple=mips64-unknown-linux-gnu flags=-march=mips64r2 ;;
    mips64el) target=Mips triple=mips64el-unknown-linux-gnu flags=-march=mips64r2 ;;
    *) echo "ARCH must be x86_64, aarch64, ppc64le, ppc64, riscv64, loongarch64, mips64 or mips64el, not $arch" >&2
       exit 2 ;;
esac
machine=$(uname -m)
# uname -m names both MIPS64 byte orders mips64: the bytes 1, 0 read as the 16-bit 1 on a little-endian machine
if [ "$machine" = mips64 ] && [ "$(printf '\001\000' | od -An -tu2 | tr -d ' ')" = 1 ]; then machine=mips64el; fi
if [ "$arch" != "$machine" ]; then
    sysroot=${SNX_SYSROOT:?SNX_SYSROOT names the system root of $arch to cross-build against}
    # the tablegens run here: this machine's Clang builds them, without the target's flags
    set -- -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR="$arch" -DCMAKE_SYSROOT="$sysroot" \
        -DCMAKE_C_COMPILER_TARGET="$triple" -DCMAKE_CXX_COMPILER_TARGET="$triple" \
        -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld \
        -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_AR="$(command -v llvm-ar)" \
        -DCMAKE_RANLIB="$(command -v llvm-ranlib)" -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
        -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
        -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY -DLLVM_HOST_TRIPLE="$triple" \
        -DCROSS_TOOLCHAIN_FLAGS_NATIVE="-DCMAKE_C_COMPILER=clang;-DCMAKE_CXX_COMPILER=clang++" "$@"
fi
version=${LLVM_VERSION:-23.1.3}
work=$prefix.build/$version
mkdir -p "$work"
if [ -z "$source" ]; then
    source=$work/llvm-project-$version.src.tar.xz
    [ -f "$source" ] || curl -fL -o "$source" \
        "https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/llvm-project-$version.src.tar.xz"
fi
if [ -f "$source" ]; then
    [ -d "$work/src/llvm" ] || { mkdir -p "$work/src"; tar -xJf "$source" -C "$work/src" --strip-components=1; }
    source=$work/src
fi
# LLVM_INCLUDE_TOOLS=OFF leaves out tools/, whose shared libraries (libRemarks, libLTO) the install targets would
# still build
cmake -S "$source/llvm" -B "$work/$arch" -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DLLVM_TARGETS_TO_BUILD="$target" \
    -DLLVM_ENABLE_PIC=ON -DLLVM_INCLUDE_TOOLS=OFF -DLLVM_BUILD_TOOLS=OFF -DLLVM_BUILD_UTILS=OFF \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_DOCS=OFF \
    -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF \
    -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF "$@"
# the libraries the recompiler links and those they link (scripts/llvm-libraries), their headers and the CMake
# package ScarletNX finds them with: LLVM's others are neither built nor in the package
cmake -S "$(dirname "$0")/llvm-libraries" -B "$work/$arch-libraries" -DLLVM_BUILD="$work/$arch" \
    -DOUTPUT="$work/$arch-libraries.txt"
cmake "$work/$arch" -DLLVM_DISTRIBUTION_COMPONENTS="$(cat "$work/$arch-libraries.txt");llvm-headers;cmake-exports"
cmake --build "$work/$arch" --parallel "$(getconf _NPROCESSORS_ONLN)" --target install-distribution
echo "LLVM for $arch in $prefix: configure ScarletNX with -DLLVM_DIR=$prefix/lib/cmake/llvm"
