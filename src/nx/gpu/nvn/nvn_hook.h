/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* GPU hooks at the NVN replay (docs/platforms/nx/hooks.md, "Modules"): a hook module's chains of draws and dispatches,
   recognised as a queue's records replay, the steps it skips held back, and the chain's last dispatch replaced by the
   module's handler (an upscale through renderer.h's upscale, which names no technology). When the chain breaks, or the
   handler says no, what was held back runs first, in the order recorded: the game's own passes are the fallback,
   exactly as recorded. With no module applied every record costs one test */
#ifndef SNX_NVN_HOOK_H
#define SNX_NVN_HOOK_H
#include "nx/gpu/nvn/nvn_draw.h"
#include "nx/gpu/gpu.h"

enum { NVN_HOOK_DISPATCH = 1, NVN_HOOK_DRAW = 2 };
/* OBSERVE runs as recorded, its state kept for the handler. SKIP is held back, then dropped when the chain is
   replaced. ANCHOR is the chain's last step, a dispatch, which the handler replaces. Every SKIP step comes after every
   OBSERVE step: from the first SKIP on, only state binds and barriers may come between two steps */
enum { NVN_HOOK_OBSERVE, NVN_HOOK_SKIP, NVN_HOOK_ANCHOR };
enum { NVN_HOOK_STEPS = 8, NVN_HOOK_HELD = 16 };

/* A step: what a draw or dispatch must be, each field 0 for any. program: nvn_hook_program_hash of the dispatch's
   compute program, or of the draw's fragment program. caller: a dispatch's call site in the game, as an offset from its
   main module's base. block: a dispatch's block size. format: a draw's color target 0 format (the render target code,
   DsSurf.format) */
typedef struct {
    u8 kind, role;
    u64 program, caller;
    u32 block[3];
    u32 format;
} NvnHookStep;

/* what a chain's steps were when it reached its anchor, for the handler */
typedef struct NvnHookChain NvnHookChain;
typedef struct {
    const NvnHookChain* chain;
    NvnDraw* d; /* the queue's state at the anchor */
    /* for each step: a dispatch's state (what the renderer's dispatch gets) and caller (an offset from main), or a
       draw's color target 0 and depth target as the 2D engine would address them (Surf2D, gpu.h), depth zero when
       none */
    DispatchState dispatch[NVN_HOOK_STEPS];
    u64 caller[NVN_HOOK_STEPS];
    Surf2D color[NVN_HOOK_STEPS], depth[NVN_HOOK_STEPS];
} NvnHookMatch;

struct NvnHookChain {
    const char* name;
    const NvnHookStep* steps;
    u32 nsteps; /* at most NVN_HOOK_STEPS, the last the ANCHOR */
    /* at the anchor: 1 when the handler recorded the replacement (the skipped steps and the anchor never run), 0 for
       the game's own passes */
    int (*replace)(const NvnHookMatch* m);
};

/* A handler's upscale: u through the renderer's (renderer.h's upscale), as a module's (GPU_UPSCALE_MODULE): 1 when the
   renderer wrote the output, 0 when no upscaler the module takes can run, or there is no renderer */
int nvn_hook_upscale(GpuUpscale* u);

/* the hash a step names a program by: FNV-1a 64 of its code, bytes of it from code (a guest address). A dispatch's
   code is the compute program's whole code where the record says its size (an Ounce program), else its first
   NVN_HOOK_CODE bytes, as a draw's fragment program's always is. The gpu_hook_trace setting logs it */
enum { NVN_HOOK_CODE = 1024 };
u64 nvn_hook_program_hash(u64 code, u32 bytes);

/* On the GPU thread, before the queue's other handlers (nvn_replay.c's replay): 1 when the hooks took the record (held
   it back, or replaced it), 0 when it replays as usual. A record that breaks a chain first runs what was held back */
int nvn_hook_record(NvnDraw* d, u32 op, u64 payload, u32 size);
/* what is held back runs now, its chain starting again (the others' OBSERVE steps carry over): at a wait, and at the
   end of a submission */
void nvn_hook_flush(NvnDraw* d);
/* the queue is gone */
void nvn_hook_forget(const NvnDraw* d);
/* the chains of the modules that apply (hle/hook/module.c, after hook_install chose them): nvn_hook_start takes them
   once, when the game makes its first queue, and nvn_hook_arm takes them again, for the tests */
void nvn_hook_start(void);
void nvn_hook_arm(void);
#endif
