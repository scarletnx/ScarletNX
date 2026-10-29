/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_verify=1: nvn_direct checked against the game's own NVN driver, draw by draw, while the game runs
   (gpu/nvn/nvn_verify.c, docs/NVN_HLE.md). Each NVN call runs nvn_direct's handler on the game's objects, then the
   driver's code on shadow objects of its own. The driver's submissions go to a GPU channel that only decodes, and each
   draw nvn_direct replays is compared with the draw state the 3D engine's registers give for the driver's */
#ifndef SNX_NVN_VERIFY_H
#define SNX_NVN_VERIFY_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/gpu/draw_state.h"

struct GpuChannel;

/* the setting, once nvn_direct runs the game's NVN calls */
int nvn_verify_on(void);
/* nvn_direct took the function name at addr for handler, where the slot held original (the driver's code): what the
   slot holds from now on. One nvn_direct doesn't implement keeps its handler, and the shadow can still call the
   driver's code */
GuestFn nvn_verify_take(u64 addr, const char* name, GuestFn handler, GuestFn original, int implemented);
/* whether fn is the adapter nvn_verify_take returns */
int nvn_verify_ours(GuestFn fn);

/* nvdrv: the calling thread runs the driver's code for the shadow (its fds are the shadow's), an fd opened (1) or
   closed (0), whether an fd is the shadow's, and a shadow channel's GPFIFO entries, copied for the decode */
int nvn_verify_shadow_thread(void);
/* the NVN function the calling thread's shadow call runs, NULL outside one (svcBreak and crash reports name it) */
const char* nvn_verify_running(void);
void nvn_verify_fd(u32 fd, int open);
int nvn_verify_fd_is(u32 fd);
void nvn_verify_gpfifo(u32 fd, const u8* entries, u32 n);
/* nvn_direct takes back the command memory [memory, memory + size) it lent: the recordings with handles there are gone */
void nvn_verify_memory_back(u64 memory, u64 size);

/* On the GPU thread: nvn_direct's replay starts a submission of queue (its 1-up index), and draws */
void nvn_verify_replay_submit(u32 queue);
void nvn_verify_live_draw(const DsState* ds, const DrawCall* dc);
/* the 3D engine's draw and constant buffer data on a verify channel (gpu.h gpu_channel_verify) */
void nvn_verify_shadow_draw(struct GpuChannel* ch, const DrawCall* dc);
void nvn_verify_shadow_cb(u64 gva, const u32* words, u32 n);

#endif
