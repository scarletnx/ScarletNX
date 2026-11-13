/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* pipeline_wait on the recording thread (gpu/vk/record.c) against a stand-in GPU and clock: the packets of a bind and
   its draws run as the thread runs them, over pipelines a stand-in worker makes in a given time. Auto (0) waits for a
   pipeline while the frame's budget lasts, every wait of the frame from one budget, and draws it; past the budget it
   skips the draws, but one of 6 vertices or indices or fewer, which waits for it whole and binds it there, and an
   indirect draw, whose count is not known, is skipped; a frame gives the next its budget, half the game's frame, or
   half a 60 FPS one where it has none. Wait (1) waits for every pipeline whole, Skip (2) for none. The log's line says
   what Auto waited for and skipped, with the numbers */
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/descriptor_heaps.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/record.c"

VkState g_vk;

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

#define MS 1000000ll
/* the stand-in clock and worker: pipeline h (1 to 7) is made at made_at[h] on the clock, which waits move on */
static struct {
    u64 now;
    u64 made_at[8];
    int broken[8]; /* the worker couldn't make it */
    int bound;     /* the last pipeline bound, 0 none */
    int draws;     /* draws recorded */
    char said[2048];
} t;
#define PIPE(h) ((VkPipeline)(uintptr_t)(0x100 + (h)))
u64 plat_ns(void) { return t.now; }
static int ready(VkPipeline h) { return t.now >= t.made_at[(uintptr_t)h & 7]; }
static VkPipeline made(VkPipeline h) { return t.broken[(uintptr_t)h & 7] ? VK_NULL_HANDLE : PIPE((uintptr_t)h & 7); }
VkPipeline vk_pipeline_now(VkPipeline h, int* pending) {
    *pending = !ready(h);
    return *pending ? VK_NULL_HANDLE : made(h);
}
VkPipeline vk_pipeline_wait(VkPipeline h, i64 ns, int* pending) {
    u64 at = t.made_at[(uintptr_t)h & 7];
    if (!ready(h) && ns > 0) t.now = at < t.now + (u64)ns ? at : t.now + (u64)ns;
    return vk_pipeline_now(h, pending);
}
VkPipeline vk_pipeline_resolve(VkPipeline h) {
    if (!ready(h)) t.now = t.made_at[(uintptr_t)h & 7];
    return made(h);
}
static void VKAPI_CALL bind(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    (void)cb;
    (void)bp;
    t.bound = (int)((uintptr_t)p - 0x100);
}
static void VKAPI_CALL draw(VkCommandBuffer cb, u32 a, u32 b, u32 c, u32 d) {
    (void)cb, (void)a, (void)b, (void)c, (void)d;
    t.draws++;
}
static void VKAPI_CALL draw_indexed(VkCommandBuffer cb, u32 a, u32 b, u32 c, i32 d, u32 e) {
    (void)cb, (void)a, (void)b, (void)c, (void)d, (void)e;
    t.draws++;
}
static void VKAPI_CALL draw_indirect(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, u32 n, u32 stride) {
    (void)cb, (void)buf, (void)off, (void)n, (void)stride;
    t.draws++;
}

