/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vk_gpu_profile=1: where the GPU's time goes. A scene can be bound by the GPU while every CPU thread idles (one
   game's lookout ran at 6 frames a second with the GPU 91% busy and no thread past 20%), and the CPU's profilers can't
   say which work it is. Each rendering pass of the frame's command buffers gets a timestamp at its start and its end,
   with its first color target (or its depth target), its draws, and what was recorded between it and the pass before
   (copies by kind and bytes, barriers, dispatches: record.c counts them). When the frame's work is done
   (frame_recycle) the times are read, and every 10 s (a slow scene takes minutes for 600 presents) the log gets the
   frame's span, the time in passes, the time in the gaps between them by what they held, and the targets that took the
   most. That is how 955 MB of buffer copies a frame between passes were found. Off, it costs a branch at each pass.

   The graphics queue's idle time comes with it. Every command buffer gets a timestamp at its start and its end, with
   the CPU's time (CLOCK_MONOTONIC) of its submit and why cmd.c submitted it. The device's clock is put next to the
   CPU's with VK_EXT_calibrated_timestamps (Windows has no CLOCK_MONOTONIC: the performance counter there, plat_ns's
   clock). Then the time between the end of one submission and the start of the next splits into the time before the
   CPU submitted the next one (the GPU had nothing to do) and the time after it (a semaphore wait, the recording
   thread). The first part goes by what the CPU did: it waited for the submission that ended (a finish, a readback, a
   release's gate: a round trip), it was between frames (after the present's submission), or it recorded slower than
   the GPU ran.

   The stats box (settings.txt's stats) takes only the command buffers' timestamps, and their sum: the graphics queue's
   busy time, from the slot's results read at its reuse like the rest. Off with the profile off too, nothing is written
   or read. */
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "common/gpu/vk/vk_common.h"
#include "nx/gpu/vk/gpuprof_gap.h"
#include "nx/gpu/vk/gpuprof_setting.h"
#include "nx/gpu/vk/gpuprof_time.h"
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

#define GP_QUERIES 16384 /* timestamps per frame slot: two per pass, two per dispatch */
#define GP_DISP    4096  /* dispatches timed per frame slot */
#define GP_KEYS    1024 /* targets summed over 600 presents */

typedef struct {
    u64 gva;
    u32 w, h, fmt, draws;
    u32 xfers, barriers, dispatches; /* recorded since the pass before it ended: what the gap before it holds */
    u32 kind[3];
    u64 bytes;
    u16 q0, q1; /* the Tracy query ids its begin and end went out with, 0 when it emitted none */
    u64 ep; /* the connection the begin went out on: an end or a time from another one is dropped */
    u32 qi; /* its begin's timestamp in the slot's pool, its end's the one after */
} GpPass;
#define GP_SUBS 512 /* submissions per frame slot: cmd.c's slots take at most 128 */
typedef struct {
    u32 q0, q1; /* its begin's and its end's timestamps, q1 0 while it is not submitted */
    /* what it holds: passes, dispatches and copies recorded into it, the counts at its begin until its end */
    u32 passes, dispatches;
    u64 xfers;
    u64 cpu;    /* CLOCK_MONOTONIC at its submit */
    u64 cpu0;   /* and at its begin: from then on it held recorded work */
    u64 value;  /* its value on the graphics timeline */
    int why;    /* VK_SUB_* */
    /* the command buffer its begin went in: an end skipped while timing was off (stats turned off and on again
       within a slot's use) must not close it with a later buffer's end */
    VkCommandBuffer cb;
} GpSub;
typedef struct {
    GpOps ops;
    u32 npass, nsub;
    GpPass pass[GP_QUERIES / 2];
    GpSub sub[GP_SUBS];
    GpOp op[GP_DISP];
} GpSlot;
typedef struct {
    u64 gva;
    u32 w, h, fmt;
    u64 ns, passes, draws;
} GpKey;

/* The slow passes, by what they had: their target and the gap before them (copies and dispatches; a barrier is an
   ordinary part of a pass). A pass that costs milliseconds at a few thousand pixels is a stall or a gap, not its
   pixels, and this says which it was: the target and its size are the pixels, the gap is the stall, and the draws are
   what the CPU asked the GPU to do. Each entry sums the passes of one shape, and keeps the most any one of them had.
   Only passes of 0.3 ms or more are kept: a pass of a tenth of a millisecond says nothing about a frame. The table
   keeps the mean time of a pass, not the window's total for it, since a stall of one frame is what this looks for.
   A pass that runs a thousand times and stays cheap is the other table's line (g_key, summed per target) */
#define GP_SLOW 8
typedef struct {
    u64 gva, ns, n;
    u32 w, h, fmt;
    u32 gap; /* what the gap before it held: 1 copies, 2 dispatches */
    u32 draws, xfers, barriers, dispatches; /* the most any one of them had */
} GpSlow;
static GpSlow g_slow[GP_SLOW];
static void gp_slow(const GpPass* p, u64 ns) {
    u32 worst = 0;
    const u32 gap = (p->xfers != 0) | (p->dispatches != 0) << 1;
    if (ns < 300000) return;
    for (u32 i = 0; i < GP_SLOW; i++) { /* the same pass again: summed with it, and its worst kept */
        GpSlow* s = &g_slow[i];
        if (!s->n || s->gva != p->gva || s->w != p->w || s->h != p->h || s->fmt != p->fmt || s->gap != gap) continue;
        s->ns += ns, s->n++;
        if (p->draws > s->draws) s->draws = p->draws;
        if (p->xfers > s->xfers) s->xfers = p->xfers;
        if (p->barriers > s->barriers) s->barriers = p->barriers;
        if (p->dispatches > s->dispatches) s->dispatches = p->dispatches;
        return;
    }
    for (u32 i = 0; i < GP_SLOW; i++) { /* the least of them, an empty one first, gives way to a pass that costs more */
        const GpSlow* s = &g_slow[i];
        if (!s->n) {
            worst = i;
            break;
        }
        if (!g_slow[worst].n || s->ns / s->n < g_slow[worst].ns / g_slow[worst].n) worst = i;
    }
    if (g_slow[worst].n && ns <= g_slow[worst].ns / g_slow[worst].n) return;
    g_slow[worst] = (GpSlow){.gva = p->gva,
                             .ns = ns,
                             .n = 1,
                             .w = p->w,
                             .h = p->h,
                             .fmt = p->fmt,
                             .gap = gap,
                             .draws = p->draws,
                             .xfers = p->xfers,
                             .barriers = p->barriers,
                             .dispatches = p->dispatches};
}
/* The value in effect, -1 until it is worked out, and what it was worked out for: log's value may change during a
   run, and the GPU thread and the recording thread both ask. Read relaxed, as the stats box's setting is */
static _Atomic int g_on = -1;
static _Atomic int g_asked = -1;
static GpSlot* g_slot[VK_FRAMES];
static GpKey g_key[GP_KEYS];
static u64 g_pass_ns, g_span_ns, g_frames, g_lost;
static u64 g_draws_at_begin;
static void gp_ops_done(GpOps* o, const u64* ts, double period, int ctx);
/* the gaps between passes by what was recorded in them: dispatches, else copies, else barriers, else nothing (the
   GPU waiting for the next submission or a semaphore) */
static const char* const g_gap_name[4] = {"with dispatches", "with copies", "with barriers only", "with nothing"};
/* the same gaps and idle sums as plots, for a --tracy run whose vk_gpu_profile leaves the log lines out */
static const char* const g_gap_plot[4] = {"gpu gap: dispatches", "gpu gap: copies", "gpu gap: barriers",
                                          "gpu gap: nothing"};
static u64 g_gap_ns[4], g_gaps[4];
static u64 g_x_at_end, g_b_at_end, g_d_at_end, g_k_at_end[3], g_bytes_at_end;
/* the copy gaps by their mix: buffer copies only, with buffer-image copies, with image copies or blits */
static const char* const g_mix_name[3] = {"buffer copies only", "with buffer-image copies", "with image copies"};
static u64 g_mix_ns[3], g_mixes[3], g_mix_bytes[3], g_mix_n[3];
static int g_in_pass;
static int g_dead; /* the query pool was refused once: stop asking */
/* vk_gpuprof_busy_ns's sum, and the device's tick at the latest end counted in it while the slot uses read follow one
   another (ended) */
static struct {
    _Atomic u64 ns;
    u64 end;
    int ended;
} g_stats_busy;
u64 g_vk_timestamps, g_vk_timestamps_read;
/* the graphics queue's idle time between submissions, summed until the report: VK_SUB_* by what the CPU did */
enum { IDLE_WAITED, IDLE_FRAME, IDLE_RECORDING, IDLE_QUEUED, IDLE_N };
static const char* const g_idle_name[IDLE_N] = {
    "the CPU waited for the submission that ended (a round trip)", "the CPU between frames (after a present)",
    "the CPU still recording the next one", "submitted, not started (semaphores, the recording thread)"};
static const char* const g_idle_plot[IDLE_N] = {"gpu idle: waited", "gpu idle: frame", "gpu idle: recording",
                                                "gpu idle: queued"};
static u64 g_idle_ns[IDLE_N], g_idles[IDLE_N], g_busy_ns, g_subs, g_sub_why[VK_SUB_N];
static u64 g_round_ns[VK_WAIT_N]; /* IDLE_WAITED by the kind of the CPU's wait */
/* The idle time before a submission while it already held recorded work (from its begin to its submit): what
   submitting its commands as they were recorded could have run, up to the submission's own GPU time, by its VK_SUB_*,
   and the part of it before a submission the CPU then waited for (a round trip that much shorter) */
static u64 g_fill_ns[VK_SUB_N], g_fill_waited_ns;
static u64 g_idle_subs_all, g_fill_all_ns; /* the submissions gp_idle placed and their fill since the start (tests) */
void vk_gpuprof_fill_totals(u64* subs, u64* fill_ns) {
    *subs = g_idle_subs_all;
    *fill_ns = g_fill_all_ns;
}
/* The busy time by what the submissions held: rendering passes, else dispatches, else copies, else nothing. Most of
   one game's submissions held no pass, so the passes' times said nothing of most of the GPU's */
enum { HOLD_PASSES, HOLD_DISPATCHES, HOLD_COPIES, HOLD_NOTHING, HOLD_N };
static const char* const g_hold_name[HOLD_N] = {"rendering passes", "dispatches and no pass", "copies only", "nothing"};
static u64 g_hold_ns[HOLD_N], g_holds[HOLD_N];
/* The dispatches recorded into the frame's command buffers, timed each, summed by tag (vk_gpuprof_tag) and by where
   they were asked for (vk_gpuprof_tag_at), and the transfers by where they were recorded */
#define GP_DKEYS 512
typedef struct {
    u64 key, ns, n, bytes;
    i64 site; /* GpOp's */
    u32 kind;
} GpDKey;
static GpDKey g_dkey[GP_DKEYS];
static u64 g_disp_ns, g_disps;
static VkCommandBuffer g_cur_cb;  /* the frame's command buffer being recorded, between cb_begin and cb_end */
static u64 g_tag, g_dispatched;   /* the next dispatch's tag, and the dispatches recorded into g_cur_cb */
static i64 g_tag_site;            /* the next dispatch's site (vk_gpuprof_tag_at), 0 none */
static u64 g_last_end;            /* CLOCK_MONOTONIC of the last submission's end, 0 none */
static int g_prev_why, g_prev_kind; /* that submission's VK_SUB_*, and the CPU's wait for it (VK_WAIT_* + 1, 0 none) */
static u64 g_t0;        /* the report window's start (plat_ns) */
/* the CPU's clock the idle times are on, CLOCK_MONOTONIC (0 where there is none) or the performance counter's */
static u64 gp_host_ns(void) { return g_vk.has_calibrated_qpc ? plat_ns() : plat_monotonic_ns(); }
static u32 g_present0; /* and g_vk_present then */
/* the timeline values the CPU waited for, with the kind of wait (VK_WAIT_* + 1), by value modulo the ring */
#define GP_WAITS 1024
static struct {
    u64 value;
    int kind;
} g_waited[GP_WAITS];

/* Tracy's GPU track: the passes as zones on the GPU's clock, their timestamps read back with the frame's results,
   the emissions themselves in the platform layer (tracy.c). A profiling run asks for them: the setting, or the
   client being up. A new connection's server re-anchors at the replayed context's original pair, so the caller's
   CPU delta is measured from that pair again (SnxTracyGpuCal). */
static SnxTracyGpuCal g_cal; /* the announcement's anchor, and the connection it belongs to */

int vk_gpuprof_calibrated(void) {
    return g_vk.has_calibrated_host && g_vk.props.limits.timestampComputeAndGraphics;
}

static int gp_on(void) {
    /* log=3's counters are the same work as the setting's: the CPU's per resource, and the GPU's a pass */
    int by_log = g_settings.log_level >= 3, asked = g_settings.gpu_profile || by_log;
    if (atomic_load_explicit(&g_on, memory_order_relaxed) < 0 ||
        asked != atomic_load_explicit(&g_asked, memory_order_relaxed)) {
        /* log's value may change during a run (it is not a startup setting) */
        char say[640], by[32];
        if (by_log && !g_settings.gpu_profile)
            snprintf(by, sizeof by, "log=%d", g_settings.log_level);
        else
            snprintf(by, sizeof by, "vk_gpu_profile=%d", g_settings.gpu_profile);
        atomic_store_explicit(&g_asked, asked, memory_order_relaxed);
        atomic_store_explicit(&g_on,
                              vk_gpuprof_setting(asked, by, g_vk.props.limits.timestampComputeAndGraphics,
                                                 g_vk.has_portability, g_vk.props.deviceName, say, sizeof say),
                              memory_order_relaxed);
        if (say[0]) WARN("%s\n", say);
    }
    if (g_dead) return 0;
    /* The client's track without the setting. Attached, not merely started: on-demand Tracy drops the GPU events
       of a client no server has connected to, so a --tracy run waiting for a GUI would otherwise pay a query pool
       reset and two timestamps for every pass, a read-back at each recycle, and keep none of it. */
    if (snx_tracy_attached()) return g_vk.props.limits.timestampComputeAndGraphics;
    return atomic_load_explicit(&g_on, memory_order_relaxed);
}
int vk_gpuprof_timed(void) { return g_vk.gfx_timestamp_bits && !g_dead; }
/* whether the profile is on and its report will log: gates the recorder's command-kind count, which runs on every
   command it drains */
u64 vk_gpuprof_busy_ns(void) { return atomic_load_explicit(&g_stats_busy.ns, memory_order_relaxed); }
/* the frame's command buffers timed: for the profile (gp_on), or for the stats box, which shows */
static int gp_timed(void) {
    return gp_on() ||
           (atomic_load_explicit((_Atomic int*)&g_settings.stats, memory_order_relaxed) && vk_gpuprof_timed());
}
/* a timestamp into the slot's pool, counted */
static void gp_stamp(VkCommandBuffer cb, VkPipelineStageFlags2 stage, VkQueryPool pool, u32 query) {
    rec_CmdWriteTimestamp2(cb, stage, pool, query);
    g_vk_timestamps++;
}
static GpSlot* gp_slot(void) {
    u32 s = vk_frame_slot();
    if (!g_slot[s]) {
        /* A driver may refuse a pool this size, and its refusal turned the whole profile off for the run: the size is
           halved until one is taken, and a smaller pool times fewer passes a frame rather than none. MoltenVK takes a
           pool larger than its Metal counter sample buffer (32768 bytes, 4096 timestamps) and then emulates every
           timestamp, which reads as 0.00 ms a pass: a portability driver starts at 4096 */
        u32 count = g_vk.has_portability ? 4096 : GP_QUERIES;
        GpSlot* g = calloc(1, sizeof *g);
        VkResult r = VK_ERROR_OUT_OF_HOST_MEMORY, refused = VK_SUCCESS;
        for (; g && count >= 256; count /= 2) {
            VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                        .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                        .queryCount = count};
            r = vkCreateQueryPool(g_vk.dev, &qi, NULL, &g->ops.pool);
            if (r == VK_SUCCESS) break;
            refused = r; /* said once, with the size taken; with none, VK_OK below says the last */
        }
        if (!g || !VK_OK(r)) {
            free(g);
            g_dead = 1;
            return NULL;
        }
        g->ops.op = g->op;
        g->ops.cap = GP_DISP;
        g->ops.queries = count;
        if (refused != VK_SUCCESS)
            LOG("vulkan: vk_gpu_profile's pool takes %u timestamps where %u were asked for: the driver refused %u "
                "(%s)\n",
                count, (u32)GP_QUERIES, count * 2, snx_vk_result_str(refused));
        else if (count != GP_QUERIES)
            LOG("vulkan: vk_gpu_profile's pool takes %u timestamps where %u were asked for: a portability driver's "
                "Metal counter buffer holds %u\n",
                count, (u32)GP_QUERIES, count);
        g_slot[s] = g;
    }
    return g_slot[s];
}
/* the slot's pool while on, reset by cb if this is its first use since the slot was recycled: outside any pass */
static GpSlot* gp_slot_reset(VkCommandBuffer cb, int on) {
    GpSlot* g;
    if (!on || !(g = gp_slot())) return NULL;
    if (!g->ops.reset) {
        rec_CmdResetQueryPool(cb, g->ops.pool, 0, g->ops.queries);
        g->ops.reset = 1;
        g->ops.used = g->npass = g->nsub = g->ops.nop = 0;
    }
    return g;
}
/* a command buffer of the frame begun (cmd.c's vk_cmd) */
void vk_gpuprof_cb_begin(VkCommandBuffer cb) {
    GpSlot* g = gp_slot_reset(cb, gp_timed());
    if (!g) return;
    if (g->ops.used + 1 > g->ops.queries || g->nsub >= GP_SUBS) {
        g_lost++;
        return;
    }
    g->sub[g->nsub] = (GpSub){.q0 = g->ops.used,
                              .passes = g->npass,
                              .dispatches = (u32)g_dispatched,
                              .xfers = g_rec_xfers,
                              .cb = cb,
                              .cpu0 = gp_host_ns()};
    gp_stamp(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, g->ops.pool, g->ops.used++);
    g->nsub++;
    g_cur_cb = cb;
}
/* the command buffer ends: submitted for why (VK_SUB_*) as the timeline's value */
void vk_gpuprof_cb_end(VkCommandBuffer cb, int why, u64 value) {
    GpSlot* g = gp_timed() ? g_slot[vk_frame_slot()] : NULL;
    GpSub* s;
    if (!g || !g->ops.reset || !g->nsub || (s = &g->sub[g->nsub - 1])->q1 || s->cb != cb) return;
    if (g->ops.used + 1 > g->ops.queries) {
        g_lost++;
        g->nsub--; /* no end: its begin is never read */
        return;
    }
    s->q1 = g->ops.used;
    s->passes = g->npass - s->passes;
    s->dispatches = (u32)g_dispatched - s->dispatches;
    s->xfers = g_rec_xfers - s->xfers;
    g_cur_cb = VK_NULL_HANDLE;
    s->cpu = gp_host_ns();
    s->value = value;
    s->why = why;
    gp_stamp(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, g->ops.pool, g->ops.used++);
}
static struct {
    VkCommandBuffer cb;
    GpOps* ops;
} g_cq;
void vk_gpuprof_tag(u64 key) {
    g_tag = key;
    g_tag_site = 0;
}
void vk_gpuprof_tag_at(u64 key, const void* site) {
    g_tag = key;
    g_tag_site = (i64)((const u8*)site - (const u8*)vk_gpuprof_base());
}
int vk_gpuprof_profiled(VkCommandBuffer cb) {
    return cb && (cb == g_cur_cb || (cb == g_cq.cb && g_cq.ops));
}
static GpOps* gp_ops(VkCommandBuffer cb, int* ctx) {
    if (!cb) return NULL;
    if (cb == g_cur_cb) {
        GpSlot* g = g_slot[vk_frame_slot()];
        if (!g || !g->ops.reset) return NULL;
        *ctx = VK_GPU_CTX_GRAPHICS;
        return &g->ops;
    }
    if (cb == g_cq.cb && g_cq.ops) {
        *ctx = VK_GPU_CTX_COMPUTE;
        return g_cq.ops;
    }
    return NULL;
}
static const char* const g_kind_name[VK_OP_N] = {
    [VK_OP_DISPATCH] = "dispatch",     [VK_OP_COPY] = "buffer copy", [VK_OP_TO_IMAGE] = "buffer to image copy",
    [VK_OP_TO_BUFFER] = "image to buffer copy", [VK_OP_IMAGE] = "image copy", [VK_OP_DRAW] = "draw",
    [VK_OP_FILL] = "buffer fill",      [VK_OP_BLIT] = "image blit",  [VK_OP_CLEAR] = "image clear",
    [VK_OP_RESOLVE] = "image resolve"};
