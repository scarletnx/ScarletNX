/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The guest code this host's native back end leaves (mach/native.h): a function or a block it does not make whole, all
   of them on a host that has none, and there an AOT module's index. fallback_llvm.c makes it with LLVM where the build
   has the backend (SNX_WITH_LLVM=ON); fallback_none.c refuses it where not, each error naming what was asked for and
   why the native back end left it. A program links the one its build has, and what compiles guest code calls it here
   and takes no header of LLVM's */
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
/* the function the fallback is to make whatever the native back end can, its IR printed (the setting ir_dump_pc), for
   SnxNativeOptions.llvm_at: 0 for none, as always without LLVM */
SnxU64 snx_fallback_dump_pc(void);
/* 1 where the fallback counts each unit's code (the setting ir_stats), 0 without LLVM */
int snx_fallback_stats(void);

struct SnxIrTuning; /* recompiler/llvm/ir.h's */
/* What an AOT module's unit asks of the fallback besides its code, all of it LLVM's own: snx_fallback_object takes
   NULL for none of it, as a JIT unit asks */
typedef struct {
    const char* triple;               /* another machine's module (snx_native_target's), NULL for this host's */
    const struct SnxIrTuning* tuning; /* the game code's LLVM settings (snx_llvm_tuning), NULL for LLVM's own */
    const char* keep_ir;              /* where the unit's IR is kept, as <name>.ll (--keep-ir); NULL for nowhere */
    /* per function of codegen, its place in the --layout order, which puts it in a code section of its own
       (SNX_LINK_HOT_TEXT and the place), or SIZE_MAX; NULL for no order */
    const size_t* rank;
    int callees_first; /* each function made after those of the unit it calls or jumps to: ir_promote.c's summaries */
    char* stats;       /* its answer: the unit's IR counts (snx_fallback_stats) as a line's text, malloc'd; or NULL */
} SnxFallbackUnit;

/* The object of the functions [first, last) of codegen the native back end did not make (made as it left it, 1 for each
   it did; NULL: none of them), as it makes them for options (jit, table_name, direct, bound and stale; its llvm_at,
   alone, refused and without are its own) and unit (NULL: none of it): an AOT module's, or code the JIT's loader loads
   into this process (options->jit). name is the unit's, and opt the -O letter: 0, 1, 2, 3, s or z. 0, with *object
   (malloc'd) NULL where every function was made; -1 with the reason in error */
int snx_fallback_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                        const unsigned char* made, SnxFallbackUnit* unit, const char* name, char opt,
                        unsigned char** object, size_t* size, char* error, size_t error_size);
/* The runtime library (rtlib.c) made for this host's processor at opt, as a module whose code the fallback made links
   it: 0 with the object (malloc'd), or -1 with the reason in error */
int snx_fallback_rtlib(char opt, unsigned char** object, size_t* size, char* error, size_t error_size);
/* Whether the fallback can make this host's code, started where it must be (LLVM's targets, once a process): 0, or -1
   with why not in error. None's 0 where the host has a native back end, which then makes all of it */
int snx_fallback_start(char* error, size_t error_size);

/* AArch32 and Thumb blocks: a maker of them for one thread, for an AOT module or (jit) for code the JIT's loader loads
   into this process, at opt's -O level. NULL out of memory */
typedef struct SnxFallbackArm SnxFallbackArm;
SnxFallbackArm* snx_fallback_arm_new(const char* name, char opt, int jit);
void snx_fallback_arm_free(SnxFallbackArm* f);
/* Whether it can make code for this host, for what has no other: 0, or -1 with why not in error. A block starts it */
int snx_fallback_arm_start(SnxFallbackArm* f, char* error, size_t error_size);
/* The block at pc, of count words or halfwords (thumb) at most, as host integers, into the object being made, as name
   (hidden in an AOT module's): 0 with *consumed how many it covers, or -1 with the reason in error. why is the reason
   the native back end gave */
int snx_fallback_arm_block(SnxFallbackArm* f, const char* name, const void* code, size_t count, uint32_t pc, int thumb,
                           const SnxArmCodegenOptions* options, const char* why, size_t* consumed, char* error,
                           size_t error_size);
