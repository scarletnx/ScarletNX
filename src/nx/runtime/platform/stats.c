/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/platform/stats.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/vpad.h"
#include "common/utilities/common.h"
#include "gui/icons.h"
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
#define STATS_NOTE_MAX     256           /* a line's letters under the graph at most, with its NUL */

static struct {
    u64 at[STATS_FRAMES]; /* the presents of the game's last frames, a ring that next writes on */
    unsigned next, count;
    u64 frames;             /* the frames kept since stats went on */
    u64 since, since_ns;    /* the frames and the time the frame rate is counted from */
    int fps10;              /* the frames a second over the last half second, in tenths; -1 for none yet */
    u64 sampled_ns, cpu_ns; /* when the processor's time was read last, and what it was: the renderer's thread's */
    u64 sampled_frames;     /* the frames kept then */
    u64 busy_ns;            /* and the GPU's busy time read with that frame, if it was timed */
    int busy_timed;
    SnxStatsMeasure shown;            /* stats_measured's */
    int corner;                       /* stats_corner's, as vpad_corner reads it */
    SnxStatsCapture capture;          /* stats_capture's, which stats off keeps */
    char debugger[SNX_DEBUGGER_NAME]; /* stats_debugger's */
} g_stats = {.fps10 = -1, .shown = {.cpu = -1, .gpu = -1}};

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
void stats_measured(const SnxStatsMeasure* m) {
    ui_lock();
    g_stats.shown = *m;
    ui_unlock();
}
void stats_capture(const SnxStatsCapture* c) {
    ui_lock();
    g_stats.capture = *c;
    ui_unlock();
}
void stats_debugger(const char* name) {
    ui_lock();
    snprintf(g_stats.debugger, sizeof g_stats.debugger, "%s", name);
    ui_unlock();
}
void stats_frame(u64 now_ns, const SnxStatsGpu* gpu) {
    u64 cpu_ns, total;
    unsigned cores;
    char debugger[SNX_DEBUGGER_NAME];
    if (!__atomic_load_n(&g_settings.stats, __ATOMIC_RELAXED)) {
        if (g_stats.frames || g_stats.sampled_ns) { /* the next turn on starts from nothing but RenderDoc's lines */
            int corner = g_stats.corner;
            ui_lock();
            SnxStatsCapture capture = g_stats.capture;
            memset(&g_stats, 0, sizeof g_stats);
            g_stats.fps10 = g_stats.shown.cpu = g_stats.shown.gpu = -1;
            g_stats.corner = corner;
            g_stats.capture = capture;
            ui_unlock();
        }
        return;
    }
    ui_lock();
    g_stats.at[g_stats.next] = now_ns;
    g_stats.next = (g_stats.next + 1) % STATS_FRAMES;
    if (g_stats.count < STATS_FRAMES) ++g_stats.count;
    ++g_stats.frames;
    ui_unlock();
    if (g_stats.sampled_ns && now_ns - g_stats.sampled_ns < STATS_SAMPLE_NS) return;
    stats_debugger(snx_debugger(debugger, sizeof debugger) ? debugger : "");
    cpu_ns = snx_process_cpu_ns();
    cores = snx_cpu_count();
    total = 0;
    if (g_stats.sampled_ns) {
        u64 wall = now_ns - g_stats.sampled_ns;
        SnxStatsMeasure m = {
            .cpu = cpu_ns && g_stats.cpu_ns && cores ? (int)((cpu_ns - g_stats.cpu_ns) * 100 / (wall * cores)) : -1,
            .memory = snx_footprint(),
            .memory_total = snx_memory_total(&total) ? total : 0,
            .gpu = -1,
            .vram = gpu->vram,
            .vram_budget = gpu->vram_budget};
        /* the GPU's busy time read since the last measure, over its time and its frames: the renderer reads a frame's
           once its slot is reused, two frames on, a shift a second's sum hardly shows */
        if (gpu->timed && g_stats.busy_timed) {
            u64 busy = gpu->busy_ns - g_stats.busy_ns;
            m.gpu = (int)(busy * 100 / wall);
            m.gpu_ns = busy / (g_stats.frames - g_stats.sampled_frames); /* this frame is one of them */
        }
        stats_measured(&m);
    }
    g_stats.sampled_ns = now_ns;
    g_stats.cpu_ns = cpu_ns;
    g_stats.sampled_frames = g_stats.frames;
    g_stats.busy_ns = gpu->busy_ns;
    g_stats.busy_timed = gpu->timed;
}

