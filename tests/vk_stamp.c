/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The timeline of NVN's timestamp reports (gpu/vk/stamp.c), with no device: a model of the graphics queue's timestamp
   queries, each with the value of the GPU's clock where the test says the GPU reaches it, and of the graphics
   timeline's submissions. A query answers only once its submission is done, and is written only while reset, as the
   host's are. The GPU's clock starts far from the guest's, as a device's does: only spans of it count.

   Checked: the submissions of one present packed back to back, so that the time the GPU sat idle between them is in
   no span; a new present from the time its work became ready, or from the end of the work before it when that is
   later; the runtime's own work left out, also where a command buffer's end cuts it; a submission the GPU began
   before the one before it ended counted once; values that rise and stay at the guest's clock or a step past it; a
   burst of reports that fills the pool; the submission a report waits for; a counter of 36 valid bits that wraps; a
   reset, and a clock that went back without one; and a queue without timestamps. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/stamp.c"

static int failed, checks;
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(cond)) {                                                                                                 \
            failed++;                                                                                                  \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                       \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)
#define LL(x) ((unsigned long long)(x))

VkState g_vk;
u32 g_vk_present;
void logf_(int lvl, const char* fmt, ...) { (void)lvl, (void)fmt; }
int vk_ok_(VkResult r, const char* what, const char* file, int line) {
    return (void)what, (void)file, (void)line, r == VK_SUCCESS;
}

