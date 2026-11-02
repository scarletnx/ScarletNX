/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What runs a pc the game module has no function for (RtAPI.miss, rtapi.c): the module's AOT variant table
   (common/cpu/recompiler/variants.h), linked in at build time from a profile run's cache, then the runtime JIT for the
   functions the pack does not cover. There is no interpreter: a pc neither the pack nor the JIT can run ends the game */
#ifndef SNX_RUNTIME_GUEST_JIT_H
#define SNX_RUNTIME_GUEST_JIT_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/loader/hw.h"

/* once the game's executables are loaded, the dispatch table built and the variant table stored, before any guest
   thread runs: builds the images the hash reads, the dense variant leader index, starts the JIT for the CPU of hw (as
   the AOT module was made for it) and registers the invalidation. A module without a table is normal: the JIT handles
   every miss and writes its cache */
void jit_init(SnxHw hw);
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
/* why jit_wait answered NULL: the pack has no code and the JIT failed or is unavailable */
const char* jit_failure(void);
/* Code the game maps while it runs (an NRO, hle/services/ldr.c), text_size bytes of text at base out of size: it is
   registered as an image for the hash and the JIT. 0, or -1 (no table, or as many such images as it keeps) */
int jit_code_added(u64 base, u64 text_size, u64 size);
/* before the memory at base is unmapped: forget its image */
void jit_code_removed(u64 base);
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
