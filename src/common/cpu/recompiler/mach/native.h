/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native back ends' options for a unit of guest functions (mach/mach_codegen.inc, each machine's code generator:
   x64/x64_codegen.h, arm64/arm64_codegen.h) */
#ifndef SNX_RECOMPILER_NATIVE_H
#define SNX_RECOMPILER_NATIVE_H
#include "common/cpu/arm/codegen.h"
#include "common/cpu/recompiler/codegen.h"
#include <stddef.h>
#include <stdint.h>
typedef struct {
    /* a JIT unit: every function a local symbol, and table_name an array of their addresses in codegen's order, its
       one global (jit_service.c's emit_unit); else the AOT module's F_<address>, global and hidden */
    int jit;
    const char* table_name;
    /* nonzero for a target called through snx_aot_<address> rather than looked up (SnxIr.direct): the AOT module's
       function a JIT unit calls, or another module's function an AOT module imports */
    SnxU64 (*direct)(void* user, SnxU64 target);
    void* direct_user;
    /* the function the BR at address reaches once the program's linker has bound it, 0 for none, with *guard set when
       its slot may hold another (SnxIr.bound): a BL or B to such a PLT stub calls that function in the stub's place,
       on the machines that compare the slot (MG_BOUND: x86-64 and arm64) */
    SnxU64 (*bound)(void* user, SnxU64 address, int* guard);
    void* bound_user;
    /* a function left to LLVM, 0 for none: the one whose IR the setting ir_dump_pc prints */
    SnxU64 llvm_at;
    /* no other back end makes what this one can't: an instruction it has no code for raises an undefined instruction
       where it runs, and its function is made. 0: the function is left to LLVM */
    int alone;
    /* where not NULL, how many functions it could not make whole, added to it; each is said once (snx_error) */
    size_t* refused;
    /* words taken as ones there is no code for, how many: what a test of that asks for */
    const SnxU32* without;
    size_t without_count;
    /* the AOT module's stale checks (ir_codegen.c's snx_ir_stale_checks): snx_stale[the function's index] read at the
       entry and after each call's return */
    int stale;
    /* arm64: every SIMD&FP word by the A64 Sail's own code, none run as the host's own instruction (arm64_vec.c's
       vec): what a test holds that one to */
    int sail_alone;
} SnxNativeOptions;

/* what r's modules are made without (SnxNativeOptions.without), as a test of it asks: words is kept as it is */
struct snx_recompiler;
void snx_native_without(struct snx_recompiler* r, const SnxU32* words, size_t count);

/* this host's native back end (mach/native.c): x86-64's on an x86-64 host, arm64's on an arm64 one whose objects are
   ELF or COFF...; 1 where there is one */
int snx_native_available(void);
/* its name, for the recompiler's lines ("x86-64", "arm64"; "none") */
const char* snx_native_name(void);
/* the version of the code the native back ends make, 16 hex digits: a hash of what decides it (native_code.c, which
   cmake/native_code_hash.cmake generates), for what a cache of that code is keyed by */
const char* snx_native_code(void);
/* the host's features its code is made with (SGX_F_* of x64.h's on x86-64, with Windows' ABI; arm64_codegen.h's on
   arm64), for what a cache of that code is keyed by: 0 on the other machines, whose code takes none */
unsigned snx_native_features(void);

/* What this host lacks of what its architecture's back end takes of every host past the program's own baseline: on
   x86-64 SSE3, SSSE3, SSE4.1, SSE4.2, PCLMULQDQ and POPCNT, on a big-endian PowerPC64 ISA 2.06's LDBRX and STDBRX and
   the ELFv2 ABI. 1 with the line of it in why, what is needed and what the host has ("cpu: needs SSE4.2, this host has
   SSE3, SSSE3, SSE4.1, PCLMULQDQ and POPCNT"); 0 and "" where there is a back end, or none of the architecture is
   built */
int snx_native_floor(char* why, size_t size);
/* 1 where nothing makes this host's code, snx_native_floor's line in error: a host below the floor, in a build without
   LLVM (SNX_WITH_LLVM=OFF). 0 with LLVM, which makes the code there, and the line is said beside it; error is as it
   was then */
int snx_native_refused(char* error, size_t size);
/* What this host lacks that its back end does without, through its fallback (x86-64's PCLMULQDQ): 1 with the line of
   it in why ("cpu: this host has no PCLMULQDQ, done by the fallback"); 0 and "" for a host that has it all */
int snx_native_fallback(char* why, size_t size);
/* for a test: from here on as a build without LLVM (1) or as this one (0) */
void snx_native_set_alone(int on);
/* a feature a back end takes of every host: its name, and its bit of the host's */
typedef struct {
    const char* name;
    unsigned bit;
} SnxNativeFeature;
/* snx_native_floor's line of the features of need that has lacks into why: 1, or 0 and "" for a host that has them */
int snx_native_lacks(const SnxNativeFeature* need, size_t count, unsigned has, char* why, size_t size);
/* snx_native_fallback's of the features of done that has lacks: 1, or 0 and "" */
int snx_native_done(const SnxNativeFeature* done, size_t count, unsigned has, char* why, size_t size);
/* the object of the functions [first, last) of codegen the host's back end can make, made[i - first] 1 for each, as
   snx_x64_object and snx_arm64_object make it (for this host's features) */
