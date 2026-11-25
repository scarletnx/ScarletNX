/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The gpu_hook_trace setting (docs/platforms/nx/hooks.md, "Finding a game's passes"): an NVN game's draws and
   dispatches logged as its queues replay them, to find the passes a hook module replaces and the game's code that
   records them. Each dispatch with its program's hash, code, call site in main, grid, block and its packed c[0]'s
   buffers and images, each draw with its fragment program's hash, targets and viewport (and constant buffer words,
   gpu_hook_trace_words), and the records between them that aren't state binds or barriers. gpu_hook_trace_from lets
   that many presents go by first. Off (the default), the replay tests one flag per record and calls nothing here */
#ifndef SNX_NVN_TRACE_H
#define SNX_NVN_TRACE_H
#include "nx/gpu/nvn/nvn_draw.h"
#include "nx/gpu/gpu.h"

/* whether the trace logs now: lines left, and the presents it waits for went by. The GPU thread's alone */
extern int g_nvn_trace_live;
/* the settings, once, when the game makes its first queue (nvn_replay.c). nvn_trace_arm reads them again, for the
   tests */
void nvn_trace_start(void);
void nvn_trace_arm(void);
/* a record d's queue replays, when g_nvn_trace_live: a draw or dispatch logged, another record noted */
void nvn_trace_record(NvnDraw* d, u32 op, u64 payload);
/* a wait or the end of a submission, marked in the log, when g_nvn_trace_live */
void nvn_trace_flush(const NvnDraw* d);
/* a present replayed (every queue's, in order): counted for gpu_hook_trace_from, and marked while the trace logs */
void nvn_trace_present(void);

/* the hash a trace line names a program by: FNV-1a 64 of its code, bytes of it from code (a guest address). A
   dispatch's code is the compute program's whole code where the record says its size (an Ounce program's), else its
   first NVN_TRACE_CODE bytes, as a draw's fragment program's always is. 0 when its first page is unmapped */
enum { NVN_TRACE_CODE = 1024 };
u64 nvn_trace_program_hash(u64 code, u32 bytes);
/* The image a dispatch's handle names, as the color target the renderer would draw it to (Surf2D, nvn_tic_surface):
   the 32-bit handle at byte off of its constant buffer slot (slot 0 off 0x40: the word at 0x40 of an Ounce program's
   packed c[0]), its texture ID (bits 0-19) an entry of the dispatch's texture pool. 1, or 0 (out zeroed) when the
   handle lies outside the buffer, its ID outside the pool, or the entry isn't one 2D color image a target takes */
int nvn_trace_image(const DispatchState* x, u32 slot, u32 off, Surf2D* out);
#endif
