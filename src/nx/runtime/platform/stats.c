/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/platform/stats.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/vpad.h"
#include "common/utilities/common.h"
#include "gui_theme_colors.h"
#include "snx_settings.h"
#include <stdio.h>
#include <string.h>

/* the menu's colours (gui_theme_colors.h, the YoRHa theme) as R G B A bytes */
#define STATS_RGBA(rgb, a) ((u32)(a) << 24 | ((rgb) & 0xFFu) << 16 | ((rgb) & 0xFF00u) | ((rgb) >> 16 & 0xFFu))
#define STATS_BACK         STATS_RGBA(SNX_GUI_THEME_YORHA_INK, 0xE0u)
#define STATS_TEXT         STATS_RGBA(SNX_GUI_THEME_YORHA_PAGE, 0xFFu)
#define STATS_GUIDE        STATS_RGBA(SNX_GUI_THEME_YORHA_SOFT, 0xFFu)
#define STATS_SLOW         STATS_RGBA(SNX_GUI_THEME_YORHA_FAILURE, 0xFFu)
#define STATS_SPAN_NS      500000000ull  /* the frame rate is over the last half second */
#define STATS_SAMPLE_NS    1000000000ull /* the processor and the memory are measured once a second */
#define STATS_GRAPH_LINES  3             /* the graph's height, in lines */

static struct {
    u64 at[STATS_FRAMES]; /* the presents of the game's last frames, a ring that next writes on */
    unsigned next, count;
    u64 frames;             /* the frames kept since stats went on */
    u64 generated;          /* the generated frames presented beside them (frame interpolation's blends) */
    u64 since, since_ns;    /* the frames and the time the frame rate is counted from */
    u64 since_gen;          /* the generated frames at that time */
    int fps10;              /* the frames a second over the last half second, in tenths; -1 for none yet */
    int gen10;              /* the generated frames a second over the same span, in tenths */
    u64 sampled_ns, cpu_ns; /* when the processor's time was read last, and what it was: the renderer's thread's */
    int cpu;                /* stats_measured's */
    u64 memory;
    int corner; /* stats_corner's, as vpad_corner reads it */
} g_stats = {.fps10 = -1, .cpu = -1};

void stats_corner_set(const IniOpt* o, const char* v) {
    int i = vpad_corner(v);
    if (i >= 0) {
        __atomic_store_n(&g_stats.corner, i, __ATOMIC_RELAXED); /* the renderer's thread reads it */
        return;
    }
    i = __atomic_load_n(&g_stats.corner, __ATOMIC_RELAXED);
    LOG("settings: stats_corner=%s is not tl, tr, bl or br: it keeps %s\n", v, vpad_corner_word(i));
    snprintf((char*)o->var, o->size, "%s", vpad_corner_word(i));
}
void stats_measured(int cpu, u64 memory) {
    ui_lock();
    g_stats.cpu = cpu;
    g_stats.memory = memory;
    ui_unlock();
}
/* the counters run while either overlay shows: the stats box reads them, and the showcase's line too */
static int counting(void) {
    return __atomic_load_n(&g_settings.stats, __ATOMIC_RELAXED) || g_settings.showcase;
}
int stats_rate10(void) { return g_stats.fps10; }
int stats_gen10(void) { return g_stats.gen10; }

void stats_generated(void) {
    if (!counting()) return;
    ui_lock();
    ++g_stats.generated;
    ui_unlock();
}

