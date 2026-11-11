/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The stats box over the game (runtime/platform/stats.c): nothing while off; its letters a step below the menu's; each
   line after its icon, the GPU's beside the processor's; its lines as the frames and the measures say them, the
   longest inside the box; ? for what is not known; the GPU's share and time a frame from its busy time over a second,
   ? where the renderer can't time it or wasn't at the second before; the graph's title and its guides' labels; the
   graph's bars, a slow frame's in the failure colour; its corner, and beside
   showcase's box in showcase's corner; a word that is no corner; and off forgetting what it had. Under the graph,
   while they apply: RenderDoc's lines (a capture can be taken; the last capture's file, ? where RenderDoc gave none, or
   its failure in the failure colour) and the debugger's, the box wider for a line that needs it and a file's name cut
   from its start past the safe area, RenderDoc's lines kept while stats is off; and on Linux and FreeBSD a child the
   test traces shows the test as its debugger */
#include "nx/runtime/platform/stats.h"
#include "nx/runtime/platform/vpad.h"
#include "nx/runtime/platform/controls_atlas.h"
#include "nx/runtime/platform/plat.h"
#include "gui/icons.h"
#include "gui_theme_colors.h"
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__) || defined(__FreeBSD__)
#include <errno.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define TRACED_CHILD 1
/* the child asks to be traced by its parent; the parent lets it go on from a stop */
#if defined(__linux__)
#define TRACE_ME()    ptrace(PTRACE_TRACEME, 0, NULL, NULL)
#define TRACE_GO(pid) ptrace(PTRACE_CONT, (pid), NULL, NULL)
#else
#define TRACE_ME()    ptrace(PT_TRACE_ME, 0, NULL, 0)
#define TRACE_GO(pid) ptrace(PT_CONTINUE, (pid), (caddr_t)1, 0)
#endif
#endif

/* ui.c's lock and log, and what vpad.c and the Display table name, without the platform layer */
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    abort();
}
void options_language_set(const IniOpt* o, const char* v) {
    (void)o;
    (void)v;
}
void options_tv_mode_set(const IniOpt* o, const char* v) {
    (void)o;
    (void)v;
}
/* the test's state: the last line logged, the quads drawn, the line line_at read, what the renderer reads of the GPU
   at each frame (nothing, at first), and the menu's scale draw gives (3: the box's letters at 2) */
static struct {
    char logged[256];
    SnxUiQuad quads[4096];
    SnxUiList l;
    char line[256]; /* past STATS_COLS: a line under the graph widens the box */
    SnxStatsGpu gpu;
    int menu;
} g_test = {.menu = 3};
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    vsnprintf(g_test.logged, sizeof g_test.logged, fmt, ap);
    va_end(ap);
}

#define FAILURE                                                                                                        \
    (0xFFu << 24 | (SNX_GUI_THEME_YORHA_FAILURE & 0xFFu) << 16 | (SNX_GUI_THEME_YORHA_FAILURE & 0xFF00u) |             \
     (SNX_GUI_THEME_YORHA_FAILURE >> 16 & 0xFFu))
#define TEXT_RGBA                                                                                                      \
    (0xFFu << 24 | (SNX_GUI_THEME_YORHA_PAGE & 0xFFu) << 16 | (SNX_GUI_THEME_YORHA_PAGE & 0xFF00u) |                   \
     (SNX_GUI_THEME_YORHA_PAGE >> 16 & 0xFFu))
#define GROUND_RGBA                                                                                                    \
    (0xFFu << 24 | (SNX_GUI_THEME_YORHA_INK & 0xFFu) << 16 | (SNX_GUI_THEME_YORHA_INK & 0xFF00u) |                     \
     (SNX_GUI_THEME_YORHA_INK >> 16 & 0xFFu))
#define MS 1000000ull /* a millisecond, in ns */