static int op_begin(VkCommandBuffer cb, u32 kind, u64 key, i64 site, u64 bytes) {
    GpOp* x;
    int ctx = 0;
    GpOps* o;
    if (kind == VK_OP_DRAW && !snx_tracy_attached()) return 0; /* a draw's time has no sum: only a client's track */
    if (cb == g_cur_cb && !gp_slot_reset(cb, gp_on())) return 0;
    o = gp_ops(cb, &ctx);
    if (!o) return 0;
    if (kind == VK_OP_DISPATCH) g_dispatched++;
    if (o->used + 2 > o->queries || o->nop >= o->cap) {
        if (ctx == VK_GPU_CTX_GRAPHICS) g_lost++;
        return 0;
    }
    x = &o->op[o->nop];
    x->qi = o->used;
    x->kind = kind;
    x->key = key;
    x->bytes = bytes;
    x->site = site;
    x->q0 = x->q1 = 0;
    gp_stamp(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, o->pool, o->used);
    if (snx_tracy_attached()) {
        char name[64];
        snprintf(name, sizeof name, "gpu %s %llx", g_kind_name[kind], (unsigned long long)key);
        x->q0 = snx_tracy_gpu_zone_begin(name, 0x3d8f4a, ctx, &x->ep);
    }
    return 1;
}
int vk_gpuprof_op_begin(VkCommandBuffer cb, u32 kind, u64 key, u64 bytes) { return op_begin(cb, kind, key, 0, bytes); }
void vk_gpuprof_op_end(VkCommandBuffer cb) {
    int ctx = 0;
    GpOps* o = gp_ops(cb, &ctx);
    if (!o) return;
    gp_stamp(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, o->pool, o->used + 1);
    o->used += 2;
    GpOp* x = &o->op[o->nop];
    /* the end goes out whenever the begin did: a dropped pair never reaches the server apart */
    if (x->q0) x->q1 = snx_tracy_gpu_zone_end(x->q0, ctx, x->ep);
    o->nop++;
}
int vk_gpuprof_dispatch_begin(VkCommandBuffer cb) {
    u64 key = g_tag;
    i64 site = g_tag_site;
    g_tag = 0;
    g_tag_site = 0;
    return op_begin(cb, VK_OP_DISPATCH, key, site, 0);
}
void vk_gpuprof_dispatch_end(VkCommandBuffer cb) { vk_gpuprof_op_end(cb); }
void vk_gpuprof_compute_begin(GpOps* o, VkCommandBuffer cb) {
    g_cq.cb = cb;
    g_cq.ops = gp_on() ? o : NULL;
    o->used = o->nop = 0;
    o->span0 = o->span1 = 0;
    if (!g_cq.ops) return;
    if (!o->pool) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                    .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                    .queryCount = VK_GP_QUERIES};
        if (!VK_OK(vkCreateQueryPool(g_vk.dev, &qi, NULL, &o->pool))) {
            g_dead = 1;
            g_cq.ops = NULL;
            return;
        }
    }
    rec_CmdResetQueryPool(cb, o->pool, 0, VK_GP_QUERIES);
    rec_CmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, o->pool, 0);
    o->used = 2;
    if (snx_tracy_attached())
        o->span0 = snx_tracy_gpu_zone_begin("gpu compute batch", 0x2f9e8f, VK_GPU_CTX_COMPUTE, &o->span_ep);
}
void vk_gpuprof_compute_submit(GpOps* o, VkCommandBuffer cb) {
    if (!o->used) return;
    rec_CmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, o->pool, 1);
    /* the end goes out whenever the begin did: a dropped pair never reaches the server apart */
    if (o->span0) o->span1 = snx_tracy_gpu_zone_end(o->span0, VK_GPU_CTX_COMPUTE, o->span_ep);
}
static SnxTracyGpuCal g_ccs_cal; /* the compute context's anchor, and the connection it belongs to */
static void gpc_times(const u64* ts, u32 n, u16 span0, u16 span1, u64 ep) {
    i64 last = (i64)ts[n - 1], cpu = 0, gpu = 0;
    int cal, have;
    if (!snx_tracy_attached()) return;
    cal = vk_gpuprof_calibrated();
    have = cal && vk_gpuprof_clock(&cpu, &gpu);
    snx_tracy_gpu_cal_reconnect(&g_ccs_cal);
    /* A context is announced calibrated only with a pair in hand: without one the server has no anchor and
       applies no calibration to the times sent after it, which shows as times far from the CPU's. The flag
       latches at the announcement, so a context announced without one is never sent calibration events: the
       server would assert on a context it was told is uncalibrated. */
    if (snx_tracy_gpu_context(VK_GPU_CTX_COMPUTE, have ? gpu : last, g_vk.props.limits.timestampPeriod, have,
                              "compute")) {
        snx_tracy_gpu_cal_announce(&g_ccs_cal, have, have ? cpu : 0); /* the anchor is the announcement's pair */
    }
    if (have) {
        i64 delta = snx_tracy_gpu_cal_delta(&g_ccs_cal, cpu);
        if (delta > 0) snx_tracy_gpu_calibrate(gpu, delta, VK_GPU_CTX_COMPUTE);
    } else
        snx_tracy_gpu_sync(last, VK_GPU_CTX_COMPUTE);
    if (span0 && span1) {
        snx_tracy_gpu_time((i64)ts[0], span0, VK_GPU_CTX_COMPUTE, ep);
        snx_tracy_gpu_time((i64)ts[1], span1, VK_GPU_CTX_COMPUTE, ep);
    }
}
void vk_gpuprof_compute_land(GpOps* o) {
    static u64 ts[VK_GP_QUERIES];
    if (!o->pool || !o->used) return;
    /* no wait: the batch is done, and a failed read leaves its batch out of the sums */
    if (!VK_OK(vkGetQueryPoolResults(g_vk.dev, o->pool, 0, o->used, o->used * sizeof *ts, ts, 8,
                                     VK_QUERY_RESULT_64_BIT)))
        return;
    gpc_times(ts, o->used, o->span0, o->span1, o->span_ep);
    gp_ops_done(o, ts, g_vk.props.limits.timestampPeriod, VK_GPU_CTX_COMPUTE);
}
const void* vk_gpuprof_base(void) { return (const void*)vk_gpuprof_tag; }
static GpDKey* gp_dkey(u32 kind, u64 key, i64 site) {
    u64 h = key ^ (key >> 29) ^ kind ^ (u64)site ^ ((u64)site >> 17);
    for (u32 i = 0; i < GP_DKEYS; i++) {
        GpDKey* k = &g_dkey[(h + i) & (GP_DKEYS - 1)];
        if (!k->n || (k->key == key && k->kind == kind && k->site == site)) {
            k->key = key;
            k->kind = kind;
            k->site = site;
            return k;
        }
    }
    return NULL;
}
/* the CPU waits for the timeline's value (kind: VK_WAIT_*) */
void vk_gpuprof_waited(u64 value, int kind) {
    if (gp_on() <= 0) return; /* worked out here too: the first wait after log changes is classified as well */
    g_waited[value % GP_WAITS].value = value;
    g_waited[value % GP_WAITS].kind = kind + 1;
}
/* before vkCmdBeginRendering, outside any pass */
void vk_gpuprof_pass_begin(VkCommandBuffer cb, const Surf* rt, const Surf* zt) {
    GpSlot* g = gp_slot_reset(cb, gp_on());
    if (!g) return;
    if (g->ops.used + 2 > g->ops.queries || g->npass >= GP_QUERIES / 2) {
        g_lost++;
        return;
    }
    const Surf* s = rt ? rt : zt;
    GpPass* p = &g->pass[g->npass];
    p->qi = g->ops.used;
    p->gva = s ? s->gva : 0;
    p->w = s ? s->w : 0;
    p->h = s ? s->h : 0;
    p->fmt = s ? (u32)s->f.vk : 0;
    p->xfers = (u32)(g_rec_xfers - g_x_at_end);
    p->barriers = (u32)(g_rec_barriers - g_b_at_end);
    p->dispatches = (u32)(g_vk_dispatches - g_d_at_end);
    for (int i = 0; i < 3; i++) p->kind[i] = (u32)(g_rec_xfer_kind[i] - g_k_at_end[i]);
    p->bytes = g_rec_buf_bytes - g_bytes_at_end;
    gp_stamp(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, g->ops.pool, g->ops.used);
    g->ops.used += 2; /* its two, kept for the end */
    g_draws_at_begin = g_vk_drawn;
    g_in_pass = 1;
    p->q0 = p->q1 = 0;
    if (snx_tracy_attached()) {
        char name[48] = "gpu pass";
        if (s) snprintf(name, sizeof name, "gpu pass %ux%u fmt %u", s->w, s->h, (u32)s->f.vk);
        p->q0 = snx_tracy_gpu_zone_begin(name, 0x3d8f4a, VK_GPU_CTX_GRAPHICS, &p->ep);
    }
}
/* after vkCmdEndRendering */
void vk_gpuprof_pass_end(VkCommandBuffer cb) {
    GpSlot* g;
    GpPass* p;
    if (!g_in_pass) return;
    g_in_pass = 0;
    if (!(g = gp_slot())) return;
    p = &g->pass[g->npass];
    p->draws = (u32)(g_vk_drawn - g_draws_at_begin);
    gp_stamp(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, g->ops.pool, p->qi + 1);
    g->npass++;
    g_x_at_end = g_rec_xfers;
    g_b_at_end = g_rec_barriers;
    g_d_at_end = g_vk_dispatches;
    for (int i = 0; i < 3; i++) g_k_at_end[i] = g_rec_xfer_kind[i];
    g_bytes_at_end = g_rec_buf_bytes;
    /* the end goes out whenever the begin did: a dropped pair never reaches the server apart */
    if (p->q0) p->q1 = snx_tracy_gpu_zone_end(p->q0, VK_GPU_CTX_GRAPHICS, p->ep);
}
static void gp_report(void);
/* the slot's submissions into the sums of busy and idle time, on the CPU's clock: one calibrated pair of the two clocks
   now puts the timestamps on it (they are a frame old at most, and the clocks drift by far less in that time) */
