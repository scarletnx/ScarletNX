/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The ppc64 back end's AArch32 blocks for a big-endian PowerPC64 (ELFv2) host: ppc64_arm.h's
   functions, the same back end's with PPC_BE 1 (ppc64be_arm.c): its words, its guest memory's accesses and
   its objects in that order */
#ifndef SNX_RECOMPILER_PPC64BE_ARM_H
#define SNX_RECOMPILER_PPC64BE_ARM_H

#include "common/cpu/arm/codegen.h"
#include <stddef.h>
#include <stdint.h>

typedef struct SnxPpc64beArm SnxPpc64beArm;

/* the blocks the code may go on to directly after the block at key (its address, bit 0 set for Thumb), which covers
   units words of the count it was given: their keys, at most 2, and how many. Only these are jumped to (their
   functions must be in the link); any other block is reached by returning to the caller's loop */
typedef size_t (*SnxPpc64beArmNext)(void* user, uint32_t key, size_t units, size_t count, uint32_t keys[2]);

/* 1 where this host runs the code and loads its objects (little-endian PowerPC64, ELF) */
int snx_ppc64be_arm_available(void);
/* a compiler of its own for one thread (NULL out of memory, or where unavailable) */
SnxPpc64beArm* snx_ppc64be_arm_new(void);
void snx_ppc64be_arm_free(SnxPpc64beArm* c);
/* what the next blocks are made for (cpu/arm/codegen.h; NULL the Cortex-A57 on flat memory, as a new compiler's): 0, or
   -1 for options no block compiler takes */
int snx_ppc64be_arm_options(SnxPpc64beArm* c, const SnxArmCodegenOptions* options);
/* the A32 block at pc, its code as guest memory holds it (little-endian words, count of them at most: the block
   compiler's limit, 65536, at most; read as far as the block goes), as the function name (NULL: A_<pc> in eight hex
   digits, as module_arm.c names blocks): 1 with *consumed the words it covers, 0 where it can't make it (nothing
   kept; snx_ppc64be_arm_why says what) */
int snx_ppc64be_arm_block(SnxPpc64beArm* c, const char* name, const unsigned char* code, size_t count, uint32_t pc,
                      size_t* consumed, SnxPpc64beArmNext next, void* user);
/* the Thumb block at pc (halfword aligned), its code little-endian halfwords, as snx_ppc64be_arm_block (name NULL:
   T_<pc>), *consumed the halfwords it covers */
int snx_ppc64be_arm_thumb_block(SnxPpc64beArm* c, const char* name, const unsigned char* code, size_t count,
                                uint32_t pc, size_t* consumed, SnxPpc64beArmNext next, void* user);
/* what the last refused block had that this back end doesn't make */
const char* snx_ppc64be_arm_why(const SnxPpc64beArm* c);
/* the ELF object of the blocks made since the last one, in their addresses' order (malloc'd), 0; -1 out of memory.
   Its count of blocks in *blocks where not NULL; the compiler starts a new object after */
int snx_ppc64be_arm_object(SnxPpc64beArm* c, unsigned char** object, size_t* size, size_t* blocks);
/* the blocks another compiler (another thread's) made, taken into c's next object: 0; -1 out of memory */
int snx_ppc64be_arm_absorb(SnxPpc64beArm* c, SnxPpc64beArm* from);
/* the powerpc64 back end's blocks go on to another of theirs past its checks (its function's X and its name: the code
   there reads no R15): a block another back end makes (key: the address, bit 0 set for Thumb) gets that entry here,
   R15 stored and its function jumped to, in c's next object: 0; -1 out of memory */
int snx_ppc64be_arm_entry(SnxPpc64beArm* c, uint32_t key);
/* for tests: values in the first pool host registers alone (sailgen's sgx_pool), so they spill; 0 for all of them.
   And the host's features as if it had only these (none yet: 0) */
void snx_ppc64be_arm_limit(SnxPpc64beArm* c, int pool, unsigned features);

#endif