/* what record.c calls that this test has no part of */
void logf_(int lvl, const char* fmt, ...) {
    size_t at = strlen(t.said);
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    vsnprintf(t.said + at, sizeof t.said - at, fmt, ap);
    va_end(ap);
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}
int plat_thread(PThreadFn fn, void* arg, size_t stack) {
    (void)fn, (void)arg, (void)stack;
    return -1;
}
void plat_yield(void) {}
void snx_thread_name(const char* name) { (void)name; }
char* snx_os_error(char* out, size_t n) {
    snprintf(out, n, "none");
    return out;
}
void vk_ck_(VkCommandBuffer cb, u32 kind, u32 a, u32 b, u32 c, u32 d) {
    (void)cb, (void)kind, (void)a, (void)b, (void)c, (void)d;
}
void vk_ck_report(void) {}
VkCommandBuffer vk_cmd(void) { return VK_NULL_HANDLE; }
VkCommandBuffer vk_cmd_upload_recording(void) { return VK_NULL_HANDLE; }
VkDescriptorSet vk_desc_alloc(VkDescriptorSetLayout layout) {
    (void)layout;
    return VK_NULL_HANDLE;
}
VkDescriptorPool vk_desc_pool_new(u32 array_sets) {
    (void)array_sets;
    return VK_NULL_HANDLE;
}
void vk_desc_taken(VkDescriptorSetLayout layout, u32 n) { (void)layout, (void)n; }
int vk_descriptor_heaps_active(void) { return 0; }
void vk_descriptor_heaps_invalidate(void) {}
void vk_descriptor_heaps_pipeline_state(int active) { (void)active; }
void vk_descriptors_invalidate(void) {}
void vk_display_made(u64 id, VkResult r) { (void)id, (void)r; }
int vk_dslayout_arrays(VkDescriptorSetLayout layout) {
    (void)layout;
    return 0;
}
u32 vk_frame_slot(void) { return 0; }
VkSemaphore vk_gfx_timeline(void) { return VK_NULL_HANDLE; }
const void* vk_gpuprof_base(void) { return NULL; }
int vk_gpuprof_dispatch_begin(VkCommandBuffer cb) {
    (void)cb;
    return 0;
}
void vk_gpuprof_dispatch_end(VkCommandBuffer cb) { (void)cb; }
int vk_gpuprof_op_begin(VkCommandBuffer cb, u32 kind, u64 key, u64 bytes) {
    (void)cb, (void)kind, (void)key, (void)bytes;
    return 0;
}
void vk_gpuprof_op_end(VkCommandBuffer cb) { (void)cb; }
int vk_gpuprof_profiled(VkCommandBuffer cb) {
    (void)cb;
    return 0;
}
void vk_gpuprof_tag(u64 key) { (void)key; }
int vk_graphics_playout_is(VkPipelineLayout layout) {
    (void)layout;
    return 0;
}
void vk_lock(void) {}
void vk_unlock(void) {}
int vk_ok_(VkResult r, const char* what, const char* file, int line) {
    (void)what, (void)file, (void)line;
    return r == VK_SUCCESS;
}
void vk_wait_foreground(void) {}

/* the recording thread's packets, run as it runs them */
static void bind_run(int h) {
    BindPipelineArgs x = {VK_NULL_HANDLE, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)(uintptr_t)h};
    bind_pipeline_run(&x);
}
static void draw_run_n(u32 vertices) {
    DrawArgs x = {.a1 = vertices, .a2 = 1};
    draw_run(&x);
}
static void draw_indexed_run_n(u32 indices) {
    DrawIndexedArgs x = {.a1 = indices, .a2 = 1};
    draw_indexed_run(&x);
}
static void draw_indirect_run_once(void) {
    DrawIndirectArgs x = {.kind = 0, .max = 1, .stride = 16};
    draw_indirect_run(&x);
}
static void frame(u64 period_ns) {
    i64 budget = snx_vk_pipeline_budget(period_ns);
    frame_run(&budget);
}
/* a pipeline h made in ms from now */
static void make(int h, i64 ms) { t.made_at[h] = t.now + (u64)(ms * MS); }

