/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The arm64 back end's AArch32 blocks (arm64_arm.c): what the block compiler cpu/arm/llvm.h makes of an A32 block on
   any of its profiles and memory modes (snx_arm_llvm_block_ex, the options' below), made without LLVM in the arm64
   host's own code: mach/mach_arm.inc over sailgen's arm64 emitters of the AArch32 Sail (a32_arm64.inc), each block a
   function void name(SnxArmCPU*) of an ELF AArch64 object (a COFF one on Windows), global and hidden as LLVM's blocks
   are. It builds on any host (its code is arm64's: a test runs it under Unicorn). A block with an instruction it doesn't
   make is refused: LLVM makes it */
#ifndef SNX_RECOMPILER_ARM64_ARM_H
#define SNX_RECOMPILER_ARM64_ARM_H

#include "common/cpu/arm/codegen.h"
#include <stddef.h>
#include <stdint.h>

typedef struct SnxArm64Arm SnxArm64Arm;

/* the blocks the code may go on to directly after the block at key (its address, bit 0 set for Thumb), which covers
   units words of the count it was given: their keys, at most 2, and how many. Only these are jumped to (their
   functions must be in the link); any other block is reached by returning to the caller's loop */
typedef size_t (*SnxArm64ArmNext)(void* user, uint32_t key, size_t units, size_t count, uint32_t keys[2]);

/* 1 where this host runs the code and loads its objects (arm64: snx_arm64_available) */
int snx_arm64_arm_available(void);
/* a compiler of its own for one thread (NULL out of memory, or where unavailable) */
SnxArm64Arm* snx_arm64_arm_new(void);
void snx_arm64_arm_free(SnxArm64Arm* c);
/* what the next blocks are made for (cpu/arm/codegen.h; NULL the Cortex-A57 on flat memory, as a new compiler's): 0, or
   -1 for options no block compiler takes */
int snx_arm64_arm_options(SnxArm64Arm* c, const SnxArmCodegenOptions* options);
/* the A32 block at pc, its code as guest memory holds it (little-endian words, count of them at most: the block
   compiler's limit, 65536, at most; read as far as the block goes), as the function name (NULL: A_<pc> in eight hex
   digits, as module_arm.c names blocks): 1 with *consumed the words it covers, 0 where it can't make it (nothing
   kept; snx_arm64_arm_why says what) */
int snx_arm64_arm_block(SnxArm64Arm* c, const char* name, const unsigned char* code, size_t count, uint32_t pc,
                      size_t* consumed, SnxArm64ArmNext next, void* user);
/* the Thumb block at pc (halfword aligned), its code little-endian halfwords, as snx_arm64_arm_block (name NULL:
   T_<pc>), *consumed the halfwords it covers */
int snx_arm64_arm_thumb_block(SnxArm64Arm* c, const char* name, const unsigned char* code, size_t count, uint32_t pc,
                              size_t* consumed, SnxArm64ArmNext next, void* user);
/* what the last refused block had that this back end doesn't make */
const char* snx_arm64_arm_why(const SnxArm64Arm* c);
/* the object of the blocks made since the last one, in their addresses' order (malloc'd), 0; -1 out of memory.
   Its count of blocks in *blocks where not NULL; the compiler starts a new object after */
int snx_arm64_arm_object(SnxArm64Arm* c, unsigned char** object, size_t* size, size_t* blocks);
/* the blocks another compiler (another thread's) made, taken into c's next object: 0; -1 out of memory */
int snx_arm64_arm_absorb(SnxArm64Arm* c, SnxArm64Arm* from);
/* the arm64 back end's blocks go on to another of theirs past its checks (its function's X and its name: the code
   there reads no R15): a block another back end makes (key: the address, bit 0 set for Thumb) gets that entry here,
   R15 stored and its function jumped to, in c's next object: 0; -1 out of memory */
int snx_arm64_arm_entry(SnxArm64Arm* c, uint32_t key);
/* for tests: values in the first pool host registers alone (sailgen's sgx_pool), so they spill; 0 for all of them.
   And the host's features as if it had only these (arm64_codegen.h's SNX_ARM64_F_*) */
void snx_arm64_arm_limit(SnxArm64Arm* c, int pool, unsigned features);

#endif