int stats_scale(int menu_scale) { return menu_scale > 1 ? menu_scale - 1 : 1; }

/* the lines under the graph that apply, each after its icon in its colour, the longest's letters */
typedef struct {
    int n;
    size_t longest;
    char text[STATS_NOTES][STATS_NOTE_MAX];
    unsigned icon[STATS_NOTES];
    u32 rgba[STATS_NOTES];
} StatsNotes;
/* a line of lead and value, in room letters at most: a value past them keeps its end after "...", where a capture's
   file name tells it from the others (_capture_2.rdc) */
static void note(StatsNotes* s, unsigned icon, u32 rgba, size_t room, const char* lead, const char* value) {
    size_t lead_n = strlen(lead), n = strlen(value), len;
    char* t = s->text[s->n];
    if (room > STATS_NOTE_MAX - 1) room = STATS_NOTE_MAX - 1;
    if (lead_n + n <= room)
        snprintf(t, STATS_NOTE_MAX, "%s%s", lead, value);
    else if (lead_n + 3 < room)
        snprintf(t, STATS_NOTE_MAX, "%s...%s", lead, value + n - (room - lead_n - 3));
    else
        snprintf(t, room + 1, "%s", lead);
    s->icon[s->n] = icon;
    s->rgba[s->n] = rgba;
    len = strlen(t);
    if (len > s->longest) s->longest = len;
    s->n++;
}
/* a path's file name, after its last separator */
/* the lines that apply, in room letters each: RenderDoc's, that a capture can be taken (by the capture key's setting,
   hotkeys.h, or with none the menu's item) and what the last one asked for wrote (its file's name, ? where RenderDoc
   didn't say it) or that it failed, in the failure colour; and the debugger tracing the game */
static void notes_make(StatsNotes* s, size_t room) {
    char lead[96];
    s->n = 0;
    s->longest = 0;
    if (g_stats.capture.ready) {
        if (*g_settings.render_doc_capture_shortcut)
            snprintf(lead, sizeof lead, "RenderDoc: %s captures", g_settings.render_doc_capture_shortcut);
        else
            snprintf(lead, sizeof lead, "RenderDoc: Capture frame in the menu");
        note(s, GUI_ICON_IMAGE, STATS_TEXT, room, lead, "");
    }
    if (g_stats.capture.failed) {
        snprintf(lead, sizeof lead, "capture %u failed", g_stats.capture.taken + 1);
        note(s, GUI_ICON_SAVE, STATS_SLOW, room, lead, "");
    } else if (g_stats.capture.taken) {
        const char* name = snx_path_leaf(g_stats.capture.path);
        snprintf(lead, sizeof lead, "capture %u: ", g_stats.capture.taken);
        note(s, GUI_ICON_SAVE, STATS_TEXT, room, lead, *name ? name : "?");
    }
    if (g_stats.debugger[0]) note(s, GUI_ICON_BUG, STATS_TEXT, room, "debugger attached: ", g_stats.debugger);
}
/* stats_box's, with the lines under the graph it is made for */
static void box_notes(const int safe[4], int scale, float box[4], StatsNotes* notes) {
    float cw = 8.0f * (float)scale, ch = 16.0f * (float)scale, gap = ch / 2, showcase[4];
    int corner = __atomic_load_n(&g_stats.corner, __ATOMIC_RELAXED);
    /* a line's room: the safe area's letters within its gaps, past the icon's three and a letter at the right; a
       picture narrower than the box still gets the box's */
    float fit = ((float)safe[2] - 2 * gap) / cw;
    size_t across = fit > 0 ? (size_t)fit : 0;
    notes_make(notes, (across > STATS_COLS ? across : STATS_COLS) - 5);
    box[2] = (float)(notes->longest + 5 > STATS_COLS ? notes->longest + 5 : STATS_COLS) * cw;
    /* the lines and the graph, a quarter line around each, and the lines under it with a quarter line after them */
    box[3] = (STATS_LINES + STATS_GRAPH_LINES) * ch + 3 * ch / 4 + (notes->n ? (float)notes->n * ch + ch / 4 : 0);
    if (vpad_showcase_box(safe, showcase) == corner) { /* beside showcase's box, toward the picture's middle */
        box[0] = corner & 1 ? showcase[0] + showcase[2] - box[2] : showcase[0];
        box[1] = corner & 2 ? showcase[1] - gap - box[3] : showcase[1] + showcase[3] + gap;
        return;
    }
    box[0] = corner & 1 ? (float)(safe[0] + safe[2]) - gap - box[2] : (float)safe[0] + gap;
    box[1] = corner & 2 ? (float)(safe[1] + safe[3]) - gap - box[3] : (float)safe[1] + gap;
}
void stats_box(const int safe[4], int scale, float box[4]) {
    StatsNotes notes;
    box_notes(safe, scale, box, &notes);
}