void stats_frame(u64 now_ns) {
    u64 cpu_ns;
    unsigned cores;
    if (!counting()) {
        if (g_stats.frames || g_stats.sampled_ns || g_stats.since_ns) { /* the next turn on starts from nothing */
            int corner = g_stats.corner;
            ui_lock();
            memset(&g_stats, 0, sizeof g_stats);
            g_stats.fps10 = g_stats.cpu = -1;
            g_stats.corner = corner;
            ui_unlock();
        }
        return;
    }
    ui_lock();
    g_stats.at[g_stats.next] = now_ns;
    g_stats.next = (g_stats.next + 1) % STATS_FRAMES;
    if (g_stats.count < STATS_FRAMES) ++g_stats.count;
    ++g_stats.frames;
    /* the half second's rates here, not at the draw: the showcase's line reads them too, and it draws no box */
    if (!g_stats.since_ns) {
        g_stats.since_ns = now_ns;
        g_stats.since = g_stats.frames;
    } else if (now_ns > g_stats.since_ns && now_ns - g_stats.since_ns >= STATS_SPAN_NS) {
        g_stats.fps10 = (int)(((g_stats.frames - g_stats.since) * 10000000000ull + (now_ns - g_stats.since_ns) / 2) /
                              (now_ns - g_stats.since_ns));
        g_stats.gen10 = (int)(((g_stats.generated - g_stats.since_gen) * 10000000000ull +
                               (now_ns - g_stats.since_ns) / 2) /
                              (now_ns - g_stats.since_ns));
        g_stats.since_ns = now_ns;
        g_stats.since = g_stats.frames;
        g_stats.since_gen = g_stats.generated;
    }
    ui_unlock();
    if (g_stats.sampled_ns && now_ns - g_stats.sampled_ns < STATS_SAMPLE_NS) return;
    cpu_ns = snx_process_cpu_ns();
    cores = snx_cpu_count();
    if (g_stats.sampled_ns)
        stats_measured(cpu_ns && g_stats.cpu_ns && cores
                           ? (int)((cpu_ns - g_stats.cpu_ns) * 100 / ((now_ns - g_stats.sampled_ns) * cores))
                           : -1,
                       snx_footprint());
    g_stats.sampled_ns = now_ns;
    g_stats.cpu_ns = cpu_ns;
}

void stats_box(const int safe[4], int scale, int cols, float box[4]) {
    float cw = 8.0f * (float)scale, ch = 16.0f * (float)scale, gap = ch / 2, showcase[4];
    int corner = __atomic_load_n(&g_stats.corner, __ATOMIC_RELAXED);
    box[2] = (float)cols * cw;
    box[3] = (2 + STATS_GRAPH_LINES) * ch + 3 * ch / 4; /* two lines and the graph, a quarter line around each */
    if (vpad_showcase_box(safe, showcase) == corner) {  /* beside showcase's box, toward the picture's middle */
        box[0] = corner & 1 ? showcase[0] + showcase[2] - box[2] : showcase[0];
        box[1] = corner & 2 ? showcase[1] - gap - box[3] : showcase[1] + showcase[3] + gap;
        return;
    }
    box[0] = corner & 1 ? (float)(safe[0] + safe[2]) - gap - box[2] : (float)safe[0] + gap;
    box[1] = corner & 2 ? (float)(safe[1] + safe[3]) - gap - box[3] : (float)safe[1] + gap;
}

