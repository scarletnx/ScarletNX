/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The riscv64 back end's AArch32 blocks: mach/mach_arm.inc over sailgen's riscv64 emitters of the AArch32 Sail
   (a32_rv64.inc), with the RISC back ends' glue (mach/risc_arm.inc). The calls and tail jumps are R_RISCV_CALL_PLT, the
   poll's address an R_RISCV_64 in the code; a load-acquire a FENCE after it. It builds on any host: its blocks are
   riscv64's code, which the tests run under Unicorn */
#include "common/cpu/recompiler/rv64/rv64_arm.h"
#include "common/cpu/recompiler/rv64/rv64_codegen.h"
#include "common/cpu/recompiler/x64/x64_elf.h"
#include "common/cpu/arm/arm.h"
#include "common/cpu/arm/simd.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
/* the ELF RISC-V relocations the blocks have (the RISC-V ELF psABI's numbers) */
enum { RV_EM = 243, RV_R_32_PCREL = 57, RV_R_CALL_PLT = 19 };
#define MG_ARM_INC         "a32_rv64.inc"
#define MG_ARM             SnxRv64Arm
#define MG_ARM_NEXT        SnxRv64ArmNext
#define MG_ARM_API(name)   snx_rv64_arm_##name
#define MG_ARM_AVAILABLE() snx_rv64_available()
#define MG_ARM_MAKES()     1
#define MG_ARM_FEATURES()  0u
#define MG_ARM_EM          RV_EM
#define MG_ARM_ELF_FLAGS   0x5 /* RVC, the LP64D float ABI */
#define MG_ARM_CALL_RELOC  RV_R_CALL_PLT
#define MG_ARM_R_TAIL      RV_R_CALL_PLT
#define MG_ARM_R_LITERAL   RV_R_32_PCREL
#define MG_ARM_ENTRY_EXIT  "snx_rv64_a32_entry_exit"
#define MG_ARM_THUMB_ENTRY "snx_rv64_thumb_entry"
#define MG_ARM_LOAD_FENCE  1 /* a load is no acquire: a FENCE after it */
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
int snx_rv64_arm_available(void) { return 0; }
SnxRv64Arm* snx_rv64_arm_new(void) { return NULL; }
void snx_rv64_arm_free(SnxRv64Arm* c) { (void)c; }
int snx_rv64_arm_options(SnxRv64Arm* c, const SnxArmCodegenOptions* options) {
    (void)c, (void)options;
    return -1;
}
int snx_rv64_arm_block(SnxRv64Arm* c, const char* name, const unsigned char* code, size_t count, uint32_t pc,
                       size_t* consumed, SnxRv64ArmNext next, void* user) {
    (void)c, (void)name, (void)code, (void)count, (void)pc, (void)next, (void)user;
    *consumed = 0;
    return 0;
}
int snx_rv64_arm_thumb_block(SnxRv64Arm* c, const char* name, const unsigned char* code, size_t count, uint32_t pc,
                             size_t* consumed, SnxRv64ArmNext next, void* user) {
    return snx_rv64_arm_block(c, name, code, count, pc, consumed, next, user);
}
const char* snx_rv64_arm_why(const SnxRv64Arm* c) {
    (void)c;
    return "no riscv64 back end in this build";
}
int snx_rv64_arm_object(SnxRv64Arm* c, unsigned char** object, size_t* size, size_t* blocks) {
    (void)c;
    *object = NULL;
    *size = 0;
    if (blocks) *blocks = 0;
    return -1;
}
void snx_rv64_arm_limit(SnxRv64Arm* c, int pool, unsigned features) { (void)c, (void)pool, (void)features; }
int snx_rv64_arm_absorb(SnxRv64Arm* c, SnxRv64Arm* from) {
    (void)c, (void)from;
    return -1;
}
int snx_rv64_arm_entry(SnxRv64Arm* c, uint32_t key) {
    (void)c, (void)key;
    return -1;
}
#endif
