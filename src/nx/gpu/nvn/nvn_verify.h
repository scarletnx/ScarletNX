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
#include "nx/gpu/gpu.h" /* PbState */
#include "common/filesystem/host.h"

struct GpuChannel;

/* ---- the file's state, g_nvn.verify (gpu/nvn/nvn_state.h) */
#define VF_FNS        1024 /* functions, a power of two above the ~530 NVN has */
#define VF_QUEUES     16
#define VF_DIFF_KEYS  256
#define VF_RANGES     8192
#define VF_PENDING    64
#define VF_LEND_MAX   16384
#define VF_REC_MAX    16384
#define VF_REC_PIECES 15
#define VF_CMD_POOLS  256
typedef struct VfKinds VfKinds; /* a function's argument kinds (tools/nvn_verify_kinds.py) */
typedef struct VfState VfState; /* the driver's bytes for a state nvn_direct's bytes stand for */
typedef struct VfBatch VfBatch; /* a shadow channel's words after one live submission */
/* an NVN function the game resolved */
typedef struct {
    u64 addr;
    GuestFn handler, original;
    const VfKinds* k;
    u32 flags;
    u64 calls, skipped, differed, ran, failed;
    const char* why; /* the first reason a call was skipped */
} VfFn;
typedef struct { /* u64 to u64, open addressing (key 0 free, ~0 a removed one) */
    u64* key;
    u64* val;
    u32 cap, used;
} VfMap;
/* the shadow range of the live memory [live, live + size) */
typedef struct {
    u64 live, shadow, size;
} VfRange;
/* a live draw waiting for the shadow's */
typedef struct {
    DsState ds;
    DrawCall dc;
    u32 serial;
} VfPending;
typedef struct {
    /* the game's side, under g_nvn.verify.lock */
    VfBatch *head, *tail;
    u32 shadow_serial; /* the live submissions so far, which the batches are tagged with */
    u64 words;         /* in the batches not decoded */
    PbState rel_pb;    /* the releases made at once (releases): the stream's decode state, */
    u32 rel_class[8];  /* the subchannels' classes, */
    u32 rel_sem[3];    /* the puller's semaphore address and payload, */
    u32 rel_rep[2][3]; /* the 3D and compute engines' report semaphore address and payload, */
    int rel_lost;      /* and whether the stream went out of step */
    /* the GPU thread's */
    struct GpuChannel* ch;
    VfBatch* cur; /* the batch decoding, from its word at */
    u32 at;
    u32 live_serial, draw; /* the submission replaying, and its draws so far */
    u32 batch_serial;      /* the serial of the words decoding */
    VfPending pend[VF_PENDING];
    u32 npend;
    u64 compared, matched, live_only, shadow_only;
} VfQueue;
typedef struct {
    const char* what;
    u64 n;
} VfDiffKey;
/* a piece of the shadow's command or control memory */
typedef struct {
    u64 at; /* the pool offset (command) or the guest address (control) */
    u32 size, kind, refs;
    u32 next;  /* the next free or cooling piece, 1 up */
    u32 freed; /* the present it was free at (cooling) */
} VfPiece;
typedef struct { /* the pieces a recording has records in, 1 up */
    u16 p[VF_REC_PIECES], n;
} VfRec;
/* the shadow's command and control memory */
typedef struct {
    u32 state;         /* 0 not made, 1 making, 2 made, 3 failed */
    u64 pool, area[2]; /* the shadow memory pool of the command pieces; the two areas (the pool's storage first) */
    u64 used[2];       /* of each area, in pieces made (a piece is never given back to its area) */
    u64 callback;      /* the guest address of the shadow's memory callback */
    VfPiece piece[VF_LEND_MAX];
    u32 n, nrec, rec_free;
    u32 free[2][32];               /* the free pieces of each kind and size (log2), 1 up */
    u32 cool, cool_tail, presents; /* the pieces free since fewer than COOL presents, oldest first, 1 up */
    VfRec rec[VF_REC_MAX];         /* a free one's n is the next free one, 1 up */
    VfMap cb, handle; /* shadow command buffer to its pieces (CB_*); nvn_direct's handle to its recording, 1 up */
} VfLend;
/* a pool the game takes command memory from, and its scaled shadow (nvn_verify.c's cmd_pool) */
typedef struct {
    u64 live, pool, storage, size; /* the game's pool, the scaled one, its storage, the game's size */
} VfCmdPool;
typedef struct {
    PMutex lock;
    VfFn fn[VF_FNS];
    VfMap obj, handle;
    VfRange mem[VF_RANGES]; /* append only: the GPU thread reads them without the lock, up to nmem */
    u32 nmem;
    u64 device, live_device; /* the last device made: the shadow's, and the game's */
    int swept;               /* storage_sweep ran */
    VfState* state;          /* STATE_SLOTS, by the hash of nvn_direct's bytes */
    VfState* astate; /* STATE_SLOTS, by live address: the state made at that address, with its live bytes then */
    u64 slab, slab_used;
    u64 free_list[16][64]; /* freed objects by size class (a few sizes): small stacks */
    u32 free_n[16], free_size[16];
    u8 fd_queue[256]; /* nvdrv fd: 0 not the shadow's, else 1 + the replay queue (0x80 the shadow's, no queue) */
    VfQueue q[VF_QUEUES];
    u32 cb_key_bits;
    u64* cb_key; /* the shadow's constant buffer words: GPU address of a word, and its value */
    u32* cb_val;
    VfDiffKey diff[VF_DIFF_KEYS];
    u32 ndiff;
    u64 lossy, unknown_state, unknown_obj, texreg_diff;
    int on;
    VfLend lend;       /* under lock, but state and what is written before state is 2 (pool, area, callback) */
    VfQueue* decoding; /* the queue whose channel is decoding (the GPU thread's) */
    struct {
        PMutex lock; /* the pools are made outside the lock above: making one maps guest memory */
        VfCmdPool p[VF_CMD_POOLS];
        u32 n;
    } cmd;
} NvnVerifyState;
/* the file's thread state, t_nvn.verify (gpu/nvn/nvn_state.h) */
typedef struct {
    int shadow;          /* in a shadow call: slots run the driver's code */
    u32 queue;           /* the replay queue (1 up) of the shadow call's queue, 0 none */
    const char* running; /* the function the shadow call runs */
    u64 stack;           /* the top of the thread's shadow stack (shadow_stack) */
} NvnVerifyThread;

/* the setting, once nvn_direct runs the game's NVN calls */
int nvn_verify_on(void);
/* nvn_direct took the function name (len characters) at addr for handler, where the slot held original (the driver's code): what the
   slot holds from now on. One nvn_direct doesn't implement keeps its handler, and the shadow can still call the
   driver's code */
GuestFn nvn_verify_take(u64 addr, const char* name, u32 len, GuestFn handler, GuestFn original, int implemented);
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
