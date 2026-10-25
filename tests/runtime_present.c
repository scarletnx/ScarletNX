/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime's clock and vi's presenting side (clock.c, present.c) against a clock the test sets: the system tick
   exact at any uptime, fps_limit's pacing of DequeueBuffer, and the buffer vi_show_buffer hands the renderer. A
   buffer no render target holds is converted on the GPU (vk/renderer.c's cpu_frame): tests/glsl/texconv_glsl.cpp
   checks its formats, tests/astc_vulkan.c a block linear one on a device */
#include "common/utilities/bits.h"
#include "nx/runtime/clock.c"
#include "nx/runtime/present.c"
#include <stdarg.h>
#include <stdio.h>

int g_opt_fps = SNX_FPS_GAME;
static u64 now_ns, slept_ns;
static unsigned checks, failures;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            failures++;                                                                                                \
            fprintf(stderr, "runtime_present.c:%d: ", __LINE__);                                                       \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

u64 plat_ns(void) { return now_ns; }
void plat_sleep_ns(i64 ns) {
    slept_ns += (u64)ns;
    now_ns += (u64)ns;
}
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
}
u64 nvmap_addr_by_id(u32 id) { return id == 7 ? 0x1000 : 0; }
void cheats_frame(void) {}
static u64 presented;
static void test_present(u64 a, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4], u32 t,
                         u64 released) {
    (void)w, (void)h, (void)pitch, (void)layout, (void)bhl, (void)fmt, (void)crop, (void)t, (void)released;
    presented = a;
}
static Renderer test_renderer;
const Renderer* g_renderer = &test_renderer;

static void ticks(void) {
    static const u64 at[] = {0, 1, 9999, 10000, 52083, 1000000000u, 0x02D79883D2000000ull /* 10 years */,
                             0x7FFFFFFFFFFFFFFFull};
    size_t i;
    for (i = 0; i < sizeof at / sizeof *at; i++) {
        u64 hi, rem, lo = snx_mul_wide(at[i], 192, &hi), exact = snx_div_wide(hi, lo, 10000, &rem);
        now_ns = at[i]; /* clock_start ran at 0 */
        CHECK(guest_ticks() == (u64)exact, "ticks at %llu ns: %llu, not %llu", (unsigned long long)at[i],
              (unsigned long long)guest_ticks(), (unsigned long long)exact);
    }
}

static void pacing(void) {
    const u64 refresh = 1000000000u / 60;
    u64 before;
    int i;
    now_ns = 1000;
    slept_ns = 0;
    present_pace(2, -1); /* the first frame doesn't wait */
    CHECK(!slept_ns, "the first frame waited %llu ns", (unsigned long long)slept_ns);
    for (i = 0; i < 3; i++) {
        before = slept_ns;
        now_ns += 1000000; /* the game took 1 ms for its frame */
        present_pace(2, -1);
        CHECK(slept_ns - before == 2 * refresh - 1000000, "interval 2 waited %llu ns",
              (unsigned long long)(slept_ns - before));
    }
    before = slept_ns;
    present_pace(0, -1);
    CHECK(slept_ns == before, "the game's pace with interval 0 waited");
    now_ns += 10 * refresh; /* a load: the schedule starts again rather than catching up */
    present_pace(1, -1);
    CHECK(slept_ns == before, "a late frame waited");
    present_pace(1, -1);
    CHECK(slept_ns - before == refresh, "the frame after a late one waited %llu ns",
          (unsigned long long)(slept_ns - before));
    g_opt_fps = 30;
    before = slept_ns;
    present_pace(0, -1);
    present_pace(4, -1); /* a number ignores the interval */
    CHECK(slept_ns - before == 2 * (1000000000u / 30), "fps_limit=30 waited %llu ns over two frames",
          (unsigned long long)(slept_ns - before));
    g_opt_fps = 0;
    before = slept_ns;
    present_pace(1, -1);
    CHECK(slept_ns == before, "fps_limit=0 waited");
    g_opt_fps = SNX_FPS_GAME;
}

int main(void) {
    i32 crop[4] = {0, 0, 0, 0};
    clock_start();
    ticks();
    pacing();
    test_renderer.present_buffer = test_present;
    vi_show_buffer(7, 0x40, 1, 1, 4, 1, 0, 1, crop, 0, 0);
    vi_show_buffer(8, 0, 1, 1, 4, 1, 0, 1, crop, 0, 0);
    CHECK(presented == 0x1040 && g_frames == 1, "presented %llx, %d frames", (unsigned long long)presented,
          g_frames);
    if (failures) {
        fprintf(stderr, "%u of %u checks failed\n", failures, checks);
        return 1;
    }
    printf("runtime present: %u checks passed\n", checks);
    return 0;
}
