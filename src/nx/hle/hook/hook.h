/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Hooks: functions the game exports, run by a handler of the runtime's instead of their recompiled code. A handler is
   an accelerator: it does what the function is known to do, from reading its code, natively (a host memmove for the
   guest's memcpy), on the guest's own data as the function leaves it, so a copy of it the game's compiler inlined
   reads the same state. The hooks are declared in hooks.hook (their symbols, as the game's dynamic symbol table
   spells them, their C arguments and handlers) and tools/hookgen generates their adapters.

   A hook takes the function's slot in the dispatch table (g_disp), which every call to an exported function goes
   through: the game's calls into another of its modules (through the PLT and GOT the game's dynamic linker fills),
   its calls through function pointers, its direct calls within a module (the recompiler calls an exported function
   through its slot, snx_codegen_exported) and the runtime JIT's calls (jit_service.c calls an AOT function directly
   only while its slot still holds it). Hooks are installed once the game's executables are loaded, before its first
   thread, and stay for the whole run. A hook may hold only for some versions of the SDK that built the module
   exporting its function, and its handler may read guest structs through the layout for that version
   (docs/HOOKS.md) */
#ifndef SNX_HLE_HOOK_H
#define SNX_HLE_HOOK_H
#include "common/cpu/aarch64/cpu.h"
#include "common/utilities/endian.h"
#include "nx/hle/hook/verify.h"
#include <stddef.h>
#include <string.h>

/* What a hook's versions compare (hookgen's @version): HOOK_SDK the nnSdk version of the SDK label in the module that
   exports the function (x << 16 | y << 8 | z), HOOK_NVN the NVN API version (major << 16 | minor). HOOK_ANY is an
   unversioned hook's, which holds for every module. A version of 0 is unknown, and only HOOK_ANY holds for it */
enum { HOOK_ANY, HOOK_SDK, HOOK_NVN, HOOK_KEYS };

/* a generated table (hookgen's <prefix>_table): its entries sorted by symbol and then by version, entry i for the
   versions [lo[i], hi[i]) of key[i] */
typedef struct {
    u32 count;
    const char* const* symbol;
    const unsigned char* key;
    const u32* lo;
    const u32* hi;
} HookTable;

/* the version an SDK middleware label in bytes gives lib ("nnSdk" or "NVN"): the first "SDK MW+<vendor>+<vendor>SDK_"
   (or "...Sdk_") followed by lib, '-' and x_y_z, packed x << 16 | y << 8 | z; 0 when bytes holds none */
u32 hook_label_version(const u8* bytes, size_t size, const char* lib);
/* the entry of t for symbol whose versions hold the module's: versions[key] for each key (versions[HOOK_ANY]
   unused). -1 when t has no entry for symbol, -2 when it has but none holds; *first is the symbol's first entry, or
   -1 */
int hook_select(const HookTable* t, const char* symbol, const u32 versions[HOOK_KEYS], int* first);
/* a version of key as the declarations write it (20.5.22, or 55.12 for NVN), "unknown" for 0 */
void hook_version_text(char* out, size_t n, int key, u32 version);
/* an entry's versions as @version writes them ("sdk >= 12.0.0, < 16.0.0"), "any version" for HOOK_ANY */
void hook_range_text(char* out, size_t n, int key, u32 lo, u32 hi);

/* A field of a guest struct, as a layout hookgen generated from a struct declaration gives it for the module a hook
   was installed in: its offset, in a type of its own per field type, so that hook_get_u32 takes only a u32 field.
   hook_get_<type>(c, L->field, addr) reads the field of the struct at guest address addr, hook_put_<type> writes it,
   little-endian whatever the host's order; an atomic access takes c->mem + addr + L->field.offset itself */
#define HOOK_FIELD(T, C, U, LD, ST)                                                                                    \
    typedef struct {                                                                                                   \
        u32 offset;                                                                                                    \
    } HookField_##T;                                                                                                   \
    static inline C hook_get_##T(const CPU* c, HookField_##T f, u64 addr) { return (C)LD(c->mem + addr + f.offset); }  \
    static inline void hook_put_##T(CPU* c, HookField_##T f, u64 addr, C v) { ST(c->mem + addr + f.offset, (U)v); }