static void gp_idle(const GpSlot* g, const u64* ts, double period) {
    const int qpc = g_vk.has_calibrated_qpc;
    const VkCalibratedTimestampInfoEXT spec[2] = {
        {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, NULL, VK_TIME_DOMAIN_DEVICE_EXT},
        {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, NULL,
         qpc ? VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT : VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT}};
    u64 pair[2], deviation;
    if (!g->nsub || !(qpc || (g_vk.has_calibrated_host && g_vk.has_calibrated_mono)) ||
        !VK_OK(vkGetCalibratedTimestampsEXT(g_vk.dev, 2, spec, pair, &deviation)))
        return;
    if (qpc) pair[1] = plat_qpc_ns(pair[1]); /* counter ticks: plat_ns's nanoseconds */
    for (u32 i = 0; i < g->nsub; i++) {
        const GpSub* s = &g->sub[i];
        if (!s->q1) continue;
        /* the GPU's times on the CPU's clock: before the pair, as almost all are */
        u64 b = pair[1] - (u64)((double)(pair[0] - ts[s->q0]) * period);
        u64 e = pair[1] - (u64)((double)(pair[0] - ts[s->q1]) * period);
        if (e < b) e = b;
        g_busy_ns += e - b;
        int hold = s->passes ? HOLD_PASSES : s->dispatches ? HOLD_DISPATCHES : s->xfers ? HOLD_COPIES : HOLD_NOTHING;
        g_hold_ns[hold] += e - b;
        g_holds[hold]++;
        g_subs++;
        g_sub_why[s->why]++;
        const VkGpGap gap = vk_gpuprof_gap(g_last_end, s->cpu0, s->cpu, b, e);
        if (gap.before) {
            /* the GPU idled from the last submission's end to this one's submit, as the CPU was busy */
            int c = g_prev_kind ? IDLE_WAITED : g_prev_why == VK_SUB_PRESENT ? IDLE_FRAME : IDLE_RECORDING;
            g_idle_ns[c] += gap.before;
            g_idles[c]++;
            if (g_prev_kind) g_round_ns[g_prev_kind - 1] += gap.before;
        }
        if (gap.queued) {
            g_idle_ns[IDLE_QUEUED] += gap.queued;
            g_idles[IDLE_QUEUED]++;
        }
        g_fill_ns[s->why] += gap.fill;
        g_fill_all_ns += gap.fill;
        g_idle_subs_all++;
        if (g_waited[s->value % GP_WAITS].value == s->value) g_fill_waited_ns += gap.fill;
        if (e > g_last_end) g_last_end = e;
        g_prev_why = s->why;
        g_prev_kind = g_waited[s->value % GP_WAITS].value == s->value ? g_waited[s->value % GP_WAITS].kind : 0;
    }
}
static GpKey* gp_key(const GpPass* p) {
    u64 h = (p->gva >> 8) ^ ((u64)p->w << 40) ^ ((u64)p->h << 20) ^ p->fmt;
    for (u32 i = 0; i < GP_KEYS; i++) {
        GpKey* k = &g_key[(h + i) & (GP_KEYS - 1)];
        if (!k->passes) {
            k->gva = p->gva, k->w = p->w, k->h = p->h, k->fmt = p->fmt;
            return k;
        }
        if (k->gva == p->gva && k->w == p->w && k->h == p->h && k->fmt == p->fmt) return k;
    }
    return NULL;
}
/* on the graphics queue, whose counter may wrap */
static i64 gp_ticks(u64 from, u64 to) { return vk_gpuprof_ticks(g_vk.gfx_timestamp_bits, from, to); }
static u64 gp_ns(u64 from, u64 to, double period) { return vk_gpuprof_ns(g_vk.gfx_timestamp_bits, from, to, period); }
/* The graphics queue's busy time in the slot's submissions, into g_stats_busy.ns: the union of their spans from begin
   to end, on the whole timeline. A command buffer's begin is written once the queue reaches it, which may be before
   the one before it ended, in this frame or the last (the slots are read in the order they were used): only what runs
   past the latest end counted is added. The gaps between them, the GPU waiting for the CPU, are left out */
