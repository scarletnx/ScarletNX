/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The guest code this host's native back end leaves (mach/native.h): a function or a block it does not make whole, and
   all of them on a host that has none. fallback_llvm.c makes it with LLVM where the build has the backend
   (SNX_WITH_LLVM=ON); fallback_none.c refuses it where not, each error naming the code asked for and why the native
   back end left it. A program links the one its build has, and what compiles guest code calls it here and takes no
   header of LLVM's */
#ifndef SNX_RECOMPILER_FALLBACK_H
#define SNX_RECOMPILER_FALLBACK_H
#include "common/cpu/arm/codegen.h"
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/mach/native.h"
#include <stddef.h>
#include <stdint.h>

/* 1 where this build has a fallback, 0 where each call below that makes code fails */
int snx_fallback_available(void);
/* its name, for the lines a recompile prints ("LLVM"; "none") */
const char* snx_fallback_name(void);

/* The object of the functions [first, last) of codegen the native back end did not make (made as it left it, 1 for each
   it did; NULL: none of them), as it makes them for options (jit, table_name, direct, bound and stale; its llvm_at,
   alone, refused and without are its own): an AOT module's, or code the JIT's loader loads into this process
   (options->jit). name is the unit's, and opt the -O letter: 0, 1, 2, 3, s or z. 0, with *object (malloc'd) NULL where
   every function was made; -1 with the reason in error */
int snx_fallback_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                        const unsigned char* made, const char* name, char opt, unsigned char** object, size_t* size,
                        char* error, size_t error_size);

/* AArch32 and Thumb blocks: a maker of them for one thread, for an AOT module or (jit) for code the JIT's loader loads
   into this process, at opt's -O level. NULL out of memory */
typedef struct SnxFallbackArm SnxFallbackArm;
SnxFallbackArm* snx_fallback_arm_new(const char* name, char opt, int jit);
void snx_fallback_arm_free(SnxFallbackArm* f);
/* Whether it can make code for this host, for what has no other: 0, or -1 with why not in error. A block starts it */
int snx_fallback_arm_start(SnxFallbackArm* f, char* error, size_t error_size);
/* The block at pc, of count words or halfwords (thumb) at most, as host integers, into the object being made, as name:
   0 with *consumed how many it covers, or -1 with the reason in error. why is the reason the native back end gave */
int snx_fallback_arm_block(SnxFallbackArm* f, const char* name, const void* code, size_t count, uint32_t pc, int thumb,
                           const SnxArmCodegenOptions* options, const char* why, size_t* consumed, char* error,
                           size_t error_size);
/* The object of the blocks since the last one (malloc'd): 0, or -1 with the reason in error. Either way the next block
   starts another */
int snx_fallback_arm_object(SnxFallbackArm* f, unsigned char** object, size_t* size, char* error, size_t error_size);
#endif