#define CB ((VkCommandBuffer)(uintptr_t)0x1234)
#define MS 1000000ull
static struct {
    u64 raw[STAMP_N], sub[STAMP_N]; /* a query's value, and the submission that writes it */
    u8 set[STAMP_N], top[STAMP_N];  /* written since its reset, and at the top of the pipe */
    u64 gpu;                        /* the guest time at which the GPU reaches the next timestamp recorded */
    u64 epoch;                      /* the GPU's clock when the guest's is 0 */
    u64 mask;                       /* the counter's valid bits */
    u64 now, ready;                 /* guest_ns and gpu_ready_ns */
    int open;                       /* a command buffer is being recorded */
    u64 submitted, done;            /* the graphics timeline */
    u32 written, pools, opened;     /* timestamps recorded, pools made, vk_submit_open's submissions */
} m;
u64 guest_ns(void) { return m.now; }
u64 gpu_ready_ns(void) { return m.ready; }
void rec_CmdWriteTimestamp2(VkCommandBuffer cb, VkPipelineStageFlags2 stage, VkQueryPool pool, u32 query) {
    (void)pool;
    CHECK(cb == CB && query < STAMP_N && !m.set[query], "timestamp %u was written before its reset", query);
    if (query >= STAMP_N) return;
    CHECK(stage == VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT || stage == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
          "timestamp %u is at stage %#llx", query, LL(stage));
    m.top[query] = stage == VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    m.raw[query] = (m.epoch + m.gpu) & m.mask;
    m.sub[query] = m.submitted + 1;
    m.set[query] = 1;
    m.written++;
}
static VKAPI_ATTR VkResult VKAPI_CALL pool_create(VkDevice dev, const VkQueryPoolCreateInfo* ci,
                                                  const VkAllocationCallbacks* a, VkQueryPool* pool) {
    (void)dev, (void)a;
    CHECK(ci->queryType == VK_QUERY_TYPE_TIMESTAMP && ci->queryCount == STAMP_N, "the pool is not STAMP_N timestamps");
    m.pools++;
    *pool = (VkQueryPool)(uintptr_t)0x77;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL pool_reset(VkDevice dev, VkQueryPool pool, uint32_t first, uint32_t count) {
    (void)dev, (void)pool;
    for (u32 i = first; i < first + count && i < STAMP_N; i++) {
        CHECK(!m.set[i] || m.sub[i] <= m.done, "timestamp %u was reset before its submission was done", i);
        m.set[i] = 0;
    }
}
static VKAPI_ATTR VkResult VKAPI_CALL pool_results(VkDevice dev, VkQueryPool pool, uint32_t first, uint32_t count,
                                                   size_t size, void* data, VkDeviceSize stride,
                                                   VkQueryResultFlags flags) {
    (void)dev, (void)pool, (void)stride;
    CHECK(count == 1 && size == 8 && flags == VK_QUERY_RESULT_64_BIT, "a read of %u timestamps", count);
    if (first >= STAMP_N || !m.set[first] || m.sub[first] > m.done) return VK_NOT_READY;
    memcpy(data, &m.raw[first], 8);
    return VK_SUCCESS;
}
PFN_vkCreateQueryPool vkCreateQueryPool = pool_create;
PFN_vkResetQueryPool vkResetQueryPool = pool_reset;
PFN_vkGetQueryPoolResults vkGetQueryPoolResults = pool_results;

VkCommandBuffer vk_cmd(void) {
    if (!m.open) {
        m.open = 1;
        vk_stamp_cb_begin(CB);
    }
    return CB;
}
/* the command buffer ends at the GPU's time at, and goes as the timeline's next value */
static void submit(u64 at) {
    if (!m.open) return;
    m.gpu = at;
    vk_stamp_cb_end(CB, m.submitted + 1);
    m.submitted++;
    m.open = 0;
}
void vk_submit_open(void) {
    m.opened++;
    submit(m.gpu);
}
void vk_submit_progress(u64* submitted, u64* done) {
    *submitted = m.submitted;
    *done = m.done;
}
int vk_rec_unsafe(void) { return 0; }

/* a command buffer that the GPU begins at, in present frame, its work ready at ready */
static void begin(u32 frame, u64 ready, u64 at) {
    g_vk_present = frame;
    m.ready = ready;
    m.gpu = at;
    vk_cmd();
}
static u64 report(int top, u64 at) {
    m.gpu = at;
    return vk_stamp_report(top);
}
/* every submission is done at the guest's time now */
static void finish(u64 now) {
    m.done = m.submitted;
    m.now = now;
    vk_stamp_fold(m.done);
}
/* a report's value in guest ns, 0 when it has none */
static u64 value(u64 n) {
    u64 ticks = 0;
    if (!n || vk_stamp_ticks(n, &ticks) != 1) return 0;
    CHECK(ticks == snx_gpu_ticks_of_ns(g_st.ns[slot(n)]), "a report's ticks are not its value's");
    return g_st.ns[slot(n)];
}
static void device(u32 bits) {
    memset(&g_st, 0, sizeof g_st);
    memset(&m, 0, sizeof m);
    m.mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
    /* a counter of few bits wraps 52 ms into the guest's time, inside a submission and the runtime's work */
    m.epoch = bits >= 64 ? 977ull * 1000 * MS : (14ull << bits) - 52 * MS;
    g_vk.gfx_timestamp_bits = bits;
    g_vk.has_host_query_reset = 1;
    g_vk.props.limits.timestampPeriod = 1.0f;
}

static void timeline(u32 bits) {
    u64 ticks = 0, a, b, c;
    device(bits);
    /* before the first report no command buffer has a timestamp, and the report that starts them has none: its
       command buffer began without its start */
    m.now = 1 * MS;
    begin(10, 1 * MS, 1 * MS);
    CHECK(!m.written && !m.pools, "a command buffer took a timestamp before any report asked for one");
    a = report(0, 1 * MS);
    CHECK(!a && m.pools == 1 && vk_stamp_ticks(a, &ticks) == 0, "the first report has a timestamp (%llu)", LL(a));
    submit(1 * MS);
    finish(1 * MS);

    /* one present, two submissions: 3 ms of work from 0.1 ms after it was ready, the GPU idle for 4 ms until the
       present's later work is ready, at 8 ms, then 2 ms. The span is the 5 ms the GPU worked, and it starts when the
       first work was ready. A TIMESTAMP_TOP report is written at the top of the pipe, a TIMESTAMP report after all
       the commands before it */
    begin(11, 2 * MS, 2 * MS + MS / 10);
    a = report(1, 2 * MS + MS / 10);
    submit(5 * MS + MS / 10);
    begin(11, 8 * MS, 9 * MS + MS / 10);
    b = report(0, 11 * MS + MS / 10);
    CHECK(a && b && m.top[slot(a)] && !m.top[slot(b)],
          "a TIMESTAMP_TOP report at the top of the pipe: %d, the other: %d", m.top[slot(a)], m.top[slot(b)]);
    CHECK(vk_stamp_ticks(b, &ticks) == -1, "a report has its value before its submission is done");
    submit(11 * MS + MS / 10);
    finish(12 * MS);
    CHECK(value(a) == 2 * MS && value(b) == 7 * MS,
          "a present of 3 ms of work ready at 2 ms and 2 ms ready at 8 ms: %llu to %llu ns", LL(value(a)),
          LL(value(b)));

    /* the next present's work was ready later than that: its time starts there. The one after was ready before its
       work ended (the GPU is behind): it follows it, a step later */
    begin(12, 20 * MS, 20 * MS + MS / 20);
    a = report(1, 20 * MS + MS / 20);
    b = report(0, 21 * MS + MS / 20);
    submit(21 * MS + MS / 20);
    begin(13, 20 * MS + MS / 2, 21 * MS + MS / 20);
    c = report(1, 21 * MS + MS / 20);
    const u64 d = report(0, 23 * MS + MS / 20);
    submit(23 * MS + MS / 20);
    finish(30 * MS);
    CHECK(value(a) == 20 * MS && value(b) == 21 * MS, "a present ready at 20 ms with 1 ms of work: %llu to %llu ns",
          LL(value(a)), LL(value(b)));
    CHECK(value(c) == 21 * MS + STEP_NS && value(d) == 23 * MS,
          "a present ready while the GPU worked on the one before, with 2 ms of work: %llu to %llu ns", LL(value(c)),
          LL(value(d)));

    /* the runtime's own work (a render target's load): 1 ms of the game's, 10 ms of the runtime's, 2 ms of the
       game's. Then the same with the command buffer ended inside the runtime's work */
    begin(14, 40 * MS, 40 * MS);
    a = report(1, 40 * MS);
    m.gpu = 41 * MS;
    vk_stamp_runtime_begin();
    m.gpu = 51 * MS;
    vk_stamp_runtime_end();
    b = report(0, 53 * MS);
    submit(53 * MS);
    finish(60 * MS);
    CHECK(value(a) == 40 * MS && value(b) == 43 * MS, "3 ms of work around 10 ms of the runtime's: %llu to %llu ns",
          LL(value(a)), LL(value(b)));
    begin(15, 70 * MS, 70 * MS);
    a = report(1, 70 * MS);
    m.gpu = 71 * MS;
    vk_stamp_runtime_begin();
    submit(75 * MS);
    begin(15, 70 * MS, 75 * MS);
    m.gpu = 81 * MS;
    vk_stamp_runtime_end();
    b = report(0, 83 * MS);
    submit(83 * MS);
    finish(90 * MS);
    CHECK(value(a) == 70 * MS && value(b) == 73 * MS,
          "3 ms of work around 10 ms of the runtime's that a command buffer's end cuts: %llu to %llu ns", LL(value(a)),
          LL(value(b)));

    /* the GPU reaches the top of a submission 2 ms before the one before it ends: its 5 ms count as the 3 ms after
       that end */
    begin(16, 100 * MS, 100 * MS);
    a = report(1, 100 * MS);
    submit(104 * MS);
    begin(16, 100 * MS, 102 * MS);
    b = report(0, 107 * MS);
    submit(107 * MS);
    finish(110 * MS);
    CHECK(value(a) == 100 * MS && value(b) == 107 * MS, "two submissions that overlap by 2 ms: %llu to %llu ns",
          LL(value(a)), LL(value(b)));

    /* reports at one instant of the GPU rise by a step, in ticks too, and none is past the guest's clock when its
       submission is done, however long the GPU says it worked */
    begin(17, 120 * MS, 120 * MS);
    a = report(0, 121 * MS);
    b = report(0, 121 * MS);
    c = report(0, 150 * MS);
    submit(150 * MS);
    finish(125 * MS);
    CHECK(value(a) == 121 * MS && value(b) == value(a) + STEP_NS &&
              snx_gpu_ticks_of_ns(value(b)) > snx_gpu_ticks_of_ns(value(a)),
          "two reports at one instant: %llu and %llu ns", LL(value(a)), LL(value(b)));
    CHECK(value(c) > value(b) && value(c) <= 125 * MS, "a report of a submission done at 125 ms has %llu ns",
          LL(value(c)));

    /* values still rise after a submission whose reports all stand at the guest's clock, each a step after the one
       before: the clock has not gone back because the last value is those steps past it */
    begin(17, 120 * MS, 150 * MS);
    for (u32 i = 0; i < 100; i++) c = report(0, 180 * MS);
    submit(180 * MS);
    begin(17, 120 * MS, 180 * MS);
    const u64 e = report(0, 181 * MS);
    submit(181 * MS);
    finish(125 * MS + 1000);
    CHECK(value(e) > value(c), "a report after 100 at the guest's clock has %llu ns, the last of those %llu",
          LL(value(e)), LL(value(c)));

    /* a report in the command buffer being recorded: asking for its submission submits it, and nothing is left to
       wait for once it is done */
    begin(18, 200 * MS, 200 * MS);
    a = report(0, 201 * MS);
    const u32 opened = m.opened;
    CHECK(vk_stamp_submit_of(a) == m.submitted && m.opened == opened + 1 && !m.open,
          "the submission of a report being recorded is %llu, with %llu submitted", LL(vk_stamp_submit_of(a)),
          LL(m.submitted));
    finish(210 * MS);
    CHECK(!vk_stamp_submit_of(a) && value(a) == 201 * MS, "a report done still waits, or has %llu ns", LL(value(a)));

    /* more reports in one command buffer than the pool has timestamps: the ones past it keep the GPU thread's time
       (0), the command buffer still gets its end, and the next one has timestamps again */
    begin(19, 300 * MS, 300 * MS);
    u32 with = 0, without = 0;
    u64 last = 0, first = 0;
    for (u32 i = 0; i < STAMP_N + 100; i++) {
        const u64 n = report(0, 300 * MS + i * 100);
        if (n) {
            with++;
            last = n;
            if (!first) first = n;
        } else
            without++;
    }
    submit(301 * MS);
    finish(310 * MS);
    CHECK(with == STAMP_N - 2 && without == 102, "%u reports of %u have a timestamp in a pool of %u", with,
          STAMP_N + 100, STAMP_N);
    CHECK(value(first) == 300 * MS && value(last) == 300 * MS + (with - 1) * 100,
          "the first and last report of a full pool have %llu and %llu ns", LL(value(first)), LL(value(last)));
    begin(20, 320 * MS, 320 * MS);
    a = report(0, 321 * MS);
    submit(321 * MS);
    finish(330 * MS);
    CHECK(value(a) == 321 * MS, "a report after a full pool has %llu ns", LL(value(a)));

    /* a save state's load: the guest's clock is the state's again, and the values follow it */
    vk_stamp_reset();
    m.now = 5 * MS;
    begin(21, 5 * MS, 400 * MS);
    a = report(1, 400 * MS);
    b = report(0, 402 * MS);
    submit(402 * MS);
    finish(8 * MS);
    CHECK(value(a) == 5 * MS && value(b) == 7 * MS, "after a reset at 5 ms, 2 ms of work: %llu to %llu ns",
          LL(value(a)), LL(value(b)));
    /* and a clock that went back with no reset: the timeline starts again where the clock is, and the values are a
       step apart there */
    begin(22, 2 * MS, 500 * MS);
    a = report(1, 500 * MS);
    b = report(0, 501 * MS);
    submit(501 * MS);
    finish(4 * MS);
    CHECK(value(a) == 4 * MS + STEP_NS && value(b) == 4 * MS + 2 * STEP_NS,
          "after the clock went back to 4 ms: %llu and %llu ns", LL(value(a)), LL(value(b)));
}

int main(void) {
    timeline(64);
    /* a counter of 36 valid bits wraps every 68 s at 1 ns a tick: the spans are the same across a wrap */
    timeline(36);
    /* without timestamps on the graphics queue, or without host query reset, no report has one and no pool is made */
    for (u32 i = 0; i < 2; i++) {
        device(64);
        if (i)
            g_vk.has_host_query_reset = 0;
        else
            g_vk.gfx_timestamp_bits = 0;
        begin(1, MS, MS);
        CHECK(!report(0, MS) && !report(1, MS) && !m.pools && !m.written, "a report has a timestamp on a device %s",
              i ? "without host query reset" : "whose graphics queue has none");
    }
    if (failed) {
        fprintf(stderr, "vk_stamp: %d of %d checks failed\n", failed, checks);
        return 1;
    }
    printf("vk_stamp: %d checks: report timestamps on the GPU's busy time\n", checks);
    return 0;
}