/* How many of the count words or halfwords the block at pc would cover, as snx_fallback_arm_block makes it, for a
   discovery that goes on past it: 0 with *consumed, or -1 with the reason in error. Nothing goes in the object */
int snx_fallback_arm_length(SnxFallbackArm* f, const void* code, size_t count, uint32_t pc, int thumb,
                            const SnxArmCodegenOptions* options, const char* why, size_t* consumed, char* error,
                            size_t error_size);
/* The block name of the object being made, at key (its address, bit 0 set for Thumb), made to go on by itself as the
   native back ends' blocks do for chained (cpu/arm/codegen.h): where it ends at one of the count keys of next, a tail
   call to its function names[i], the object's or declared for the linker, the poll counted down first for one at or
   before key; any other target through the lookup table. With chained->direct it is an import's stub, whose SVC calls
   chained->import through the function there and goes on past it to next[0] (count 1). 0, or -1 with the reason */
int snx_fallback_arm_chain(SnxFallbackArm* f, const char* name, uint32_t key, const SnxArmChainedOptions* chained,
                           const uint32_t* next, const char* const* names, size_t count, char* error,
                           size_t error_size);
/* The text of the blocks since the last object as the block compiler made them, before the passes (--keep-ir), into
   *text (malloc'd): 0, or -1 with the reason in error */
int snx_fallback_arm_ir(SnxFallbackArm* f, char** text, char* error, size_t error_size);
/* The object of the blocks since the last one (malloc'd): 0, or -1 with the reason in error. Either way the next block
   starts another */
int snx_fallback_arm_object(SnxFallbackArm* f, unsigned char** object, size_t* size, char* error, size_t error_size);

/* An object of constants for this host, an AOT module's index, where the native back end makes none (mach/native.h's
   snx_native_data) or its IR is asked for, which why says: module, made as unit's modules at opt's -O level, of the
   globals asked for in their order, each constant and internal to the object but its descriptor. NULL out of memory */
typedef struct SnxFallbackData SnxFallbackData;
SnxFallbackData* snx_fallback_data_new(const char* unit, const char* module, char opt, const char* why);
void snx_fallback_data_free(SnxFallbackData* d);
/* count pointers, each to the function of the block name_of names for it (writing the name into out, of size bytes,
   and returning 1), or null (returning 0). The functions are hidden, of one pointer, each declared at its first. 0, or
   -1 with the reason in error */
int snx_fallback_data_blocks(SnxFallbackData* d, const char* name, size_t count,
                             int (*name_of)(void* user, size_t i, char* out, size_t size), void* user, char* error,
                             size_t error_size);
/* count integers of bits bits (8, 32 or 64), values an array of uint8_t, uint32_t or uint64_t: 0, or -1 */
int snx_fallback_data_integers(SnxFallbackData* d, const char* name, unsigned bits, size_t count, const void* values,
                               char* error, size_t error_size);
/* A field of a struct, at offset in the C struct read as it: an integer of bits bits (8, 32 or 64), value; with no bits
   count bytes, or a pointer to the global named, made before (NULL for null) */
typedef struct {
    unsigned bits;
    uint64_t value;
    const unsigned char* bytes;
    size_t count;
    const char* global;
    size_t offset;
} SnxFallbackField;
/* count structs of nfields fields each, as a C struct of size bytes lays them out: fields holds count * nfields of
   them, or where count is 0 nfields that give their kinds alone. 0, or -1 with the reason in error, a layout C does not
   have among them */
int snx_fallback_data_structs(SnxFallbackData* d, const char* name, size_t count, const SnxFallbackField* fields,
                              size_t nfields, size_t size, char* error, size_t error_size);
/* the struct the object exports, its descriptor, as one of snx_fallback_data_structs' */
int snx_fallback_data_descriptor(SnxFallbackData* d, const char* name, const SnxFallbackField* fields, size_t nfields,
                                 size_t size, char* error, size_t error_size);
/* its text before the passes (--keep-ir), as snx_fallback_arm_ir's; and its object (malloc'd). 0, or -1 */
int snx_fallback_data_ir(SnxFallbackData* d, char** text, char* error, size_t error_size);
int snx_fallback_data_object(SnxFallbackData* d, unsigned char** object, size_t* size, char* error, size_t error_size);
#endif
