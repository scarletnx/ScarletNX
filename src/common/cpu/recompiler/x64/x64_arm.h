/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The x86-64 back end's AArch32 blocks (x64_arm.c): what the block compiler cpu/arm/llvm.h makes of an A32 block on
   the Cortex-A57 with flat memory (its snx_arm_llvm_block, a 32-bit NX program's), made without LLVM. The data
   processing and branches are sailgen's x86-64 emitters of the AArch32 Sail (a32_x64.inc, the same roots as LLVM's),
   the rest written over the same SSA; the block ends where llvm.c's does, and the code it goes on to it reaches by a
   jump of its own where that is a block the caller names (the chaining nx/loader/module_arm.c gives LLVM's blocks).
   Each block is a function void name(SnxArmCPU*) of an ELF object, global and hidden as LLVM's blocks are, so the
   two back ends' blocks call each other. A Thumb block is made the same way (snx_x64_thumb_block, what
   cpu/arm/llvm_thumb.inc makes of it). A block with an instruction it doesn't make is refused: LLVM makes it */
#ifndef SNX_RECOMPILER_X64_ARM_H
#define SNX_RECOMPILER_X64_ARM_H

#include <stddef.h>
#include <stdint.h>

typedef struct SnxX64Arm SnxX64Arm;

/* the blocks the code may go on to directly after the block at key (its address, bit 0 set for Thumb), which covers
   units words or halfwords of the count it was given: their keys, at most 2, and how many. Only these are jumped to
   (their functions must be in the link); any other block is reached by returning to the caller's loop */
typedef size_t (*SnxX64ArmNext)(void* user, uint32_t key, size_t units, size_t count, uint32_t keys[2]);

/* 1 where this host runs the code and loads its objects (x86-64, ELF) */
int snx_x64_arm_available(void);
/* a compiler of its own for one thread (NULL out of memory, or where unavailable) */
SnxX64Arm* snx_x64_arm_new(void);
void snx_x64_arm_free(SnxX64Arm* c);
/* the A32 block at pc, its code as guest memory holds it (little-endian words, count of them at most: the block
   compiler's limit, 65536, at most; read as far as the block goes), as the function name (NULL: A_<pc> in eight hex
   digits, as module_arm.c names blocks): 1 with *consumed the words it covers, 0 where it can't make it (nothing
   kept; snx_x64_arm_why says what) */
int snx_x64_arm_block(SnxX64Arm* c, const char* name, const unsigned char* code, size_t count, uint32_t pc,
                      size_t* consumed, SnxX64ArmNext next, void* user);
/* the Thumb block at pc (halfword aligned), its code little-endian halfwords as guest memory holds it, as
   snx_x64_arm_block (name NULL: T_<pc>), *consumed the halfwords it covers */
int snx_x64_thumb_block(SnxX64Arm* c, const char* name, const unsigned char* code, size_t count, uint32_t pc,
                        size_t* consumed, SnxX64ArmNext next, void* user);
/* what the last refused block had that this back end doesn't make */
const char* snx_x64_arm_why(const SnxX64Arm* c);
/* the ELF object of the blocks made since the last one, in their addresses' order (malloc'd), 0; -1 out of memory.
   Its count of blocks in *blocks where not NULL; the compiler starts a new object after */
int snx_x64_arm_object(SnxX64Arm* c, unsigned char** object, size_t* size, size_t* blocks);
/* the blocks another compiler (another thread's) made, taken into c's next object: 0; -1 out of memory */
int snx_x64_arm_absorb(SnxX64Arm* c, SnxX64Arm* from);
/* the x86-64 back end's blocks go on to another of theirs past its checks (its function's X and its name: the code
   there reads no R15): a block another back end makes (key: the address, bit 0 set for Thumb) gets that entry here,
   R15 stored and its function jumped to, in c's next object: 0; -1 out of memory */
int snx_x64_arm_entry(SnxX64Arm* c, uint32_t key);
/* for tests: values in the first pool host registers alone (sailgen's sgx_pool), so they spill; 0 for all of them.
   And the host's features as if it had only these (SGX_F_*: BMI1 1, BMI2 2, LZCNT 4), ~0u for its own */
void snx_x64_arm_limit(SnxX64Arm* c, int pool, unsigned features);

#endif
