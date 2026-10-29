/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_verify=1: nvn_direct against the game's own NVN driver, draw by draw (docs/NVN_HLE.md, "Verify mode").

   The game side, on the game's threads. Every NVN function the game resolves runs verify_fn: nvn_direct's handler
   first, on the game's objects and memory as always, then the driver's own code on shadows, on the game's thread.
   The shadows are guest memory of the runtime's (shadow_alloc, outside every region the game knows):
   - an object the game keeps at one address (a device, queue, pool, buffer, texture, sampler, program, sync...) has a
     shadow from its Initialize to its Finalize (g_v.obj, live address to shadow address);
   - a builder or state object the game may copy by value has its shadow by content: the driver's bytes for each
     state nvn_direct's bytes stand for (g_v.state), learned as the game calls the setters;
   - memory the driver writes (a pool's storage, queue memory) has a shadow range of its size (g_v.mem), and its
     command and control memory are the runtime's (Lend, cmd_pool);
   - a command handle the driver's EndRecording answered stands for nvn_direct's (g_v.handle).
   An argument the shadow has no counterpart for skips the driver's call (counted per function). The driver's calls
   that act on the game or the host are never made: waits, the window and its presents (a present flushes the shadow
   queue instead), the game's callbacks. The driver's calls into other NVN functions go through their slots and so
   through verify_fn, which runs the driver's code directly while the thread is in a shadow call (s_tls.t_shadow). A shadow
   call runs on a stack of the runtime's (shadow_stack).

   The driver's queue opens its GPU channel from such a call, so nvdrv marks the fd (nvn_verify_fd) and gives its
   GPFIFO submissions here instead of to the GPU thread: their words are copied (nvn_verify_gpfifo) into the queue's
   batches, each tagged with the number of the game's submission it follows, and their semaphore releases are made
   at once (releases). A submission is flushed in the shadow at once, so each live submission has its own batch.

   The GPU thread side. nvn_direct's replay of a submission (nvn_verify_replay_submit) and each of its draws
   (nvn_verify_live_draw) pull the shadow's batches through a verify channel (gpu.h gpu_channel_verify): the Maxwell
   decoder with its registers and macros and nothing else, whose draws come back through nvn_verify_shadow_draw with
   the DsState the 3D engine's registers give (maxwell/draw_state.c). The two draws of the same submission, in order,
   are compared group by group; GPU addresses are compared as the guest bytes they name, a shadow range standing for
   the live memory it shadows. Each difference is logged once, with its count at each power of 4 */
#include "nx/gpu/nvn/nvn_verify.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/maxwell/maxwell_3d_regs.h"
#include "nx/gpu/nvn/nvn_direct.h"
#include "nx/gpu/nvn/nvn.h"
#include "nx/gpu/nvn/nvn_object.h"
#include "gpu/nvn/nvn.hook.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the kinds of each function's arguments (tools/nvn_verify_kinds.py) */
enum { NVK_RET_NONE, NVK_RET_ADDR, NVK_RET_BOOL, NVK_RET_X32, NVK_RET_X64, NVK_RET_F32 };
#define NVK_ARGS 8
typedef struct {
    const char* name;
    const char* kinds; /* NULL: no prototype, and the shadow doesn't run it (each nvn.hook function has one) */
    int ret;
    u32 size[NVK_ARGS];
} Kinds;
static const Kinds g_kinds[] = {
#include "nx/gpu/nvn/nvn_verify_kinds.inc"
};

/* ---- the file's state */
#define FNS        1024 /* functions, a power of two above the ~530 NVN has */
#define QUEUES     16
#define STATE_MAX  0x180 /* the largest state object or builder (NVNtextureBuilder's 0x80, NVNqueueBuilder's...) */
#define SCRATCH    0x2000 /* guest stack the shadow call's copies take below the game's stack pointer */
#define OBJ_SLAB   (256ull << 20)
#define STATE_SLOTS 0x8000
#define CB_SLOTS   (1u << 20)
#define DIFF_KEYS  256
#define RANGES     8192

enum { /* what a function is to the shadow, from its name */
    F_INIT = 1,      /* arg 0 is the object it makes */
    F_FINAL = 2,     /* arg 0 is the object it ends */
    F_DEFAULTS = 4,  /* arg 0 is a builder or state it sets to its defaults */
    F_SKIP = 8,      /* never run in the shadow */
    F_SUBMIT = 16,   /* nvnQueueSubmitCommands */
    F_PRESENT = 32,  /* nvnQueuePresentTexture: the shadow queue flushes instead */
    F_END = 64,      /* nvnCommandBufferEndRecording: its handle */
    F_HANDLES = 128, /* arg 2 is an array of command handles, as many as arg 1 */
    F_QUEUE = 256,   /* arg 0 is a queue: an fd opened in its call is that queue's channel */
    F_CMD = 512,     /* arg 0 is a command buffer */
    F_TEXREG = 1024, /* nvnTexturePoolRegisterTexture: the descriptors compared */
    F_NORESULT = 2048, /* its result is an address or a time, which the two sides make apart */
    F_STATESET = 4096, /* a state object's setter: a state never seen is the zeroed memory the game started from */
    F_ADDCMD = 8192,   /* nvnCommandBufferAddCommandMemory: the command buffer writes in the game's memory, or a piece */
    F_ADDCTRL = 16384, /* nvnCommandBufferAddControlMemory: the command buffer writes in the game's memory */
    F_BEGIN = 32768,   /* nvnCommandBufferBeginRecording: a recording, which may have records in lent pieces */
    F_CALLS = 65536    /* nvnCommandBufferCallCommands: the recording points at the callees' commands */
};
typedef struct {
    u64 addr;
    GuestFn handler, original;
    const Kinds* k;
    u32 flags;
    u64 calls, skipped, differed, ran, failed;
    const char* why; /* the first reason a call was skipped */
} Fn;

typedef struct { /* u64 to u64, open addressing (key 0 free, ~0 a removed one) */
    u64* key;
    u64* val;
    u32 cap, used;
} Map;

typedef struct {
    u64 live, shadow, size;
} Range;

typedef struct {
    u32 len, filled;
    u64 hash, addr; /* addr: the live address (g_v.astate), 0 in the content map */
    u8 live[STATE_MAX], drv[STATE_MAX];
} State;

/* a shadow channel's words after one live submission */
typedef struct Batch {
    struct Batch* next;
    u32 serial, n;
    u32 w[];
} Batch;
typedef struct {
    DsState ds;
    DrawCall dc;
    u32 serial;
} Pending;
#define PENDING 64
typedef struct {
    /* the game's side, under g_v.lock */
    Batch *head, *tail;
    u32 shadow_serial; /* the live submissions so far, which the batches are tagged with */
    u64 words;         /* in the batches not decoded */
    PbState rel_pb;    /* the releases made at once (releases): the stream's decode state, */
    u32 rel_class[8];  /* the subchannels' classes, */
    u32 rel_sem[3];    /* the puller's semaphore address and payload, */
    u32 rel_rep[2][3]; /* the 3D and compute engines' report semaphore address and payload, */
    int rel_lost;      /* and whether the stream went out of step */
    /* the GPU thread's */
    GpuChannel* ch;
    Batch* cur;            /* the batch decoding, from its word at */
    u32 at;
    u32 live_serial, draw; /* the submission replaying, and its draws so far */
    u32 batch_serial;      /* the serial of the words decoding */
    Pending pend[PENDING];
    u32 npend;
    u64 compared, matched, live_only, shadow_only;
} VQueue;

typedef struct {
    const char* what;
    u64 n;
} DiffKey;

enum { LEND_CMD, LEND_CTRL };
#define LEND_MAX   16384
#define REC_MAX    16384
#define REC_PIECES 15
#define PINNED     0xFFFFFFFFu /* refs of a piece a recording took more than REC_PIECES of: never free */
#define COOL       4           /* the presents a piece stays unused after it is free (lend_added) */
static const u64 lend_area[2] = {2ull << 30, 2ull << 30};
static const u32 lend_min[2] = {64u << 10, 64u << 10};
typedef struct {
    u64 at; /* the pool offset (command) or the guest address (control) */
    u32 size, kind, refs;
    u32 next;  /* the next free or cooling piece, 1 up */
    u32 freed; /* the present it was free at (cooling) */
} Piece;
typedef struct { /* the pieces a recording has records in, 1 up */
    u16 p[REC_PIECES], n;
} Rec;
/* a shadow command buffer's pieces, packed in a map value: the one it writes in of each kind (1 up), the recording
   it makes (1 up), and whether it is recording */
#define CB_CUR(v, k) ((u32)((v) >> (16 * (k))) & 0xFFFF)
#define CB_REC(v)    ((u32)((v) >> 32) & 0xFFFF)
#define CB_RECORDING (1ull << 48)
typedef struct {
    u32 state;         /* 0 not made, 1 making, 2 made, 3 failed */
    u64 pool, area[2]; /* the shadow memory pool of the command pieces; the two areas (the pool's storage first) */
    u64 used[2];       /* of each area, in pieces made (a piece is never given back to its area) */
    u64 callback;      /* the guest address of the shadow's memory callback */
    Piece piece[LEND_MAX];
    u32 n, nrec, rec_free;
    u32 free[2][32];               /* the free pieces of each kind and size (log2), 1 up */
    u32 cool, cool_tail, presents; /* the pieces free since fewer than COOL presents, oldest first, 1 up */
    Rec rec[REC_MAX];              /* a free one's n is the next free one, 1 up */
    Map cb, handle; /* shadow command buffer to its pieces (CB_*); nvn_direct's handle to its recording, 1 up */
} Lend;
typedef struct {
    PMutex lock;
    Fn fn[FNS];
    Map obj, handle;
    Range mem[RANGES]; /* append only: the GPU thread reads them without the lock, up to nmem */
    u32 nmem;
    u64 device, live_device; /* the last device made: the shadow's, and the game's */
    int swept;                /* storage_sweep ran */
    State* state;  /* STATE_SLOTS, by the hash of nvn_direct's bytes */
    State* astate; /* STATE_SLOTS, by live address: the state made at that address, with its live bytes then */
    u64 slab, slab_used;
    u64 free_list[16][64]; /* freed objects by size class (a few sizes): small stacks */
    u32 free_n[16], free_size[16];
    u8 fd_queue[256]; /* nvdrv fd: 0 not the shadow's, else 1 + the replay queue (0x80 the shadow's, no queue) */
    VQueue q[QUEUES];
    u32 cb_key_bits;
    u64* cb_key; /* the shadow's constant buffer words: GPU address of a word, and its value */
    u32* cb_val;
    DiffKey diff[DIFF_KEYS];
    u32 ndiff;
    u64 lossy, unknown_state, unknown_obj, texreg_diff;
    int on;
    Lend lend;        /* under lock, but state and what is written before state is 2 (pool, area, callback) */
    VQueue* decoding; /* the queue whose channel is decoding (the GPU thread's) */
} Verify;
static Verify g_v = {PMUTEX_INIT};
static _Thread_local struct {
    int t_shadow;          /* in a shadow call: slots run the driver's code */
    u32 t_queue;           /* the replay queue (1 up) of the shadow call's queue, 0 none */
    const char* t_running; /* the function the shadow call runs */
    u64 t_stack;           /* the top of the thread's shadow stack (shadow_stack) */
} s_tls;
const char* nvn_verify_running(void) { return s_tls.t_running; }

int nvn_verify_on(void) { return g_v.on; }
int nvn_verify_shadow_thread(void) { return s_tls.t_shadow; }

/* ---- maps */
static u64 mix(u64 k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdull;
    k ^= k >> 33;
    return k;
}
#define MAP_GONE (~0ull)
static void map_put(Map* m, u64 key, u64 val);
static void map_grow(Map* m) {
    Map old = *m;
    u32 i;
    m->cap = old.cap ? old.cap * 2 : 1024;
    m->used = 0;
    m->key = (u64*)calloc(m->cap, sizeof *m->key);
    m->val = (u64*)calloc(m->cap, sizeof *m->val);
    if (!m->key || !m->val) fatal("nvn verify: out of memory for a map of %u", m->cap);
    for (i = 0; i < old.cap; i++)
        if (old.key[i] && old.key[i] != MAP_GONE) map_put(m, old.key[i], old.val[i]);
    free(old.key);
    free(old.val);
}
static u64* map_slot(const Map* m, u64 key) {
    u32 i, n;
    if (!m->cap) return NULL;
    for (n = 0, i = (u32)mix(key) & (m->cap - 1); n < m->cap; n++, i = (i + 1) & (m->cap - 1)) {
        if (m->key[i] == key) return &m->val[i];
        if (!m->key[i]) return NULL;
    }
    return NULL;
}
static u64 map_get(const Map* m, u64 key) {
    const u64* v = map_slot(m, key);
    return v ? *v : 0;
}
static void map_put(Map* m, u64 key, u64 val) {
    u32 i;
    u64* v = map_slot(m, key);
    if (v) {
        *v = val;
        return;
    }
    if ((m->used + 1) * 2 > m->cap) map_grow(m);
    for (i = (u32)mix(key) & (m->cap - 1); m->key[i] && m->key[i] != MAP_GONE; i = (i + 1) & (m->cap - 1)) {
    }
    m->key[i] = key;
    m->val[i] = val;
    m->used++;
}
static void map_del(Map* m, u64 key) {
    u64* v = map_slot(m, key);
    if (v) m->key[v - m->val] = MAP_GONE;
}

/* ---- the shadow's guest memory: outside every region the game maps in */
static u64 shadow_map(u64 size) {
    /* the runs no region covers: past the kernel's pages, past the stack region, below the first region */
    const u64 lo[3] = {g_layout.kern_end, g_layout.stack + g_layout.stack_size, g_layout.base};
    const u64 hi[3] = {g_layout.end, g_layout.kern, g_layout.alias < g_layout.heap ? g_layout.alias : g_layout.heap};
    u64 a = 0;
    int i, r = -1;
    size = (size + 0xFFFF) & ~0xFFFFull;
    for (i = 0; i < 3 && r; i++) /* another thread may map the run found first: look again */
        while (lo[i] < hi[i] && (a = mem_find_free(lo[i], hi[i], size, 0x10000)) != 0 &&
               (r = mem_map(a, size, MS_NORMAL, PERM_RW)) != 0) {
        }
    if (r)
        fatal("nvn verify: no guest memory for %llu KiB of shadow (heap %llx+%llx alias %llx+%llx stack %llx+%llx "
              "kernel %llx-%llx end %llx)",
              (unsigned long long)(size >> 10), (unsigned long long)g_layout.heap,
              (unsigned long long)g_layout.heap_size, (unsigned long long)g_layout.alias,
              (unsigned long long)g_layout.alias_size, (unsigned long long)g_layout.stack,
              (unsigned long long)g_layout.stack_size, (unsigned long long)g_layout.kern,
              (unsigned long long)g_layout.kern_end, (unsigned long long)g_layout.end);
    return a;
}
/* a shadow object of size bytes, zeroed. Without the lock: mapping memory takes the renderer's, which the GPU thread
   may hold while it takes this file's */
static u64 obj_alloc(u32 size) {
    u32 c;
    u64 a = 0, slab = 0;
    size = (size + 15) & ~15u;
    for (;;) {
        pm_lock(&g_v.lock);
        for (c = 0; c < 16 && g_v.free_size[c] && g_v.free_size[c] != size; c++) {
        }
        if (c < 16 && g_v.free_size[c] == size && g_v.free_n[c])
            a = g_v.free_list[c][--g_v.free_n[c]];
        else {
            if (slab && (!g_v.slab || g_v.slab_used + size > OBJ_SLAB)) {
                g_v.slab = slab;
                g_v.slab_used = 0;
                slab = 0;
            }
            if (g_v.slab && g_v.slab_used + size <= OBJ_SLAB) {
                a = g_v.slab + g_v.slab_used;
                g_v.slab_used += size;
            }
        }
        pm_unlock(&g_v.lock);
        if (a) break;
        slab = shadow_map(OBJ_SLAB);
    }
    memset(GP(a), 0, size);
    return a;
}
static void obj_free(u64 a, u32 size) {
    u32 c;
    size = (size + 15) & ~15u;
    for (c = 0; c < 16 && g_v.free_size[c] && g_v.free_size[c] != size; c++) {
    }
    if (c == 16) return; /* a 17th size: left */
    g_v.free_size[c] = size;
    if (g_v.free_n[c] < 64) g_v.free_list[c][g_v.free_n[c]++] = a;
}
/* the shadow range for the live memory [live, live + size), made at its first use. Without the lock (obj_alloc) */
static u64 mem_find(u64 live, u64 size) {
    u32 i, n = __atomic_load_n(&g_v.nmem, __ATOMIC_ACQUIRE);
    for (i = n; i-- > 0;)
        if (g_v.mem[i].live == live && g_v.mem[i].size >= size) return g_v.mem[i].shadow;
    return 0;
}
static u64 mem_shadow(u64 live, u64 size) {
    u64 s = mem_find(live, size), m;
    if (s) return s;
    m = shadow_map(size);
    pm_lock(&g_v.lock);
    s = mem_find(live, size);
    if (!s && g_v.nmem < RANGES) {
        Range* r = &g_v.mem[g_v.nmem];
        r->live = live;
        r->size = size;
        r->shadow = s = m;
        __atomic_store_n(&g_v.nmem, g_v.nmem + 1, __ATOMIC_RELEASE);
        m = 0;
    }
    pm_unlock(&g_v.lock);
    if (m) mem_unmap(m, (size + 0xFFFF) & ~0xFFFFull);
    if (!s) fatal("nvn verify: more than %u shadow memory ranges", RANGES);
    return s;
}
/* the live guest address a shadow range's address stands for, 0 when it is in none. From any thread, no lock */
static u64 mem_live(u64 shadow) {
    u32 i, n = __atomic_load_n(&g_v.nmem, __ATOMIC_ACQUIRE);
    for (i = 0; i < n; i++)
        if (shadow >= g_v.mem[i].shadow && shadow - g_v.mem[i].shadow < g_v.mem[i].size)
            return g_v.mem[i].live + (shadow - g_v.mem[i].shadow);
    return 0;
}

/* ---- states by content */
static u64 fnv(const u8* p, u32 n) {
    u64 h = 0xcbf29ce484222325ull;
    while (n--) h = (h ^ *p++) * 0x100000001b3ull;
    return h;
}
static State* state_find(const u8* live, u32 len, int make) {
    u64 h = fnv(live, len) ^ len;
    u32 i, n;
    for (n = 0, i = (u32)h & (STATE_SLOTS - 1); n < 64; n++, i = (i + 1) & (STATE_SLOTS - 1)) {
        State* s = &g_v.state[i];
        if (!s->len) {
            if (!make) return NULL;
            s->len = len;
            s->filled = 0;
            s->hash = h;
            memcpy(s->live, live, len);
            return s;
        }
        if (s->hash == h && s->len == len && !memcmp(s->live, live, len)) return s;
    }
    if (!make) return NULL;
    { /* full here: the slot it hashes to is taken over */
        State* s = &g_v.state[(u32)h & (STATE_SLOTS - 1)];
        s->len = len;
        s->filled = 0;
        s->hash = h;
        memcpy(s->live, live, len);
        return s;
    }
}

/* ---- functions */
static u32 fn_hash(u64 addr) { return (u32)((addr >> 2) * 2654435761u) & (FNS - 1); }
static Fn* fn_at(u64 addr) {
    u32 n, i;
    for (n = 0, i = fn_hash(addr); n < FNS; n++, i = (i + 1) & (FNS - 1)) {
        Fn* f = &g_v.fn[i];
        u64 a = __atomic_load_n(&f->addr, __ATOMIC_ACQUIRE);
        if (a == addr) return f;
        if (!a) return NULL;
    }
    return NULL;
}
static const Kinds* kinds_of(const char* name) {
    size_t lo = 0, hi = sizeof g_kinds / sizeof g_kinds[0];
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(g_kinds[mid].name, name);
        if (!c) return &g_kinds[mid];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}
static int ends(const char* s, const char* tail) {
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && !strcmp(s + a - b, tail);
}
static int starts(const char* s, const char* head) { return !strncmp(s, head, strlen(head)); }
static u32 flags_of(const char* n, const Kinds* k) {
    static const char* const skip[] = {
        "nvnQueueAcquireTexture", "nvnQueueFinish", "nvnSyncWait", "nvnDeviceInstallDebugCallback",
        "nvnCommandBufferSetMemoryCallback", "nvnCommandBufferSetMemoryCallbackData", "nvnEventWait",
        "nvnQueueGetError", "nvnMemoryPoolMapVirtual", "nvnDeviceSetIntermediateShaderCache",
        "nvnCommandBufferSetCommandMemoryCallbackEnabled", "nvnDeviceFinalizeCommandHandle",
        /* a CPU read of the texels of a region into the game's memory: nothing the GPU draws, and the shadow has no
           room of the region's size for them (the 0x100 bytes of an output argument) */
        "nvnTextureReadTexels"};
    u32 f = 0, i;
    if (k && k->kinds && k->kinds[0] && ends(n, "Initialize") && !strstr(n, "Builder")) f |= F_INIT;
    if (ends(n, "Finalize")) f |= F_FINAL;
    /* a builder's first calls: NVN sets its device before its defaults */
    if (ends(n, "SetDefaults") || (strstr(n, "Builder") && ends(n, "SetDevice"))) f |= F_DEFAULTS;
    if (starts(n, "nvnWindow")) f |= F_SKIP;
    if (ends(n, "State") == 0 && strstr(n, "StateSet") && !strstr(n, "Builder")) f |= F_STATESET;
    for (i = 0; i < sizeof skip / sizeof skip[0]; i++)
        if (!strcmp(n, skip[i])) f |= F_SKIP;
    if (!strcmp(n, "nvnQueueSubmitCommands")) f |= F_SUBMIT | F_HANDLES;
    if (!strcmp(n, "nvnCommandBufferCallCommands") || !strcmp(n, "nvnCommandBufferCopyCommands")) f |= F_HANDLES;
    if (!strcmp(n, "nvnQueuePresentTexture")) f |= F_PRESENT;
    if (!strcmp(n, "nvnCommandBufferAddCommandMemory")) f |= F_ADDCMD;
    if (!strcmp(n, "nvnCommandBufferAddControlMemory")) f |= F_ADDCTRL;
    if (!strcmp(n, "nvnCommandBufferEndRecording")) f |= F_END;
    if (!strcmp(n, "nvnCommandBufferBeginRecording")) f |= F_BEGIN;
    if (!strcmp(n, "nvnCommandBufferCallCommands")) f |= F_CALLS;
    if (!strcmp(n, "nvnTexturePoolRegisterTexture")) f |= F_TEXREG;
    if (starts(n, "nvnQueue") && !starts(n, "nvnQueueBuilder")) f |= F_QUEUE;
    if (starts(n, "nvnCommandBuffer")) f |= F_CMD;
    if (strstr(n, "Address") || ends(n, "Map") || strstr(n, "Timestamp") || strstr(n, "MemoryPool") ||
        strstr(n, "GetDevice") || strstr(n, "Callback") || strstr(n, "MemoryUsed") || strstr(n, "MemoryFree") ||
        ends(n, "GetMemory") || ends(n, "EndRecording") || strstr(n, "DebugLabel") || ends(n, "GetPool"))
        f |= F_NORESULT;
    if (k && k->kinds && strchr(k->kinds, 'f')) f |= F_SKIP;
    return f;
}

/* ---- running the driver's code */
/* The stack a shadow call runs on: guest memory of the runtime's for each thread, never the game's. On the game's
   stack the copies of the arguments and the driver's frames went below the game's stack pointer, where a thread with
   little stack left has another's memory, and a call that writes more than its argument copy holds wrote over the
   game's frames above it */
#define SHADOW_STACK (1u << 20)
static u64 shadow_stack(void) {
    if (!s_tls.t_stack) s_tls.t_stack = shadow_map(SHADOW_STACK) + SHADOW_STACK;
    return s_tls.t_stack;
}
#define REGS_SIZE offsetof(CPU, mem)
/* fn's code at entry on c, args already in c, until it returns to lr with the stack pointer sp */
static void run(CPU* c, GuestFn fn, u64 entry, u64 lr, u64 sp) {
    int outer = !s_tls.t_shadow;
    u32 flag = nvn_direct_flag();
    c->x[30] = lr;
    c->x[31] = sp;
    s_tls.t_shadow = 1;
    fn(c, entry);
    while (c->pc != lr || c->x[31] != sp) snx_lookup(c, c->pc)(c, c->pc);
    if (outer) s_tls.t_shadow = 0;
    if (nvn_direct_flag() != flag) { /* it would turn on the game's inlined command writes into nvn_direct's memory */
        nvn_direct_flag_set(flag);
        LOG_LIMIT(4, "nvn verify: the driver changed nvnPrivate0 in a shadow call: put back\n");
    }
}
/* the driver's code of the function name, 0 when it has none */
static u64 drv_addr(CPU* c, const char* name, GuestFn* fn) {
    u64 a = nvn_direct_lookup(c, name);
    const Fn* f;
    if (!a || a < g_code_lo || a >= g_code_hi || (a & 3)) return 0;
    f = fn_at(a);
    *fn = f ? f->original : g_disp[(a - g_code_lo) / 4];
    return *fn && !nvn_verify_ours(*fn) ? a : 0;
}
/* the driver's name(x0, x1, x2, x3) on c, from a shadow call's registers base: its x0 */
static u64 drv_call(CPU* c, const CPU* base, u64 sp, const char* name, u64 a0, u64 a1, u64 a2, u64 a3) {
    GuestFn fn = NULL;
    u64 at, lr = base->x[30];
    memcpy(c, base, REGS_SIZE);
    c->x[31] = sp; /* the lookup puts the name below it, past the shadow call's copies */
    at = drv_addr(c, name, &fn);
    if (!at) {
        LOG_LIMIT(8, "nvn verify: the driver has no %s for the shadow\n", name);
        return 0;
    }
    c->x[0] = a0, c->x[1] = a1, c->x[2] = a2, c->x[3] = a3;
    run(c, fn, at, lr, sp);
    return c->x[0];
}

/* the shadow queue's commands so far, sent as the game's submission serial's batch */
static void shadow_flush(CPU* c, const CPU* base, u64 sp, u64 queue) { drv_call(c, base, sp, "nvnQueueFlush", queue, 0, 0, 0); }

/* ---- the shadow driver's command and control memory
   The game places its command and control memory by what the driver takes: it places each recording by
   GetCommandMemoryUsed and GetControlMemoryUsed, and its memory callback adds more when the driver runs short. Under
   nvn_direct those answers are nvn_direct's: its records are other sizes than the driver's methods, and it keeps no
   control memory (GetControlMemoryUsed is 0). In shadows of the game's memory, offset for offset, the driver wrote
   recordings over each other: the game gave several command buffers, one after the other, the same control memory,
   which to it was still empty, and placed command memory after recordings the driver's methods made longer than
   nvn_direct's records. The recordings it still called were garbage: CallCommands, copying a callee's records,
   aborted "out of control memory with no out-of-memory callback", and the shadow's command streams went out of step.

   So the shadow's command memory is a shadow of the game's pool at CMD_SCALE times each offset (cmd_pool): each
   recording keeps the place the game gave it, with more room, and is reused when the game reuses its own. Its control
   memory is the runtime's, in pieces (lend_take): each AddControlMemory of the game adds a piece of at least its size
   in place of the game's memory. Command memory the shadow pool doesn't hold (a game can place it by an address it
   read, as an offset from its pool: one game's offsets put it in memory nvn_direct lent, gpu/nvn/nvn_cmd.c) is a
   piece too. Each shadow command buffer has this file's memory callback (shadow_memory): when the driver's own check
   finds too little, it adds a piece at least as big as the driver asks for.

   A piece is free again when no recording the game can still call or submit has records in it. Each piece counts its
   users (refs): the command buffer writing in it, each recording made in it, and each recording that calls one with
   commands in it, until the game records again at that recording's handle, or the memory the handle is in is reused
   (nvn_direct's handle is its first record's address: the game adds that command memory again, or nvn_direct takes
   back the memory it lent there). A game can still call a recording right after it adds the memory its handle is in
   again (one game records a recording, gives its command memory to another command buffer, and calls it from there:
   the driver kept all of it in control memory), so a free piece is taken again only after COOL presents */
static u32 log2_of(u64 pow2) { return 63u - (u32)__builtin_clzll(pow2); }

/* the areas and the pool, on the first shadow command buffer's device. Another thread waits for the one that makes
   them */
static int lend_ready(CPU* c, const CPU* base, u64 sp) {
    u32 st = 0;
    u64 dev = g_v.device, b;
    GuestFn fn;
    if (__atomic_compare_exchange_n(&g_v.lend.state, &st, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        g_v.lend.area[LEND_CMD] = shadow_map(lend_area[LEND_CMD]);
        g_v.lend.area[LEND_CTRL] = shadow_map(lend_area[LEND_CTRL]);
        g_v.lend.pool = obj_alloc(sizeof(NVNmemoryPool));
        b = obj_alloc(sizeof(NVNmemoryPoolBuilder));
        drv_call(c, base, sp, "nvnMemoryPoolBuilderSetDefaults", b, 0, 0, 0);
        drv_call(c, base, sp, "nvnMemoryPoolBuilderSetDevice", b, dev, 0, 0);
        drv_call(c, base, sp, "nvnMemoryPoolBuilderSetFlags", b,
                 NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED, 0, 0);
        drv_call(c, base, sp, "nvnMemoryPoolBuilderSetStorage", b, g_v.lend.area[LEND_CMD], lend_area[LEND_CMD], 0);
        /* the callback: the driver's SetMemoryCallback, which the shadow never runs, at an address whose slot is
           verify_fn (an event, 0 or 1, is never a function's address in x1) */
        g_v.lend.callback = drv_addr(c, "nvnCommandBufferSetMemoryCallback", &fn);
        if (!dev || !g_v.lend.callback || !fn_at(g_v.lend.callback) ||
            !(drv_call(c, base, sp, "nvnMemoryPoolInitialize", g_v.lend.pool, b, 0, 0) & 0xFF)) {
            WARN("nvn verify: no memory for the shadow driver when it runs short (device %llx, callback %llx): it "
                 "aborts then\n",
                 (unsigned long long)dev, (unsigned long long)g_v.lend.callback);
            st = 3;
        } else {
            LOG("nvn verify: the shadow driver's command memory: %llu MiB at %llx\n",
                (unsigned long long)(lend_area[LEND_CMD] >> 20), (unsigned long long)g_v.lend.area[LEND_CMD]);
            st = 2;
        }
        __atomic_store_n(&g_v.lend.state, st, __ATOMIC_RELEASE);
    }
    while ((st = __atomic_load_n(&g_v.lend.state, __ATOMIC_ACQUIRE)) == 1) plat_yield();
    return st == 2;
}

/* A pool the game takes command memory from has a shadow of CMD_SCALE times its size for that memory (cmd_pool), at
   CMD_SCALE times each offset: each recording keeps the place the game gives it, with CMD_SCALE times the room
   nvn_direct's records took there, and its memory is reused when the game reuses its own. The pool's own shadow,
   offset for offset, stays for the buffers and textures in it */
#define CMD_SCALE 4
#define CMD_POOLS 256
typedef struct {
    u64 live, pool, storage, size; /* the game's pool, the scaled one, its storage, the game's size */
} CmdPool;
static struct {
    PMutex lock; /* the pools are made outside g_v.lock: making one maps guest memory */
    CmdPool p[CMD_POOLS];
    u32 n;
} g_cmd = {PMUTEX_INIT};
/* the scaled pool for the live pool, made at its first use (0 when there is none) */
static u64 cmd_pool(CPU* c, const CPU* base, u64 sp, u64 live) {
    const NvnPool* lp = NVN_OBJ(const NvnPool, live);
    u64 pool = 0, b;
    u32 i;
    pm_lock(&g_cmd.lock);
    for (i = 0; i < g_cmd.n && g_cmd.p[i].live != live; i++) {
    }
    if (i < g_cmd.n)
        pool = g_cmd.p[i].pool;
    else if (g_cmd.n < CMD_POOLS && lp->size && g_v.device) {
        CmdPool* p = &g_cmd.p[g_cmd.n];
        p->live = live;
        p->size = lp->size;
        p->storage = shadow_map(lp->size * CMD_SCALE);
        p->pool = obj_alloc(sizeof(NVNmemoryPool));
        b = obj_alloc(sizeof(NVNmemoryPoolBuilder));
        drv_call(c, base, sp, "nvnMemoryPoolBuilderSetDefaults", b, 0, 0, 0);
        drv_call(c, base, sp, "nvnMemoryPoolBuilderSetDevice", b, g_v.device, 0, 0);
        drv_call(c, base, sp, "nvnMemoryPoolBuilderSetFlags", b,
                 NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED, 0, 0);
        drv_call(c, base, sp, "nvnMemoryPoolBuilderSetStorage", b, p->storage, lp->size * CMD_SCALE, 0);
        if (drv_call(c, base, sp, "nvnMemoryPoolInitialize", p->pool, b, 0, 0) & 0xFF) {
            pool = p->pool;
            g_cmd.n++;
        } else {
            WARN("nvn verify: the shadow's pool for the command memory of pool %llx didn't initialize\n",
                 (unsigned long long)live);
            mem_unmap(p->storage, (lp->size * CMD_SCALE + 0xFFFF) & ~0xFFFFull);
        }
        obj_free(b, sizeof(NVNmemoryPoolBuilder));
    }
    pm_unlock(&g_cmd.lock);
    return pool;
}
/* the game finalizes the live pool: so is its scaled one */
static void cmd_pool_final(CPU* c, const CPU* base, u64 sp, u64 live) {
    CmdPool p = {0};
    u32 i;
    pm_lock(&g_cmd.lock);
    for (i = 0; i < g_cmd.n && g_cmd.p[i].live != live; i++) {
    }
    if (i < g_cmd.n) {
        p = g_cmd.p[i];
        g_cmd.p[i] = g_cmd.p[--g_cmd.n];
    }
    pm_unlock(&g_cmd.lock);
    if (!p.pool) return;
    drv_call(c, base, sp, "nvnMemoryPoolFinalize", p.pool, 0, 0, 0);
    obj_free(p.pool, sizeof(NVNmemoryPool));
    mem_unmap(p.storage, (p.size * CMD_SCALE + 0xFFFF) & ~0xFFFFull);
}

/* the users of the pieces, under g_v.lock (i: 1 up) */
static void piece_unref(u32 i) {
    Piece* p = i ? &g_v.lend.piece[i - 1] : NULL;
    if (p && p->refs != PINNED && !--p->refs) {
        p->next = 0;
        p->freed = g_v.lend.presents;
        if (g_v.lend.cool_tail)
            g_v.lend.piece[g_v.lend.cool_tail - 1].next = i;
        else
            g_v.lend.cool = i;
        g_v.lend.cool_tail = i;
    }
}
/* the pieces free since COOL presents can be taken */
static void pieces_cooled(void) {
    while (g_v.lend.cool && g_v.lend.presents - g_v.lend.piece[g_v.lend.cool - 1].freed >= COOL) {
        u32 i = g_v.lend.cool;
        Piece* p = &g_v.lend.piece[i - 1];
        g_v.lend.cool = p->next;
        if (!g_v.lend.cool) g_v.lend.cool_tail = 0;
        p->next = g_v.lend.free[p->kind][log2_of(p->size)];
        g_v.lend.free[p->kind][log2_of(p->size)] = i;
    }
}
static void rec_drop(u32 r) { /* a recording no one can call */
    u32 j;
    if (!r) return;
    for (j = 0; j < g_v.lend.rec[r - 1].n; j++) piece_unref(g_v.lend.rec[r - 1].p[j]);
    g_v.lend.rec[r - 1].n = (u16)g_v.lend.rec_free;
    g_v.lend.rec_free = r;
}
/* piece i (1 up) has records of the recording r (1 up) */
static void rec_add(u32 r, u32 i) {
    Rec* rec = &g_v.lend.rec[r - 1];
    u32 j;
    if (!i) return;
    for (j = 0; j < rec->n; j++)
        if (rec->p[j] == i) return;
    if (rec->n < REC_PIECES) {
        rec->p[rec->n++] = (u16)i;
        if (g_v.lend.piece[i - 1].refs != PINNED) g_v.lend.piece[i - 1].refs++;
    } else {
        LOG_LIMIT(4, "nvn verify: a shadow recording has records in more than %u pieces of the runtime's memory: "
                     "they are never free again\n", REC_PIECES);
        g_v.lend.piece[i - 1].refs = PINNED;
    }
}
/* the recordings whose handles are in [memory, memory + size) are gone */
static void handles_gone(u64 memory, u64 size) {
    u32 i;
    for (i = 0; i < g_v.lend.handle.cap; i++) {
        u64 h = g_v.lend.handle.key[i];
        if (h && h != MAP_GONE && h - memory < size) {
            rec_drop((u32)g_v.lend.handle.val[i]);
            g_v.lend.handle.key[i] = MAP_GONE;
        }
    }
}
void nvn_verify_memory_back(u64 memory, u64 size) {
    if (!g_v.on) return;
    pm_lock(&g_v.lock);
    handles_gone(memory, size);
    pm_unlock(&g_v.lock);
}
/* the shadow command buffer cb's packed pieces, with a recording if it records and has none (0 when none is left) */
static u64 cb_get(u64 cb) {
    u64 v = map_get(&g_v.lend.cb, cb);
    if ((v & CB_RECORDING) && !CB_REC(v)) {
        u32 r = g_v.lend.rec_free;
        if (r)
            g_v.lend.rec_free = g_v.lend.rec[r - 1].n;
        else if (g_v.lend.nrec < REC_MAX)
            r = ++g_v.lend.nrec;
        if (r) {
            g_v.lend.rec[r - 1].n = 0;
            v |= (u64)r << 32;
        } else
            LOG_LIMIT(4, "nvn verify: more than %u shadow recordings in the runtime's memory: the next ones' pieces "
                         "are free while they are called\n", REC_MAX);
    }
    return v;
}
/* cb writes in piece i of kind from now on (i 1 up, 0 the game's memory) */
static void cb_moves(u64 cb, u32 kind, u32 i) {
    u64 v = cb_get(cb);
    piece_unref(CB_CUR(v, kind));
    if (i) {
        g_v.lend.piece[i - 1].refs++;
        if (CB_REC(v)) rec_add(CB_REC(v), i);
    }
    v = (v & ~(0xFFFFull << (16 * kind))) | (u64)i << (16 * kind);
    map_put(&g_v.lend.cb, cb, v);
}
/* a piece of kind for the shadow command buffer cb, of at least min bytes, which cb writes in from now on: ~0 when
   there is none */
static u64 lend_take(u32 kind, u64 cb, u64 min, u32* size) {
    u64 want = lend_min[kind], at = ~0ull;
    u32 i;
    while (want < min) want <<= 1;
    pm_lock(&g_v.lock);
    pieces_cooled();
    i = g_v.lend.free[kind][log2_of(want)];
    if (i)
        g_v.lend.free[kind][log2_of(want)] = g_v.lend.piece[i - 1].next;
    else if (g_v.lend.n < LEND_MAX && g_v.lend.used[kind] + want <= lend_area[kind]) {
        Piece* p = &g_v.lend.piece[g_v.lend.n];
        p->at = (kind == LEND_CTRL ? g_v.lend.area[kind] : 0) + g_v.lend.used[kind];
        p->size = (u32)want;
        p->kind = kind;
        p->refs = 0;
        g_v.lend.used[kind] += want;
        i = ++g_v.lend.n;
        if (!(i & (i - 1)))
            LOG("nvn verify: %u pieces of the runtime's memory for the shadow driver (%llu KiB command, %llu KiB "
                "control)\n",
                i, (unsigned long long)(g_v.lend.used[LEND_CMD] >> 10),
                (unsigned long long)(g_v.lend.used[LEND_CTRL] >> 10));
    }
    if (i) {
        cb_moves(cb, kind, i);
        at = g_v.lend.piece[i - 1].at;
        *size = (u32)want;
    }
    pm_unlock(&g_v.lock);
    if (at == ~0ull)
        LOG_LIMIT(4, "nvn verify: the shadow driver needs %s memory, and the runtime's is used up\n",
                  kind == LEND_CMD ? "command" : "control");
    return at;
}
/* the game's calls that change what the pieces are used by: cb is the shadow command buffer */
static void lend_added(u64 cb, u64 memory, u64 size) { /* the game added its own command memory */
    pm_lock(&g_v.lock);
    cb_moves(cb, LEND_CMD, 0);
    handles_gone(memory, size);
    pm_unlock(&g_v.lock);
}
static void lend_begin(u64 cb) { /* a recording starts in the pieces cb writes in */
    u64 v;
    pm_lock(&g_v.lock);
    v = map_get(&g_v.lend.cb, cb) | CB_RECORDING;
    map_put(&g_v.lend.cb, cb, v);
    if (CB_CUR(v, LEND_CMD) || CB_CUR(v, LEND_CTRL)) {
        v = cb_get(cb);
        if (CB_REC(v)) {
            rec_add(CB_REC(v), CB_CUR(v, LEND_CMD));
            rec_add(CB_REC(v), CB_CUR(v, LEND_CTRL));
        }
        map_put(&g_v.lend.cb, cb, v);
    }
    pm_unlock(&g_v.lock);
}
/* cb's recording calls the recordings of n of nvn_direct's handles: its records point at their commands, which stay
   while it does (the game may record again at a callee's handle before it submits the caller). Their control records
   are copied */
static void lend_calls(u64 cb, u64 handles, u32 n) {
    u32 i, j;
    u64 v;
    pm_lock(&g_v.lock);
    v = map_get(&g_v.lend.cb, cb);
    for (i = 0; i < n && (v & CB_RECORDING); i++) {
        u64 h;
        u32 r;
        memcpy(&h, GP(handles + 8 * (u64)i), 8);
        r = (u32)map_get(&g_v.lend.handle, h);
        if (!r) continue;
        if (!CB_REC(v)) {
            v = cb_get(cb);
            map_put(&g_v.lend.cb, cb, v);
            if (!CB_REC(v)) break;
        }
        for (j = 0; j < g_v.lend.rec[r - 1].n; j++) /* control records are copied */
            if (g_v.lend.piece[g_v.lend.rec[r - 1].p[j] - 1].kind == LEND_CMD)
                rec_add(CB_REC(v), g_v.lend.rec[r - 1].p[j]);
    }
    pm_unlock(&g_v.lock);
}
static void lend_end(u64 cb, u64 handle) { /* the recording is nvn_direct's handle's, in place of the one before */
    u64 v;
    pm_lock(&g_v.lock);
    v = map_get(&g_v.lend.cb, cb);
    rec_drop((u32)map_get(&g_v.lend.handle, handle));
    if (CB_REC(v))
        map_put(&g_v.lend.handle, handle, CB_REC(v));
    else
        map_del(&g_v.lend.handle, handle);
    if (v) map_put(&g_v.lend.cb, cb, v & ~(CB_RECORDING | 0xFFFFull << 32));
    pm_unlock(&g_v.lock);
}
static void lend_final(u64 cb) { /* cb is gone: a recording it didn't end too */
    u64 v;
    pm_lock(&g_v.lock);
    v = map_get(&g_v.lend.cb, cb);
    piece_unref(CB_CUR(v, LEND_CMD));
    piece_unref(CB_CUR(v, LEND_CTRL));
    rec_drop(CB_REC(v));
    map_del(&g_v.lend.cb, cb);
    pm_unlock(&g_v.lock);
}
/* the shadow's memory callback, called by the driver in a shadow call: x0 the command buffer, w1 the event (0 command
   memory, 1 control memory), x2 the least it needs. It returns as a guest function returns, its registers as they were */
static void shadow_memory(CPU* c) {
    CPU saved;
    u64 cb = c->x[0], min = c->x[2], sp, at;
    u32 kind = c->x[1] ? LEND_CTRL : LEND_CMD, size = 0;
    memcpy(&saved, c, REGS_SIZE);
    sp = (saved.x[31] - 0x100) & ~15ull;
    /* the command event's least is in words or bytes (the driver's checks before and after the call differ): taken
       as words */
    at = lend_take(kind, cb, kind == LEND_CMD ? min * 4 : min, &size);
    if (at != ~0ull) {
        if (kind == LEND_CMD)
            drv_call(c, &saved, sp, "nvnCommandBufferAddCommandMemory", cb, g_v.lend.pool, at, size);
        else
            drv_call(c, &saved, sp, "nvnCommandBufferAddControlMemory", cb, at, size, 0);
    }
    memcpy(c, &saved, REGS_SIZE);
    c->pc = saved.x[30];
}

/* ---- a call */
typedef struct {
    u64 live;   /* the argument's live value */
    u64 at;     /* its scratch copy (states) */
    u32 len, n; /* a state's bytes, and an array's count */
    u8 before[STATE_MAX];
} Arg;

static void skip(Fn* f, const char* why) {
    if (__atomic_add_fetch(&f->skipped, 1, __ATOMIC_RELAXED) == 1) {
        f->why = why;
        LOG("nvn verify: the shadow skips %s: %s\n", f->k ? f->k->name : "a function", why);
    }
}
/* the slot of the state the game made at addr (a builder or state on its stack or in its objects), by address */
static State* astate_slot(u64 addr) { return &g_v.astate[(u32)mix(addr) & (STATE_SLOTS - 1)]; }
/* The driver's bytes for the state at live address addr, whose bytes are live, copied to the scratch at *at: 0 when
   the shadow doesn't know it. First the state the calls at that address made, while the game left its bytes as they
   were: nvn_direct's bytes keep less than the driver's (the device, fields the driver derives), so two driver states
   can share them, and only the address tells which. Else the state by content: a copy the game made by value */
static int state_in(u64 addr, const u8* live, u32 len, int defaults, u64* at) {
    State* s;
    *at = (*at - len) & ~15ull;
    pm_lock(&g_v.lock);
    s = astate_slot(addr);
    if (!(s->filled && s->addr == addr && s->len == len && !memcmp(s->live, live, len))) {
        s = state_find(live, len, 0);
        if (s && !s->filled) s = NULL;
    }
    if (!s && !defaults) { /* never set: the zeroed memory the game made it in stands for the driver's zeros */
        u32 k;
        for (k = 0; k < len && !live[k]; k++) {
        }
        defaults = k == len;
    }
    if (s)
        memcpy(GP(*at), s->drv, len);
    else if (defaults)
        memset(GP(*at), 0, len);
    pm_unlock(&g_v.lock);
    return s || defaults;
}
/* the state the driver made of a scratch copy, for the live address and the bytes the same call left there */
static void state_out(Fn* f, u64 addr, const u8* live, u32 len, u64 at) {
    State* s = astate_slot(addr);
    pm_lock(&g_v.lock);
    s->addr = addr;
    s->len = len;
    memcpy(s->live, live, len);
    memcpy(s->drv, GP(at), len);
    s->filled = 1;
    s = state_find(live, len, 1);
    if (s->filled && memcmp(s->drv, GP(at), len)) {
        /* the same live bytes for other driver bytes: nvn_direct keeps less of the state than the driver */
        if (__atomic_add_fetch(&g_v.lossy, 1, __ATOMIC_RELAXED) <= 8) {
            u32 k;
            for (k = 0; k < len && s->drv[k] == GP(at)[k]; k++) {
            }
            LOG("nvn verify: after %s, nvn_direct's bytes stand for two of the driver's states (they differ at byte "
                "%u: %02x, now %02x): a copy by value of such a state is ambiguous\n",
                 f->k->name, k, s->drv[k], GP(at)[k]);
        }
    }
    memcpy(s->drv, GP(at), len);
    s->filled = 1;
    pm_unlock(&g_v.lock);
}

/* the arguments for the driver, from the live ones in c (the call's registers before nvn_direct's handler):
   1, or 0 with the reason when the shadow has no counterpart for one */
static int shadow_args(Fn* f, CPU* c, Arg* a, u64* scratch, const char** why) {
    const char* k = f->k->kinds;
    u32 i, xi = 0, count = 0; /* an array's count: the last integer argument before it */
    for (i = 0; k[i] && i < NVK_ARGS; i++) {
        u32 size = f->k->size[i];
        u64 v;
        if (k[i] == 'F') continue;
        if (xi >= 8) {
            *why = "an argument on the stack";
            return 0;
        }
        v = a[i].live = c->x[xi];
        switch (k[i]) {
            case 'o':
                if (v) {
                    u64 s;
                    pm_lock(&g_v.lock);
                    s = map_get(&g_v.obj, v);
                    pm_unlock(&g_v.lock);
                    if (!s && i == 0 && (f->flags & F_INIT)) {
                        s = obj_alloc(size);
                        pm_lock(&g_v.lock);
                        map_put(&g_v.obj, v, s);
                        pm_unlock(&g_v.lock);
                    }
                    if (!s) {
                        __atomic_add_fetch(&g_v.unknown_obj, 1, __ATOMIC_RELAXED);
                        *why = "an object the shadow didn't see made";
                        return 0;
                    }
                    c->x[xi] = s;
                }
                break;
            case 'v': count = (u32)v; break;
            case 'O':
            case 'T': {
                u32 n = count, j;
                u64 arr;
                if (!v || !n) break;
                if (n > 32) {
                    *why = "an array longer than 32";
                    return 0;
                }
                *scratch = (*scratch - n * 8) & ~15ull;
                arr = *scratch;
                for (j = 0; j < n; j++) {
                    u64 p, s = 0;
                    memcpy(&p, GP(v + j * 8), 8);
                    if (p && k[i] == 'O') {
                        pm_lock(&g_v.lock);
                        s = map_get(&g_v.obj, p);
                        pm_unlock(&g_v.lock);
                        if (!s) {
                            *why = "an object the shadow didn't see made, in an array";
                            return 0;
                        }
                    } else if (p) {
                        if (!state_in(p, GP(p), size, 0, scratch)) {
                            *why = "a state the shadow doesn't know, in an array";
                            return 0;
                        }
                        s = *scratch;
                    }
                    memcpy(GP(arr + j * 8), &s, 8);
                }
                c->x[xi] = arr;
                break;
            }
            case 's':
                if (v) {
                    a[i].len = size;
                    if (!state_in(v, a[i].before, size, i == 0 && (f->flags & (F_DEFAULTS | F_STATESET)), scratch)) {
                        __atomic_add_fetch(&g_v.unknown_state, 1, __ATOMIC_RELAXED);
                        *why = "a state the shadow doesn't know";
                        return 0;
                    }
                    a[i].at = *scratch;
                    c->x[xi] = *scratch;
                }
                break;
            case 'S': {
                u32 n = count, j;
                u64 arr;
                if (!v || !n) break;
                if (n > 64) {
                    *why = "a state array longer than 64";
                    return 0;
                }
                *scratch = (*scratch - n * size) & ~15ull;
                arr = *scratch;
                for (j = 0; j < n; j++) {
                    u64 one = *scratch;
                    if (!state_in(v + j * size, GP(v + j * size), size, 0, &one)) {
                        *why = "a state the shadow doesn't know, in an array";
                        return 0;
                    }
                    memmove(GP(arr + j * size), GP(one), size);
                }
                c->x[xi] = arr;
                break;
            }
            case 'm':
                if (v && c->x[xi + 1]) {
                    /* queue memory: the size the driver asks for may be more than nvn_direct told the game (the
                       call's result compare says so), and the shadow's has room for it */
                    u64 n = c->x[xi + 1];
                    if (!strcmp(f->k->name, "nvnQueueBuilderSetQueueMemory") && n < (8u << 20)) n = 8u << 20;
                    if (f->flags & F_ADDCTRL) break; /* control memory: a piece of the runtime's (verify_fn) */
                    if (n >> 36) {
                        *why = "a memory size past 64 GiB";
                        return 0;
                    }
                    c->x[xi] = mem_shadow(v, n);
                    c->x[xi + 1] = n;
                }
                break;
            case 'w':
                *scratch = (*scratch - 0x100) & ~15ull;
                memset(GP(*scratch), 0, 0x100);
                c->x[xi] = *scratch;
                break;
            case 'r': *why = "mapping requests"; return 0;
            default: break;
        }
        if (k[i] == 'p' && (f->flags & F_HANDLES) && i == 2 && v) { /* command handles: the shadow's */
            u32 n = (u32)c->x[1], j;
            if (n > 4096) {
                *why = "more than 4096 command handles";
                return 0;
            }
            *scratch = (*scratch - n * 8) & ~15ull;
            for (j = 0; j < n; j++) {
                u64 h, s;
                memcpy(&h, GP(v + j * 8), 8);
                pm_lock(&g_v.lock);
                s = map_get(&g_v.handle, h);
                pm_unlock(&g_v.lock);
                if (!s) {
                    *why = "a command handle the shadow didn't record";
                    return 0;
                }
                memcpy(GP(*scratch + j * 8), &s, 8);
            }
            c->x[xi] = *scratch;
        }
        xi++;
    }
    return 1;
}

/* the result's bits as compared */
static u64 result_bits(const CPU* c, int ret) {
    switch (ret) {
        case NVK_RET_BOOL: return (c->x[0] & 0xFF) != 0;
        case NVK_RET_X32: return (u32)c->x[0];
        case NVK_RET_F32: return (u32)SNX_VS(c->v[0], 0);
        default: return c->x[0];
    }
}

/* a texture descriptor (TIC entry) the same call registered on both sides: the same words but for the address
   (word 1 and the low half of word 2), which is compared as the guest bytes it names */
static void texreg_check(u64 live_pool, u32 id) {
    const NvnDescPool* lp = NVN_OBJ(const NvnDescPool, live_pool);
    u64 live_mem = nvn_pool_memory(lp->pool), at = (u64)lp->offset + (u64)id * NVN_DESCRIPTOR_SIZE, sm;
    u32 lw[8], sw[8], i;
    if (!live_mem || !(sm = mem_find(live_mem, at + NVN_DESCRIPTOR_SIZE))) return;
    memcpy(lw, GP(live_mem + at), sizeof lw);
    memcpy(sw, GP(sm + at), sizeof sw);
    for (i = 0; i < 8; i++) {
        u32 mask = i == 1 ? 0 : i == 2 ? 0xFFFF0000u : ~0u;
        if ((lw[i] ^ sw[i]) & mask && __atomic_add_fetch(&g_v.texreg_diff, 1, __ATOMIC_RELAXED) <= 8)
            WARN("nvn verify: texture %u's descriptor word %u: nvn_direct %08x, driver %08x\n", id, i, lw[i], sw[i]);
    }
    {
        u64 la = (u64)lw[1] | (u64)(lw[2] & 0xFFFF) << 32, sa = (u64)sw[1] | (u64)(sw[2] & 0xFFFF) << 32;
        u64 lt = gmmu_translate(la), st = gmmu_translate(sa), sl = st ? mem_live(st) : 0;
        if (lt && sl && lt != sl && __atomic_add_fetch(&g_v.texreg_diff, 1, __ATOMIC_RELAXED) <= 8)
            WARN("nvn verify: texture %u's descriptor address: nvn_direct's names %llx, the driver's %llx\n", id,
                 (unsigned long long)lt, (unsigned long long)sl);
    }
}

/* Once, at the game's first texture storage query: nvn_direct's and the driver's texture builder getters on the same
   builder states, over targets, formats, sizes, levels, samples and flags. The live builder is the runtime's guest
   memory, set by nvn_direct's own handlers; the shadow's by the driver's setters. The log names each getter's
   differences, the first few with their state */
static void storage_sweep(CPU* c, const CPU* base, u64 sp) {
    static const i32 formats[] = {NVN_FORMAT_R8, NVN_FORMAT_RG16F, NVN_FORMAT_RGBA8, NVN_FORMAT_RGBA8_SRGB,
                                  NVN_FORMAT_RGBA16F, NVN_FORMAT_RGBA32F, NVN_FORMAT_R32F, NVN_FORMAT_RGB10A2,
                                  NVN_FORMAT_R11G11B10F, NVN_FORMAT_DEPTH16, NVN_FORMAT_DEPTH32F,
                                  NVN_FORMAT_DEPTH24_STENCIL8, NVN_FORMAT_STENCIL8, NVN_FORMAT_RGBA_DXT1,
                                  NVN_FORMAT_RGBA_DXT5, NVN_FORMAT_BPTC_UNORM, NVN_FORMAT_RGBA_ASTC_4x4,
                                  NVN_FORMAT_RGBA_ASTC_8x8};
    static const struct {
        i32 target, depth, samples;
    } targets[] = {{NVN_TEXTURE_TARGET_1D, 1, 0},       {NVN_TEXTURE_TARGET_2D, 1, 0},
                   {NVN_TEXTURE_TARGET_2D_ARRAY, 6, 0}, {NVN_TEXTURE_TARGET_3D, 16, 0},
                   {NVN_TEXTURE_TARGET_CUBEMAP, 6, 0},  {NVN_TEXTURE_TARGET_2D_MULTISAMPLE, 1, 4},
                   {NVN_TEXTURE_TARGET_2D_MULTISAMPLE, 1, 2}, {NVN_TEXTURE_TARGET_RECTANGLE, 1, 0}};
    static const u32 sizes[][2] = {{1, 1}, {7, 5}, {16, 16}, {33, 17}, {64, 64}, {100, 60}, {256, 256},
                                   {1000, 3}, {1280, 720}, {1600, 896}, {4096, 2048}};
    static const i32 flags[] = {0, NVN_TEXTURE_FLAGS_COMPRESSIBLE, NVN_TEXTURE_FLAGS_IMAGE};
    static const char* const what[4] = {"size", "alignment", "class", "zcull size"};
    static const char* const getter[4] = {"nvnTextureBuilderGetStorageSize", "nvnTextureBuilderGetStorageAlignment",
                                          "nvnTextureBuilderGetStorageClass",
                                          "nvnTextureBuilderGetZCullStorageSize"};
    u64 live = obj_alloc(sizeof(NVNtextureBuilder)), shadow = obj_alloc(sizeof(NVNtextureBuilder)), n = 0;
    u64 differ_n[4] = {0};
    size_t t, f, z, l, g;
    if (!g_v.device || !g_v.live_device) return;
    for (t = 0; t < sizeof targets / sizeof targets[0]; t++)
        for (f = 0; f < sizeof formats / sizeof formats[0]; f++)
            for (z = 0; z < sizeof sizes / sizeof sizes[0]; z++)
                for (l = 0; l < 2; l++)
                    for (g = 0; g < sizeof flags / sizeof flags[0]; g++) {
                        u32 w = sizes[z][0], h = targets[t].target == NVN_TEXTURE_TARGET_1D ? 1 : sizes[z][1];
                        i32 d = targets[t].depth, levels = 1, k;
                        u64 a[4], b[4];
                        if (targets[t].target == NVN_TEXTURE_TARGET_CUBEMAP) h = w;
                        if (l)
                            for (u32 m = w > h ? w : h; m > 1; m >>= 1) levels++;
                        if (targets[t].samples && (l || f >= 13)) continue; /* no multisampled levels or BCn */
                        nvn_TextureBuilderSetDevice(c, live, g_v.live_device);
                        nvn_TextureBuilderSetDefaults(c, live);
                        nvn_TextureBuilderSetTarget(c, live, targets[t].target);
                        nvn_TextureBuilderSetFormat(c, live, formats[f]);
                        nvn_TextureBuilderSetSize3D(c, live, (i32)w, (i32)h, d);
                        nvn_TextureBuilderSetLevels(c, live, levels);
                        nvn_TextureBuilderSetSamples(c, live, targets[t].samples);
                        nvn_TextureBuilderSetFlags(c, live, flags[g]);
                        a[0] = nvn_TextureBuilderGetStorageSize(c, live);
                        a[1] = nvn_TextureBuilderGetStorageAlignment(c, live);
                        a[2] = (u32)nvn_TextureBuilderGetStorageClass(c, live);
                        a[3] = nvn_TextureBuilderGetZCullStorageSize(c, live);
                        drv_call(c, base, sp, "nvnTextureBuilderSetDevice", shadow, g_v.device, 0, 0);
                        drv_call(c, base, sp, "nvnTextureBuilderSetDefaults", shadow, 0, 0, 0);
                        drv_call(c, base, sp, "nvnTextureBuilderSetTarget", shadow, (u64)targets[t].target, 0, 0);
                        drv_call(c, base, sp, "nvnTextureBuilderSetFormat", shadow, (u64)formats[f], 0, 0);
                        drv_call(c, base, sp, "nvnTextureBuilderSetSize3D", shadow, w, h, (u64)d);
                        drv_call(c, base, sp, "nvnTextureBuilderSetLevels", shadow, (u64)levels, 0, 0);
                        drv_call(c, base, sp, "nvnTextureBuilderSetSamples", shadow, (u64)targets[t].samples, 0, 0);
                        drv_call(c, base, sp, "nvnTextureBuilderSetFlags", shadow, (u64)flags[g], 0, 0);
                        for (k = 0; k < 4; k++) {
                            b[k] = drv_call(c, base, sp, getter[k], shadow, 0, 0, 0);
                            if (k == 2) b[k] = (u32)b[k];
                        }
                        n++;
                        for (k = 0; k < 4; k++)
                            if (a[k] != b[k]) {
                                if (++differ_n[k] <= 16)
                                    LOG("nvn verify: storage %s: target %d format %#x %ux%ux%d levels %d samples %d "
                                        "flags %#x: nvn_direct %llx, driver %llx\n",
                                        what[k], targets[t].target, formats[f], w, h, d, levels, targets[t].samples,
                                        flags[g], (unsigned long long)a[k], (unsigned long long)b[k]);
                            }
                    }
    LOG("nvn verify: storage sweep of %llu builder states: %llu sizes, %llu alignments, %llu classes and %llu zcull "
        "sizes differ from the driver's\n",
        (unsigned long long)n, (unsigned long long)differ_n[0], (unsigned long long)differ_n[1],
        (unsigned long long)differ_n[2], (unsigned long long)differ_n[3]);
    obj_free(live, sizeof(NVNtextureBuilder));
    obj_free(shadow, sizeof(NVNtextureBuilder));
}

static void verify_fn(CPU* RESTRICT c, u64 entry) {
    Fn* f = fn_at(entry);
    CPU before, after;
    Arg a[NVK_ARGS];
    const char* why = NULL;
    u64 scratch, lr, live_ret;
    u32 i, queue = 0, serial = 0;
    int ok;
    if (!f) fatal("nvn verify: no function at %llx", (unsigned long long)entry);
    if (s_tls.t_shadow) { /* the driver calling NVN: its own code, or its memory callback (lend_ready) */
        if (entry == g_v.lend.callback && c->x[1] < 2 && __atomic_load_n(&g_v.lend.state, __ATOMIC_ACQUIRE) == 2)
            shadow_memory(c);
        else
            f->original(c, entry);
        return;
    }
    memcpy(&before, c, REGS_SIZE);
    memset(a, 0, sizeof a);
    if (f->k && f->k->kinds) /* the states as they are before nvn_direct's handler changes them */
        for (i = 0; f->k->kinds[i] && i < NVK_ARGS; i++)
            if (f->k->kinds[i] == 's') {
                u32 xi = 0, j;
                for (j = 0; j < i; j++) xi += f->k->kinds[j] != 'F';
                if (xi < 8 && c->x[xi]) memcpy(a[i].before, GP(c->x[xi]), f->k->size[i]);
            }
    f->handler(c, entry);
    __atomic_add_fetch(&f->calls, 1, __ATOMIC_RELAXED);
    if (!f->k || !f->k->kinds) {
        skip(f, "no prototype in nvn.hook");
        return;
    }
    if ((f->flags & F_SKIP) && !(f->flags & F_PRESENT)) {
        skip(f, "the shadow never makes it");
        return;
    }
    memcpy(&after, c, REGS_SIZE);
    live_ret = result_bits(&after, f->k->ret);
    lr = before.x[30];
    scratch = shadow_stack();
    memcpy(c, &before, REGS_SIZE);
    if (f->flags & F_QUEUE) { /* the queue's replay index, which a channel the driver opens now is for */
        const NvnQueue* q = NVN_OBJ(const NvnQueue, before.x[0]);
        queue = before.x[0] ? q->replay : 0;
    }
    if (f->flags & F_SUBMIT) { /* the game's submission count, which this one's batch is tagged with */
        if ((i32)before.x[1] <= 0) {
            memcpy(c, &after, REGS_SIZE);
            return;
        }
        if (queue && queue <= QUEUES) {
            pm_lock(&g_v.lock);
            serial = ++g_v.q[queue - 1].shadow_serial;
            pm_unlock(&g_v.lock);
        }
    }
    if (f->flags & F_PRESENT) { /* the queue alone: the window has no shadow */
        pm_lock(&g_v.lock);
        c->x[0] = map_get(&g_v.obj, before.x[0]);
        pm_unlock(&g_v.lock);
        ok = c->x[0] != 0;
        why = "a queue the shadow didn't see made";
    } else
        ok = shadow_args(f, c, a, &scratch, &why);
    if (!ok) {
        skip(f, why);
        if (f->flags & F_INIT) { /* an object the shadow can't make: none */
            pm_lock(&g_v.lock);
            map_del(&g_v.obj, before.x[0]);
            pm_unlock(&g_v.lock);
        }
        memcpy(c, &after, REGS_SIZE);
        return;
    }
    {
        CPU base;
        u64 run_sp = (scratch - 16) & ~15ull;
        u32 prev_queue = s_tls.t_queue;
        GuestFn fn = f->original;
        u64 at = entry;
        s_tls.t_queue = queue;
        memcpy(&base, c, REGS_SIZE);
        if (__atomic_add_fetch(&f->ran, 1, __ATOMIC_RELAXED) == 1) /* what ran last when the driver faults */
            LOG("nvn verify: the shadow runs %s for the first time (the driver's code at %llx)\n", f->k->name,
                (unsigned long long)entry);
        /* memory the game adds: command memory in the pool's scaled shadow (cmd_pool), or a piece for memory outside
           the pool; control memory in a piece (the lend section) */
        if (f->flags & F_ADDCMD) {
            const NvnPool* lp = NVN_OBJ(const NvnPool, before.x[1]);
            u64 off = before.x[2], size = before.x[3], pool = 0;
            u32 got;
            lend_added(base.x[0], lp->memory + off, size);
            if (off <= lp->size && size <= lp->size - off) pool = cmd_pool(c, &base, run_sp, before.x[1]);
            memcpy(c, &base, REGS_SIZE);
            if (pool) {
                c->x[1] = pool;
                c->x[2] = off * CMD_SCALE;
                c->x[3] = size * CMD_SCALE;
            } else if (__atomic_load_n(&g_v.lend.state, __ATOMIC_ACQUIRE) == 2 &&
                       (off = lend_take(LEND_CMD, base.x[0], size, &got)) != ~0ull) {
                c->x[1] = g_v.lend.pool;
                c->x[2] = off;
                c->x[3] = got;
            }
        }
        if (f->flags & F_ADDCTRL) {
            u32 got;
            u64 at = __atomic_load_n(&g_v.lend.state, __ATOMIC_ACQUIRE) == 2
                         ? lend_take(LEND_CTRL, base.x[0], before.x[2], &got)
                         : ~0ull;
            if (at != ~0ull) {
                c->x[1] = at;
                c->x[2] = got;
            } else {
                c->x[1] = mem_shadow(before.x[1], before.x[2]);
                c->x[2] = before.x[2];
            }
        }
        s_tls.t_running = f->k->name;
        if (f->flags & F_PRESENT) {
            shadow_flush(c, &base, run_sp, base.x[0]);
            pm_lock(&g_v.lock);
            g_v.lend.presents++;
            pm_unlock(&g_v.lock);
        } else
            run(c, fn, at, lr, run_sp);
        s_tls.t_running = NULL;
        if (f->flags & F_BEGIN) lend_begin(base.x[0]);
        if (f->flags & F_CALLS) lend_calls(base.x[0], before.x[2], (u32)before.x[1]);
        if (f->flags & F_END) lend_end(base.x[0], after.x[0]);
        if ((f->flags & F_SUBMIT) && serial) {
            CPU done;
            memcpy(&done, c, REGS_SIZE);
            shadow_flush(c, &base, run_sp, base.x[0]);
            memcpy(c, &done, REGS_SIZE);
        }
        (void)serial;
        s_tls.t_queue = prev_queue;
    }
    /* what the call left */
    if ((f->flags & F_INIT) && f->k->ret == NVK_RET_BOOL && !(c->x[0] & 0xFF)) { /* no object: the next calls skip */
        u64 s;
        pm_lock(&g_v.lock);
        s = map_get(&g_v.obj, before.x[0]);
        map_del(&g_v.obj, before.x[0]);
        if (s) obj_free(s, f->k->size[0]);
        pm_unlock(&g_v.lock);
        if (__atomic_add_fetch(&f->failed, 1, __ATOMIC_RELAXED) <= 4)
            WARN("nvn verify: the driver's %s(%llx) failed where nvn_direct's %s: the shadow has no such object\n",
                 f->k->name, (unsigned long long)before.x[0], (after.x[0] & 0xFF) ? "didn't" : "failed too");
    }
    if ((f->flags & (F_CMD | F_INIT)) == (F_CMD | F_INIT) && (c->x[0] & 0xFF)) { /* the shadow's memory callback */
        CPU ran;
        u64 cb, sp_ = (scratch - 16) & ~15ull;
        memcpy(&ran, c, REGS_SIZE);
        pm_lock(&g_v.lock);
        cb = map_get(&g_v.obj, before.x[0]);
        pm_unlock(&g_v.lock);
        if (cb && lend_ready(c, &before, sp_))
            drv_call(c, &before, sp_, "nvnCommandBufferSetMemoryCallback", cb, g_v.lend.callback, 0, 0);
        memcpy(c, &ran, REGS_SIZE);
    }
    if ((f->flags & F_END) && after.x[0] && c->x[0]) {
        pm_lock(&g_v.lock);
        map_put(&g_v.handle, after.x[0], c->x[0]);
        pm_unlock(&g_v.lock);
    }
    for (i = 0; f->k->kinds[i] && i < NVK_ARGS; i++)
        if (f->k->kinds[i] == 's' && a[i].at && a[i].live) state_out(f, a[i].live, GP(a[i].live), a[i].len, a[i].at);
    if (f->k->ret != NVK_RET_NONE && f->k->ret != NVK_RET_ADDR && !(f->flags & (F_NORESULT | F_PRESENT))) {
        u64 got = result_bits(c, f->k->ret);
        if (got != live_ret && __atomic_add_fetch(&f->differed, 1, __ATOMIC_RELAXED) <= 4)
            WARN("nvn verify: %s(%llx, %llx, %llx, %llx) returns %llx, the driver %llx\n", f->k->name,
                 (unsigned long long)before.x[0], (unsigned long long)before.x[1], (unsigned long long)before.x[2],
                 (unsigned long long)before.x[3], (unsigned long long)live_ret, (unsigned long long)got);
    }
    if (f->flags & F_TEXREG) texreg_check(before.x[0], (u32)before.x[1]);
    if (!strcmp(f->k->name, "nvnDeviceInitialize") && a[0].live) {
        pm_lock(&g_v.lock);
        g_v.device = map_get(&g_v.obj, a[0].live);
        g_v.live_device = a[0].live;
        pm_unlock(&g_v.lock);
    }
    if (!strcmp(f->k->name, "nvnTextureBuilderGetStorageSize") && !__atomic_exchange_n(&g_v.swept, 1, __ATOMIC_ACQ_REL)) {
        memcpy(c, &before, REGS_SIZE);
        storage_sweep(c, &before, (scratch - 16) & ~15ull);
    }
    if (f->flags & F_FINAL) {
        u64 s;
        pm_lock(&g_v.lock);
        s = map_get(&g_v.obj, before.x[0]);
        map_del(&g_v.obj, before.x[0]);
        if (s) obj_free(s, f->k->size[0]);
        pm_unlock(&g_v.lock);
        if (!strcmp(f->k->name, "nvnMemoryPoolFinalize")) {
            memcpy(c, &before, REGS_SIZE);
            cmd_pool_final(c, &before, (scratch - 16) & ~15ull, before.x[0]);
        }
        if (s && (f->flags & F_CMD)) { /* the pieces it writes in, and a recording it didn't end */
            lend_final(s);
        }
    }
    memcpy(c, &after, REGS_SIZE);
}

int nvn_verify_ours(GuestFn fn) { return fn == verify_fn; }

GuestFn nvn_verify_take(u64 addr, const char* name, GuestFn handler, GuestFn original, int implemented) {
    u32 n, i;
    if (!g_v.on) {
        if (!g_settings.nvn_verify) return handler;
        pm_lock(&g_v.lock);
        if (!g_v.on) {
            g_v.state = (State*)calloc(STATE_SLOTS, sizeof *g_v.state);
            g_v.astate = (State*)calloc(STATE_SLOTS, sizeof *g_v.astate);
            g_v.cb_key_bits = 20;
            g_v.cb_key = (u64*)calloc(CB_SLOTS, sizeof *g_v.cb_key);
            g_v.cb_val = (u32*)calloc(CB_SLOTS, sizeof *g_v.cb_val);
            if (!g_v.state || !g_v.astate || !g_v.cb_key || !g_v.cb_val) fatal("nvn verify: out of memory");
            LOG("nvn verify: on: each NVN call runs the game's driver too, on shadows, and each draw is compared\n");
            __atomic_store_n(&g_v.on, 1, __ATOMIC_RELEASE);
        }
        pm_unlock(&g_v.lock);
    }
    if (!original || nvn_verify_ours(original)) return handler;
    pm_lock(&g_v.lock);
    for (n = 0, i = fn_hash(addr); n < FNS; n++, i = (i + 1) & (FNS - 1)) {
        Fn* f = &g_v.fn[i];
        if (f->addr == addr) break;
        if (!f->addr) {
            f->handler = handler;
            f->original = original;
            f->k = kinds_of(name);
            f->flags = flags_of(name, f->k);
            __atomic_store_n(&f->addr, addr, __ATOMIC_RELEASE);
            break;
        }
    }
    pm_unlock(&g_v.lock);
    if (n == FNS) fatal("nvn verify: more than %u NVN functions", FNS);
    return implemented ? verify_fn : handler;
}

/* ---- nvdrv: the shadow's channels */
void nvn_verify_fd(u32 fd, int open) {
    if (fd >= sizeof g_v.fd_queue) return;
    __atomic_store_n(&g_v.fd_queue[fd],
                     (u8)(open ? (s_tls.t_queue && s_tls.t_queue <= QUEUES ? s_tls.t_queue : 0x80) : 0),
                     __ATOMIC_RELEASE);
    if (open) LOG("nvn verify: fd %u is the shadow driver's (queue %u)\n", fd, s_tls.t_queue);
}
int nvn_verify_fd_is(u32 fd) { return fd < sizeof g_v.fd_queue && __atomic_load_n(&g_v.fd_queue[fd], __ATOMIC_ACQUIRE); }

/* a release's bytes at the GPU address a, in guest memory now */
static void release_write(u64 a, const u8* d, u32 n) {
    while (n) {
        u64 cpu, run = gmmu_run(a, n, &cpu);
        if (!run) return;
        memcpy(GP(cpu), d, (size_t)run);
        a += run;
        d += run;
        n -= (u32)run;
    }
}
/* The semaphore releases of a shadow batch, made when it is submitted, as its syncpoint increments are: the driver
   waits for them (its queue reuses its memory once the GPU wrote a semaphore of its own there, and in one game its
   wait spun forever), and the verify channel makes none. They write the shadow driver's memory only: each address in
   the shadow's stream is one the shadow driver mapped. A stream out of step makes none from then on */
static void releases(VQueue* v, const u32* w, u32 n) {
    u32 i, sub, m, val;
    int last;
    for (i = 0; i < n; i++) {
        u8 d[16];
        if (!pb_decode(&v->rel_pb, w[i], &sub, &m, &val, &last)) continue;
        if (m < PULLER_NON_PULLER_METHODS) {
            if (m == PULLER_BIND_OBJECT) {
                v->rel_class[sub & 7] = val & CLASS_ID_MASK;
                if ((val >> 16 || !gpu_class_known(val)) && !v->rel_lost) {
                    v->rel_lost = 1;
                    WARN("nvn verify: queue %u's shadow stream binds class %x, which no engine here runs: its words are "
                         "out of step, and its releases are made no more\n",
                         (u32)(v - g_v.q) + 1, val);
                }
            }
            if (m >= PULLER_SEMAPHORE_ADDRESS_HIGH && m <= PULLER_SEMAPHORE_SEQUENCE_PAYLOAD)
                v->rel_sem[m - PULLER_SEMAPHORE_ADDRESS_HIGH] = val;
            if (m == PULLER_SEMAPHORE_OPERATION && (val & PULLER_SEMAPHORE_OP_MASK) == PULLER_SEMAPHORE_OP_RELEASE &&
                !v->rel_lost) {
                snx_st_le32(d, v->rel_sem[2]);
                snx_st_le32(d + 4, 0);
                snx_st_le64(d + 8, gpu_ticks());
                release_write(((u64)(v->rel_sem[0] & PULLER_SEMAPHORE_ADDRESS_HIGH_MASK) << 32) | v->rel_sem[1], d,
                              (val & PULLER_SEMAPHORE_RELEASE_SIZE_4BYTE) ? 4 : 16);
            }
        } else if ((v->rel_class[sub & 7] == CLASS_MAXWELL_B || v->rel_class[sub & 7] == CLASS_KEPLER_COMPUTE_B) &&
                   m >= MAXWELL3D_REPORT_SEMAPHORE && m <= MAXWELL3D_REPORT_SEMAPHORE_QUERY) {
            u32* r = v->rel_rep[v->rel_class[sub & 7] == CLASS_KEPLER_COMPUTE_B];
            if (m < MAXWELL3D_REPORT_SEMAPHORE_QUERY)
                r[m - MAXWELL3D_REPORT_SEMAPHORE] = val;
            else if (report_semaphore_operation(val) == REPORT_SEMAPHORE_OPERATION_RELEASE && !v->rel_lost) {
                /* as engines.c report_write: one word, or the payload's 64 bits and the timestamp */
                snx_st_le64(d, r[2]);
                snx_st_le64(d + 8, gpu_ticks());
                release_write(((u64)(r[0] & 0xFF) << 32) | r[1], d, report_semaphore_short_query(val) ? 4 : 16);
            }
        }
    }
}
void nvn_verify_gpfifo(u32 fd, const u8* entries, u32 n) {
    u32 q = fd < sizeof g_v.fd_queue ? __atomic_load_n(&g_v.fd_queue[fd], __ATOMIC_ACQUIRE) : 0, i, words = 0, at = 0;
    VQueue* v;
    Batch* b;
    if (!q || q > QUEUES) return; /* a channel of no queue's */
    v = &g_v.q[q - 1];
    for (i = 0; i < n; i++) {
        u64 e;
        memcpy(&e, entries + (size_t)i * 8, 8);
        words += gpfifo_size(e);
    }
    b = (Batch*)malloc(sizeof *b + (size_t)words * 4);
    if (!b) fatal("nvn verify: out of memory for %u words", words);
    for (i = 0; i < n; i++) {
        u64 e, gva, left;
        memcpy(&e, entries + (size_t)i * 8, 8);
        gva = gpfifo_addr(e);
        left = (u64)gpfifo_size(e) * 4;
        while (left) {
            u64 cpu, run = gmmu_run(gva, left, &cpu);
            if (!run) { /* unmapped: zeros, as nothing */
                memset(b->w + at, 0, (size_t)left);
                at += (u32)(left / 4);
                break;
            }
            memcpy(b->w + at, GP(cpu), (size_t)run);
            at += (u32)(run / 4);
            gva += run;
            left -= run;
        }
    }
    b->next = NULL;
    b->n = at;
    pm_lock(&g_v.lock);
    releases(v, b->w, at);
    b->serial = v->shadow_serial;
    if (v->tail)
        v->tail->next = b;
    else
        v->head = b;
    v->tail = b;
    v->words += at;
    pm_unlock(&g_v.lock);
}

/* ---- the GPU thread: decode and compare */
static VQueue* vqueue(u32 queue) { return queue && queue <= QUEUES ? &g_v.q[queue - 1] : NULL; }

void nvn_verify_shadow_cb(u64 gva, const u32* words, u32 n) {
    u32 i;
    for (i = 0; i < n; i++) {
        u64 a = gva + (u64)i * 4;
        u32 slot = (u32)mix(a) & (CB_SLOTS - 1);
        g_v.cb_key[slot] = a;
        g_v.cb_val[slot] = words[i];
    }
}
static int cb_word(u64 a, u32* v) {
    u32 slot = (u32)mix(a) & (CB_SLOTS - 1);
    if (g_v.cb_key[slot] != a) return 0;
    *v = g_v.cb_val[slot];
    return 1;
}

void nvn_verify_shadow_draw(GpuChannel* ch, const DrawCall* dc) {
    VQueue* v = g_v.decoding;
    Pending* p;
    (void)ch;
    if (!v) return;
    if (v->npend == PENDING) { /* a macro's many draws, none compared yet: the oldest go */
        memmove(v->pend, v->pend + 1, (PENDING - 1) * sizeof *v->pend);
        v->npend--;
        v->shadow_only++;
    }
    p = &v->pend[v->npend++];
    p->ds = ch->ds;
    p->dc = *dc;
    p->dc.inline_indices = NULL;
    p->serial = v->batch_serial;
}

/* decodes v's batches up to (with serials at most) last, or until a draw of serial last is pending */
static void decode(VQueue* v, u32 last, int one_draw) {
    for (;;) {
        Batch* b = v->cur;
        if (one_draw && v->npend && v->pend[v->npend - 1].serial == last) return;
        if (!b) {
            pm_lock(&g_v.lock);
            b = v->head;
            if (b && b->serial <= last) {
                v->head = b->next;
                if (!v->head) v->tail = NULL;
                v->words -= b->n;
            } else
                b = NULL;
            pm_unlock(&g_v.lock);
            if (!b) return;
            v->cur = b;
            v->at = 0;
        }
        if (!v->ch) {
            g_gpu_verify_draw = nvn_verify_shadow_draw;
            g_gpu_verify_cb = nvn_verify_shadow_cb;
            v->ch = gpu_verify_channel_new(GPU_VERIFY_ID + (u32)(v - g_v.q));
        }
        g_v.decoding = v;
        v->batch_serial = b->serial;
        if (!v->at) r3d_pending_flush();
        /* one draw at a time: a submission's draws, hundreds in one batch, pair with nvn_direct's as they come (the
           pending ones are few, each a DsState of its own) */
        while (v->at < b->n && !(one_draw && v->npend && v->pend[v->npend - 1].serial == last))
            gpu_verify_word(v->ch, b->w[v->at++]);
        r3d_pending_flush(); /* the held draw is this batch's */
        g_v.decoding = NULL;
        if (v->at == b->n) {
            free(b);
            v->cur = NULL;
        }
    }
}

/* drops the pending draws of submissions before serial: the shadow drew them, the live replay didn't */
static void drop_before(VQueue* v, u32 serial) {
    u32 k = 0;
    while (k < v->npend && v->pend[k].serial < serial) k++;
    if (!k) return;
    v->shadow_only += k;
    if (v->shadow_only <= 4 || !(v->shadow_only & (v->shadow_only - 1)))
        LOG("nvn verify: queue %u submission %u: the driver drew %u more times than nvn_direct (%llu such draws)\n",
            (u32)(v - g_v.q) + 1, v->pend[0].serial, k, (unsigned long long)v->shadow_only);
    memmove(v->pend, v->pend + k, (v->npend - k) * sizeof *v->pend);
    v->npend -= k;
}

void nvn_verify_replay_submit(u32 queue) {
    VQueue* v = vqueue(queue);
    if (!g_v.on || !v) return;
    v->live_serial++;
    v->draw = 0;
    decode(v, v->live_serial - 1, 0); /* every batch before this submission's */
    drop_before(v, v->live_serial);
}

/* ---- the comparison */
typedef struct {
    u32 queue, serial, draw;
} At;
static void differ(const At* at, const char* what, u32 i, u64 a, u64 b, int fl) {
    DiffKey* d = NULL;
    u32 k;
    for (k = 0; k < g_v.ndiff; k++)
        if (g_v.diff[k].what == what) d = &g_v.diff[k];
    if (!d && g_v.ndiff < DIFF_KEYS) {
        d = &g_v.diff[g_v.ndiff++];
        d->what = what;
    }
    if (!d) return;
    d->n++;
    if (d->n == 1) {
        if (fl) {
            float x, y;
            u32 xa = (u32)a, xb = (u32)b;
            memcpy(&x, &xa, 4);
            memcpy(&y, &xb, 4);
            WARN("nvn verify: queue %u submission %u draw %u: %s[%u] nvn_direct %g, driver %g\n", at->queue,
                 at->serial, at->draw, what, i, (double)x, (double)y);
        } else
            WARN("nvn verify: queue %u submission %u draw %u: %s[%u] nvn_direct %llx, driver %llx\n", at->queue,
                 at->serial, at->draw, what, i, (unsigned long long)a, (unsigned long long)b);
    } else if (d->n >= 1024 && !(d->n & (d->n - 1)) && __builtin_ctzll(d->n) % 2 == 0)
        LOG("nvn verify: %s differed in %llu draws\n", what, (unsigned long long)d->n);
}
#define CMP(what, i, x, y)                                                                                             \
    do {                                                                                                               \
        if ((u64)(x) != (u64)(y)) differ(at, what, (u32)(i), (u64)(x), (u64)(y), 0), same = 0;                        \
    } while (0)
#define CMPF(what, i, x, y)                                                                                            \
    do {                                                                                                               \
        u32 xa_, xb_;                                                                                                  \
        float fx_ = (x), fy_ = (y);                                                                                    \
        memcpy(&xa_, &fx_, 4);                                                                                         \
        memcpy(&xb_, &fy_, 4);                                                                                         \
        if (xa_ != xb_) differ(at, what, (u32)(i), xa_, xb_, 1), same = 0;                                             \
    } while (0)
/* GPU addresses: the guest bytes they name, a shadow range's as the live memory it shadows; one the page table
   doesn't map, or the driver's own memory, isn't compared */
#define CMPA(what, i, x, y)                                                                                            \
    do {                                                                                                               \
        u64 xa_ = (x), xb_ = (y), ga_, gb_, lb_;                                                                       \
        if (!xa_ || !xb_) {                                                                                            \
            CMP(what, i, xa_, xb_);                                                                                    \
            break;                                                                                                     \
        }                                                                                                              \
        ga_ = gmmu_translate(xa_), gb_ = gmmu_translate(xb_);                                                          \
        lb_ = gb_ ? mem_live(gb_) : 0;                                                                                 \
        if (ga_ && gb_ && !lb_) lb_ = gb_;                                                                             \
        if (ga_ && lb_ && ga_ != lb_) differ(at, what, (u32)(i), ga_, lb_, 0), same = 0;                               \
    } while (0)

static int compare(const At* at, const DsState* l, const DrawCall* ld, const DsState* s, const DrawCall* sd) {
    int same = 1;
    u32 i, j;
    /* programs */
    /* VertexB runs whatever its bit says (draw_state.h), and the driver leaves it 0 */
    CMP("programs.on", 0, l->prog.on & ~2u, s->prog.on & ~2u);
    for (i = 0; i < DS_PROGRAM_SLOTS; i++)
        if (((l->prog.on & s->prog.on) | 2) >> i & 1) CMPA("programs.code", i, l->prog.code[i], s->prog.code[i]);
    CMP("programs.clip_enable", 0, l->prog.clip_enable, s->prog.clip_enable);
    CMP("programs.tess_params", 0, l->prog.tess_params, s->prog.tess_params);
    CMP("programs.early_z", 0, l->prog.early_z, s->prog.early_z);
    CMP("programs.xfb_on", 0, l->prog.xfb_on, s->prog.xfb_on);
    /* vertex format */
    CMP("vfmt.constant", 0, l->vfmt.constant, s->vfmt.constant);
    for (i = 0; i < DS_ATTRS; i++) {
        if ((l->vfmt.constant & s->vfmt.constant) >> i & 1) continue;
        CMP("vfmt.stream", i, l->vfmt.stream[i], s->vfmt.stream[i]);
        CMP("vfmt.size", i, l->vfmt.size[i], s->vfmt.size[i]);
        CMP("vfmt.type", i, l->vfmt.type[i], s->vfmt.type[i]);
        CMP("vfmt.offset", i, l->vfmt.offset[i], s->vfmt.offset[i]);
        CMP("vfmt.is_int", i, l->vfmt.is_int >> i & 1, s->vfmt.is_int >> i & 1);
        CMP("vfmt.bgra", i, l->vfmt.bgra >> i & 1, s->vfmt.bgra >> i & 1);
    }
    CMP("vfmt.stream_on", 0, l->vfmt.stream_on, s->vfmt.stream_on);
    for (i = 0; i < DS_STREAMS; i++)
        if ((l->vfmt.stream_on & s->vfmt.stream_on) >> i & 1) {
            CMP("vfmt.stride", i, l->vfmt.stride[i], s->vfmt.stride[i]);
            CMP("vfmt.instanced", i, l->vfmt.instanced >> i & 1, s->vfmt.instanced >> i & 1);
            if (l->vfmt.instanced >> i & 1) CMP("vfmt.divisor", i, l->vfmt.divisor[i], s->vfmt.divisor[i]);
            CMPA("vbuf.addr", i, l->vbuf.addr[i], s->vbuf.addr[i]);
            CMP("vbuf.size", i, l->vbuf.size[i], s->vbuf.size[i]);
        }
    /* targets */
    CMP("targets.count", 0, l->tgt.count, s->tgt.count);
    CMP("targets.srgb_write", 0, l->tgt.srgb_write, s->tgt.srgb_write);
    CMP("targets.msaa", 0, l->tgt.msaa, s->tgt.msaa);
    for (i = 0; i <= DS_TARGETS_MAX; i++) {
        const DsSurf* a = i < DS_TARGETS_MAX ? &l->tgt.color[i] : &l->tgt.depth;
        const DsSurf* b = i < DS_TARGETS_MAX ? &s->tgt.color[i] : &s->tgt.depth;
        if (i < DS_TARGETS_MAX && (i >= l->tgt.count || i >= s->tgt.count)) continue;
        CMPA("targets.gva", i, a->gva, b->gva);
        if (!a->gva || !b->gva) continue;
        CMP("targets.width", i, a->width, b->width);
        CMP("targets.height", i, a->height, b->height);
        CMP("targets.format", i, a->format, b->format);
        CMP("targets.mem", i, a->mem, b->mem);
        CMP("targets.layers", i, a->layers ? a->layers : 1, b->layers ? b->layers : 1);
        if ((a->layers > 1 || b->layers > 1)) CMP("targets.layer_stride", i, a->layer_stride, b->layer_stride);
        /* blending and the color write, of the bound targets */
        if (i < DS_TARGETS_MAX) {
            CMP("blend.mask", i, l->blend.mask[i], s->blend.mask[i]);
            CMP("blend.en", i, l->blend.en[i], s->blend.en[i]);
            if (l->blend.en[i] && s->blend.en[i]) {
                CMP("blend.cop", i, l->blend.cop[i], s->blend.cop[i]);
                CMP("blend.sc", i, l->blend.sc[i], s->blend.sc[i]);
                CMP("blend.dc", i, l->blend.dc[i], s->blend.dc[i]);
                CMP("blend.aop", i, l->blend.aop[i], s->blend.aop[i]);
                CMP("blend.sa", i, l->blend.sa[i], s->blend.sa[i]);
                CMP("blend.da", i, l->blend.da[i], s->blend.da[i]);
            }
        }
    }
    CMP("blend.logic_op", 0, l->blend.logic_op, s->blend.logic_op);
    /* rasterization */
    CMP("raster.polygon", 0, l->rkey.polygon, s->rkey.polygon);
    CMP("raster.provoking_last", 0, l->rkey.provoking_last, s->rkey.provoking_last);
    CMP("raster.depth_clamp", 0, l->rkey.depth_clamp, s->rkey.depth_clamp);
    CMP("raster.alpha_to_coverage", 0, l->rkey.alpha_to_coverage, s->rkey.alpha_to_coverage);
    CMP("raster.alpha_to_one", 0, l->rkey.alpha_to_one, s->rkey.alpha_to_one);
    if (ld->topology == 14) CMP("raster.patch_vertices", 0, l->rkey.patch_vertices, s->rkey.patch_vertices);
    CMP("raster.sample_mask", 0, l->rkey.sample_mask, s->rkey.sample_mask);
    /* the enable alone: the 3D engine's registers don't show the dilation (maxwell/draw_state.c mx_draw_raster_key),
       and the sample shading goes into the driver's program, not a register the shadow's draw state reads */
    CMP("raster.conservative", 0, l->rkey.conservative != 0, s->rkey.conservative != 0);
    /* dynamic state */
    CMP("dynamic.cull", 0, l->dyn.cull, s->dyn.cull);
    CMP("dynamic.front_cw", 0, l->dyn.front_cw, s->dyn.front_cw);
    CMP("dynamic.discard", 0, l->dyn.discard, s->dyn.discard);
    CMP("dynamic.bias_on", 0, l->dyn.bias_on, s->dyn.bias_on);
    if (l->tgt.depth.gva && s->tgt.depth.gva) {
        CMP("dynamic.depth_test", 0, l->dyn.depth_test, s->dyn.depth_test);
        CMP("dynamic.depth_write", 0, l->dyn.depth_write, s->dyn.depth_write);
        if (l->dyn.depth_test) CMP("dynamic.depth_cmp", 0, l->dyn.depth_cmp, s->dyn.depth_cmp);
        CMP("dynamic.stencil_test", 0, l->dyn.stencil_test, s->dyn.stencil_test);
        if (l->dyn.stencil_test)
            for (i = 0; i < 2; i++) {
                CMP("dynamic.stencil.fail", i, l->dyn.face[i].fail, s->dyn.face[i].fail);
                CMP("dynamic.stencil.zfail", i, l->dyn.face[i].zfail, s->dyn.face[i].zfail);
                CMP("dynamic.stencil.pass", i, l->dyn.face[i].pass, s->dyn.face[i].pass);
                CMP("dynamic.stencil.cmp", i, l->dyn.face[i].cmp, s->dyn.face[i].cmp);
                CMP("dynamic.stencil.ref", i, l->dyn.face[i].ref & 0xFF, s->dyn.face[i].ref & 0xFF);
                CMP("dynamic.stencil.cmask", i, l->dyn.face[i].cmask & 0xFF, s->dyn.face[i].cmask & 0xFF);
                CMP("dynamic.stencil.wmask", i, l->dyn.face[i].wmask & 0xFF, s->dyn.face[i].wmask & 0xFF);
            }
    }
    CMP("dynamic.alpha_test", 0, l->dyn.alpha_test, s->dyn.alpha_test);
    if (l->dyn.alpha_test) {
        CMP("dynamic.alpha_func", 0, l->dyn.alpha_func, s->dyn.alpha_func);
        CMPF("dynamic.alpha_ref", 0, l->dyn.alpha_ref, s->dyn.alpha_ref);
    }
    CMP("dynamic.window_origin", 0, l->dyn.window_origin, s->dyn.window_origin);
    CMP("dynamic.sprite_origin", 0, l->dyn.sprite_origin, s->dyn.sprite_origin);
    CMP("dynamic.depth_mode", 0, l->dyn.depth_mode, s->dyn.depth_mode);
    CMP("dynamic.point_nudge", 0, l->dyn.point_nudge, s->dyn.point_nudge);
    CMPF("dynamic.line_width", 0, l->dyn.line_width, s->dyn.line_width);
    if (l->dyn.bias_on) {
        CMPF("dynamic.bias_units", 0, l->dyn.bias_units, s->dyn.bias_units);
        CMPF("dynamic.bias_slope", 0, l->dyn.bias_slope, s->dyn.bias_slope);
        CMPF("dynamic.bias_clamp", 0, l->dyn.bias_clamp, s->dyn.bias_clamp);
    }
    for (i = 0; i < 4; i++) CMPF("dynamic.blend_const", i, l->dyn.blend_const[i], s->dyn.blend_const[i]);
    if (l->dyn.window_origin & DS_ORIGIN_LOWER_LEFT) CMP("dynamic.clip_height", 0, l->dyn.clip_height, s->dyn.clip_height);
    CMP("dynamic.depth_word", 0, l->dyn.depth_word, s->dyn.depth_word);
    /* viewports: the first, and those nvn_direct scissors. The driver sets all 16 at its start, with full scissors,
       and nvn_direct only those the game sets: the others draw nothing unless a shader picks its viewport */
    CMP("viewports.transform_on", 0, l->vp.transform_on, s->vp.transform_on);
    CMP("viewports.scissor_on", 0, l->vp.scissor_on, s->vp.scissor_on & (l->vp.scissor_on | 1));
    for (i = 0; i < DS_VIEWPORTS_MAX; i++) {
        if (i && !(l->vp.scissor_on >> i & 1)) continue;
        for (j = 0; j < 3; j++) {
            CMPF("viewports.scale", i * 3 + j, l->vp.scale[i][j], s->vp.scale[i][j]);
            CMPF("viewports.translate", i * 3 + j, l->vp.translate[i][j], s->vp.translate[i][j]);
        }
        CMP("viewports.swizzle", i, l->vp.swizzle[i], s->vp.swizzle[i]);
        CMPF("viewports.z_near", i, l->vp.z_near[i], s->vp.z_near[i]);
        CMPF("viewports.z_far", i, l->vp.z_far[i], s->vp.z_far[i]);
        if ((l->vp.scissor_on & s->vp.scissor_on) >> i & 1) {
            CMP("viewports.scissor_x", i, l->vp.scissor_x[i], s->vp.scissor_x[i]);
            CMP("viewports.scissor_y", i, l->vp.scissor_y[i], s->vp.scissor_y[i]);
        }
    }
    /* the index buffer */
    if (ld->indexed == 1) {
        CMPA("index.addr", 0, l->index.addr, s->index.addr);
        CMP("index.format", 0, l->index.format, s->index.format);
        CMP("index.restart", 0, l->index.restart, s->index.restart);
        if (l->index.restart) CMP("index.restart_index", 0, l->index.restart_index, s->index.restart_index);
    }
    /* bindings: the constant buffers in memory, the pools */
    for (i = 0; i < DS_GFX_STAGES; i++) {
        if (!(l->prog.on >> (i + 1) & 1) && i) continue;
        for (j = 0; j < DS_CBUFS; j++) {
            u8 lk = l->bind.cb_inline[i][j];
            if (lk == DS_CB_MEMORY) {
                CMPA("bind.cb_addr", i * DS_CBUFS + j, l->bind.cb_addr[i][j], s->bind.cb_addr[i][j]);
                if (l->bind.cb_addr[i][j] && s->bind.cb_addr[i][j])
                    CMP("bind.cb_size", i * DS_CBUFS + j, l->bind.cb_size[i][j], s->bind.cb_size[i][j]);
            } else if (s->bind.cb_addr[i][j]) { /* an inline bank against the driver's bank in memory */
                const u8* bank = lk == DS_CB_BANK0 ? l->inl.bank0[i] : l->inl.bank2[i];
                u32 len = lk == DS_CB_BANK0 ? DS_INLINE_BANK0 : DS_INLINE_BANK2, off;
                for (off = 0; off < len; off += 4) {
                    u32 sv, lv;
                    if (!cb_word(s->bind.cb_addr[i][j] + off, &sv)) continue;
                    memcpy(&lv, bank + off, 4);
                    if (lv != sv) {
                        differ(at, lk == DS_CB_BANK0 ? "inline.bank0" : "inline.bank2", i * 0x10000 + off, lv, sv, 0);
                        same = 0;
                        break;
                    }
                }
            }
        }
    }
    CMPA("bind.tex_pool", 0, l->bind.tex_pool, s->bind.tex_pool);
    CMPA("bind.smp_pool", 0, l->bind.smp_pool, s->bind.smp_pool);
    CMP("bind.tex_max", 0, l->bind.tex_max, s->bind.tex_max);
    CMP("bind.smp_max", 0, l->bind.smp_max, s->bind.smp_max);
    CMP("bind.smp_mode", 0, l->bind.smp_mode, s->bind.smp_mode);
    CMP("bind.tex_bank", 0, l->bind.tex_bank, s->bind.tex_bank);
    /* the draw */
    CMP("draw.topology", 0, ld->topology, sd->topology);
    CMP("draw.indexed", 0, ld->indexed, sd->indexed);
    if (!ld->indirect) {
        CMP("draw.first", 0, ld->first, sd->first);
        CMP("draw.count", 0, ld->count, sd->count);
        CMP("draw.base_vertex", 0, ld->base_vertex, sd->base_vertex);
        CMP("draw.base_instance", 0, ld->base_instance, sd->base_instance);
        CMP("draw.instances", 0, ld->instances, sd->instances);
    }
    return same;
}

void nvn_verify_live_draw(const DsState* ds, const DrawCall* dc) {
    VQueue* v;
    At at;
    if (!g_v.on || !(ds->owner & 0x80000000u)) return;
    v = vqueue((ds->owner & 0x7FFFFFFFu) + 1);
    if (!v) return;
    v->draw++;
    drop_before(v, v->live_serial);
    if (!v->npend || v->pend[0].serial != v->live_serial) decode(v, v->live_serial, 1);
    drop_before(v, v->live_serial);
    at.queue = (u32)(v - g_v.q) + 1;
    at.serial = v->live_serial;
    at.draw = v->draw;
    if (!v->npend || v->pend[0].serial != v->live_serial) {
        v->live_only++;
        if (v->live_only <= 4 || !(v->live_only & (v->live_only - 1)))
            LOG("nvn verify: queue %u submission %u draw %u: nvn_direct drew, the driver didn't (%llu such draws)\n",
                at.queue, at.serial, at.draw, (unsigned long long)v->live_only);
        return;
    }
    v->compared++;
    if (compare(&at, ds, dc, &v->pend[0].ds, &v->pend[0].dc)) v->matched++;
    memmove(v->pend, v->pend + 1, (v->npend - 1) * sizeof *v->pend);
    v->npend--;
    if (v->compared == 1 || (v->compared >= 1024 && !(v->compared & (v->compared - 1)) &&
                             __builtin_ctzll(v->compared) % 2 == 0))
        LOG("nvn verify: queue %u: %llu draws compared, %llu the same; %llu only nvn_direct's, %llu only the "
            "driver's; %llu shadow calls skipped for unknown objects, %llu for unknown states, %llu lossy states\n",
            at.queue, (unsigned long long)v->compared, (unsigned long long)v->matched,
            (unsigned long long)v->live_only, (unsigned long long)v->shadow_only,
            (unsigned long long)g_v.unknown_obj, (unsigned long long)g_v.unknown_state,
            (unsigned long long)g_v.lossy);
}