static inline u8 hook_ld8(const void* p) { return *(const u8*)p; }
static inline void hook_st8(void* p, u8 v) { *(u8*)p = v; }
HOOK_FIELD(u8, u8, u8, hook_ld8, hook_st8)
HOOK_FIELD(u16, u16, u16, snx_ld_le16, snx_st_le16)
HOOK_FIELD(u32, u32, u32, snx_ld_le32, snx_st_le32)
HOOK_FIELD(u64, u64, u64, snx_ld_le64, snx_st_le64)
HOOK_FIELD(i8, i8, u8, (i8)hook_ld8, hook_st8)
HOOK_FIELD(i16, i16, u16, (i16)snx_ld_le16, snx_st_le16)
HOOK_FIELD(i32, i32, u32, (i32)snx_ld_le32, snx_st_le32)
HOOK_FIELD(i64, i64, u64, (i64)snx_ld_le64, snx_st_le64)
HOOK_FIELD(addr, u64, u64, snx_ld_le64, snx_st_le64)
#undef HOOK_FIELD
/* a bool is a byte, 0 or 1 */
typedef struct {
    u32 offset;
} HookField_bool;
static inline int hook_get_bool(const CPU* c, HookField_bool f, u64 addr) { return c->mem[addr + f.offset] != 0; }
static inline void hook_put_bool(CPU* c, HookField_bool f, u64 addr, int v) { c->mem[addr + f.offset] = (u8)(v != 0); }
typedef struct {
    u32 offset;
} HookField_f32;
static inline f32 hook_get_f32(const CPU* c, HookField_f32 f, u64 addr) {
    u32 bits = snx_ld_le32(c->mem + addr + f.offset);
    f32 v;
    memcpy(&v, &bits, sizeof v);
    return v;
}
static inline void hook_put_f32(CPU* c, HookField_f32 f, u64 addr, f32 v) {
    u32 bits;
    memcpy(&bits, &v, sizeof bits);
    snx_st_le32(c->mem + addr + f.offset, bits);
}
typedef struct {
    u32 offset;
} HookField_f64;
static inline f64 hook_get_f64(const CPU* c, HookField_f64 f, u64 addr) {
    u64 bits = snx_ld_le64(c->mem + addr + f.offset);
    f64 v;
    memcpy(&v, &bits, sizeof v);
    return v;
}
static inline void hook_put_f64(CPU* c, HookField_f64 f, u64 addr, f64 v) {
    u64 bits;
    memcpy(&bits, &v, sizeof bits);
    snx_st_le64(c->mem + addr + f.offset, bits);
}

/* the functions hooked: a hook's symbol may name one in each of the game's modules */
#define HOOK_SITES 256

/* verify mode's scratch memory: slots of guest memory in the kernel region, each call verified taking one without
   waiting (a guest thread may lose its core inside the function's own code, and another verifying a call then must
   not wait for it) */
#define HOOK_VERIFY_SLOTS 16
#define HOOK_VERIFY_SLOT  (4u << 20)

typedef struct {
    u32 count;
    u16 hook[HOOK_SITES];     /* its hook, in the generated table */
    u64 addr[HOOK_SITES];     /* the function's guest address */
    GuestFn orig[HOOK_SITES]; /* its recompiled code, which its slot held */
    GuestFn fn[HOOK_SITES];   /* what its slot holds now: the hook's adapter, or verify mode's check */
    /* verify mode (hooks_verify): the scratch slots' base and which are taken (bit i, atomic), and each site's calls
       that agreed, differed and were skipped (atomic) */
    u64 verify_base;
    u32 verify_taken;
    u64 agreed[HOOK_SITES], differed[HOOK_SITES], skipped[HOOK_SITES];
} HleHook;

/* hooks the functions the game exports that hooks.hook names (unless hooks=0), once the game's executables are
   loaded and their symbols read (kernel_load_symbols), before the JIT starts */
void hook_install(void);
/* puts the hooks in [addr, addr + size) back in their slots after the JIT dropped what was compiled there
   (jit_code_invalidate), where the function is still executable code */
void hook_restore(u64 addr, u64 size);
/* the recompiled code of the function at addr when it is hooked, for a handler that hands a case back to it with the
   CPU as the function's call left it; NULL when addr isn't hooked */
GuestFn hook_original(u64 addr);

/* the float arguments and results of the generated adapters: lane 0 of a vector register, the guest's bytes */
static inline f32 hook_f32(const CPU* c, int r) {
    u32 bits = SNX_VS(c->v[r], 0);
    f32 f;
    memcpy(&f, &bits, sizeof f);
    return f;
}
static inline f64 hook_f64(const CPU* c, int r) {
    u64 bits = SNX_VD(c->v[r], 0);
    f64 f;
    memcpy(&f, &bits, sizeof f);
    return f;
}
/* a scalar write to s0 or d0 zeroes the rest of v0, as the instruction that writes it would */
static inline void hook_set_f32(CPU* c, f32 f) {
    u32 bits;
    memcpy(&bits, &f, sizeof bits);
    memset(&c->v[0], 0, sizeof c->v[0]);
    SNX_VSET_S(c->v[0], 0, bits);
}
static inline void hook_set_f64(CPU* c, f64 f) {
    u64 bits;
    memcpy(&bits, &f, sizeof bits);
    memset(&c->v[0], 0, sizeof c->v[0]);
    SNX_VSET_D(c->v[0], 0, bits);
}
#endif
