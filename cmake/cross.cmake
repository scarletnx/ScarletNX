# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
# The target's flags for what is compiled outside CMake's own compiles (FFmpeg's and OpenSSL's builds and the
# recompiler's runtime library), as cmake/apple.cmake gives them for Apple's: SNX_TARGET_FLAGS. It holds:
#   - SNX_ARCH_FLAGS, the CPU baseline the whole build compiles for, which CMake's compiles get too (added here).
#     PowerPC64 builds for SNX_PPC64_CPU, POWER8 unless the build names an older big-endian machine (970 for the
#     PowerPC G5, power4 to power7). ppc64le starts at POWER8. Before it there is no lqarx/stqcx., and Clang would call
#     libatomic's __atomic_compare_exchange_16 for snx_cas128 (helpers/integer.c), which a module links no library to
#     have: such a build's modules call RtAPI.cas128 (common/runtime/cas128.c), which takes POWER8's instructions where
#     the machine has them and a lock elsewhere. The recompiler builds modules for the same CPU when the host's name
#     isn't known (SNX_PPC64_LLVM_CPU, LLVM's name for it). SNX_PPC64_ELFV2 builds big-endian glibc for ELFv2 (Arch
#     POWER's), as Clang builds it for ELFv1 (Debian ports'). The flags go into CMAKE_C_FLAGS and CMAKE_CXX_FLAGS,
#     so the configure checks (the libraries' among them) compile for the same CPU and ABI. RISC-V builds rv64gc and
#     LoongArch LA64 v1.0, as the recompiler's modules are (ir.c), and both without linker relaxation: the runtime
#     library is merged into each module with its functions' own target features, and the module's linker (link_elf.c)
#     moves no code.
#   - for a Linux cross build with Clang (the linux-*-cross presets): the target triple and the sysroot, which CMake
#     passes to its own compiles from CMAKE_C_COMPILER_TARGET and CMAKE_SYSROOT and the others don't get from the
#     compiler's path. SNX_TARGET_LDFLAGS adds what their links need: LLD, as the build machine's ld only knows its
#     own architecture.
# All are empty for the other hosts and builds.
include_guard(GLOBAL)
option(SNX_PPC64_ELFV2 "Build big-endian PowerPC64 glibc for ELFv2 (Arch POWER), not ELFv1 (Debian ports)" ON)
set(SNX_ARCH_FLAGS "")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ppc64|ppc64le|powerpc64|powerpc64le)$")
    if(SNX_PPC64_ELFV2)
        list(APPEND SNX_ARCH_FLAGS -mabi=elfv2)
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "riscv64")
    set(SNX_ARCH_FLAGS -march=rv64gc -mno-relax)
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "loongarch64")
    set(SNX_ARCH_FLAGS -march=la64v1.0 -mno-relax)
endif()
list(JOIN SNX_ARCH_FLAGS " " snx_arch_flags)
string(APPEND CMAKE_C_FLAGS " ${snx_arch_flags}")
string(APPEND CMAKE_CXX_FLAGS " ${snx_arch_flags}")
# the links' too: Clang names the program's loader by it (ELFv2's ld64.so.2, ELFv1's ld64.so.1)
if(SNX_PPC64_ELFV2)
    foreach(kind EXE SHARED MODULE)
        string(APPEND CMAKE_${kind}_LINKER_FLAGS " -mabi=elfv2")
    endforeach()
endif()

set(SNX_TARGET_FLAGS "")
set(SNX_TARGET_LDFLAGS "")
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
    if(NOT CMAKE_C_COMPILER_TARGET)
        message(FATAL_ERROR "A Linux cross build names its target in CMAKE_C_COMPILER_TARGET (e.g. "
                            "powerpc64le-linux-gnu), which FFmpeg, OpenSSL and the runtime library are built for.")
    endif()
    list(APPEND SNX_TARGET_FLAGS --target=${CMAKE_C_COMPILER_TARGET})
    if(CMAKE_SYSROOT)
        list(APPEND SNX_TARGET_FLAGS --sysroot=${CMAKE_SYSROOT})
    endif()
    # LLD links no ELFv1 object, the ABI of big-endian PowerPC64 glibc on Debian ports: GNU ld does
    # (binutils-powerpc64-linux-gnu). Big-endian musl and Arch POWER's glibc are ELFv2, as LLD takes it
    if(CMAKE_C_COMPILER_TARGET MATCHES "^(powerpc64|ppc64)-([a-z]+-)?linux-gnu$" AND NOT SNX_PPC64_ELFV2)
        set(SNX_TARGET_LDFLAGS -fuse-ld=bfd)
    else()
        set(SNX_TARGET_LDFLAGS -fuse-ld=lld)
    endif()
endif()
list(APPEND SNX_TARGET_FLAGS ${SNX_ARCH_FLAGS})

# SNX_TARGET_C_COMPILER: a C compiler for the target, for the tests that give one to the program they run
# (native_recompiler), which builds a library with it and loads it. In a cross
# build that program runs under the emulator, so the compiler is this one with the target's flags, in a wrapper
set(SNX_TARGET_C_COMPILER "${CMAKE_C_COMPILER}")
if(SNX_TARGET_LDFLAGS)
    string(REPLACE ";" " " snx_cross_flags "${SNX_TARGET_FLAGS};${SNX_TARGET_LDFLAGS}")
    file(WRITE "${PROJECT_BINARY_DIR}/target-cc" "#!/bin/sh\nexec '${CMAKE_C_COMPILER}' ${snx_cross_flags} \"$@\"\n")
    file(CHMOD "${PROJECT_BINARY_DIR}/target-cc" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ
         GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    set(SNX_TARGET_C_COMPILER "${PROJECT_BINARY_DIR}/target-cc")
endif()