static const int safe[4] = {100, 50, 1280, 720};
static void draw(u64 now) {
    g_test.l.q = g_test.quads;
    g_test.l.n = 0;
    g_test.l.max = 4096;
    stats_draw(&g_test.l, safe, g_test.menu, now);
}
/* the line of letters drawn at x, y in the box's letters over cols of them, its spaces back, trimmed */
static const char* line_at(float x, float y, int cols) {
    int k, i, end = 0;
    float cw = 8.0f * (float)stats_scale(g_test.menu);
    assert(cols >= 0 && cols < (int)sizeof g_test.line);
    for (k = 0; k < cols; ++k) {
        g_test.line[k] = ' ';
        for (i = 0; i < g_test.l.n; ++i)
            if (g_test.quads[i].uw == 8 && g_test.quads[i].vh == 16 && g_test.quads[i].x == x + (float)k * cw &&
                g_test.quads[i].y == y)
                g_test.line[k] = (char)((g_test.quads[i].v - SNX_CONTROLS_FONT_Y) / 16 * 16 +
                                        (g_test.quads[i].u - SNX_CONTROLS_FONT_X) / 8);
        if (g_test.line[k] != ' ') end = k + 1;
    }
    g_test.line[end] = 0;
    return g_test.line;
}
/* the box's line k from the top; the processor's (line 1) up to the GPU's icon */
static const char* text(int k) {
    int s = stats_scale(g_test.menu);
    float box[4], cw = 8.0f * (float)s, ch = 16.0f * (float)s;
    stats_box(safe, s, box);
    return line_at(box[0] + 4 * cw, box[1] + ch / 4 + (float)k * ch, k == 1 ? STATS_GPU_AT - 4 : STATS_COLS - 4);
}
/* the GPU's text, after its icon beside the processor's line */
static const char* gpu_text(void) {
    int s = stats_scale(g_test.menu);
    float box[4], cw = 8.0f * (float)s, ch = 16.0f * (float)s;
    stats_box(safe, s, box);
    return line_at(box[0] + (STATS_GPU_AT + 3) * cw, box[1] + ch / 4 + ch, STATS_COLS - STATS_GPU_AT - 3);
}
/* the label at the graph's right of its guide of ms */
static const char* guide_label(float ms) {
    int s = stats_scale(g_test.menu);
    float box[4], cw = 8.0f * (float)s, ch = 16.0f * (float)s, gw, gy, gh = 3 * ch;
    stats_box(safe, s, box);
    gw = box[2] - 2 * cw - STATS_AXIS * cw;
    gy = box[1] + STATS_LINES * ch + ch / 2;
    return line_at(box[0] + cw + gw + cw, gy + gh - gh * ms / STATS_TOP_MS - ch / 2, STATS_AXIS);
}
/* the menus' icon drawn at the box's cell `at` of line k, two of its letters wide in the text's colour, sharp:
   GUI_ICON_NONE for none */
static unsigned icon_at(int k, int at) {
    int s = stats_scale(g_test.menu), i;
    float box[4], cw = 8.0f * (float)s, ch = 16.0f * (float)s;
    stats_box(safe, s, box);
    for (i = 0; i < g_test.l.n; ++i) {
        const SnxUiQuad* q = &g_test.quads[i];
        if (q->uw == 16 && q->vh == 16 && q->v == SNX_CONTROLS_ATLAS_HEIGHT && q->x == box[0] + (float)at * cw &&
            q->y == box[1] + ch / 4 + (float)k * ch && q->w == 2 * cw && q->h == ch && q->rgba == TEXT_RGBA &&
            q->sample == UI_SHARP)
            return GUI_ICON_FIRST + (unsigned)(q->u - SNX_UI_ICONS_X) / 16;
    }
    return GUI_ICON_NONE;
}
/* the icon before the box's line k */
static unsigned icon(int k) { return icon_at(k, 1); }
/* the line k under the graph, after its icon */
static const char* note(int k) {
    int s = stats_scale(g_test.menu);
    float box[4], cw = 8.0f * (float)s, ch = 16.0f * (float)s;
    stats_box(safe, s, box);
    return line_at(box[0] + 4 * cw, box[1] + 8 * ch + 3 * ch / 4 + (float)k * ch, (int)(box[2] / cw) - 4);
}
/* the icon before the line k under the graph, drawn in rgba: GUI_ICON_NONE for none */
static unsigned note_icon(int k, u32 rgba) {
    int s = stats_scale(g_test.menu), i;
    float box[4], cw = 8.0f * (float)s, ch = 16.0f * (float)s;
    stats_box(safe, s, box);
    for (i = 0; i < g_test.l.n; ++i) {
        const SnxUiQuad* q = &g_test.quads[i];
        if (q->uw == 16 && q->vh == 16 && q->v == SNX_CONTROLS_ATLAS_HEIGHT && q->x == box[0] + cw &&
            q->y == box[1] + 8 * ch + 3 * ch / 4 + (float)k * ch && q->rgba == rgba)
            return GUI_ICON_FIRST + (unsigned)(q->u - SNX_UI_ICONS_X) / 16;
    }
    return GUI_ICON_NONE;
}
/* the letters of the line k under the graph drawn in rgba, its spaces (backed: the ground) left out */
static int note_letters(int k, u32 rgba) {
    int s = stats_scale(g_test.menu), i, n = 0;
    float box[4], ch = 16.0f * (float)s;
    stats_box(safe, s, box);
    for (i = 0; i < g_test.l.n; ++i)
        if (g_test.quads[i].uw == 8 && g_test.quads[i].vh == 16 && g_test.quads[i].glyph != ' ' &&
            g_test.quads[i].y == box[1] + 8 * ch + 3 * ch / 4 + (float)k * ch && g_test.quads[i].rgba == rgba)
            ++n;
    return n;
}
/* the icons and letters drawn past a cell in from the box's sides: 0 when they all keep it */
static int past_margin(void) {
    int s = stats_scale(g_test.menu), i, n = 0;
    float box[4], cw = 8.0f * (float)s;
    stats_box(safe, s, box);
    for (i = 0; i < g_test.l.n; ++i)
        if (g_test.quads[i].vh == 16 &&
            (g_test.quads[i].x < box[0] + cw || g_test.quads[i].x + g_test.quads[i].w > box[0] + box[2] - cw))
            ++n;
    return n;
}
/* the scale every letter was drawn at, its 8x16 texels 8 by 16 times it, each backed (its clear pixels the ground,
   ui.h's UI_BACKED, the ground the menu's ink): 0 where one differs */