int snx_native_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                      unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size);
/* snx_native_object through the memory an x86-64 unit is held in (x64_cache.h), *reused 1 for a unit that came from
   there; another machine's unit is made each time (the key, snx_x64_unit_facts, is x86-64's) */
int snx_native_cached_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                             unsigned char* made, unsigned char** object, size_t* size, int* reused, char* error,
                             size_t error_size);

/* 1 where this host's build makes the module of another machine's triple (module_llvm.h's: the pack of objects a JIT
   loader takes) with a native back end: arm64's, for arm64-apple-ios<version>-elf, whose objects are the ELF that
   loader reads, with Darwin's calling convention. 0 for another triple, on a Windows host, whose objects are COFF
   alone, and on a big-endian one, whose descriptors are its own numbers. Its code takes the features of the host it is
   made on only where that is the device (an app that recompiles for itself), else the architecture's baseline */
int snx_native_target(const char* triple);
/* snx_native_object for a triple snx_native_target takes; nothing is reused */
int snx_native_target_object(const char* triple, SnxCodegen* codegen, size_t first, size_t last,
                             const SnxNativeOptions* options, unsigned char* made, unsigned char** object, size_t* size,
                             char* error, size_t error_size);

/* the host's back end's AArch32 blocks (x64/x64_arm.h's or arm64/arm64_arm.h's, whose functions these are): a compiler
   of its own for one thread, NULL where the host has none or out of memory */
typedef struct SnxNativeArm SnxNativeArm;
typedef size_t (*SnxNativeArmNext)(void* user, uint32_t key, size_t units, size_t count, uint32_t keys[2]);
int snx_native_arm_available(void);
SnxNativeArm* snx_native_arm_new(void);
void snx_native_arm_free(SnxNativeArm* c);
/* what the next blocks are made for (cpu/arm/codegen.h; NULL the Cortex-A57 on flat memory, as a new compiler's): 0, or
   -1 for options no block compiler takes */
int snx_native_arm_options(SnxNativeArm* c, const SnxArmCodegenOptions* options);
int snx_native_arm_block(SnxNativeArm* c, const char* name, const unsigned char* code, size_t count, uint32_t pc,
                         size_t* consumed, SnxNativeArmNext next, void* user);
/* the Thumb block at pc, its code little-endian halfwords, *consumed the halfwords it covers */
int snx_native_thumb_block(SnxNativeArm* c, const char* name, const unsigned char* code, size_t count, uint32_t pc,
                           size_t* consumed, SnxNativeArmNext next, void* user);
const char* snx_native_arm_why(const SnxNativeArm* c);
int snx_native_arm_object(SnxNativeArm* c, unsigned char** object, size_t* size, size_t* blocks);
int snx_native_arm_absorb(SnxNativeArm* c, SnxNativeArm* from);
int snx_native_arm_entry(SnxNativeArm* c, uint32_t key);

/* An object of data alone, in the host's native back end's format (x64/x64_elf.h's builder), as an AOT module's
   descriptor may be: NULL with no native back end on this host, or out of memory. *pointer is the relocation that makes
   8 bytes of the data a symbol's address */
typedef struct SnxXElf SnxXElf;
SnxXElf* snx_native_data(unsigned* pointer);

/* An AOT module descriptor's few functions in a RISC machine's code (mach/risc_desc.inc, each one's code generator),
   for nx/loader/module_x64.c, which writes the data and puts each function in: its code and its relocations (of sym +
   addend at the code's byte at; type 0 a literal's, the machine's literal), the object's machine, e_flags and byte order
   the machine's. The code holds no address of the module's (abs64, the data's): a text relocation, which the linker
   refuses. Each 0, -1 out of room */
typedef struct {
    unsigned char code[1024];
    size_t size;
    struct {
        size_t at;
        const char* sym;
        long long addend;
        unsigned type;
    } rel[4];
    int nrel;
} SnxNativeFn;
typedef struct SnxNativeDesc {
    const char* name;
    unsigned em, elf_flags, abs64, literal;
    int big;
    /* a function returning &sym */
    int (*returns)(SnxNativeFn* f, const char* sym);
    /* snx_module(): ENT_ADDR[i] = base + ENT_OFF[i], ENT_PTR[i] = FNS[ENT_FN[i]] for count entries, then &MOD;
       ENT_FN fn_at bytes past ENT_OFF and ENT_PTR ptr_at past ENT_ADDR (their tables', one after the other) */
    int (*module_fn)(SnxNativeFn* f, size_t count, uint64_t base, size_t fn_at, size_t ptr_at);
    /* snx_import_dyn(c, e): CPU.disp's function for e where e is in [code_lo, code_hi) and aligned, else
       c->rt->miss, tail called with c and e */
    int (*import_dyn)(SnxNativeFn* f);
    /* a tail call through the pointer at sym + addend (read when it runs: IMPORT_FN's, a cached unit's table's) */
    int (*tail_slot)(SnxNativeFn* f, const char* sym, long long addend);
    /* a tail call of sym (the code's: a guest function's F_) */
    int (*tail_to)(SnxNativeFn* f, const char* sym);
} SnxNativeDesc;
/* the host's RISC machine's (NULL on x86-64 and arm64, whose module_x64.c writes itself, or with none); a machine's
   by its back end's name ("riscv64", "ppc64le"...) for the tests, NULL for none */
const SnxNativeDesc* snx_native_desc(void);
const SnxNativeDesc* snx_native_desc_named(const char* name);
#endif