static void gp_busy(const GpSlot* g, const u64* ts) {
    u64 ticks = 0;
    for (u32 i = 0; i < g->nsub; i++) {
        const GpSub* s = &g->sub[i];
        u64 b = ts[s->q0], e = ts[s->q1];
        if (!s->q1 || gp_ticks(b, e) <= 0) continue;
        if (g_stats_busy.ended) {
            if (gp_ticks(g_stats_busy.end, e) <= 0) continue; /* within what is counted */
            if (gp_ticks(g_stats_busy.end, b) < 0) b = g_stats_busy.end;
        }
        ticks += (u64)gp_ticks(b, e);
        g_stats_busy.end = e;
        g_stats_busy.ended = 1;
    }
    atomic_fetch_add_explicit(&g_stats_busy.ns, (u64)((double)ticks * g_vk.props.limits.timestampPeriod),
                              memory_order_relaxed);
}
static void gp_ops_done(GpOps* o, const u64* ts, double period, int ctx) {
    for (u32 i = 0; i < o->nop; i++) {
        GpOp* x = &o->op[i];
        u64 ns = vk_gpuprof_ns(ctx == VK_GPU_CTX_COMPUTE ? g_vk.compute_timestamp_bits : g_vk.gfx_timestamp_bits,
                               ts[x->qi], ts[x->qi + 1], period);
        GpDKey* k;
        if (x->kind != VK_OP_DRAW) { /* a pass's sum has its draws in it already */
            k = gp_dkey(x->kind, x->key, x->site);
            g_disp_ns += ns;
            g_disps++;
            if (k) {
                k->ns += ns;
                k->n++;
                k->bytes += x->bytes;
            }
        }
        if (x->q0 && x->q1) {
            snx_tracy_gpu_time((i64)ts[x->qi], x->q0, ctx, x->ep);
            snx_tracy_gpu_time((i64)ts[x->qi + 1], x->q1, ctx, x->ep);
        }
    }
    o->nop = 0;
}
/* the slot's work is done (frame_recycle, before the slot records again): its times into the sums */
void vk_gpuprof_frame_done(u32 slot) {
    GpSlot* g = g_slot[slot];
    int profile;
    /* a use not timed, or not read, comes between: the next spans don't continue from the last end */
    if (!g || !g->ops.reset) {
        g_stats_busy.ended = 0;
        return;
    }
    /* its reset goes in again at the next use timed even when timing went off in this one: that use's timestamps
       would otherwise follow this one's in the pool, and be read with them */
    g->ops.reset = 0;
    profile = gp_on();
    /* one buffer: the recording thread under vk_lock is its only writer, frame_recycle its only reader */
    static u64 ts[GP_QUERIES];
    /* no wait: the slot's submissions are done (frame_recycle waited for them), and a failed one's results aren't
       there, which leaves its frame out */
    if (!g->ops.used || !gp_timed() ||
        vkGetQueryPoolResults(g_vk.dev, g->ops.pool, 0, g->ops.used, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
        g_stats_busy.ended = 0;
        return;
    }
    g_vk_timestamps_read += g->ops.used;
    gp_busy(g, ts);
    if (!profile) return;
    /* the frame's timestamps onto the GPU track. The context is announced calibrated when the device can put its
       clock next to the client's (a fresh pair of the two every frame, vk_gpuprof_clock), with the anchor then
       that very pair; else the GPU clock is anchored at this moment: the last timestamp was written before
       timeline_wait returned, so this is its CPU time. */
    if (snx_tracy_attached()) {
        int cal = vk_gpuprof_calibrated();
        i64 cpu = 0, gpu = 0;
        int have = cal && vk_gpuprof_clock(&cpu, &gpu);
        snx_tracy_gpu_cal_reconnect(&g_cal);
        /* A context is announced calibrated only with a pair in hand: without one the server has no anchor and
           applies no calibration to the times sent after it, which shows as times far from the CPU's. The flag
           latches at the announcement, so a context announced without one is never sent calibration events:
           the server would assert on a context it was told is uncalibrated. */
        if (snx_tracy_gpu_context(VK_GPU_CTX_GRAPHICS, have ? gpu : (i64)ts[g->ops.used - 1],
                                  g_vk.props.limits.timestampPeriod, have, "graphics")) {
            snx_tracy_gpu_cal_announce(&g_cal, have, have ? cpu : 0); /* the anchor is the announcement's pair */
        }
        if (have) {
            i64 delta = snx_tracy_gpu_cal_delta(&g_cal, cpu);
            if (delta > 0) snx_tracy_gpu_calibrate(gpu, delta, VK_GPU_CTX_GRAPHICS);
        } else
            snx_tracy_gpu_sync((i64)ts[g->ops.used - 1], VK_GPU_CTX_GRAPHICS);
        for (u32 i = 0; i < g->npass; i++) {
            const GpPass* p = &g->pass[i];
            if (!p->q0 || !p->q1) continue;
            snx_tracy_gpu_time((i64)ts[p->qi], p->q0, VK_GPU_CTX_GRAPHICS, p->ep);
            snx_tracy_gpu_time((i64)ts[p->qi + 1], p->q1, VK_GPU_CTX_GRAPHICS, p->ep);
        }
    }
    double period = g_vk.props.limits.timestampPeriod;
    gp_ops_done(&g->ops, ts, period, VK_GPU_CTX_GRAPHICS);
    for (u32 i = 0; i < g->npass; i++) {
        const GpPass* p = &g->pass[i];
        u64 ns = gp_ns(ts[p->qi], ts[p->qi + 1], period);
        GpKey* k = gp_key(p);
        g_pass_ns += ns;
        gp_slow(p, ns);
        if (!k) continue;
        k->ns += ns;
        k->passes++;
        k->draws += p->draws;
    }
    gp_idle(g, ts, period);
    for (u32 i = 1; i < g->npass; i++) {
        const GpPass* p = &g->pass[i];
        int c = p->dispatches ? 0 : p->xfers ? 1 : p->barriers ? 2 : 3;
        /* 0 for a pass the queue began before the one recorded before it ended */
        u64 gns = gp_ns(ts[g->pass[i - 1].qi + 1], ts[p->qi], period);
        g_gap_ns[c] += gns;
        g_gaps[c]++;
        if (c == 1) {
            int m = p->kind[2] ? 2 : p->kind[1] ? 1 : 0;
            g_mix_ns[m] += gns;
            g_mixes[m]++;
            g_mix_bytes[m] += p->bytes;
            g_mix_n[m] += p->kind[0] + p->kind[1] + p->kind[2];
        }
    }
    g_span_ns += gp_ns(ts[0], ts[g->ops.used - 1], period);
    g_frames++;
    static u64 last;
    u64 now = plat_ns();
    if (!last) last = now;
    if (now - last >= 10000000000ull) {
        last = now;
        gp_report();
    }
}
/* a fresh pair of the GPU's clock and the client's, one driver call: CLOCK_MONOTONIC_RAW is the clock the Tracy
   client keeps its own time in, so the deltas between the pairs map the device's timestamps onto it. 0 without
   the extension or when the driver refused. */
int vk_gpuprof_clock(i64* cpu, i64* gpu) {
    static const VkCalibratedTimestampInfoEXT spec[2] = {
        {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, NULL, VK_TIME_DOMAIN_DEVICE_EXT},
        {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, NULL, VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT}};
    u64 ts2[2], deviation;
    if (!vk_gpuprof_calibrated()) return 0;
    if (!VK_OK(vkGetCalibratedTimestampsEXT(g_vk.dev, 2, spec, ts2, &deviation))) return 0;
    *cpu = (i64)ts2[1];
    *gpu = (i64)ts2[0];
    return 1;
}
/* the graphics queue's busy and idle time over the report's window, a frame (present) at a time, and the CPU's waits
   for the GPU by kind (cmd.c) */
static void gp_idle_report(void) {
    static const char* const why[VK_SUB_N] = {"other",    "present",   "finish", "1 ms",
                                              "readback", "frame end", "split",  "background"};
    static const char* const wait[VK_WAIT_N] = {"finish", "frame slot reuse", "a release or result's submit"};
    static u64 wait0[VK_WAIT_N], waits0[VK_WAIT_N], cs0;
    u64 now = plat_ns(), presents = (u32)(g_vk_present - g_present0), wall = now - g_t0;
    double f = presents ? (double)presents : 1.0;
    char line[512];
    int at = 0;
    if (g_t0 && presents && g_subs) {
        u64 idle = 0;
        for (int c = 0; c < IDLE_N; c++) idle += g_idle_ns[c];
        LOG("vulkan: GPU queue over %llu presents (%.2f ms each): busy %.2f ms a present (%.0f%%), idle %.2f ms "
            "between submissions, %.1f submissions a present\n",
            (unsigned long long)presents, (double)wall / 1e6 / f, (double)g_busy_ns / 1e6 / f,
            100.0 * (double)g_busy_ns / (double)wall, (double)idle / 1e6 / f, (double)g_subs / f);
        LOG("vulkan: GPU   waits a present: %.1f sparse binds, %.1f compute results, %.1f ASTC uploads\n",
            (double)g_vk_sparse_waits / f, (double)g_vk_cs_waits / f, (double)g_vk_astc_waits / f);
        if (g_submit_count)
            LOG("vulkan: GPU   submissions a present: %.1f, %.0f%% with an upload buffer, end %.2f us and queue submit "
                "%.2f us each\n",
                (double)g_submit_count / f, 100.0 * (double)g_submit_uploads / (double)g_submit_count,
                (double)g_submit_end_ns / 1000.0 / (double)g_submit_count,
                (double)g_submit_queue_ns / 1000.0 / (double)g_submit_count);
        if (g_submit_count)
            LOG("vulkan: GPU   submission in the ring %.2f us before the recorder runs it, backlog %.1f KiB (%.1f KiB "
                "at most) when the GPU thread made it\n",
                (double)g_submit_dwell_ns / 1000.0 / (double)g_submit_count,
                (double)g_submit_backlog / (double)g_submit_count / 1024.0, (double)g_submit_backlog_max / 1024.0);
        vk_rec_command_report(f, gp_on() > 0);
        for (int c = 0; c < IDLE_N; c++)
            if (g_idles[c])
                LOG("vulkan: GPU   idle, %s: %.2f ms a present, %.1f times\n", g_idle_name[c],
                    (double)g_idle_ns[c] / 1e6 / f, (double)g_idles[c] / f);
        for (int k = 0; k < VK_WAIT_N; k++)
            at += snprintf(line + at, sizeof line - (size_t)at, "%s %s %.2f ms (%.1f, idle after them %.2f ms)",
                           k ? "," : "", wait[k], (double)(g_vk_wait_ns[k] - wait0[k]) / 1e6 / f,
                           (double)(g_vk_waits[k] - waits0[k]) / f, (double)g_round_ns[k] / 1e6 / f);
        LOG("vulkan: GPU   the GPU thread's waits for the graphics queue a present:%s; for the compute queue %.2f ms\n",
            line, (double)(atomic_load_explicit(&g_vk_cs_wait_ns, memory_order_relaxed) - cs0) / 1e6 / f);
        at = 0;
        for (int w = 0; w < VK_SUB_N; w++)
            at += snprintf(line + at, sizeof line - (size_t)at, "%s %s %.1f", w ? "," : "", why[w],
                           (double)g_sub_why[w] / f);
        LOG("vulkan: GPU   submissions a present by why:%s\n", line);
        u64 fill = 0;
        at = 0;
        for (int w = 0; w < VK_SUB_N; w++) {
            fill += g_fill_ns[w];
            at += snprintf(line + at, sizeof line - (size_t)at, "%s %s %.2f", w ? "," : "", why[w],
                           (double)g_fill_ns[w] / 1e6 / f);
        }
        LOG("vulkan: GPU   idle while the next submission held commands not yet submitted, up to its GPU time: %.2f "
            "ms a present, %.2f ms of it before a submission the CPU waited for; by its why:%s\n",
            (double)fill / 1e6 / f, (double)g_fill_waited_ns / 1e6 / f, line);
        for (int c = 0; c < HOLD_N; c++)
            if (g_holds[c])
                LOG("vulkan: GPU   busy in submissions holding %s: %.2f ms a present, %.1f of them\n", g_hold_name[c],
                    (double)g_hold_ns[c] / 1e6 / f, (double)g_holds[c] / f);
    }
    memset(g_hold_ns, 0, sizeof g_hold_ns);
    memset(g_holds, 0, sizeof g_holds);
    for (int k = 0; k < VK_WAIT_N; k++) {
        wait0[k] = g_vk_wait_ns[k];
        waits0[k] = g_vk_waits[k];
    }
    cs0 = atomic_load_explicit(&g_vk_cs_wait_ns, memory_order_relaxed);
    memset(g_idle_ns, 0, sizeof g_idle_ns);
    memset(g_idles, 0, sizeof g_idles);
    memset(g_round_ns, 0, sizeof g_round_ns);
    memset(g_fill_ns, 0, sizeof g_fill_ns);
    g_fill_waited_ns = 0;
    memset(g_sub_why, 0, sizeof g_sub_why);
    g_busy_ns = g_subs = 0;
    g_vk_sparse_binds = g_vk_sparse_waits = 0;
    g_vk_cs_waits = g_vk_astc_waits = 0;
    g_submit_end_ns = g_submit_queue_ns = g_submit_uploads = g_submit_count = 0;
    g_submit_dwell_ns = g_submit_backlog = g_submit_backlog_max = 0;
    g_t0 = now;
    g_present0 = g_vk_present;
}
static int dkey_cmp(const void* a, const void* b) {
    u64 x = ((const GpDKey*)a)->ns, y = ((const GpDKey*)b)->ns;
    return x < y ? 1 : x > y ? -1 : 0;
}
static int key_cmp(const void* a, const void* b) {
    u64 x = ((const GpKey*)a)->ns, y = ((const GpKey*)b)->ns;
    return x < y ? 1 : x > y ? -1 : 0;
}
/* the slow passes by their mean, the slowest first, an empty one last however it compares */
static int slow_cmp(const void* a, const void* b) {
    const GpSlow *x = a, *y = b;
    if (!x->n || !y->n) return x->n ? -1 : y->n ? 1 : 0;
    const u64 m = x->ns / x->n, n = y->ns / y->n;
    return m < n ? 1 : m > n ? -1 : 0;
}
/* The log's other breakdowns as plots too: why the CPU waited, what the busy time held, and why submissions went
   out. Each is the window's sum divided by the same presents or frames as the log. */
static const char* const g_round_plot[VK_WAIT_N] = {"gpu round: finish ms", "gpu round: recycle ms",
                                                    "gpu round: submit ms"};
static const char* const g_hold_plot[HOLD_N] = {"gpu hold: passes ms", "gpu hold: dispatches ms",
                                                "gpu hold: copies ms", "gpu hold: nothing ms"};
static const char* const g_sub_plot[VK_SUB_N] = {"gpu submit: other",    "gpu submit: present",
                                                 "gpu submit: finish",   "gpu submit: 1 ms",
                                                 "gpu submit: readback", "gpu submit: frame end",
                                                 "gpu submit: split",    "gpu submit: background"};
/* the window's gaps and idle time as plots: the log's sums, so a --tracy run whose setting is off still shows where
   the GPU's time went. The gaps are per frame, the idle time per present, and the busy share of the window's wall */
static void gp_plots(void) {
    u64 now = plat_ns(), presents = (u32)(g_vk_present - g_present0), wall = now - g_t0;
    double f = g_frames ? (double)g_frames : 1.0;
    for (int c = 0; c < 4; c++) SNX_PLOT(g_gap_plot[c], (double)g_gap_ns[c] / 1e6 / f);
    for (int c = 0; c < IDLE_N; c++)
        SNX_PLOT(g_idle_plot[c], presents ? (double)g_idle_ns[c] / 1e6 / (double)presents : 0.0);
    if (g_t0 && wall) SNX_PLOT("gpu busy", 100.0 * (double)g_busy_ns / (double)wall);
    SNX_PLOT("gpu submissions/frame", (double)g_subs / f);
    /* a Memory plot takes bytes and adds the unit itself, so send bytes per frame and let the GUI print MB */
    double p = presents ? (double)presents : 1.0;
    SNX_PLOT("copy MB", (double)(g_mix_bytes[0] + g_mix_bytes[1] + g_mix_bytes[2]) / f);
    for (int k = 0; k < VK_WAIT_N; k++) SNX_PLOT(g_round_plot[k], (double)g_round_ns[k] / 1e6 / p);
    for (int k = 0; k < HOLD_N; k++) SNX_PLOT(g_hold_plot[k], (double)g_hold_ns[k] / 1e6 / p);
    for (int k = 0; k < VK_SUB_N; k++) SNX_PLOT(g_sub_plot[k], (double)g_sub_why[k] / p);
}
/* the pass bookkeeping is file-global: vk_lock serializes recording (record.c) */
/* the targets that took the most GPU time, then a fresh sum */
static void gp_report(void) {
    if (!g_frames) return;
    gp_plots();
    if (atomic_load_explicit(&g_on, memory_order_relaxed) <= 0) {
        /* the setting is off and the client asked for the sums: the plots have them, and the next window starts */
        memset(g_mix_ns, 0, sizeof g_mix_ns);
        memset(g_mixes, 0, sizeof g_mixes);
        memset(g_mix_bytes, 0, sizeof g_mix_bytes);
        memset(g_mix_n, 0, sizeof g_mix_n);
        memset(g_gap_ns, 0, sizeof g_gap_ns);
        memset(g_gaps, 0, sizeof g_gaps);
        memset(g_key, 0, sizeof g_key);
        memset(g_slow, 0, sizeof g_slow);
        g_pass_ns = g_span_ns = g_frames = g_lost = 0;
        gp_idle_report();
        return;
    }
    static GpKey sorted[GP_KEYS];
    memcpy(sorted, g_key, sizeof sorted);
    qsort(sorted, GP_KEYS, sizeof *sorted, key_cmp);
    LOG("vulkan: GPU over %llu frames: %.2f ms a frame in passes, %.2f ms from the first pass to the last, %llu passes "
        "untimed\n",
        (unsigned long long)g_frames, (double)g_pass_ns / 1e6 / (double)g_frames,
        (double)g_span_ns / 1e6 / (double)g_frames, (unsigned long long)g_lost);
    for (int c = 0; c < 4; c++)
        if (g_gaps[c])
            LOG("vulkan: GPU   gaps %s: %.2f ms a frame, %llu of them\n", g_gap_name[c],
                (double)g_gap_ns[c] / 1e6 / (double)g_frames, (unsigned long long)g_gaps[c]);
    for (int m = 0; m < 3; m++)
        if (g_mixes[m])
            LOG("vulkan: GPU     copy gaps %s: %.2f ms a frame, %llu gaps, %llu copies, %.2f MB of buffer copies a frame\n",
                g_mix_name[m], (double)g_mix_ns[m] / 1e6 / (double)g_frames, (unsigned long long)g_mixes[m],
                (unsigned long long)g_mix_n[m], (double)g_mix_bytes[m] / 1048576.0 / (double)g_frames);
    memset(g_mix_ns, 0, sizeof g_mix_ns);
    memset(g_mixes, 0, sizeof g_mixes);
    memset(g_mix_bytes, 0, sizeof g_mix_bytes);
    memset(g_mix_n, 0, sizeof g_mix_n);
    memset(g_gap_ns, 0, sizeof g_gap_ns);
    memset(g_gaps, 0, sizeof g_gaps);
    for (int i = 0; i < 12 && sorted[i].passes; i++)
        LOG("vulkan: GPU   %.2f ms a frame: %llu passes, %llu draws, target %llx %ux%u format %u\n",
            (double)sorted[i].ns / 1e6 / (double)g_frames, (unsigned long long)sorted[i].passes,
            (unsigned long long)sorted[i].draws, (unsigned long long)sorted[i].gva, sorted[i].w, sorted[i].h,
            sorted[i].fmt);
    memset(g_key, 0, sizeof g_key);
    {
        /* the slowest pass first: the top of this list is where a frame's GPU time goes */
        static GpSlow slow[GP_SLOW];
        memcpy(slow, g_slow, sizeof slow);
        qsort(slow, GP_SLOW, sizeof *slow, slow_cmp);
        for (u32 i = 0; i < GP_SLOW; i++) {
            const GpSlow* s = &slow[i];
            if (!s->n) break;
            LOG("vulkan: GPU   a pass of %.2f ms on average, %llu of them at 0.3 ms or more: target %llx %ux%u format "
                "%u, up to %u draws, gap before it up to %u copies, %u barriers, %u dispatches\n",
                (double)s->ns / 1e6 / (double)s->n, (unsigned long long)s->n, (unsigned long long)s->gva, s->w, s->h,
                s->fmt, s->draws, s->xfers, s->barriers, s->dispatches);
        }
    }
    memset(g_slow, 0, sizeof g_slow);
    /* the dispatches by tag: compute programs by key (vulkan: compute KEY ready), and the renderer's own pipelines by
       their VK_PROF_* */
    if (g_disps) {
        static const char* const own[VK_PROF_N] = {[VK_PROF_NONE] = "untagged",
                                                   [VK_PROF_ASTC] = "the ASTC decode",
                                                   [VK_PROF_BC7] = "the BC7 encode",
                                                   [VK_PROF_BC7_BACKGROUND] = "the background's ASTC decode and BC7 "
                                                                              "encode",
                                                   [VK_PROF_MIRROR_FOLD] = "the GPU mirror's fold",
                                                   [VK_PROF_MIRROR_HARVEST] = "the GPU mirror's harvest",
                                                   [VK_PROF_WLOG_MERGE] = "the write log's merge",
                                                   [VK_PROF_BL_MOVE] = "a block linear move",
                                                   [VK_PROF_DEPTH_PACK] = "the depth pack",
                                                   [VK_PROF_TEXCONV] = "a texture conversion",
                                                   [VK_PROF_VIC] = "a VIC composition",
                                                   [VK_PROF_XFB_INDIRECT] = "transform feedback's draw arguments",
                                                   [VK_PROF_INDIRECT_QUADS] = "an indirect draw's quads",
                                                   [VK_PROF_QUERY_PREDICATE] = "the queries' predicates",
                                                   [VK_PROF_QUERY_COMPARE] = "a query compare",
                                                   [VK_PROF_QUERY_COUNT] = "the queries' counts",
                                                   [VK_PROF_SHADING_RATE] = "shading rate images",
                                                   [VK_PROF_ASTC_KIND] = "ASTC blocks' kinds"};
        static GpDKey ds[GP_DKEYS];
        memcpy(ds, g_dkey, sizeof ds);
        qsort(ds, GP_DKEYS, sizeof *ds, dkey_cmp);
        LOG("vulkan: GPU   dispatches, copies, fills, blits, clears and resolves: %.2f ms a frame, %llu of them timed\n",
            (double)g_disp_ns / 1e6 / (double)g_frames, (unsigned long long)g_disps);
        for (int i = 0; i < 24 && ds[i].n; i++) {
            char what[128];
            if (ds[i].kind != VK_OP_DISPATCH) /* a copy: by where it was recorded, from vk_gpuprof_base */
                snprintf(what, sizeof what, "recorded at base%+lld", (long long)ds[i].key);
            else if (ds[i].key < VK_PROF_N && ds[i].site)
                snprintf(what, sizeof what, "the renderer's own, %s, asked for at base%+lld", own[ds[i].key],
                         (long long)ds[i].site);
            else if (ds[i].key < VK_PROF_N)
                snprintf(what, sizeof what, "the renderer's own, %s", own[ds[i].key]);
            else
                snprintf(what, sizeof what, "compute %016llx", (unsigned long long)ds[i].key);
            LOG("vulkan: GPU     %.2f ms a frame (%.3f ms each, %.1f MB a frame): %llu %s, %s\n",
                (double)ds[i].ns / 1e6 / (double)g_frames, (double)ds[i].ns / 1e6 / (double)ds[i].n,
                (double)ds[i].bytes / 1048576.0 / (double)g_frames, (unsigned long long)ds[i].n,
                g_kind_name[ds[i].kind], what);
        }
        memset(g_dkey, 0, sizeof g_dkey);
        g_disp_ns = g_disps = 0;
    }
    g_pass_ns = g_span_ns = g_frames = g_lost = 0;
    gp_idle_report();
}