static void test_auto(void) {
    g_settings.pipeline_wait = 0;
    CHECK(snx_vk_pipeline_budget(0) == 8333333 && snx_vk_pipeline_budget(33333333) == 16666666);
    frame(33333333); /* a 30 FPS game's frame: 16.6 ms to wait for new pipelines */
    /* made in 5 ms: waited for, drawn; 11.6 ms left */
    make(1, 5);
    u64 t0 = t.now;
    bind_run(1);
    CHECK(t.now - t0 == 5 * MS && t.bound == 1 && !g_rec_unbound);
    draw_run_n(300);
    CHECK(t.draws == 1);
    /* made in 20 ms: waited for what is left, 11.6 ms, then its big draws skipped, an indirect one too */
    make(2, 20);
    t0 = t.now;
    bind_run(2);
    CHECK(t.now - t0 == 16666666 - 5 * MS && t.bound == 1 && g_rec_unbound == 3);
    CHECK(strstr(t.said, "vulkan: pipeline_wait=0 (Auto): a new pipeline wasn't made within the frame's 16 ms for "
                         "them: its draws are skipped until a later frame has it, but for draws of 6 vertices or "
                         "fewer, which wait\n"));
    draw_run_n(7);
    draw_indexed_run_n(36);
    draw_indirect_run_once();
    CHECK(t.draws == 1 && g_rec_auto.skipped == 3);
    /* nothing left: another new one is not waited for at all */
    make(3, 1);
    t0 = t.now;
    bind_run(3);
    CHECK(t.now == t0 && g_rec_unbound == 3);
    /* a draw of 6 indices waits for it whole, binds it and draws, and the draws after it with it */
    draw_indexed_run_n(6);
    CHECK(t.now - t0 == 1 * MS && t.bound == 3 && !g_rec_unbound && t.draws == 2);
    draw_run_n(1000);
    CHECK(t.draws == 3 && g_rec_auto.small == 1);
    /* the next frame, a 60 FPS game's: 8.3 ms again; the pipeline of 2 is made by then */
    frame(16666666);
    t.now = t.made_at[2];
    t0 = t.now;
    bind_run(2);
    CHECK(t.now == t0 && t.bound == 2 && !g_rec_unbound);
    /* a frame with no pace: half a 60 FPS frame; one the worker couldn't make drops its draws, small ones too */
    frame(0);
    t.broken[4] = 1;
    make(4, 2);
    bind_run(4);
    CHECK(g_rec_unbound == 1 && t.bound == 2);
    draw_run_n(3);
    CHECK(t.draws == 3);
    /* the log's line, every 600 frames: the binds that waited, all of it, the longest, the budget, the skips */
    t.said[0] = 0;
    vk_rec_pipeline_log();
    CHECK(strstr(t.said, "vulkan: pipeline_wait=0 (Auto): 5 binds waited for a pipeline being made, 19 ms in all (the "
                         "longest 11 ms, a frame's budget 8 ms), 3 draws skipped past the budget, 1 draws of 6 "
                         "vertices or fewer waited whole\n"));
    t.said[0] = 0;
    vk_rec_pipeline_log(); /* nothing since: no line */
    CHECK(!t.said[0]);
}
/* Wait (1) waits for every pipeline whole; Skip (2) for none, its draws skipped, small ones too */
static void test_wait_skip(void) {
    u64 t0;
    g_settings.pipeline_wait = 1;
    frame(16666666);
    make(5, 40);
    t0 = t.now;
    bind_run(5);
    CHECK(t.now - t0 == 40 * MS && t.bound == 5 && !g_rec_unbound);
    g_settings.pipeline_wait = 2;
    make(6, 40);
    t0 = t.now;
    bind_run(6);
    CHECK(t.now == t0 && g_rec_unbound == 2 && t.bound == 5);
    int draws = t.draws;
    draw_run_n(3);
    CHECK(t.draws == draws && t.now == t0);
    t.said[0] = 0;
    vk_rec_pipeline_log(); /* Auto's counts alone: no line for Wait's and Skip's */
    CHECK(!t.said[0]);
}

int main(void) {
    vkCmdBindPipeline = bind;
    vkCmdDraw = draw;
    vkCmdDrawIndexed = draw_indexed;
    vkCmdDrawIndirect = draw_indirect;
    t.now = 1000 * MS;
    test_auto();
    test_wait_skip();
    printf("vk_record_wait: %u checks passed\n", checks);
    return 0;
}
