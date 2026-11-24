/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The draw preparation thread (draw_prep, docs/dev/parallel-draw-prep.md). While the renderer thread records the
   binds, the dynamic state and the draw command of draw N, this thread resolves draw N+1's kept state: its shaders,
   targets, textures and pipeline key (render.c vk_render_prep_ahead). The two run in lock step: the renderer thread
   gives the work (vk_prep_kick) once draw N's resolve and commits are done, and it waits for the result
   (vk_prep_join) before it lets go of vk_lock. In between, the renderer thread runs only code that touches nothing
   the resolve reads or writes, and this thread runs under the renderer thread's hold of vk_lock (vk_lock_lend).

   The state word: 0 idle, 1 given, 2 taken by this thread, 3 done. A join that finds the work still given (this
   thread asleep or late) takes it back, and the draw is resolved on the renderer thread as before: the renderer
   never waits for a wake */
#include "nx/gpu/vk/vk.h"
#include <stdatomic.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/gpu_queue.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/tracy.h"
#include "common/platform/thread.h"
#include "common/platform/time.h"

enum { PREP_IDLE, PREP_GIVEN, PREP_TAKEN, PREP_DONE };
#define PREP_SPIN_NS 200000 /* a wait for work spins this long, then sleeps */
static struct {
    _Alignas(64) _Atomic u32 state;
    _Alignas(64) _Atomic u32 sleeps;
    void (*ahead)(void);
    void (*job)(void); /* the kick's function: ahead, or a batch's (vk_prep_kick_work) */
    int lend;           /* vk_lock_lend's mode for the work */
    int started, failed;
    /* the renderer thread's view of the write epoch at the kick (vk_ww_view), which the resolve takes: it is a
       thread's own, and the memos the resolve asks hold their epoch as the renderer thread saw it */
    int ww_streaming;
    u32 ww_epoch;
    /* the timer's counts (vk_prep_times): the kick's stamp, and sums of the wait from a kick to the take, of the
       work, and of the renderer thread's wait in the join */
    u64 kick_at, lat, work, joined, join_wait;
    u64 kicks, cancels; /* the renderer thread's */
} g_prep;

static void spin_hint(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __builtin_arm_yield();
#endif
}
static void prep_thread(void* arg) {
    (void)arg;
    SNX_THREAD_NAME("GPU draw prep");
    for (;;) {
        u64 since = 0;
        for (u32 spins = 0;; spins++) {
            u32 want = PREP_GIVEN;
            if (atomic_compare_exchange_weak_explicit(&g_prep.state, &want, PREP_TAKEN, memory_order_acquire,
                                                      memory_order_relaxed))
                break;
            spin_hint();
            if (spins & 255) continue;
            u64 now = plat_ns();
            if (!since) since = now;
            if (now - since < PREP_SPIN_NS) continue;
            atomic_store_explicit(&g_prep.sleeps, 1, memory_order_seq_cst);
            if (atomic_load_explicit(&g_prep.state, memory_order_seq_cst) != PREP_GIVEN)
                plat_wait_word(&g_prep.state, atomic_load_explicit(&g_prep.state, memory_order_relaxed), 1000000);
            atomic_store_explicit(&g_prep.sleeps, 0, memory_order_relaxed);
            since = 0;
        }
        const u64 took = __builtin_readcyclecounter();
        g_prep.lat += took - g_prep.kick_at;
        vk_lock_lend(g_prep.lend);
        vk_ww_view_set(g_prep.ww_streaming, g_prep.ww_epoch);
        g_prep.job();
        g_prep.work += __builtin_readcyclecounter() - took;
        vk_lock_lend(0);
        atomic_store_explicit(&g_prep.state, PREP_DONE, memory_order_release);
    }
}
/* draw_prep=2: the thread on or off, as it measures faster, the way the renderer thread's mode is chosen (gpu_queue.c gq_auto). A
   mode's cost is the renderer thread's time in records a present (gq_run_ns), which the thread is there to cut. On a
   CPU with few cores the thread can take a core from the game's threads, and the renderer then waits longer.

   After hold presents in the chosen mode: its cost over PA_MEASURE presents (before), PA_WARM presents in the other
   mode, its cost over PA_MEASURE (the probe), then PA_WARM back and its cost again (after). The probe wins when it
   costs under 97% of both before and after. A probe whose before and after differ by more than a fifth counts for
   neither, and none starts while the renderer thread is off (the records on one thread) or costs under PA_FLOOR_MS a
   present. Two wins in a row change the choice. A loss doubles hold, up to PA_HOLD_MAX. It starts on: off, the first
   probe would run it for PA_MEASURE presents anyway */
