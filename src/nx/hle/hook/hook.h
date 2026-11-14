/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Hooks: functions the game exports, run by a handler of the runtime's instead of their recompiled code. A handler is
   an accelerator: it does what the function is known to do, from reading its code, natively (a host memmove for the
   guest's memcpy), on the guest's own data as the function leaves it, so a copy of it the game's compiler inlined
   reads the same state. The hooks are declared in hooks.hook (their symbols, as the game's dynamic symbol table
   spells them, their C arguments and handlers) and tools/hookgen generates their adapters.

   A hook takes the function's slot in the dispatch table (g_disp), which every call to a hooked function goes
   through: the game's calls into another of its modules (through the PLT and GOT the game's dynamic linker fills),
   its calls through function pointers, its direct calls within a module and its calls through a PLT stub bound to
   the function (the recompiler calls a function whose symbol a hook names through its slot, snx_codegen_routed and
   nx/loader/hook_routes.h) and the runtime JIT's calls
   (jit_service.c calls an AOT function directly only while its slot still holds it). Hooks are installed once the game's executables are loaded, before its first
   thread, and stay for the whole run. A hook may hold only for some versions of the SDK that built the module
   exporting its function, and its handler may read guest structs through the layout for that version
   (docs/platforms/nx/hooks.md) */
#ifndef SNX_HLE_HOOK_H
#define SNX_HLE_HOOK_H
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/arm/arm.h"
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
/* What hook_install left to the game's own code for its version, for the stop of a game that then reached a service
   the runtime answers only natively (sm.c): "" when every symbol with a hook got one, else a sentence with how
   many symbols have declarations that all are for other versions than their module's, and the first one's name,
   module, version and the versions it is declared for */
void hook_missed_text(char* out, size_t n);

/* Runs the guest function fn(a0, a1, a2, a3) from inside a handler, as nvn_cmd.c's guest_call runs a game's
   callback: its return to the handler's own return address ends the run, and the link register is put back
   (codec.c). In a 32-bit game it is hook_guest_call_a32 */
void hook_guest_call(CPU* c, u64 fn, u64 a0, u64 a1, u64 a2, u64 a3);
/* the same for a 32-bit game's function (bit 0 of fn set for Thumb), its four words in r0 to r3: the thread is put
   back at the hooked function its handler is for, where hook_run_original_a32 finds it (hook.c) */
void hook_guest_call_a32(CPU* c, u32 fn, u32 a0, u32 a1, u32 a2, u32 a3);
/* a readable handle of a new kernel event attached to the game's SystemEventType at event through its SDK's own
   nn::os::AttachReadableHandleToSystemEvent (auto close, auto clear), as an SDK object attaches its service's: the
   event, with a reference of the caller's beside the handle's, to drop (kobj_unref) when the caller's object goes;
   NULL when the SDK exports no such function or the handle table is full (codec.c) */
struct KEvent* hook_attach_event(CPU* c, u64 event, const char* name);
/* attaches handle, a readable handle the caller made, to the game's SystemEventType at event the same way (auto
   close, auto clear): 0 when the SDK exports no such function or handle is 0, and the handle is then the caller's */
int hook_attach_handle(CPU* c, u64 event, u32 handle);
/* the host address of guest address a, in integers as GP (memory/mem.h) makes it: under NCE c->mem is 0 */
static inline u8* hook_host(const CPU* c, u64 a) { return (u8*)(uintptr_t)((uintptr_t)c->mem + a); }
/* A field of a guest struct, as a layout hookgen generated from a struct declaration gives it for the module a hook
   was installed in: its offset, in a type of its own per field type, so that hook_get_u32 takes only a u32 field.
   hook_get_<type>(c, L->field, addr) reads the field of the struct at guest address addr, hook_put_<type> writes it,
   little-endian whatever the host's order; an atomic access takes hook_host(c, addr + L->field.offset) itself */