/* the time between the frame j back from the newest and the one before it, in ms; j below count - 1 */
static float frame_ms(unsigned j) {
    unsigned newer = (g_stats.next + 2 * STATS_FRAMES - 1 - j) % STATS_FRAMES;
    unsigned older = (newer + STATS_FRAMES - 1) % STATS_FRAMES;
    return (float)(g_stats.at[newer] - g_stats.at[older]) / 1e6f;
}
/* a share in percent into s, ? when unknown (below 0) */
static void stats_percent(char* s, size_t n, int percent) {
    if (percent >= 0)
        snprintf(s, n, "%d%%", percent);
    else
        snprintf(s, n, "?");
}
/* bytes in MiB into s, ? when unknown (0) */
static void stats_mib(char* s, size_t n, u64 bytes) {
    if (bytes)
        snprintf(s, n, "%llu", (unsigned long long)(bytes >> 20));
    else
        snprintf(s, n, "?");
}
void stats_draw(SnxUiList* l, const int safe[4], int menu_scale, u64 now_ns) {
    int scale = stats_scale(menu_scale);
    float cw = 8.0f * (float)scale, ch = 16.0f * (float)scale, box[4], gx, gy, gw, gh, bw, top;
    StatsNotes notes;
    char fps[16], ms[16], share[16], used[24], total[24], gpu[STATS_COLS + 1], line[STATS_LINES][STATS_COLS + 1];
    /* the frames' gauge, the processor (the GPU's chip beside it), the memory's stick, the video memory's layers, the
       graph's pulse */
    static const unsigned icons[STATS_LINES] = {GUI_ICON_GAUGE, GUI_ICON_CPU, GUI_ICON_MEMORY_STICK, GUI_ICON_LAYERS,
                                                GUI_ICON_ACTIVITY};
    static const struct {
        float ms;
        const char* label;
    } guides[2] = {{STATS_60_MS, "16.7 ms"}, {STATS_30_MS, "33.3 ms"}};
    unsigned j;
    if (!__atomic_load_n(&g_settings.stats, __ATOMIC_RELAXED)) return;
    if (!g_stats.since_ns) { /* the first half second starts at the first draw */
        g_stats.since_ns = now_ns;
        g_stats.since = g_stats.frames;
    } else if (now_ns - g_stats.since_ns >= STATS_SPAN_NS) {
        g_stats.fps10 = (int)(((g_stats.frames - g_stats.since) * 10000000000ull + (now_ns - g_stats.since_ns) / 2) /
                              (now_ns - g_stats.since_ns));
        g_stats.since_ns = now_ns;
        g_stats.since = g_stats.frames;
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
    snprintf(line[0], sizeof line[0], "%s  %s", fps, ms);
    stats_percent(share, sizeof share, g_stats.shown.cpu);
    snprintf(line[1], sizeof line[1], "cpu %s", share);
    if (g_stats.shown.gpu >= 0) { /* the busy time a frame in tenths of a millisecond, as the frame's own */
        unsigned long long tenths = (g_stats.shown.gpu_ns + 50000) / 100000;
        snprintf(gpu, sizeof gpu, "gpu %d%%  %llu.%llu ms", g_stats.shown.gpu, tenths / 10, tenths % 10);
    } else
        snprintf(gpu, sizeof gpu, "gpu ?  ? ms");
    stats_mib(used, sizeof used, g_stats.shown.memory);
    stats_mib(total, sizeof total, g_stats.shown.memory_total);
    snprintf(line[2], sizeof line[2], "ram %s/%s MiB", used, total);
    stats_mib(used, sizeof used, g_stats.shown.vram);
    stats_mib(total, sizeof total, g_stats.shown.vram_budget);
    snprintf(line[3], sizeof line[3], "vram %s/%s MiB", used, total);
    snprintf(line[4], sizeof line[4], "frame time");
    box_notes(safe, scale, box, &notes);
    ui_fill(l, box[0], box[1], box[2], box[3], STATS_BACK);
    for (j = 0; j < STATS_LINES; ++j) { /* the icon's two cells and one, then the line, as the menu's items */
        ui_icon(l, box[0] + cw, box[1] + ch / 4 + (float)j * ch, scale, icons[j], STATS_TEXT);
        ui_text(l, box[0] + 4 * cw, box[1] + ch / 4 + (float)j * ch, scale, line[j], STATS_TEXT);
    }
    ui_icon(l, box[0] + STATS_GPU_AT * cw, box[1] + ch / 4 + ch, scale, GUI_ICON_GPU, STATS_TEXT);
    ui_text(l, box[0] + (STATS_GPU_AT + 3) * cw, box[1] + ch / 4 + ch, scale, gpu, STATS_TEXT);
    /* the graph: each frame's time a bar up from its foot, the newest at the right, over the two guides, each labelled
       with its time at the graph's right */
    gx = box[0] + cw;
    gw = box[2] - 2 * cw - STATS_AXIS * cw;
    gy = box[1] + STATS_LINES * ch + ch / 2;
    gh = STATS_GRAPH_LINES * ch;
    bw = gw / (STATS_FRAMES - 1);
    for (j = 0; j < 2; ++j) {
        top = gy + gh - gh * guides[j].ms / STATS_TOP_MS;
        ui_fill(l, gx, top, gw, (float)scale, STATS_GUIDE);
        ui_text(l, gx + gw + cw, top - ch / 2, scale, guides[j].label, STATS_GUIDE);
    }
    for (j = 0; j + 1 < g_stats.count; ++j) {
        float t = frame_ms(j), bh = gh * (t < STATS_TOP_MS ? t : STATS_TOP_MS) / STATS_TOP_MS;
        ui_fill(l, gx + gw - (float)(j + 1) * bw, gy + gh - bh, bw, bh, t > STATS_30_MS ? STATS_SLOW : STATS_TEXT);
    }
    /* the lines under it, a quarter line down, each after its icon as the lines above */
    for (j = 0; j < (unsigned)notes.n; ++j) {
        float y = gy + gh + ch / 4 + (float)j * ch;
        ui_icon(l, box[0] + cw, y, scale, notes.icon[j], notes.rgba[j]);
        ui_text(l, box[0] + 4 * cw, y, scale, notes.text[j], notes.rgba[j]);
    }
}