static int drawn_scale(void) {
    int i, s = 0;
    if (ui_ground() != GROUND_RGBA) return 0;
    for (i = 0; i < g_test.l.n; ++i) {
        const SnxUiQuad* q = &g_test.quads[i];
        if (q->uw != 8 || q->vh != 16) continue;
        if (!s) s = (int)q->w / 8;
        if (q->w != (float)(8 * s) || q->h != (float)(16 * s) || !(q->turn & UI_BACKED)) return 0;
    }
    return s;
}
/* the graph's bars as they are drawn (ui_drawn: the ring's quads, a byte each): how many, and the newest, at the
   right */
static int bars(const SnxUiQuad** newest) {
    static SnxUiQuad at_right;
    int i, n = 0;
    *newest = NULL;
    for (i = 0; i < g_test.l.n; ++i) {
        SnxUiQuad d = ui_drawn(&g_test.quads[i]);
        if (g_test.quads[i].ink != UI_RING || !d.uw) continue;
        if (!*newest || d.x > at_right.x) {
            at_right = d;
            *newest = &at_right;
        }
        ++n;
    }
    return n;
}
/* n frames of ms each from at, the GPU busy for busy_ms of each, as the renderer reads it; the last frame's time */
static u64 frames(u64 at, int n, u64 ms, u64 busy_ms) {
    int k;
    for (k = 0; k < n; ++k) {
        g_test.gpu.busy_ns += busy_ms * MS;
        stats_frame(at += ms * MS, &g_test.gpu);
    }
    return at;
}