#define HOOK_FIELD(T, C, U, LD, ST)                                                                                    \
    typedef struct {                                                                                                   \
        u32 offset;                                                                                                    \
    } HookField_##T;                                                                                                   \
    static inline C hook_get_##T(const CPU* c, HookField_##T f, u64 addr) {                                            \
        return (C)LD(hook_host(c, addr + f.offset));                                                                   \
    }                                                                                                                  \
    static inline void hook_put_##T(CPU* c, HookField_##T f, u64 addr, C v) { ST(hook_host(c, addr + f.offset), (U)v); }
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
static inline int hook_get_bool(const CPU* c, HookField_bool f, u64 addr) {
    return *hook_host(c, addr + f.offset) != 0;
}
static inline void hook_put_bool(CPU* c, HookField_bool f, u64 addr, int v) {
    *hook_host(c, addr + f.offset) = (u8)(v != 0);
}
typedef struct {
    u32 offset;
} HookField_f32;
static inline f32 hook_get_f32(const CPU* c, HookField_f32 f, u64 addr) {
    u32 bits = snx_ld_le32(hook_host(c, addr + f.offset));
    f32 v;
    memcpy(&v, &bits, sizeof v);
    return v;
}
static inline void hook_put_f32(CPU* c, HookField_f32 f, u64 addr, f32 v) {
    u32 bits;
    memcpy(&bits, &v, sizeof bits);
    snx_st_le32(hook_host(c, addr + f.offset), bits);
}
typedef struct {
    u32 offset;
} HookField_f64;
static inline f64 hook_get_f64(const CPU* c, HookField_f64 f, u64 addr) {
    u64 bits = snx_ld_le64(hook_host(c, addr + f.offset));
    f64 v;
    memcpy(&v, &bits, sizeof v);
    return v;
}
static inline void hook_put_f64(CPU* c, HookField_f64 f, u64 addr, f64 v) {
    u64 bits;
    memcpy(&bits, &v, sizeof bits);
    snx_st_le64(hook_host(c, addr + f.offset), bits);
}

/* the functions hooked: a hook's symbol may name one in each of the game's modules. As many as a table has entries at
   most (tools/hookgen's MAX_ENTRIES), so a game that exports each function once has a site for every hook: hook.c
   checks that its tables fit */
#define HOOK_SITES 1024

/* verify mode's scratch memory: slots of guest memory in the kernel region, each call verified taking one without
   waiting (a guest thread may lose its core inside the function's own code, and another verifying a call then must
   not wait for it) */
#define HOOK_VERIFY_SLOTS 16
#define HOOK_VERIFY_SLOT  (4u << 20)

/* What a save state holds of the hooks: verify mode's scratch slots (hooks_verify), which are guest memory, so their
   base is the state's. 0 for none: the state of a run that verified nothing */
typedef struct {
    u64 verify_base;
} HleHook;

/* The host's side, which a save state never holds: the functions this process hooked, by its own code's addresses. A
   state loaded here was saved by a process with other ones, or with other hooks */
typedef struct {
    u32 count;
    u16 hook[HOOK_SITES];     /* its hook, in the generated table */
    u64 addr[HOOK_SITES];     /* the function's guest address */
    GuestFn orig[HOOK_SITES]; /* its recompiled code, which its slot held */
    u8 stale[HOOK_SITES];     /* atomic: the game changed the function's code (a mod's patch), so orig is stale */
    GuestFn fn[HOOK_SITES];   /* what its slot holds now: the hook's adapter, or verify mode's check */
    u32 word[HOOK_SITES];     /* the function's first 4 bytes at hook_install, which a mod's hook writes over */
    u8 invalid[HOOK_SITES];   /* atomic: the game changed them, so the native hook is off (hook_restore) */
    /* verify mode: which scratch slots are taken (bit i, atomic), and each site's calls that agreed, differed and
       were skipped (atomic) */
    u32 verify_taken;
    u64 agreed[HOOK_SITES], differed[HOOK_SITES], skipped[HOOK_SITES];
    /* a 32-bit game's hooked functions: each one's address (bit 0 set for Thumb) and the block its slot held, for
       hook_run_original_a32; and the symbols hook_install found declared only for other versions than their
       module's, for hook_missed_text: how many, and the first one's first entry in its table, its module and that
       module's version of the entry's key */
    u32 a32_count;
    u32 a32_addr[HOOK_SITES];
    SnxArmBlock a32_orig[HOOK_SITES];
    u32 missed, missed_entry, missed_module, missed_version;
} HostHook;