#define PA_HOLD     600u
#define PA_HOLD_MAX 4800u
#define PA_MEASURE  120u
#define PA_WARM     10u
#define PA_FLOOR_MS 2.0
static struct {
    int on; /* the chosen mode, or the probe's while it runs */
    u64 seen, until, p0, run0;
    double before_ms, probe_ms;
    u32 hold, wins;
    int phase; /* as gq_auto's */
} g_pa = {.on = 1};
static double pa_cost(u64 p) {
    return p > g_pa.p0 ? (double)(gq_run_ns() - g_pa.run0) / 1e6 / (double)(p - g_pa.p0) : 0;
}
static const char* pa_name(int on) { return on ? "the draw preparation thread" : "the renderer thread alone"; }
static void prep_auto(void) {
    u64 p = gq_presents();
    if (p == g_pa.seen) return;
    g_pa.seen = p;
    if (!g_pa.hold) g_pa.hold = PA_HOLD, g_pa.until = p + PA_HOLD;
    if (p < g_pa.until) return;
    switch (g_pa.phase) {
        case 0:
            g_pa.p0 = p, g_pa.run0 = gq_run_ns();
            g_pa.phase = 1, g_pa.until = p + PA_MEASURE;
            break;
        case 1:
            g_pa.before_ms = pa_cost(p);
            if (!gq_split_now() || g_pa.before_ms < PA_FLOOR_MS) {
                g_pa.phase = 0, g_pa.until = p + g_pa.hold;
                break;
            }
            g_pa.on = !g_pa.on;
            g_pa.phase = 2, g_pa.until = p + PA_WARM;
            break;
        case 2:
        case 4:
            g_pa.p0 = p, g_pa.run0 = gq_run_ns();
            g_pa.phase++, g_pa.until = p + PA_MEASURE;
            break;
        case 3:
            g_pa.probe_ms = pa_cost(p);
            g_pa.on = !g_pa.on;
            g_pa.phase = 4, g_pa.until = p + PA_WARM;
            break;
        default: {
            const double before = g_pa.before_ms, after = pa_cost(p), probe = g_pa.probe_ms;
            const double lo = before < after ? before : after, hi = before < after ? after : before;
            const char *chosen = pa_name(g_pa.on), *other = pa_name(!g_pa.on);
            g_pa.phase = 0;
            if (hi > lo * 1.2) {
                LOG("vulkan: draw_prep=auto: %.3f ms a present with %s between %.3f and %.3f ms with %s: unsettled, "
                    "again later\n",
                    probe, other, before, after, chosen);
                g_pa.until = p + g_pa.hold;
                break;
            }
            const int win = probe < before * 0.97 && probe < after * 0.97;
            g_pa.wins = win ? g_pa.wins + 1 : 0;
            if (g_pa.wins >= 2) {
                g_pa.on = !g_pa.on;
                g_pa.wins = 0;
                g_pa.hold = PA_HOLD;
                LOG("vulkan: draw_prep=auto: %.3f ms a present with %s against %.3f and %.3f ms with %s: %s from now\n",
                    probe, other, before, after, chosen, other);
            } else {
                if (!win) g_pa.hold = g_pa.hold * 2 < PA_HOLD_MAX ? g_pa.hold * 2 : PA_HOLD_MAX;
                LOG("vulkan: draw_prep=auto: %.3f ms a present with %s against %.3f and %.3f ms with %s: %s\n", probe,
                    other, before, after, chosen, win ? "a win, one more to change" : "it stays");
            }
            g_pa.until = p + g_pa.hold;
        }
    }
}
/* the renderer thread's, once a draw: whether the next draw is resolved ahead */
int vk_prep_on(void) {
    /* with the records on the front end's thread (the one-thread mode), no next record is there to resolve */
    if (!g_settings.draw_prep || g_prep.failed || !gq_split_now()) return 0;
    if (g_settings.draw_prep == 1) return 1;
    prep_auto();
    return g_pa.on;
}
void vk_prep_init(void (*ahead)(void)) { g_prep.ahead = ahead; }
static int prep_start(void);
int vk_prep_kick(void) {
    if (!vk_prep_on() || !g_prep.ahead) return 0;
    g_prep.job = g_prep.ahead;
    g_prep.lend = 1;
    return prep_start();
}
/* a batch's work on the thread (render.c), which may take vk_lock (lent quietly): 0 when the thread didn't start */
int vk_prep_kick_work(void (*work)(void)) {
    g_prep.job = work;
    g_prep.lend = 2;
    return prep_start();
}
static int prep_start(void) {
    if (g_prep.failed) return 0;
    if (!g_prep.started) {
        g_prep.started = 1;
        if (plat_thread(prep_thread, NULL, 1u << 20)) { /* 0 when it started */
            g_prep.failed = 1;
            LOG("vulkan: draw_prep: its thread didn't start, draws are resolved on the renderer thread\n");
            return 0;
        }
        LOG("vulkan: draw_prep=1: the next draw's kept state is resolved on a thread of its own\n");
    }
    g_prep.kicks++;
    vk_ww_view(&g_prep.ww_streaming, &g_prep.ww_epoch);
    g_prep.kick_at = __builtin_readcyclecounter();
    atomic_store_explicit(&g_prep.state, PREP_GIVEN, memory_order_seq_cst);
    if (atomic_load_explicit(&g_prep.sleeps, memory_order_seq_cst)) plat_wake_word(&g_prep.state);
    return 1;
}
int vk_prep_join(void) {
    u32 want = PREP_GIVEN;
    if (atomic_compare_exchange_strong_explicit(&g_prep.state, &want, PREP_IDLE, memory_order_relaxed,
                                                memory_order_relaxed)) {
        g_prep.cancels++;
        return 0; /* not taken yet: back to the renderer thread */
    }
    const u64 t0 = __builtin_readcyclecounter();
    while (atomic_load_explicit(&g_prep.state, memory_order_acquire) != PREP_DONE) spin_hint();
    g_prep.join_wait += __builtin_readcyclecounter() - t0;
    g_prep.joined++;
    atomic_store_explicit(&g_prep.state, PREP_IDLE, memory_order_relaxed);
    return 1;
}
/* the sums in counter ticks since the last call: the kicks' waits for the thread, its work, the joins that waited
   for it and their waits */
void vk_prep_times(u64* lat, u64* work, u64* joined, u64* join_wait) {
    *lat = g_prep.lat;
    *work = g_prep.work;
    *joined = g_prep.joined;
    *join_wait = g_prep.join_wait;
    g_prep.lat = g_prep.work = g_prep.joined = g_prep.join_wait = 0;
}
void vk_prep_join_wait(void) {
    const u64 t0 = __builtin_readcyclecounter();
    while (atomic_load_explicit(&g_prep.state, memory_order_acquire) != PREP_DONE) spin_hint();
    g_prep.join_wait += __builtin_readcyclecounter() - t0;
    g_prep.joined++;
    atomic_store_explicit(&g_prep.state, PREP_IDLE, memory_order_relaxed);
}
void vk_prep_counts(u64* kicks, u64* cancels) {
    *kicks = g_prep.kicks;
    *cancels = g_prep.cancels;
    g_prep.kicks = g_prep.cancels = 0;
}
