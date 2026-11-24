/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The loongarch64 back end's AArch32 blocks: mach/mach_arm.inc over sailgen's loongarch64 emitters of the AArch32 Sail
   (a32_la64.inc), with the RISC back ends' glue (mach/risc_arm.inc). The calls and tail jumps are R_LARCH_CALL36, the
   poll's address an R_LARCH_64 in the code; a load-acquire a DBAR after it. It builds on any host: its blocks are
   loongarch64's code, which the tests run in tests/support/la64_interp.c */
#include "common/cpu/recompiler/la64/la64_arm.h"
#include "common/cpu/recompiler/la64/la64_codegen.h"
#include "common/cpu/recompiler/x64/x64_elf.h"
#include "common/cpu/arm/arm.h"
#include "common/cpu/arm/simd.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
/* the ELF LoongArch relocations the blocks have (the LoongArch ELF psABI's numbers) */
enum { LA_EM = 258, LA_R_64 = 2, LA_R_CALL36 = 110 };
#define MG_ARM_INC         "a32_la64.inc"
#define MG_ARM             SnxLa64Arm
#define MG_ARM_NEXT        SnxLa64ArmNext
#define MG_ARM_API(name)   snx_la64_arm_##name
#define MG_ARM_AVAILABLE() snx_la64_available()
#define MG_ARM_MAKES()     1
#define MG_ARM_FEATURES()  0u
#define MG_ARM_EM          LA_EM
#define MG_ARM_ELF_FLAGS   0x43 /* the LP64D ABI, object file v1 */
#define MG_ARM_CALL_RELOC  LA_R_CALL36
#define MG_ARM_R_TAIL      LA_R_CALL36
#define MG_ARM_R_ABS64     LA_R_64
#define MG_ARM_ENTRY_EXIT  "snx_la64_a32_entry_exit"
#define MG_ARM_LOAD_FENCE  1 /* a load is no acquire: a DBAR after it */
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
int snx_la64_arm_available(void) { return 0; }
SnxLa64Arm* snx_la64_arm_new(void) { return NULL; }
void snx_la64_arm_free(SnxLa64Arm* c) { (void)c; }
int snx_la64_arm_block(SnxLa64Arm* c, const char* name, const unsigned char* code, size_t count, uint32_t pc,
                       size_t* consumed, SnxLa64ArmNext next, void* user) {
    (void)c, (void)name, (void)code, (void)count, (void)pc, (void)next, (void)user;
    *consumed = 0;
    return 0;
}
const char* snx_la64_arm_why(const SnxLa64Arm* c) {
    (void)c;
    return "no loongarch64 back end in this build";
}
int snx_la64_arm_object(SnxLa64Arm* c, unsigned char** object, size_t* size, size_t* blocks) {
    (void)c;
    *object = NULL;
    *size = 0;
    if (blocks) *blocks = 0;
    return -1;
}
void snx_la64_arm_limit(SnxLa64Arm* c, int pool, unsigned features) { (void)c, (void)pool, (void)features; }
int snx_la64_arm_absorb(SnxLa64Arm* c, SnxLa64Arm* from) {
    (void)c, (void)from;
    return -1;
}
int snx_la64_arm_entry(SnxLa64Arm* c, uint32_t key) {
    (void)c, (void)key;
    return -1;
}
#endif