int main(void) {
    const IniOpt *corner = ini_find(g_display_ini, "stats_corner"),
                 *showcase = ini_find(g_display_ini, "showcase_corner");
    const SnxUiQuad* newest;
    float box[4], show[4], gh = 3 * 32;
    u64 t = 1000 * MS, at;
    int k;
    assert(corner && showcase);
    /* off: nothing drawn, and no frame kept */
    stats_frame(t, &g_test.gpu);
    draw(t);
    assert(g_test.l.n == 0);
    /* on, in its default corner, top left: half a line in from the safe area's; ? before a frame or a measure */
    g_settings.stats = 1;
    stats_box(safe, 2, box);
    assert(box[0] == 100 + 16 && box[1] == 50 + 16 && box[2] == STATS_COLS * 16 && box[3] == 8 * 32 + 24);
    draw(t);
    assert(!strcmp(text(0), "? fps  ? ms") && !strcmp(text(1), "cpu ?") && !strcmp(gpu_text(), "gpu ?  ? ms"));
    assert(!strcmp(text(2), "ram ?/? MiB") && !strcmp(text(3), "vram ?/? MiB") && !strcmp(text(4), "frame time"));
    /* the graph's guides labelled with their times at its right */
    assert(!strcmp(guide_label(STATS_60_MS), "16.7 ms") && !strcmp(guide_label(STATS_30_MS), "33.3 ms"));
    /* its letters a step below the menu's, 1 at least: the box and each letter at that scale, the lines where it puts
       them */
    assert(stats_scale(1) == 1 && stats_scale(2) == 1 && stats_scale(3) == 2 && stats_scale(4) == 3);
    for (g_test.menu = 1; g_test.menu <= 4; ++g_test.menu) {
        draw(t);
        if (drawn_scale() != stats_scale(g_test.menu) || strcmp(text(0), "? fps  ? ms")) {
            fprintf(stderr, "the menu at %d: the box drawn at %d, not %d; its first line \"%s\"\n", g_test.menu,
                    drawn_scale(), stats_scale(g_test.menu), text(0));
            return 1;
        }
        /* each line after its icon: the frames' gauge, the processor (the GPU's chip beside it), the memory's stick,
           the video memory's layers, the graph's pulse */
        if (icon(0) != GUI_ICON_GAUGE || icon(1) != GUI_ICON_CPU || icon_at(1, STATS_GPU_AT) != GUI_ICON_GPU ||
            icon(2) != GUI_ICON_MEMORY_STICK || icon(3) != GUI_ICON_LAYERS || icon(4) != GUI_ICON_ACTIVITY) {
            fprintf(
                stderr,
                "the menu at %d: the icons U+%04X U+%04X U+%04X U+%04X U+%04X U+%04X, not gauge U+%04X, cpu U+%04X, "
                "gpu U+%04X, memory-stick U+%04X, layers U+%04X, activity U+%04X\n",
                g_test.menu, icon(0), icon(1), icon_at(1, STATS_GPU_AT), icon(2), icon(3), icon(4), GUI_ICON_GAUGE,
                GUI_ICON_CPU, GUI_ICON_GPU, GUI_ICON_MEMORY_STICK, GUI_ICON_LAYERS, GUI_ICON_ACTIVITY);
            return 1;
        }
    }
    g_test.menu = 3;
    /* 30 frames in half a second: 60.0 frames a second, the last 16.7 ms; 29 bars under the 60 FPS guide */
    for (k = 1; k <= 30; ++k) stats_frame(at = t + (u64)k * 16666667ull, &g_test.gpu);
    draw(t + 500 * MS);
    assert(!strcmp(text(0), "60.0 fps  16.7 ms"));
    assert(bars(&newest) == 29 && newest->rgba != FAILURE && newest->h > gh * 16 / 50 && newest->h < gh * 17 / 50);
    /* the processor's share and the memory as measured, and ? for one not told */
    stats_measured(&(SnxStatsMeasure){.cpu = 25, .memory = 512ull << 20, .gpu = -1});
    draw(t + 600 * MS);
    assert(!strcmp(text(1), "cpu 25%") && !strcmp(text(2), "ram 512/? MiB"));
    stats_measured(&(SnxStatsMeasure){.cpu = -1, .memory = 512ull << 20, .gpu = -1});
    draw(t + 600 * MS);
    assert(!strcmp(text(1), "cpu ?") && !strcmp(text(2), "ram 512/? MiB"));
    stats_measured(&(SnxStatsMeasure){.cpu = 25, .memory = 512ull << 20, .memory_total = 31500ull << 20, .gpu = -1});
    draw(t + 600 * MS);
    assert(!strcmp(text(2), "ram 512/31500 MiB"));
    stats_measured(&(SnxStatsMeasure){.cpu = 7, .gpu = -1});
    draw(t + 600 * MS);
    assert(!strcmp(text(1), "cpu 7%") && !strcmp(text(2), "ram ?/? MiB"));
    /* the GPU's share and its time a frame, in tenths of a millisecond rounded, and the video memory's use and
       budget, as measured; ? for one not told */
    stats_measured(&(SnxStatsMeasure){
        .cpu = 7, .gpu = 63, .gpu_ns = 10450000, .vram = 3120ull << 20, .vram_budget = 7980ull << 20});
    draw(t + 600 * MS);
    assert(!strcmp(text(1), "cpu 7%"));
    assert(!strcmp(gpu_text(), "gpu 63%  10.5 ms") && !strcmp(text(3), "vram 3120/7980 MiB"));
    stats_measured(&(SnxStatsMeasure){.cpu = 7, .gpu = -1, .vram = 3120ull << 20});
    draw(t + 600 * MS);
    assert(!strcmp(gpu_text(), "gpu ?  ? ms") && !strcmp(text(3), "vram 3120/? MiB"));
    stats_measured(&(SnxStatsMeasure){.cpu = 7, .gpu = 0, .gpu_ns = 40000, .vram_budget = 7980ull << 20});
    draw(t + 600 * MS);
    assert(!strcmp(gpu_text(), "gpu 0%  0.0 ms") && !strcmp(text(3), "vram ?/7980 MiB"));
    /* the longest lines, a 24 GiB card full and a slow GPU, inside the box with a cell to spare */
    stats_measured(&(SnxStatsMeasure){.cpu = 100,
                                      .memory = 65535ull << 20,
                                      .memory_total = 65535ull << 20,
                                      .gpu = 100,
                                      .gpu_ns = 100000000,
                                      .vram = 24564ull << 20,
                                      .vram_budget = 24564ull << 20});
    draw(t + 600 * MS);
    assert(!strcmp(text(1), "cpu 100%") && !strcmp(gpu_text(), "gpu 100%  100.0 ms"));
    assert(!strcmp(text(2), "ram 65535/65535 MiB"));
    assert(!strcmp(text(3), "vram 24564/24564 MiB") && past_margin() == 0);
    /* a 40 ms frame in the failure colour, then an 80 ms one as tall as the graph */
    stats_frame(at += 40 * MS, &g_test.gpu);
    draw(t + 700 * MS);
    assert(strstr(text(0), "40.0 ms") && bars(&newest) == 30 && newest->rgba == FAILURE);
    assert(newest->h > gh * 39 / 50 && newest->h < gh * 41 / 50);
    stats_frame(at += 80 * MS, &g_test.gpu);
    draw(t + 800 * MS);
    assert(strstr(text(0), "80.0 ms") && bars(&newest) == 31 && newest->h == gh);
    /* no frame for half a second: 0.0 frames a second */
    draw(t + 1100 * MS); /* the window that counted the two slow frames */
    draw(t + 1700 * MS);
    assert(!strncmp(text(0), "0.0 fps", 7));
    /* past STATS_FRAMES frames: the graph keeps the last of them */
    for (k = 0; k < 2 * STATS_FRAMES; ++k) stats_frame(at += 16 * MS, &g_test.gpu);
    draw(at);
    assert(bars(&newest) == STATS_FRAMES - 1);
    /* the box laid out once: other values, and another frame, take the same quads at the same places, the lines'
       letters and the bars' heights all that changes (ui_cells) */
    {
        static SnxUiQuad before[4096];
        static u8 heights[SNX_UI_RING_MAX];
        int n = g_test.l.n, moved = 0, rewritten = 0, i;
        char line[32], *end;
        memcpy(before, g_test.quads, (size_t)n * sizeof *before);
        memcpy(heights, ui_ring()->height, sizeof heights);
        stats_frame(at += 40 * MS, &g_test.gpu); /* before the measure: one it takes would go over it */
        stats_measured(&(SnxStatsMeasure){.cpu = 100, .memory = 65535ull << 20, .gpu = 100, .gpu_ns = 99990000});
        draw(at);
        assert(g_test.l.n == n && !strcmp(text(1), "cpu 100%") && !strcmp(gpu_text(), "gpu 100%  100.0 ms"));
        assert(strstr(text(0), "40.0 ms"));
        for (i = 0; i < n; ++i) {
            const SnxUiQuad *q = &g_test.quads[i], *p = &before[i];
            /* the bars' quads too: the ring's bytes are what changes */
            moved += q->x != p->x || q->w != p->w || q->y != p->y || q->h != p->h ||
                     (q->ink == UI_RING && memcmp(q, p, sizeof *q));
        }
        /* of the ring, only the new frame's byte */
        for (i = 0; i < SNX_UI_RING_MAX; ++i) rewritten += heights[i] != ui_ring()->height[i];
        assert(!moved && rewritten == 1);
        {
            /* the newest bar where the GPU scrolls it: at the graph's right edge, a cell in and STATS_AXIS from the
               box's */
            const SnxUiQuad* newest_bar;
            float cw = 8.0f * (float)stats_scale(g_test.menu), right;
            stats_box(safe, stats_scale(g_test.menu), box);
            right = box[0] + cw + (box[2] - 2 * cw - STATS_AXIS * cw);
            assert(bars(&newest_bar) == STATS_FRAMES - 1 && newest_bar->rgba == FAILURE);
            assert(fabsf(newest_bar->x + newest_bar->w - right) < 0.01f);
        }
        /* the cells' letters without printf */
        *ui_put_u(line, 0) = 0;
        assert(!strcmp(line, "0"));
        *ui_put_u(line, 18446744073709551615ull) = 0;
        assert(!strcmp(line, "18446744073709551615"));
        *ui_put_tenths(ui_put(line, "at "), 7) = 0;
        assert(!strcmp(line, "at 0.7"));
        end = ui_put_tenths(line, 12345);
        *end = 0;
        assert(!strcmp(line, "1234.5") && end == line + 6);
        /* longer than its cells: cut at their end; a shorter one blanks the rest, which draw nothing */
        {
            SnxUiQuad cells[4];
            SnxUiList kept = {cells, 0, 4};
            assert(ui_cells(&kept, 0, 0, 1, 5, TEXT_RGBA) == -1 && ui_cells(&kept, 0, 0, 1, 4, TEXT_RGBA) == 0);
            assert(ui_cells_set(cells, 4, "abcdef") == 4 && cells[3].u == SNX_CONTROLS_FONT_X + 'd' % 16 * 8);
            assert(ui_cells_set(cells, 4, "abcdef") == 0 && ui_cells_set(cells, 4, "a c") == 2);
            assert(cells[1].uw == 0 && cells[3].uw == 0 && cells[2].uw == 8 && cells[3].x == 24 && cells[3].w == 8);
        }
    }
    /* the GPU's lines from what the renderer reads at each frame, measured once a second from the first frame: 12 ms
       busy in each 20 ms frame is 60% and 12.0 ms; the video memory as it reads it */
    g_settings.stats = 0;
    stats_frame(at += 20 * MS, &g_test.gpu); /* forgets the measures so far */
    g_settings.stats = 1;
    g_test.gpu = (SnxStatsGpu){.busy_ns = 5000 * MS, .timed = 1, .vram = 2048ull << 20, .vram_budget = 8192ull << 20};
    stats_frame(at += 20 * MS, &g_test.gpu);
    at = frames(at, 49, 20, 12);
    draw(at);
    assert(!strcmp(gpu_text(), "gpu ?  ? ms") && !strcmp(text(3), "vram ?/? MiB")); /* 980 ms: not a second yet */
    at = frames(at, 1, 20, 12);
    draw(at);
    assert(!strcmp(gpu_text(), "gpu 60%  12.0 ms") && !strcmp(text(3), "vram 2048/8192 MiB"));
    /* a second the renderer couldn't time: ?, and the next too, which has no start; then 5 ms of each 20 again */
    g_test.gpu.timed = 0;
    at = frames(at, 50, 20, 12);
    draw(at);
    assert(!strcmp(gpu_text(), "gpu ?  ? ms") && !strcmp(text(3), "vram 2048/8192 MiB"));
    g_test.gpu.timed = 1;
    at = frames(at, 50, 20, 5);
    draw(at);
    assert(!strcmp(gpu_text(), "gpu ?  ? ms"));
    at = frames(at, 50, 20, 5);
    draw(at);
    assert(!strcmp(gpu_text(), "gpu 25%  5.0 ms"));
    /* a busy GPU at 30 frames a second: 33 ms of each 34; the video memory no longer told */
    g_test.gpu.vram = g_test.gpu.vram_budget = 0;
    at = frames(at, 30, 34, 33);
    draw(at);
    assert(!strcmp(gpu_text(), "gpu 97%  33.0 ms") && !strcmp(text(3), "vram ?/? MiB"));
    /* bottom right; beside showcase's box in showcase's own corner, above it there, below it in a top one; showcase in
       another corner leaves it in its own */
    stats_corner_set(corner, "br");
    stats_box(safe, 2, box);
    assert(box[0] + box[2] == 100 + 1280 - 16 && box[1] + box[3] == 50 + 720 - 16);
    g_settings.showcase = 1;
    vpad_showcase_corner_set(showcase, "br");
    assert(vpad_showcase_box(safe, show) == 3);
    stats_box(safe, 2, box);
    assert(box[0] + box[2] == show[0] + show[2] && box[1] + box[3] == show[1] - 16);
    stats_corner_set(corner, "tl");
    vpad_showcase_corner_set(showcase, "tl");
    stats_box(safe, 2, box);
    assert(vpad_showcase_box(safe, show) == 0 && box[0] == show[0] && box[1] == show[1] + show[3] + 16);
    vpad_showcase_corner_set(showcase, "br");
    stats_box(safe, 2, box);
    assert(box[0] == 100 + 16 && box[1] == 50 + 16);
    g_settings.showcase = 0;
    /* a word that is no corner: the corner it had, said with both */
    stats_corner_set(corner, "middle");
    assert(!strcmp(g_test.logged, "settings: stats_corner=middle is not tl, tr, bl or br: it keeps tl\n") &&
           !strcmp(g_settings.stats_corner, "tl"));
    /* off forgets what it had: on again, ? until it has frames and measures again */
    g_settings.stats = 0;
    stats_frame(at += 16 * MS, &g_test.gpu);
    draw(at);
    assert(g_test.l.n == 0);
    g_settings.stats = 1;
    draw(at);
    assert(!strcmp(text(0), "? fps  ? ms") && !strcmp(text(1), "cpu ?") && !strcmp(text(2), "ram ?/? MiB") &&
           !bars(&newest));
    assert(!strcmp(gpu_text(), "gpu ?  ? ms") && !strcmp(text(3), "vram ?/? MiB"));
    /* RenderDoc's lines and the debugger's under the graph, each after its icon; the box a line longer for each, and a
       quarter line after them */
    {
        SnxStatsCapture c = {.ready = 1};
        float plain = 8 * 32 + 24;
        draw(at);
        stats_box(safe, 2, box);
        assert(box[3] == plain && !note(0)[0]);
        stats_capture(&c);
        draw(at);
        stats_box(safe, 2, box);
        assert(box[3] == plain + 32 + 8 && box[2] == (34 + 5) * 16); /* the line's 34 letters past STATS_COLS */
        assert(!strcmp(note(0), "RenderDoc: Ctrl+Shift+F12 captures") && note_icon(0, TEXT_RGBA) == GUI_ICON_IMAGE);
        /* a capture written: how many, and the last one's file by its name */
        c.taken = 2;
        snprintf(c.path, sizeof c.path, "/caps/game_capture_2.rdc");
        stats_capture(&c);
        draw(at);
        assert(!strcmp(note(1), "capture 2: game_capture_2.rdc") && note_icon(1, TEXT_RGBA) == GUI_ICON_SAVE);
        stats_box(safe, 2, box);
        assert(box[3] == plain + 2 * 32 + 8 && past_margin() == 0);
        c.taken = 3;
        snprintf(c.path, sizeof c.path, "C:\\caps\\game_capture_3.rdc");
        stats_capture(&c);
        draw(at);
        assert(!strcmp(note(1), "capture 3: game_capture_3.rdc"));
        c.path[0] = 0; /* RenderDoc gave no path */
        stats_capture(&c);
        draw(at);
        assert(!strcmp(note(1), "capture 3: ?"));
        /* the next one failed: said in the failure colour, its icon too */
        c.failed = 1;
        stats_capture(&c);
        draw(at);
        assert(!strcmp(note(1), "capture 4 failed") && note_letters(1, FAILURE) == 14 &&
               note_letters(1, TEXT_RGBA) == 0);
        assert(note_icon(1, FAILURE) == GUI_ICON_SAVE);
        /* a debugger: its line last */
        stats_debugger("gdb");
        draw(at);
        assert(!strcmp(note(0), "RenderDoc: Ctrl+Shift+F12 captures") && !strcmp(note(2), "debugger attached: gdb"));
        assert(note_icon(2, TEXT_RGBA) == GUI_ICON_BUG);
        stats_box(safe, 2, box);
        assert(box[3] == plain + 3 * 32 + 8 && box[2] == (34 + 5) * 16 && past_margin() == 0);
        /* a name the box's letters don't hold: the box wider, the line whole */
        stats_debugger("lldb-server-123");
        draw(at);
        stats_box(safe, 2, box);
        assert(!strcmp(note(2), "debugger attached: lldb-server-123") && box[2] == (19 + 15 + 5) * 16);
        assert(past_margin() == 0);
        /* a file's name past the safe area: the box as wide as the area, the name's start cut, its end shown */
        stats_debugger("");
        c.failed = 0;
        c.taken = 4;
        snprintf(c.path, sizeof c.path, "/caps/%s_capture_12.rdc",
                 "a_stem_much_longer_than_any_box_of_the_stats_could_hold_across_the_whole_picture_of_the_game");
        stats_capture(&c);
        draw(at);
        stats_box(safe, 2, box);
        assert(box[2] == 78 * 16 && box[0] + box[2] <= (float)(safe[0] + safe[2]) - 16 && past_margin() == 0);
        assert(!strncmp(note(1), "capture 4: ...", 14) && strlen(note(1)) == 73);
        assert(!strcmp(note(1) + 73 - 15, "_capture_12.rdc"));
        /* bottom right: the taller box still a gap above the safe area's bottom */
        stats_corner_set(corner, "br");
        stats_box(safe, 2, box);
        assert(box[1] + box[3] == 50 + 720 - 16 && box[0] + box[2] == 100 + 1280 - 16);
        stats_corner_set(corner, "tl");
        /* off and on: RenderDoc's lines kept, the debugger forgotten, and read again at the next frame's measure
           (nothing traces the test) */
        stats_frame(at += 16 * MS, &g_test.gpu); /* a frame and a measure for off to forget */
        stats_debugger("gdb");
        g_settings.stats = 0;
        stats_frame(at += 16 * MS, &g_test.gpu);
        g_settings.stats = 1;
        draw(at);
        assert(!strcmp(note(0), "RenderDoc: Ctrl+Shift+F12 captures") && !strncmp(note(1), "capture 4: ...", 14));
        assert(!note(2)[0]);
        stats_debugger("gdb");
        stats_frame(at += 16 * MS, &g_test.gpu);
        draw(at);
        assert(!strcmp(note(0), "RenderDoc: Ctrl+Shift+F12 captures") && !note(2)[0]);
        /* the capture key as set, and with none the menu's item */
        snprintf(g_settings.render_doc_capture_shortcut, sizeof g_settings.render_doc_capture_shortcut, "Pause");
        draw(at);
        assert(!strcmp(note(0), "RenderDoc: Pause captures"));
        g_settings.render_doc_capture_shortcut[0] = 0;
        draw(at);
        assert(!strcmp(note(0), "RenderDoc: Capture frame in the menu"));
        /* none: no lines, the box as before */
        stats_capture(&(SnxStatsCapture){0});
        draw(at);
        stats_box(safe, 2, box);
        assert(box[3] == plain && box[2] == STATS_COLS * 16 && !note(0)[0]);
    }
#ifdef TRACED_CHILD
    /* a game a debugger traces: the line names it from the frames, as snx_debugger reads it (the test, here) */
    {
        int pipe_fd[2], status = 0;
        char said[128] = {0};
        pid_t child;
        assert(!pipe(pipe_fd));
        child = fork();
        assert(child >= 0);
        if (!child) {
            char name[SNX_DEBUGGER_NAME], want[64];
            close(pipe_fd[0]);
            if (TRACE_ME())
                snprintf(said, sizeof said, "refused %d", errno);
            else if (!snx_debugger(name, sizeof name))
                snprintf(said, sizeof said, "no debugger read");
            else {
                snprintf(want, sizeof want, "debugger attached: %s", name);
                g_settings.stats = 0;
                stats_frame(at += 16 * MS, &g_test.gpu); /* from nothing */
                g_settings.stats = 1;
                stats_frame(at += 16 * MS, &g_test.gpu);
                draw(at);
                snprintf(said, sizeof said, "%s", strcmp(note(0), want) ? note(0) : "ok");
            }
            if (write(pipe_fd[1], said, strlen(said) + 1) < 0) _exit(2);
            _exit(0);
        }
        close(pipe_fd[1]);
        if (read(pipe_fd[0], said, sizeof said - 1) < 0) said[0] = 0;
        close(pipe_fd[0]);
        while (waitpid(child, &status, 0) == child && WIFSTOPPED(status)) TRACE_GO(child);
        if (!strncmp(said, "refused", 7))
            printf("the system refused a traced child (%s, errno): the debugger's line is not read here\n", said);
        else if (strcmp(said, "ok") || !WIFEXITED(status) || WEXITSTATUS(status)) {
            fprintf(stderr, "a game traced by the test: the line under the graph \"%s\"\n", said);
            return 1;
        }
    }
#endif
    puts("stats passed");
    return 0;
}
