/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The AArch32 blocks of a module made ahead of time (the 3DS's, the Vita's and the DS's aot/compiler.c, the GBA's
   agb/cpu/recomp.c): each is this host's own block compiler's (recompiler/mach/native.h), and LLVM's where that has none
   on this host or refuses the block, which is counted with its reason and said (snx_arm_aot_report) */
#ifndef SNX_ARM_AOT_H
#define SNX_ARM_AOT_H
#include "common/cpu/arm/codegen.h"
#include "common/cpu/recompiler/llvm/link.h"
#include <stddef.h>
#include <stdint.h>

/* who made the blocks, and what kept the native back end from the others: the reason, how many it took and the lowest
   address of one (bit 0 set for Thumb) */
#define SNX_ARM_AOT_WHYS 32
typedef struct {
    size_t native, llvm;
    struct {
        const char* why;
        size_t count;
        uint32_t lowest;
    } whys[SNX_ARM_AOT_WHYS];
    size_t nwhys;
    size_t unlisted; /* LLVM's blocks of reasons past the list's room */
} SnxArmAotCount;
/* LLVM's block at key (its address, bit 0 set for Thumb), for why: a string that stays */
void snx_arm_aot_refused(SnxArmAotCount* count, const char* why, uint32_t key);
/* from's blocks, another thread's, into count */
void snx_arm_aot_add(SnxArmAotCount* count, const SnxArmAotCount* from);
/* the recompile's lines, named after program: how many blocks the native back end made of them all (on stdout, unless
   quiet), and for each reason it refused others, on stderr, how many LLVM made and where the first is. Nothing for no
   blocks; where there is no native back end to refuse any, or none was asked (--keep-ir), the first line alone */
void snx_arm_aot_report(const SnxArmAotCount* count, const char* program, int quiet);
/* the reasons of the blocks LLVM makes though no block was refused: no native back end on this host, or the IR kept */
extern const char snx_arm_aot_none[], snx_arm_aot_kept[];

/* one thread's maker of blocks, which LLVM's are made at the -O letter opt for; keep_ir is the folder each module of
   them is saved in (--keep-ir), NULL for nowhere: every block is LLVM's then, as their IR is what is asked. NULL out of
   memory */
typedef struct SnxArmAot SnxArmAot;
SnxArmAot* snx_arm_aot_new(char opt, const char* keep_ir);
void snx_arm_aot_free(SnxArmAot* a);
/* the block at pc (Thumb when thumb is set), its code as guest memory holds it (little-endian, count words or halfwords),
   as the function name of the next objects, hidden in the module: 0 with *consumed the words or halfwords it covers, -1
   with the reason in error */
int snx_arm_aot_block(SnxArmAot* a, const char* name, const unsigned char* code, size_t count, uint32_t pc, int thumb,
                      const SnxArmCodegenOptions* options, size_t* consumed, char* error, size_t error_size);
/* the objects of the blocks made since the last call, the native back end's and LLVM's, into out (malloc'd data), and how
   many (0 to 2); -1 with the reason in error. id names LLVM's module, and its file in keep_ir (id.ll) */
int snx_arm_aot_objects(SnxArmAot* a, const char* id, SnxLinkInput out[2], char* error, size_t error_size);
const SnxArmAotCount* snx_arm_aot_count(const SnxArmAot* a);
#endif
