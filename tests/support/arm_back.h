/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native back end whose AArch32 blocks a test makes: Back, its functions back_(name) and BACK_NAME. Built with
   SNX_TEST_ARM64, SNX_TEST_RV64 and their like it is that machine's, and SNX_TEST_NATIVE is defined: its code runs
   under Unicorn (BACK_UC, host_unicorn.h), arm64's as Windows' with SNX_TEST_ARM64WIN too, and x86-64's as Windows'
   with SNX_TEST_WIN64, by an x86-64 host itself (host_win64.c). Built with none of them it is the host's own */
#ifndef SNX_TEST_ARM_BACK_H
#define SNX_TEST_ARM_BACK_H
#if defined(SNX_TEST_ARM64) || defined(SNX_TEST_RV64) || defined(SNX_TEST_PPC64) || defined(SNX_TEST_PPC64BE) ||       \
    defined(SNX_TEST_MIPS64) || defined(SNX_TEST_MIPS64BE) || defined(SNX_TEST_LA64) || defined(SNX_TEST_S390X) ||     \
    defined(SNX_TEST_WIN64)
#define SNX_TEST_NATIVE 1
#include "support/host_unicorn.h"
#elif defined(__x86_64__) || defined(_M_X64)
#define BACK_X64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#define BACK_ARM64 1
#elif defined(__riscv)
#define BACK_RV64 1
#elif defined(__loongarch64)
#define BACK_LA64 1
#elif defined(__s390x__)
#define BACK_S390X 1
#elif defined(__powerpc64__) && defined(__LITTLE_ENDIAN__)
#define BACK_PPC64 1
#elif defined(__powerpc64__)
#define BACK_PPC64BE 1
#elif defined(__mips64) && defined(__MIPSEL__)
#define BACK_MIPS64 1
#elif defined(__mips64)
#define BACK_MIPS64BE 1
#endif

#if defined(SNX_TEST_ARM64) || defined(BACK_ARM64)
#include "common/cpu/recompiler/arm64/arm64_arm.h"
typedef SnxArm64Arm Back;
#define back_(name) snx_arm64_arm_##name
#if defined(SNX_TEST_ARM64WIN)
#include "common/cpu/recompiler/arm64/arm64_codegen.h"
#define BACK_NAME "arm64 (Windows)"
#define BACK_UC   HOSTUC_ARM64_WIN
#else
#define BACK_NAME "arm64"
#define BACK_UC   HOSTUC_ARM64
#endif
#elif defined(SNX_TEST_LA64) || defined(BACK_LA64)
#include "common/cpu/recompiler/la64/la64_arm.h"
typedef SnxLa64Arm Back;
#define back_(name) snx_la64_arm_##name
#define BACK_NAME   "loongarch64"
#define BACK_UC     HOSTUC_LA64
#elif defined(SNX_TEST_MIPS64) || defined(BACK_MIPS64)
#include "common/cpu/recompiler/mips64/mips64_arm.h"
typedef SnxMips64Arm Back;
#define back_(name) snx_mips64_arm_##name
#define BACK_NAME   "mips64el"
#define BACK_UC     HOSTUC_MIPS64EL
#elif defined(SNX_TEST_PPC64BE) || defined(BACK_PPC64BE)
#include "common/cpu/recompiler/ppc64/ppc64be_arm.h"
typedef SnxPpc64beArm Back;
#define back_(name) snx_ppc64be_arm_##name
#define BACK_NAME   "ppc64"
#define BACK_UC     HOSTUC_PPC64
#elif defined(SNX_TEST_S390X) || defined(BACK_S390X)
#include "common/cpu/recompiler/s390x/s390x_arm.h"
typedef SnxS390xArm Back;
#define back_(name) snx_s390x_arm_##name
#define BACK_NAME   "s390x"
#define BACK_UC     HOSTUC_S390X
#elif defined(SNX_TEST_MIPS64BE) || defined(BACK_MIPS64BE)
#include "common/cpu/recompiler/mips64/mips64be_arm.h"
typedef SnxMips64beArm Back;
#define back_(name) snx_mips64be_arm_##name
#define BACK_NAME   "mips64"
#define BACK_UC     HOSTUC_MIPS64
#elif defined(SNX_TEST_PPC64) || defined(BACK_PPC64)
#include "common/cpu/recompiler/ppc64/ppc64_arm.h"
typedef SnxPpc64Arm Back;
#define back_(name) snx_ppc64_arm_##name
#define BACK_NAME   "ppc64le"
#define BACK_UC     HOSTUC_PPC64LE
#elif defined(SNX_TEST_RV64) || defined(BACK_RV64)
#include "common/cpu/recompiler/rv64/rv64_arm.h"
typedef SnxRv64Arm Back;
#define back_(name) snx_rv64_arm_##name
#define BACK_NAME   "riscv64"
#define BACK_UC     HOSTUC_RV64
#elif defined(SNX_TEST_WIN64) || defined(BACK_X64)
#include "common/cpu/recompiler/x64/x64_arm.h"
typedef SnxX64Arm Back;
#define back_(name) snx_x64_arm_##name
#if defined(SNX_TEST_WIN64)
#include "common/cpu/recompiler/x64/x64_codegen.h"
#define BACK_NAME "x86-64 (Windows)"
#define BACK_UC   HOSTUC_WIN64
#else
#define BACK_NAME "x86-64"
#endif
#else
#error "no native back end for this host"
#endif
/* the blocks have a function table, their frames unwound by it (hostuc_step, hostuc_unwound) */
#if defined(SNX_TEST_WIN64) || defined(SNX_TEST_ARM64WIN)
#define SNX_TEST_UNWINDS 1
#endif

/* what the machine's build sets before its first block is made */
static inline void back_begin(void) {
#if defined(SNX_TEST_WIN64)
    snx_x64_set_win64(1);
#elif defined(SNX_TEST_ARM64WIN)
    snx_arm64_set_windows(1);
#endif
}
#endif
