/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The timestamps of NVN's timestamp counter reports (NVN_COUNTER_TYPE_TIMESTAMP and _TIMESTAMP_TOP), from the time the
   host GPU spends on the work, not the time at which the GPU thread replays the report.

   A game times its GPU with two of these reports and sizes its frame by the difference. One game puts a
   TIMESTAMP_TOP report at the start of each frame's command buffer and a TIMESTAMP report after it, and renders at a
   fraction of its output size when the span is long. When the GPU thread stamped the reports as it replayed them, the
   span was the GPU thread's time between the two records: its recording, its submissions and its waits. In gameplay
   the span was 15.5 ms (median), while the host GPU was busy 11.3 ms each present. The game then changed its render
   size every few frames, and each change made a new set of render targets (#2331, #388).

   Each command buffer of the graphics queue has a timestamp at its start and its end, and each report one where it is
   recorded. When a submission is done, it is placed on a timeline as the console's GPU would have run it: submissions
   within the same present are packed back-to-back so mid-frame CPU stalls don't inflate a frame's GPU span, and across
   presents the timeline catches up to the time its work became ready (gpu_ready_ns). Overlapping pipelined submissions
   clip against the previous submission's raw end timestamp so busy time is not double-counted. Values keep rising (by
   STEP_NS at least) and stay at or below the guest time at which they are made, except where those steps put them
   past it (STEP_NS for each report made while the timeline stands at the clock), so a game that compares them with
   the CPU's tick still can.
   The compute queue's work is not counted, nor is the runtime's own work on the graphics queue between
   vk_stamp_runtime_begin and _end: the console's GPU doesn't do it. A game changed its render size, the runtime loaded
   each new render target from memory, and the game read that load as its own GPU time and changed its size again
   (#2331).

   A report goes when the stream lets it, with the GPU thread's time, and its timestamp is written again once its
   submission is done (release.c's fix_through): waiting for the submission held the game's fences behind it. When the
   game reads a counter through nvnDeviceGetTimestampInNanoseconds while its submission is still in flight,
   vk_release_stamp_sync flushes or waits for that submission alone. Without timestamps on the graphics queue or host
   query reset, the reports keep the GPU thread's time.

   Everything here runs with vk_lock held. The design is from #388 (talon152), and LASRecomp and nxnative (both MIT)
   time their reports the same way. */
#include <stdatomic.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/vk/gpuprof_time.h"
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/log.h"

#define STAMP_N 4096 /* timestamps in the pool: each submission's start and end, and its reports' */
#define SUB_N   1024 /* submissions with timestamps not yet done */
/* values step at least this many ns, more than one GPU tick (625 / 384 ns), so that they strictly increase in ticks,
   as gpu_ticks's do */
#define STEP_NS 16u

static struct {
    VkQueryPool pool;
    int state;    /* 0: no report asked yet, 1: on, -1: no timestamps here */
    u64 next;     /* timestamps handed out, numbered from 1: the next one is next + 1, at next % STAMP_N */
    u64 folded;   /* timestamps whose values are known and whose queries are reset, through folded */
    u64 cb_q0;    /* the start timestamp of the command buffer being recorded, 0 for none */
    VkCommandBuffer cb; /* that command buffer */
    int runtime;        /* the depth of vk_stamp_runtime_begin */
    u64 cb_ready; /* the time its work became ready */
    int cb_runtime; /* it began inside the runtime's own work */
    u32 cb_frame; /* the present number when the command buffer began */
    struct {
        u64 value, q0, q1, ready; /* its number on the graphics timeline, its start and end, its ready time */
        u32 frame;
        int runtime; /* it began inside the runtime's own work */
    } sub[SUB_N];
    u64 shead, stail;
    u64 end, last; /* the timeline's end, and the last value given (guest ns) */
    u64 clock;     /* the guest's clock at the last fold: one below it has gone back */
    u64 prev_t1;   /* raw end timestamp of the latest folded submission, for clipping overlaps */
    u64 prev_ns;   /* the guest time it was folded at */
    int have_prev;
    u32 last_frame;
    u64 ns[STAMP_N]; /* a report's value in guest ns, once its submission is folded */
    u8 kind[STAMP_N]; /* STAMP_* */
} g_st;

enum { STAMP_REPORT, STAMP_RUNTIME_BEGIN, STAMP_RUNTIME_END };
static u32 slot(u64 n) { return (u32)((n - 1) % STAMP_N); }
static int start(void) {
    if (g_st.state) return g_st.state > 0;
    g_st.state = -1;
    if (!g_vk.gfx_timestamp_bits || !g_vk.has_host_query_reset) {
        LOG("vulkan: %s: report timestamps keep the GPU thread's time\n",
            !g_vk.gfx_timestamp_bits ? "no timestamps on the graphics queue" : "no host query reset");
        return 0;
    }
    VkQueryPoolCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = STAMP_N};
    if (!VK_OK(vkCreateQueryPool(g_vk.dev, &ci, NULL, &g_st.pool))) return 0;
    vkResetQueryPool(g_vk.dev, g_st.pool, 0, STAMP_N);
    g_st.last_frame = ~0u;
    /* the reports before this one kept the GPU thread's time: the values from here on come after it */
    g_st.end = g_st.last = guest_ns();
    g_st.state = 1;
    LOG("vulkan: report timestamps from the GPU's busy time, from the next command buffer on\n");
    return 1;
}
/* the next timestamp, written at stage in cb; reserve is the number of slots kept free for later writes (e.g. q1) */
static u64 write(VkCommandBuffer cb, VkPipelineStageFlags2 stage, u64 reserve) {
    if (g_st.next - g_st.folded + 1 + reserve > STAMP_N) return 0;
    rec_CmdWriteTimestamp2(cb, stage, g_st.pool, slot(g_st.next + 1));
    g_st.kind[slot(g_st.next + 1)] = STAMP_REPORT;
    return ++g_st.next;
}
static int raw(u64 n, u64* t) {
    return vkGetQueryPoolResults(g_vk.dev, g_st.pool, slot(n), 1, sizeof *t, t, sizeof *t, VK_QUERY_RESULT_64_BIT) ==
           VK_SUCCESS;
}

/* the ns from raw timestamp x to y of the graphics queue, 0 when y isn't after x */
static u64 span_ns(u64 x, u64 y) {
    const u32 bits = g_vk.gfx_timestamp_bits;
    return vk_gpuprof_ticks(bits, x, y) > 0 ? vk_gpuprof_ns(bits, x, y, g_vk.props.limits.timestampPeriod) : 0;
}
void vk_stamp_cb_begin(VkCommandBuffer cb) {
    g_st.cb_q0 = 0;
    if (g_st.state <= 0 || g_st.shead - g_st.stail >= SUB_N) return;
    g_st.cb_q0 = write(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 1); /* reserve 1 slot for q1 at cb_end */
    g_st.cb = cb;
    g_st.cb_runtime = g_st.runtime > 0;
    g_st.cb_ready = gpu_ready_ns();
    g_st.cb_frame = g_vk_present;
}
void vk_stamp_cb_end(VkCommandBuffer cb, u64 value) {
    if (!g_st.cb_q0) return;
    const u64 q0 = g_st.cb_q0, q1 = write(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
    g_st.cb_q0 = 0;
    if (!q1 || g_st.shead - g_st.stail >= SUB_N) return;
    g_st.sub[g_st.shead % SUB_N].value = value;
    g_st.sub[g_st.shead % SUB_N].q0 = q0;
    g_st.sub[g_st.shead % SUB_N].q1 = q1;
    g_st.sub[g_st.shead % SUB_N].ready = g_st.cb_ready;
    g_st.sub[g_st.shead % SUB_N].frame = g_st.cb_frame;
    g_st.sub[g_st.shead % SUB_N].runtime = g_st.cb_runtime;
    g_st.shead++;
}
/* the runtime's own work on the graphics queue, which the console's GPU doesn't do (a render target's load from
   memory): its time is left out of the reports' timeline. A bracket that its command buffer's end cuts ends there,
   and the next command buffer begins inside it */
static void runtime_mark(int kind) {
    if (!g_st.cb_q0) return;
    /* a begin keeps a slot for its end, and both keep one for q1 */
    const u64 n = write(g_st.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, kind == STAMP_RUNTIME_BEGIN ? 2 : 1);
    if (n) g_st.kind[slot(n)] = (u8)kind;
}
void vk_stamp_runtime_begin(void) {
    if (g_st.state <= 0 || g_st.runtime++) return;
    runtime_mark(STAMP_RUNTIME_BEGIN);
}
void vk_stamp_runtime_end(void) {
    if (g_st.state <= 0 || !g_st.runtime || --g_st.runtime) return;
    runtime_mark(STAMP_RUNTIME_END);
}
void vk_stamp_fold(u64 done) {
    while (g_st.stail != g_st.shead && g_st.sub[g_st.stail % SUB_N].value <= done) {
        const u64 q0 = g_st.sub[g_st.stail % SUB_N].q0, q1 = g_st.sub[g_st.stail % SUB_N].q1,
                  ready = g_st.sub[g_st.stail % SUB_N].ready, now = guest_ns();
        const u32 frame = g_st.sub[g_st.stail % SUB_N].frame;
        const u32 bits = g_vk.gfx_timestamp_bits;
        const double period = g_vk.props.limits.timestampPeriod;
        /* the guest's clock went back (a save state's load): the timeline starts again there. The clock is compared
           with its own last reading, not with the last value: reports made while the timeline stands at the clock are
           each a step past the one before, so the last value can be past a clock that only went forward */
        if (now < g_st.clock) {
            g_st.end = g_st.last = now;
            g_st.have_prev = 0;
            g_st.last_frame = ~0u;
        }
        g_st.clock = now;
        /* after a second, the previous end can't overlap, and a counter of few valid bits can have wrapped past half
           its period (36 bits of 1 ns: 34 s), which would read as an overlap */
        if (g_st.have_prev && now - g_st.prev_ns > 1000000000u) g_st.have_prev = 0;
        u64 t0 = 0, t1 = 0;
        const int have = raw(q0, &t0) && raw(q1, &t1);
        u64 b = t0;
        if (have && g_st.have_prev && vk_gpuprof_ticks(bits, g_st.prev_t1, b) < 0) b = g_st.prev_t1;
        const u64 dur = have && vk_gpuprof_ticks(bits, b, t1) > 0 ? vk_gpuprof_ns(bits, b, t1, period) : 0;
        u64 at = frame != g_st.last_frame && ready > g_st.end ? ready : g_st.end;
        if (at > now) at = now;
        g_st.last_frame = frame;
        /* the runtime's own work up to each timestamp: the time in its brackets, and where the open one began */
        int in_rt = g_st.sub[g_st.stail % SUB_N].runtime;
        u64 rt_from = b, rt_ns = 0;
        for (u64 n = q0 + 1; n < q1; n++) {
            u64 t = 0, v = at;
            const int kind = g_st.kind[slot(n)];
            if (!have || !raw(n, &t)) { /* not read: the report keeps the GPU thread's time */
                g_st.ns[slot(n)] = 0;
                continue;
            }
            if (kind != STAMP_REPORT) {
                g_st.ns[slot(n)] = 0;
                if (kind == STAMP_RUNTIME_BEGIN && !in_rt) {
                    in_rt = 1;
                    rt_from = t;
                } else if (kind == STAMP_RUNTIME_END && in_rt) {
                    rt_ns += span_ns(rt_from, t);
                    in_rt = 0;
                }
                continue;
            }
            u64 off = span_ns(b, t);
            const u64 rt_to = rt_ns + (in_rt ? span_ns(rt_from, t) : 0);
            off = off > rt_to ? off - rt_to : 0;
            if (off > dur) off = dur;
            v += off;
            if (v > now) v = now;
            if (v < g_st.last + STEP_NS) v = g_st.last + STEP_NS;
            g_st.ns[slot(n)] = g_st.last = v;
        }
        if (have && in_rt) rt_ns += span_ns(rt_from, t1);
        if (have) {
            g_st.end = at + (dur > rt_ns ? dur - rt_ns : 0);
            if (g_st.end > now) g_st.end = now;
            if (!g_st.have_prev || vk_gpuprof_ticks(bits, g_st.prev_t1, t1) > 0) {
                g_st.prev_t1 = t1;
                g_st.prev_ns = now;
                g_st.have_prev = 1;
            }
        }
        for (u64 n = g_st.folded + 1; n <= q1; n++) {
            if (n < q0) g_st.ns[slot(n)] = 0;
            vkResetQueryPool(g_vk.dev, g_st.pool, slot(n), 1);
        }
        g_st.folded = q1;
        g_st.stail++;
    }
}
void vk_stamp_reset(void) {
    g_st.end = g_st.last = g_st.clock = 0;
    g_st.have_prev = 0;
    g_st.last_frame = ~0u;
}
u64 vk_stamp_report(int top) {
    if (!start()) return 0;
    VkCommandBuffer cb = vk_cmd(); /* begins one, with its start, when none is recording */
    /* reserve 1 slot for cb_end's q1 so a burst of reports cannot strand the submission */
    return g_st.cb_q0 ? write(cb, top ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                              1 + (g_st.runtime > 0)) /* and 1 for an open bracket's end */
                      : 0; /* begun before the first report, or pool full: no start to count from */
}
int vk_stamp_ticks(u64 n, u64* ticks) {
    u64 done = 0;
    if (!n) return 0;
    if (n > g_st.folded) {
        vk_submit_progress(&(u64){0}, &done);
        vk_stamp_fold(done);
    }
    if (n > g_st.folded) return -1;
    if (g_st.next - n >= STAMP_N || !g_st.ns[slot(n)]) return 0; /* written over since, or its command buffer had no end */
    *ticks = snx_gpu_ticks_of_ns(g_st.ns[slot(n)]);
    return 1;
}
u64 vk_stamp_submit_of(u64 n) {
    if (!n || n <= g_st.folded) return 0;
    if (g_st.cb_q0 && n > g_st.cb_q0 && !vk_rec_unsafe()) vk_submit_open();
    for (u64 i = g_st.stail; i != g_st.shead; i++) {
        const u64 s = i % SUB_N;
        if (n > g_st.sub[s].q0 && n < g_st.sub[s].q1) return g_st.sub[s].value;
    }
    return 0;
}
