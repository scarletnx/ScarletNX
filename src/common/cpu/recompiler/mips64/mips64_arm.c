/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The mips64 back end's AArch32 blocks: mach/mach_arm.inc over sailgen's mips64 emitters of the AArch32 Sail
   (a32_mips64.inc), with the RISC back ends' glue (mach/risc_arm.inc). The calls', the tail jumps' and the poll's
   addresses are R_MIPS_64s in the code (n64's r_info); a load-acquire a SYNC after it. It builds on any host: its blocks are
   mips64's code, which the tests run under Unicorn */
#include "common/cpu/recompiler/mips64/mips64_arm.h"
#include "common/cpu/recompiler/mips64/mips64_codegen.h"
#include "common/cpu/recompiler/x64/x64_elf.h"
#include "common/cpu/arm/arm.h"
#include "common/cpu/arm/simd.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
/* the big-endian host's blocks (mips64be_arm.c) are these with MIPS_BE 1 */
#ifndef MIPS_BE
#define MIPS_BE 0
#endif
#define RK_BIG_ENDIAN MIPS_BE
#define MG_ARM_ELF_BIG MIPS_BE
/* the ELF MIPS64 relocation the blocks have (n64's number) */
enum { MIPS_EM = 8, MIPS_R_64 = 18 };
#define MG_ARM_INC         "a32_mips64.inc"
#define MG_ARM             SnxMips64Arm
#define MG_ARM_NEXT        SnxMips64ArmNext
#define MG_ARM_API(name)   snx_mips64_arm_##name
#define MG_ARM_AVAILABLE() snx_mips64_available()
#define MG_ARM_MAKES()     1
#define MG_ARM_FEATURES()  0u
#define MG_ARM_EM          MIPS_EM
#define MG_ARM_ELF_FLAGS   0x80000007u /* MIPS64r2, PIC, CPIC, NOREORDER */
#define MG_ARM_CALL_RELOC  MIPS_R_64
#define MG_ARM_R_TAIL      MIPS_R_64
#define MG_ARM_R_ABS64     MIPS_R_64
#define MG_ARM_ENTRY_EXIT  "snx_mips_a32_entry_exit"
#define MG_ARM_LOAD_FENCE  1 /* a load is no acquire: a SYNC after it */
#define MG_ARM_FP_HOT      0

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#include "common/cpu/recompiler/mach/mach_arm.inc"
#include "common/cpu/recompiler/mach/risc_arm.inc"

#else
int snx_mips64_arm_available(void) { return 0; }
SnxMips64Arm* snx_mips64_arm_new(void) { return NULL; }
void snx_mips64_arm_free(SnxMips64Arm* c) { (void)c; }
int snx_mips64_arm_block(SnxMips64Arm* c, const char* name, const unsigned char* code, size_t count, uint32_t pc,
                       size_t* consumed, SnxMips64ArmNext next, void* user) {
    (void)c, (void)name, (void)code, (void)count, (void)pc, (void)next, (void)user;
    *consumed = 0;
    return 0;
}
const char* snx_mips64_arm_why(const SnxMips64Arm* c) {
    (void)c;
    return "no mips64 back end in this build";
}
int snx_mips64_arm_object(SnxMips64Arm* c, unsigned char** object, size_t* size, size_t* blocks) {
    (void)c;
    *object = NULL;
    *size = 0;
    if (blocks) *blocks = 0;
    return -1;
}
void snx_mips64_arm_limit(SnxMips64Arm* c, int pool, unsigned features) { (void)c, (void)pool, (void)features; }
int snx_mips64_arm_absorb(SnxMips64Arm* c, SnxMips64Arm* from) {
    (void)c, (void)from;
    return -1;
}
int snx_mips64_arm_entry(SnxMips64Arm* c, uint32_t key) {
    (void)c, (void)key;
    return -1;
}
#endif
