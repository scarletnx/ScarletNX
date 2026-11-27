/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The AArch32 blocks of a module made ahead of time (the 3DS's, the Vita's and the DS's aot/compiler.c, the GBA's
   agb/cpu/recomp.c), each this host's own block compiler's (recompiler/mach/native.h). A block it refuses fails the
   recompile, named with the reason */
#ifndef SNX_ARM_AOT_H
#define SNX_ARM_AOT_H
#include "common/cpu/arm/codegen.h"
#include "common/cpu/recompiler/object/link.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* how many blocks the native back end made */
typedef struct {
    size_t native;
} SnxArmAotCount;
/* from's blocks, another thread's, into count */
void snx_arm_aot_add(SnxArmAotCount* count, const SnxArmAotCount* from);
/* the recompile's line of how many blocks the native back end made, on out (NULL: quiet). Nothing for no blocks */
void snx_arm_aot_report(const SnxArmAotCount* count, FILE* out);

/* one thread's maker of blocks: NULL with the reason in error, out of memory or on a host whose native back end makes
   no AArch32 blocks (its floor's line where it is below it) */
typedef struct SnxArmAot SnxArmAot;
SnxArmAot* snx_arm_aot_new(char* error, size_t error_size);
void snx_arm_aot_free(SnxArmAot* a);
/* the block at pc (Thumb when thumb is set), its code as guest memory holds it (little-endian, count words or halfwords),
   as the function name of the next objects, hidden in the module: 0 with *consumed the words or halfwords it covers, -1
   with the reason in error */
int snx_arm_aot_block(SnxArmAot* a, const char* name, const unsigned char* code, size_t count, uint32_t pc, int thumb,
                      const SnxArmCodegenOptions* options, size_t* consumed, char* error, size_t error_size);
/* the object of the blocks made since the last call into *out (malloc'd data): 1, 0 for none, -1 with the reason in
   error */
int snx_arm_aot_objects(SnxArmAot* a, SnxLinkInput* out, char* error, size_t error_size);
const SnxArmAotCount* snx_arm_aot_count(const SnxArmAot* a);
#endif
