/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NCE (native code execution) in the runtime: on an AArch64 Linux host, the game's 64-bit code runs on the host's
   CPU, at the guest's own addresses (mem_identity). An NCE recompile (the setting nce) makes a module with no code
   made ahead of time, and <game>.nce beside it: the words of the code to patch and the pools for their stubs
   (nce_patch.h). The runtime patches them after the load. The stubs call the runtime through the machine layer
   (nce_host.h), and the runtime answers them through RtAPI, as it answers the recompiled code: an SVC, the counter,
   CPU.poll.

   The hooks and NVN take a function as they do in the recompiled code: they put a handler in its dispatch slot
   (g_disp). Native code does not read the slots, so the function's first word branches to an outcall
   (nce_slot_taken), which runs what the slot holds. An empty slot is the function's own native code (nce_slot) */
#ifndef SNX_NCE_H
#define SNX_NCE_H
#include "common/cpu/aarch64/cpu.h"

struct KThread;
struct SnxModule;

/* whether the game is of an NCE recompile on a host that runs it natively: its .nce file is there. mem_reserve asks
   it first, before the settings are read */
int nce_wanted(void);
/* after the executables are loaded, before the hooks: the setting nce, the pools and the patches. The game's code
   runs natively from here when nce_on() */
void nce_init(void);
int nce_on(void);
/* thread_run's loop under NCE: t runs from pc with the state of its CPU, until its entry function returns */
void nce_thread_run(struct KThread* t, u64 pc);
/* t's CPU.poll was set: guest code that t runs stops at once and calls RtAPI.poll. Any thread */
void nce_kick(struct KThread* t);
/* the code of a dispatch slot: f, or under NCE the function's own native code for an empty slot */
GuestFn nce_slot(GuestFn f);
/* a handler took the dispatch slot of the function at addr: under NCE, its first word now branches to the runtime, and
   the log counts those calls by family (a hook's family, or NVN's object's) */
void nce_slot_taken(u64 addr, const char* family);
/* code that the game mapped (an NRO of ldr:ro), with text_size bytes of text at base: under NCE, its words are
   patched as Eden patches them, each word of the encodings that the flow does not prove to be data */
void nce_code_added(u64 base, u64 text_size);
/* the game module's descriptor as the runtime uses it: m, or a copy whose guest addresses (each module's base, the
   dispatch entries, the code range, the functions) moved up with the layout (mem_moved) */
const struct SnxModule* nce_module(const struct SnxModule* m);
/* the crash handler's signal context: the guest's registers into the crashing thread's CPU, when it ran guest code */
void nce_crash_context(const void* context);
#endif
