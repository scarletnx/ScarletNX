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
   thread, and stay for the whole run (docs/HOOKS.md) */
#ifndef SNX_HLE_HOOK_H
#define SNX_HLE_HOOK_H
#include "common/cpu/aarch64/cpu.h"
#include <string.h>

/* the functions hooked: a hook's symbol may name one in each of the game's modules */
#define HOOK_SITES 256

typedef struct {
    u32 count;
    u16 hook[HOOK_SITES];     /* its hook, in the generated table */
    u64 addr[HOOK_SITES];     /* the function's guest address */
    GuestFn orig[HOOK_SITES]; /* its recompiled code, which its slot held */
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
