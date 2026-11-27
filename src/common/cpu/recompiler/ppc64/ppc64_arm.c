/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The powerpc64 back end's AArch32 blocks: mach/mach_arm.inc over sailgen's powerpc64 emitters of the AArch32 Sail
   (a32_ppc64.inc), with the RISC back ends' glue (mach/risc_arm.inc). The calls', the tail jumps' and the poll's
   addresses are R_PPC64_ADDR64s in the code; a load-acquire a SYNC after it. It builds on any host: its blocks are
   powerpc64's code, which the tests run under Unicorn */
#include "common/cpu/recompiler/ppc64/ppc64_arm.h"
#include "common/cpu/recompiler/ppc64/ppc64_codegen.h"
#include "common/cpu/recompiler/x64/x64_elf.h"
#include "common/cpu/arm/arm.h"
#include "common/cpu/arm/simd.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
/* the big-endian host's blocks (ppc64be_arm.c) are these with PPC_BE 1 */
#ifndef PPC_BE
#define PPC_BE 0
#endif
#define RK_BIG_ENDIAN PPC_BE
#define MG_ARM_ELF_BIG PPC_BE
/* the ELF PowerPC64 relocation the blocks have (the ELFv2 ABI's number) */
enum { PPC_EM = 21, PPC_R_REL64 = 44 };
#define MG_ARM_INC         "a32_ppc64.inc"
#define MG_ARM             SnxPpc64Arm
#define MG_ARM_NEXT        SnxPpc64ArmNext
#define MG_ARM_API(name)   snx_ppc64_arm_##name
#define MG_ARM_AVAILABLE() snx_ppc64_available()
#define MG_ARM_MAKES()     1
#if PPC_BE /* LDBRX where the host has it (ppc64be_codegen.h) */
#include "common/cpu/recompiler/ppc64/ppc64be_codegen.h"
#define MG_ARM_FEATURES() snx_ppc64be_features()
#else
#define MG_ARM_FEATURES() 0u
#endif
#define MG_ARM_EM          PPC_EM
#define MG_ARM_ELF_FLAGS   2 /* ELFv2 */
#define MG_ARM_CALL_RELOC  PPC_R_REL64
#define MG_ARM_R_TAIL      PPC_R_REL64
#define MG_ARM_R_LITERAL   PPC_R_REL64
#define MG_ARM_ENTRY_EXIT  "snx_ppc_a32_entry_exit"
#define MG_ARM_THUMB_ENTRY "snx_ppc_thumb_entry"
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
int snx_ppc64_arm_available(void) { return 0; }
SnxPpc64Arm* snx_ppc64_arm_new(void) { return NULL; }
void snx_ppc64_arm_free(SnxPpc64Arm* c) { (void)c; }
int snx_ppc64_arm_options(SnxPpc64Arm* c, const SnxArmCodegenOptions* options) {
    (void)c, (void)options;
    return -1;
}
int snx_ppc64_arm_block(SnxPpc64Arm* c, const char* name, const unsigned char* code, size_t count, uint32_t pc,
                       size_t* consumed, SnxPpc64ArmNext next, void* user) {
    (void)c, (void)name, (void)code, (void)count, (void)pc, (void)next, (void)user;
    *consumed = 0;
    return 0;
}
int snx_ppc64_arm_thumb_block(SnxPpc64Arm* c, const char* name, const unsigned char* code, size_t count, uint32_t pc,
                              size_t* consumed, SnxPpc64ArmNext next, void* user) {
    return snx_ppc64_arm_block(c, name, code, count, pc, consumed, next, user);
}
const char* snx_ppc64_arm_why(const SnxPpc64Arm* c) {
    (void)c;
    return "no powerpc64 back end in this build";
}
int snx_ppc64_arm_object(SnxPpc64Arm* c, unsigned char** object, size_t* size, size_t* blocks) {
    (void)c;
    *object = NULL;
    *size = 0;
    if (blocks) *blocks = 0;
    return -1;
}
void snx_ppc64_arm_limit(SnxPpc64Arm* c, int pool, unsigned features) { (void)c, (void)pool, (void)features; }
int snx_ppc64_arm_absorb(SnxPpc64Arm* c, SnxPpc64Arm* from) {
    (void)c, (void)from;
    return -1;
}
int snx_ppc64_arm_entry(SnxPpc64Arm* c, uint32_t key) {
    (void)c, (void)key;
    return -1;
}
#endif