/* the time between the frame j back from the newest and the one before it, in ms; j below count - 1 */
static float frame_ms(unsigned j) {
    unsigned newer = (g_stats.next + 2 * STATS_FRAMES - 1 - j) % STATS_FRAMES;
    unsigned older = (newer + STATS_FRAMES - 1) % STATS_FRAMES;
    return (float)(g_stats.at[newer] - g_stats.at[older]) / 1e6f;
}
void stats_draw(SnxUiList* l, const int safe[4], int scale, u64 now_ns) {
    float cw = 8.0f * (float)scale, ch = 16.0f * (float)scale, box[4], gx, gy, gw, gh, bw;
    char fps[16], ms[16], line[2][STATS_COLS_FRAMEGEN + 1];
    unsigned j;
    int cols = STATS_COLS;
    if (!__atomic_load_n(&g_settings.stats, __ATOMIC_RELAXED)) return;
    /* the rates are also taken here, so a stall (no frame for a while) reads 0.0 rather than the last rate */
    if (!g_stats.since_ns) {
        g_stats.since_ns = now_ns;
        g_stats.since = g_stats.frames;
    } else if (now_ns > g_stats.since_ns && now_ns - g_stats.since_ns >= STATS_SPAN_NS) {
        g_stats.fps10 = (int)(((g_stats.frames - g_stats.since) * 10000000000ull + (now_ns - g_stats.since_ns) / 2) /
                              (now_ns - g_stats.since_ns));
        g_stats.gen10 = (int)(((g_stats.generated - g_stats.since_gen) * 10000000000ull +
                               (now_ns - g_stats.since_ns) / 2) /
                              (now_ns - g_stats.since_ns));
        g_stats.since_ns = now_ns;
        g_stats.since = g_stats.frames;
        g_stats.since_gen = g_stats.generated;
    }
    if (g_stats.fps10 >= 0)
        snprintf(fps, sizeof fps, "%d.%d fps", g_stats.fps10 / 10, g_stats.fps10 % 10);
    else
        snprintf(fps, sizeof fps, "? fps");
    if (g_stats.count >= 2) {
        int tenths = (int)(frame_ms(0) * 10 + 0.5f);
        snprintf(ms, sizeof ms, "%d.%d ms", tenths / 10, tenths % 10);
    } else
        snprintf(ms, sizeof ms, "? ms");
    if (g_stats.gen10 > 0) {
        /* frame interpolation on: the real rate, the generated frames a second beside it, then their total */
        int total = g_stats.fps10 + g_stats.gen10;
        snprintf(line[0], sizeof line[0], "%s  %d.%d gen  %d.%d total  %s", fps, g_stats.gen10 / 10,
                 g_stats.gen10 % 10, total / 10, total % 10, ms);
        cols = STATS_COLS_FRAMEGEN;
    } else
        snprintf(line[0], sizeof line[0], "%s  %s", fps, ms);
    if (g_stats.cpu >= 0 && g_stats.memory)
        snprintf(line[1], sizeof line[1], "cpu %d%%  %llu MiB", g_stats.cpu,
                 (unsigned long long)(g_stats.memory >> 20));
    else if (g_stats.cpu >= 0)
        snprintf(line[1], sizeof line[1], "cpu %d%%  ? MiB", g_stats.cpu);
    else if (g_stats.memory)
        snprintf(line[1], sizeof line[1], "cpu ?  %llu MiB", (unsigned long long)(g_stats.memory >> 20));
    else
        snprintf(line[1], sizeof line[1], "cpu ?  ? MiB");
    stats_box(safe, scale, cols, box);
    ui_fill(l, box[0], box[1], box[2], box[3], STATS_BACK);
    ui_text(l, box[0] + cw, box[1] + ch / 4, scale, line[0], STATS_TEXT);
    ui_text(l, box[0] + cw, box[1] + ch / 4 + ch, scale, line[1], STATS_TEXT);
    /* the graph: each frame's time a bar up from its foot, the newest at the right, over the two guides */
    gx = box[0] + cw;
    gw = box[2] - 2 * cw;
    gy = box[1] + 2 * ch + ch / 2;
    gh = STATS_GRAPH_LINES * ch;
    bw = gw / (STATS_FRAMES - 1);
    ui_fill(l, gx, gy + gh - gh * STATS_60_MS / STATS_TOP_MS, gw, (float)scale, STATS_GUIDE);
    ui_fill(l, gx, gy + gh - gh * STATS_30_MS / STATS_TOP_MS, gw, (float)scale, STATS_GUIDE);
    for (j = 0; j + 1 < g_stats.count; ++j) {
        float t = frame_ms(j), bh = gh * (t < STATS_TOP_MS ? t : STATS_TOP_MS) / STATS_TOP_MS;
        ui_fill(l, gx + gw - (float)(j + 1) * bw, gy + gh - bh, bw, bh, t > STATS_30_MS ? STATS_SLOW : STATS_TEXT);
    }
}
