/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The s390x back end's AArch32 blocks: mach/mach_arm.inc over sailgen's s390x emitters of the AArch32 Sail
   (a32_s390x.inc), with the RISC back ends' glue (mach/risc_arm.inc). The calls', the tail jumps' and the poll's
   addresses are R_390_64s in the code; a load-acquire a serialization after it. It builds on any host: its blocks are
   s390x's code, which the tests run under Unicorn */
#include "common/cpu/recompiler/s390x/s390x_arm.h"
#include "common/cpu/recompiler/s390x/s390x_codegen.h"
#include "common/cpu/recompiler/x64/x64_elf.h"
#include "common/cpu/arm/arm.h"
#include "common/cpu/arm/simd.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
/* s390x is big-endian */
#define RK_BIG_ENDIAN 1
#define MG_ARM_ELF_BIG 1
/* the ELF s390x relocation the blocks have (the s390x ELF ABI's number) */
enum { ZS_EM = 22, ZS_R_PC64 = 23 };
#define MG_ARM_INC         "a32_s390x.inc"
#define MG_ARM             SnxS390xArm
#define MG_ARM_NEXT        SnxS390xArmNext
#define MG_ARM_API(name)   snx_s390x_arm_##name
#define MG_ARM_AVAILABLE() snx_s390x_available()
#define MG_ARM_MAKES()     1
#define MG_ARM_FEATURES()  0u
#define MG_ARM_EM          ZS_EM
#define MG_ARM_ELF_FLAGS   0
#define MG_ARM_CALL_RELOC  ZS_R_PC64
#define MG_ARM_R_TAIL      ZS_R_PC64
#define MG_ARM_R_LITERAL   ZS_R_PC64
#define MG_ARM_ENTRY_EXIT  "snx_zs_a32_entry_exit"
#define MG_ARM_THUMB_ENTRY "snx_zs_thumb_entry"
#define MG_ARM_LOAD_FENCE  1 /* a load-acquire: a serialization after it */
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
int snx_s390x_arm_available(void) { return 0; }
SnxS390xArm* snx_s390x_arm_new(void) { return NULL; }
void snx_s390x_arm_free(SnxS390xArm* c) { (void)c; }
int snx_s390x_arm_options(SnxS390xArm* c, const SnxArmCodegenOptions* options) {
    (void)c, (void)options;
    return -1;
}
int snx_s390x_arm_block(SnxS390xArm* c, const char* name, const unsigned char* code, size_t count, uint32_t pc,
                       size_t* consumed, SnxS390xArmNext next, void* user) {
    (void)c, (void)name, (void)code, (void)count, (void)pc, (void)next, (void)user;
    *consumed = 0;
    return 0;
}
int snx_s390x_arm_thumb_block(SnxS390xArm* c, const char* name, const unsigned char* code, size_t count, uint32_t pc,
                              size_t* consumed, SnxS390xArmNext next, void* user) {
    return snx_s390x_arm_block(c, name, code, count, pc, consumed, next, user);
}
const char* snx_s390x_arm_why(const SnxS390xArm* c) {
    (void)c;
    return "no s390x back end in this build";
}
int snx_s390x_arm_object(SnxS390xArm* c, unsigned char** object, size_t* size, size_t* blocks) {
    (void)c;
    *object = NULL;
    *size = 0;
    if (blocks) *blocks = 0;
    return -1;
}
void snx_s390x_arm_limit(SnxS390xArm* c, int pool, unsigned features) { (void)c, (void)pool, (void)features; }
int snx_s390x_arm_absorb(SnxS390xArm* c, SnxS390xArm* from) {
    (void)c, (void)from;
    return -1;
}
int snx_s390x_arm_entry(SnxS390xArm* c, uint32_t key) {
    (void)c, (void)key;
    return -1;
}
#endif