/* hooks the functions the game exports that hooks.hook names, and those of the game's builds that the address hooks
   of src/nx/patches/ name (unless hooks=0), once the game's executables are loaded and their symbols read
   (kernel_load_symbols), before the JIT starts */
void hook_install(void);
/* whether the module calls the function of symbol name through its dispatch slot: its route list (main.h's g_routes)
   names it, or the module has no list (it routes every function it exports) or routes every call (--hook-direct-calls).
   When not, a hook in the slot sees only the calls through function pointers and the PLT stubs the module did not
   bind (loader.h's SnxBoundBranch) */
int hook_routed(const char* name);
/* whether the settings turn the hooks of family off: hooks=0, or hooks_off names it */
int hook_family_off(const char* family);
/* After the JIT dropped what was compiled in [addr, addr + size) (jit_code_invalidate): a hooked function whose first
   4 bytes the game changed there (a mod's hook over its start; a change further in is not looked at) is the game's
   from then on. Its native hook is invalidated: its adapter's own host code is rewritten into a jump to
   hook_invalidated (jump.h), so that every call still reaching it (an NRO's import, the JIT's direct call) runs the
   game's code, and a warning names it; the game stops when the system refuses to make that code writable. Every
   other hook in the range, where its function is still executable code, goes back in its slot */
void hook_restore(u64 addr, u64 size);
/* the sad path of an invalidated native hook, where its adapter jumps with the arguments its call left: runs the
   game's code now at entry, which is what a call of the function runs from then on */
void hook_invalidated(CPU* RESTRICT c, u64 entry);
/* what the dispatch slot of addr holds for a hooked function, its handler's adapter or verify mode's check, for as
   long as the game runs; NULL when addr isn't hooked, or its native hook was invalidated */
GuestFn hook_function(u64 addr);
/* the recompiled code of the function at addr when it is hooked, for a handler that hands a case back to it with the
   CPU as the function's call left it; NULL when addr isn't hooked. Once the game changed the function's code, the
   JIT's code of what is there now: the AOT code is stale, and leaves for the dispatch slot, which holds the hook */
GuestFn hook_original(u64 addr);
/* the symbol of the function hooked at addr, and the name of the runtime's handler that runs it in *handler; NULL when
   addr isn't hooked. Reads only the sites hook_install wrote before the game's first thread, so the crash report may
   call it (debug.c) */
const char* hook_at(u64 addr, const char** handler);
/* the AOT function f went stale (guest_jit.c): the sites whose own code it was run what the game put there instead */
void hook_orig_stale(GuestFn f);
/* where the hook of symbol (a symbol, or an address hook's key as hookgen writes it, BUILDID:OFFSET) was installed:
   the function's guest address, the first one for a symbol several modules export; 0 when it wasn't */
u64 hook_site(const char* symbol);
/* A handler that waits on the host (a mutex someone holds, a condition variable, a sleep): its thread leaves its core
   between the two, and counts as stopped for a pause, as a waiting SVC (sync.c has them) */
void hook_wait_begin(void);
void hook_wait_end(void);
/* family.c: why the family's hooks can't run in this process (a 32-bit game, a host without what its handlers run
   on), NULL when they can: hook_install leaves such a family's functions to the game's code, and says why */
const char* hook_family_unready(const char* family);
/* libc_std.c: errno of the calling guest thread set to e (POSIX's number), through the SDK's __errno_location; 0
   leaves it. libc_wide.c: the host has what the libc_wide family runs on (a 32-bit wchar_t and a UTF-8 locale) */
void libc_set_errno(CPU* c, int e);
int libc_wide_ready(void);
/* alloc.c: the host allocator's arena in guest memory is made (made at the first call): 0 without one, and the alloc
   family is then off */
int alloc_ready(void);
/* runs the hooked function at addr's own code, with the CPU as the call to it left it, until it returns to the call's
   return address (x30) with the call's stack, through the dispatch loop when its code leaves for code compiled apart:
   for a handler that does its work around the function's, as the function's own code then sees every call */
