/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What runs a pc the game module has no function for (RtAPI.miss, rtapi.c): the module's AOT variant table
   (common/cpu/recompiler/variants.h), linked in at build time from a profile run's cache, then the runtime JIT for the
   functions the pack does not cover. There is no interpreter: a pc neither the pack nor the JIT can run ends the game */
#ifndef SNX_RUNTIME_GUEST_JIT_H
#define SNX_RUNTIME_GUEST_JIT_H
#include "common/cpu/aarch64/cpu.h"

/* once the game's executables are loaded, the dispatch table built and the variant table stored, before any guest
   thread runs: builds the images the hash reads, the dense variant leader index, starts the JIT and registers the
   invalidation. A module without a table is normal: the JIT handles every miss and writes its cache */
void jit_init(void);
/* the AOT function for pc: the dispatch table inside the code range, else the variant table; when the pack has no
   matching variant, the JIT's code or wait for it */
GuestFn jit_resolve(u64 pc);
/* jit_resolve, and for a pc in executable memory no image covers (a block the game mapped itself, mem_query), the
   block from pc is registered as an image for the hash and the JIT. NULL when neither the pack nor the JIT can run it:
   a pc in freed memory, one that isn't code, or one with every image slot taken */
GuestFn jit_resolve_or_add(u64 pc);
/* the variant table's function for pc after checking the current bytes, else the JIT's code or wait for it; NULL only
   when neither can run pc */
GuestFn jit_wait(u64 pc);
/* the JIT's code for what is at pc now, past pc's dispatch slot and the variant table, waiting for its compile: for a
   hooked function whose AOT code went stale (hook_original), as the slot holds the hook. NULL without a JIT */
GuestFn jit_code_now(u64 pc);
/* why jit_wait answered NULL: the pack has no code and the JIT failed or is unavailable */
const char* jit_failure(void);
/* Code the game maps while it runs (an NRO, hle/services/ldr.c), text_size bytes of text at base out of size: it is
   registered as an image for the hash and the JIT. 0, or -1 (no table, or as many such images as it keeps) */
int jit_code_added(u64 base, u64 text_size, u64 size);
/* before the memory at base is unmapped: forget its image */
void jit_code_removed(u64 base);
/* The NRO of hash (the SHA-256 ldr:ro made of its image) is mapped at base, size bytes with its BSS, its module ID
   id: when that is the address g_nros gives it (nro_slots.h), its own AOT module, <hash>.so in <Game>_nro beside the
   game module, is loaded (once: it stays for the NRO's next mapping), checked to be that NRO's for that address, and
   its entries go into the dispatch table, *entries of them. 0; 1 when the NRO has no place in g_nros, is mapped
   elsewhere or has no module file, so the JIT runs it; -1 for a module that is refused, said why once */
int jit_nro_added(const u8* hash, u64 base, u64 size, const u8* id, u32* entries);
/* before the NRO at base is unmapped: its module's entries leave the dispatch table */
void jit_nro_removed(u64 base);
/* The dispatch slot of addr was given fn, which answers every call of that address from now on in the game module's
   place (a hook, NVN's implementation of a function the game resolved): an NRO's module that calls the function at
   addr directly, through its import of it, calls fn from now on. Called once the slot holds fn */
void jit_slot_taken(u64 addr, GuestFn fn);
/* a mapping, permission or write-back change under [addr, addr+size) (mem_set_code_invalidate): the dispatch slots in
   the range are cleared, the JIT's code is invalidated, and a block the game mapped that is no longer executable loses
   its image */
void jit_code_invalidate(u64 addr, u64 size);
/* the guest changed its code in [addr, addr+size) and made it visible (IC IVAU, RtAPI.code_changed): the AOT
   functions compiled from it go stale (SnxModule.stale) and leave dispatch, and the JIT's code there goes, so what
   runs there next is compiled from the code as it is now */
void jit_code_changed(u64 addr, u64 size);
/* the totals, into the log, at exit */
void jit_report(void);

#endif
