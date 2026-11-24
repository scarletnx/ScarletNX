/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native back ends' options for a unit of guest functions (mach/mach_codegen.inc, each machine's code generator:
   x64/x64_codegen.h, arm64/arm64_codegen.h) */
#ifndef SNX_RECOMPILER_NATIVE_H
#define SNX_RECOMPILER_NATIVE_H
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
    /* the AOT module's stale checks (ir_codegen.c's snx_ir_stale_checks): snx_stale[the function's index] read at the
       entry and after each call's return */
    int stale;
} SnxNativeOptions;

/* this host's native back end (mach/native.c): x86-64's on an x86-64 host that loads ELF objects, arm64's on an arm64
   one; 1 where there is one */
int snx_native_available(void);
/* its name, for the recompiler's lines ("x86-64", "arm64"; "none") */
const char* snx_native_name(void);
/* the object of the functions [first, last) of codegen the host's back end can make, made[i - first] 1 for each, as
   snx_x64_object and snx_arm64_object make it (for this host's features) */
int snx_native_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                      unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size);
/* snx_native_object through the memory an x86-64 unit is held in (x64_cache.h), *reused 1 for a unit that came from
   there; another machine's unit is made each time (the key, snx_x64_unit_facts, is x86-64's) */
int snx_native_cached_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                             unsigned char* made, unsigned char** object, size_t* size, int* reused, char* error,
                             size_t error_size);

/* the host's back end's AArch32 blocks (x64/x64_arm.h's or arm64/arm64_arm.h's, whose functions these are): a compiler
   of its own for one thread, NULL where the host has none or out of memory */
typedef struct SnxNativeArm SnxNativeArm;
typedef size_t (*SnxNativeArmNext)(void* user, uint32_t key, size_t units, size_t count, uint32_t keys[2]);
int snx_native_arm_available(void);
SnxNativeArm* snx_native_arm_new(void);
void snx_native_arm_free(SnxNativeArm* c);
int snx_native_arm_block(SnxNativeArm* c, const char* name, const unsigned char* code, size_t count, uint32_t pc,
                         size_t* consumed, SnxNativeArmNext next, void* user);
/* the Thumb block at pc, where the host's back end makes them (x86-64's); refused elsewhere */
int snx_native_thumb_block(SnxNativeArm* c, const char* name, const unsigned char* code, size_t count, uint32_t pc,
                           size_t* consumed, SnxNativeArmNext next, void* user);
const char* snx_native_arm_why(const SnxNativeArm* c);
int snx_native_arm_object(SnxNativeArm* c, unsigned char** object, size_t* size, size_t* blocks);
int snx_native_arm_absorb(SnxNativeArm* c, SnxNativeArm* from);
int snx_native_arm_entry(SnxNativeArm* c, uint32_t key);

/* An AOT module descriptor's few functions in a RISC machine's code (mach/risc_desc.inc, each one's code generator),
   for nx/loader/module_x64.c, which writes the data and puts each function in: its code and its relocations (of sym +
   addend at the code's byte at; type 0 an address of 8 bytes, the machine's abs64), the object's machine, e_flags and
   byte order the machine's. Each 0, -1 out of room */
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
    unsigned em, elf_flags, abs64;
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