void hook_run_original(CPU* c, u64 addr); /* a handler finds addr in c->pc, which its adapter sets */
/* The same for a 32-bit game, from the handler of the hooked function its thread is in: the adapter runs with r15 at
   the function, so no address is given (a 32-bit SDK's symbol is often not the 64-bit one's). The function's own
   code runs with the registers and the stack as the call left them until it returns to the call's lr; its result
   comes back, r0 with r1 above it */
u64 hook_run_original_a32(CPU* c);

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
/* an argument past x7 or v7: the 8-byte stack slot slot from the caller's sp, little-endian, a float in its low
   bytes */
static inline u64 hook_stack_u64(const CPU* c, int slot) { return snx_ld_le64(c->mem + c->x[31] + 8 * (u64)slot); }
static inline f32 hook_stack_f32(const CPU* c, int slot) {
    u32 bits = snx_ld_le32(c->mem + c->x[31] + 8 * (u64)slot);
    f32 f;
    memcpy(&f, &bits, sizeof f);
    return f;
}
static inline f64 hook_stack_f64(const CPU* c, int slot) {
    u64 bits = hook_stack_u64(c, slot);
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

/* The adapters for a 32-bit program (hookgen --a32), blocks of its code table that run a hooked function's handler.
   The handler takes the CPU of the thread whose 32-bit registers arm is (nx/runtime/arm.c), for its guest memory and
   the kernel's waits */
CPU* hook_a32_cpu(SnxArmCPU* arm);
/* the word and the double word at byte off of the caller's stack arguments, which start at sp */
static inline u32 hook_a32_word(const SnxArmCPU* arm, int off) {
    return snx_ld_le32(arm->mem + (u32)(arm->r[13] + (u32)off));
}
static inline u64 hook_a32_dword(const SnxArmCPU* arm, int off) {
    return (u64)hook_a32_word(arm, off) | (u64)hook_a32_word(arm, off + 4) << 32;
}
static inline f32 hook_a32_bits_f32(u32 bits) {
    f32 f;
    memcpy(&f, &bits, sizeof f);
    return f;
}
static inline f64 hook_a32_bits_f64(u64 bits) {
    f64 f;
    memcpy(&f, &bits, sizeof f);
    return f;
}
/* sN is half of d(N / 2): the low half for an even N */
static inline f32 hook_a32_s(const SnxArmCPU* arm, int n) {
    return hook_a32_bits_f32((u32)(arm->d[n / 2] >> (n & 1 ? 32 : 0)));
}
static inline f64 hook_a32_d(const SnxArmCPU* arm, int n) { return hook_a32_bits_f64(arm->d[n]); }
static inline f32 hook_a32_f32_at(const SnxArmCPU* arm, int off) { return hook_a32_bits_f32(hook_a32_word(arm, off)); }
static inline f64 hook_a32_f64_at(const SnxArmCPU* arm, int off) { return hook_a32_bits_f64(hook_a32_dword(arm, off)); }
static inline void hook_a32_set_r01(SnxArmCPU* arm, u64 v) {
    arm->r[0] = (u32)v;
    arm->r[1] = (u32)(v >> 32);
}
/* a write to s0 leaves s1, the rest of d0, as a VMOV to s0 would */
static inline void hook_a32_set_s0(SnxArmCPU* arm, f32 f) {
    u32 bits;
    memcpy(&bits, &f, sizeof bits);
    arm->d[0] = (arm->d[0] & 0xFFFFFFFF00000000ull) | bits;
}
static inline void hook_a32_set_d0(SnxArmCPU* arm, f64 f) { memcpy(&arm->d[0], &f, sizeof f); }
/* bx lr: to lr, in Thumb state when its bit 0 is set */
static inline void hook_a32_return(SnxArmCPU* arm) {
    u32 lr = arm->r[14];
    arm->cpsr = (arm->cpsr & ~SNX_ARM_T) | (lr & 1 ? SNX_ARM_T : 0);
    arm->r[15] = lr & ~1u;
}
#endif
