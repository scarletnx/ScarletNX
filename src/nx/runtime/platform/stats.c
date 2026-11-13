/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/platform/stats.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/vpad.h"
#include "common/utilities/common.h"
#include "gui/icons.h"
#include "nx/runtime/platform/ui_colors.h"
#include "snx_settings.h"
#include <stdio.h>
#include <string.h>

/* no box behind the lines: each letter and icon backed (ui.h's UI_BACKED), its clear pixels in the ground's colour,
   a line a band of it from its icon to its field's end, and the graph on the same ground. All of them opaque, which
   the renderer draws without blending (gpu/vk/ui.c). The layer's colours (ui_colors.h): the ground its ink, the
   letters and the bars its page's, the guides soft, a slow frame's bar and a failure's line in the failure colour */
#define STATS_SPAN_NS     500000000ull  /* the frame rate is over the last half second */
#define STATS_SAMPLE_NS   1000000000ull /* the processor and the memory are measured once a second */
#define STATS_GRAPH_LINES 3             /* the graph's height, in lines */
#define STATS_NOTE_MAX    256           /* a line's letters under the graph at most, with its NUL */

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
    u32 measures, notes;              /* counts of stats_measured's, and of the changes to the lines under the graph */
} g_stats = {.fps10 = -1, .shown = {.cpu = -1, .gpu = -1}};

/* The box as stats_draw lays it out, kept from frame to frame: laid out again only when its place or the lines under
   the graph change. Its lines that change are fields of cells (ui_cells), whose letters a frame writes where a value
   changed. The graph's STATS_FRAMES - 1 bars are the ring (ui.h's ui_ring): a frame writes its time as a byte, its
   bar's height in 255ths of the graph's (a frame past STATS_TOP_MS as tall as it), into the next slot, and moves the
   ring's head, and the GPU scrolls the others left. The bars' quads never change: a frame changes a few letters and
   a byte, which is all the renderer sends to the device (gpu/vk/ui.c) */
#define STATS_BARS      (STATS_FRAMES - 1)
_Static_assert(STATS_BARS <= SNX_UI_RING_MAX, "the bars fit the ring, a slot each, which is a quad's glyph byte");
/* the height past which a bar draws in the failure colour: a frame past 30 FPS's, STATS_30_MS of STATS_TOP_MS */
#define STATS_SLOW_FROM ((int)(255.0f * STATS_30_MS / STATS_TOP_MS + 0.5f))
#define STATS_QUADS     1536 /* its quads at most: the lines, the graph and three lines under it of 255 letters */
#define STATS_CELLS     (STATS_COLS - 6) /* a line's cells: after a blank and the icon's three, to the graph's right */
#define STATS_CPU_CELLS (STATS_GPU_AT - 5)
#define STATS_GPU_CELLS (STATS_COLS - 1 - (STATS_GPU_AT + 3))
/* a window's queue of the frames' times (stats_draw's quickest and slowest): its first the window's greatest */
typedef struct {
    u64 at[STATS_BARS + 1];
    int v[STATS_BARS + 1];
    unsigned head, n;
} StatsWindow;
/* what the box is laid out by, a few words compared each frame: the safe area, the scale, whether showcase shows and
   where (vpad.c counts its corner's changes), and the changes to the box's corner and to the lines under the graph,
   counted */
typedef struct {
    int safe[4], scale, showcase;
    u32 place, layout;
} StatsKey;
static u32 g_layout; /* the corner's changes (stats_corner_set, on the main thread) */
static struct {
    int laid;    /* the quads are laid out for key */
    float at[4]; /* the box they are laid out in: x, y, w, h */
    StatsKey key;
    char shortcut[sizeof g_settings.render_doc_capture_shortcut]; /* the capture key RenderDoc's line names */
    SnxUiQuad q[STATS_QUADS];
    int n;
    int cells[STATS_LINES], gpu; /* the first cell of each line, and of the GPU's text */
    int bars;                        /* the first of the bars, slot 0's */
    u8 height[STATS_BARS];           /* the ring's bytes (ui.h) */
    u32 ring_version;                /* counts their changes */
    unsigned head;                   /* the newest bar's slot */
    int full;                        /* every bar to be written: laid out since */
    float gx, gw, gy, gh;            /* the graph */
    int fps10, ms10;                 /* what the cells show: the frame rate and the last frame's time in tenths */
    int low10, peak10;               /* and the graph's quickest and slowest frames' times, in tenths */
    u32 measures;                    /* g_stats.measures */
    u32 version;                     /* counts the changes to the quads (ui_append_kept) */
    SnxUiSpan changed;               /* and the quads changed since they went into the renderer's list */
    /* the frames' times in the graph's window, the quickest's and the slowest's (negated, so both keep the greatest
       first): each a queue whose first is the window's, a later frame's dropping the ones it beats */
    StatsWindow low, peak;
    u64 frames;                      /* and the bars, g_stats.frames */
} g_box;

void stats_corner_set(const IniOpt* o, const char* v) {
    int i = vpad_corner(v);
    if (i >= 0) {
        __atomic_store_n(&g_stats.corner, i, __ATOMIC_RELAXED); /* the renderer's thread reads it */
        __atomic_add_fetch(&g_layout, 1, __ATOMIC_RELAXED);
        return;
    }
    i = __atomic_load_n(&g_stats.corner, __ATOMIC_RELAXED);
    LOG("settings: stats_corner=%s is not tl, tr, bl or br: it keeps %s\n", v, vpad_corner_word(i));
    snprintf((char*)o->var, o->size, "%s", vpad_corner_word(i));
}
void stats_measured(const SnxStatsMeasure* m) {
    ui_lock();
    g_stats.shown = *m;
    ++g_stats.measures;
    ui_unlock();
}
void stats_capture(const SnxStatsCapture* c) {
    ui_lock();
    g_stats.capture = *c;
    ++g_stats.notes;
    ui_unlock();
}
void stats_debugger(const char* name) {
    ui_lock();
    if (strncmp(g_stats.debugger, name, sizeof g_stats.debugger - 1)) { /* each measure gives it, mostly the same */
        snprintf(g_stats.debugger, sizeof g_stats.debugger, "%s", name);
        ++g_stats.notes;
    }
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
            g_box.laid = 0;
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
            .vram_budget = gpu->vram_budget,
            .vram_ours = gpu->vram_ours};
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
        note(s, GUI_ICON_IMAGE, UI_PAGE, room, lead, "");
    }
    if (g_stats.capture.failed) {
        snprintf(lead, sizeof lead, "capture %u failed", g_stats.capture.taken + 1);
        note(s, GUI_ICON_SAVE, UI_FAILURE, room, lead, "");
    } else if (g_stats.capture.taken) {
        const char* name = snx_path_leaf(g_stats.capture.path);
        snprintf(lead, sizeof lead, "capture %u: ", g_stats.capture.taken);
        note(s, GUI_ICON_SAVE, UI_PAGE, room, lead, *name ? name : "?");
    }
    if (g_stats.debugger[0]) note(s, GUI_ICON_BUG, UI_PAGE, room, "debugger attached: ", g_stats.debugger);
}
/* stats_box's, with the lines under the graph it is made for */
static void box_notes(const int safe[4], int scale, float box[4], StatsNotes* notes) {
    float cw = 8.0f * (float)scale, ch = 16.0f * (float)scale, gap = ch / 2, showcase[4];
    int corner = __atomic_load_n(&g_stats.corner, __ATOMIC_RELAXED);
    /* a line's room: the safe area's letters within its gaps, past the icon's three and a letter at the right; a
       picture narrower than the box still gets the box's */
    float fit = ((float)safe[2] - 2 * gap) / cw;
    size_t across = fit > 0 ? (size_t)fit : 0;
    notes_make(notes, (across > STATS_COLS ? across : STATS_COLS) - 6);
    box[2] = (float)(notes->longest + 6 > STATS_COLS ? notes->longest + 6 : STATS_COLS) * cw;
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
int stats_box_drawn(float box[4]) {
    if (!__atomic_load_n(&g_settings.stats, __ATOMIC_RELAXED) || !g_box.laid) return 0;
    memcpy(box, g_box.at, sizeof g_box.at);
    return 1;
}

/* the time between the frame j back from the newest and the one before it, in ms; j below count - 1 */
static float frame_ms(unsigned j) {
    unsigned newer = (g_stats.next + 2 * STATS_FRAMES - 1 - j) % STATS_FRAMES;
    unsigned older = (newer + STATS_FRAMES - 1) % STATS_FRAMES;
    return (float)(g_stats.at[newer] - g_stats.at[older]) / 1e6f;
}
/* a share in percent at at, ? when unknown (below 0) */
static char* put_percent(char* at, int percent) {
    if (percent < 0) return ui_put(at, "?");
    return ui_put(ui_put_u(at, (u64)percent), "%");
}
/* bytes in MiB at at, ? when unknown (0) */
static char* put_mib(char* at, u64 bytes) { return bytes ? ui_put_u(at, bytes >> 20) : ui_put(at, "?"); }
/* tenths at at, at most 999.9: a field's letters are as many as it holds */
static char* put_tenths(char* at, u64 tenths) { return ui_put_tenths(at, tenths < 9999 ? tenths : 9999); }

/* an icon, backed, and a backed space after it: the band of ground its line's letters go on from */
static void icon_backed(SnxUiList* l, float x, float y, int scale, unsigned icon, u32 rgba) {
    ui_icon_backed(l, x, y, scale, icon, rgba);
    ui_text_backed(l, x + 16.0f * (float)scale, y, scale, " ", rgba);
}

/* the letters of text into the n cells from first of the kept quads, the version counted up when one changed */
static void cells(int first, int n, const char* text) {
    if (!ui_cells_set(&g_box.q[first], n, text)) return;
    ++g_box.version;
    ui_span_add(&g_box.changed, first, first + n);
}
/* frame at's time v into a window's queue, and the frames before first out of it (stats_draw's quickest and slowest) */
#define WINDOW (STATS_BARS + 1)
static void window_push(StatsWindow* w, u64 at, int v) {
    while (w->n && w->v[(w->head + w->n - 1) % WINDOW] <= v) --w->n;
    w->at[(w->head + w->n) % WINDOW] = at;
    w->v[(w->head + w->n) % WINDOW] = v;
    ++w->n;
}
static void window_drop(StatsWindow* w, u64 first) {
    while (w->n && w->at[w->head] < first) {
        w->head = (w->head + 1) % WINDOW;
        --w->n;
    }
}
/* the box laid out for key, at the place stats_box gives it: the lines that change, the GPU's text and the bars left
   blank for stats_draw to write */
static void box_layout(const int safe[4], int scale, const StatsKey* key) {
    /* the frames' gauge, the processor (the GPU's chip beside it), the memory's stick, the video memory's layers, the
       graph's pulse */
    static const unsigned icons[STATS_LINES] = {GUI_ICON_GAUGE, GUI_ICON_CPU, GUI_ICON_MEMORY_STICK, GUI_ICON_LAYERS,
                                                GUI_ICON_ACTIVITY};
    static const float guides[2] = {STATS_60_MS, STATS_30_MS};
    SnxUiList l = {g_box.q, 0, STATS_QUADS};
    float cw = 8.0f * (float)scale, ch = 16.0f * (float)scale, box[4], top;
    StatsNotes notes;
    unsigned j;
    box_notes(safe, scale, box, &notes);
    /* what shows of it: its first column, a margin, draws nothing */
    g_box.at[0] = box[0] + cw;
    g_box.at[1] = box[1];
    g_box.at[2] = box[2] - cw;
    g_box.at[3] = box[3];
    /* the lines as the Debug settings' box (hud.c): a strip of the ink a quarter line high above them, and a blank
       cell before each icon; the graph under them keeps its own edges */
    ui_fill(&l, box[0] + cw, box[1], STATS_GRAPH_W * cw, ch / 4, UI_INK);
    for (j = 0; j < STATS_LINES; ++j) { /* a blank, the icon's two cells and one, then the line, as the menu's items */
        float y = box[1] + ch / 4 + (float)j * ch;
        ui_text_backed(&l, box[0] + cw, y, scale, " ", UI_PAGE);
        icon_backed(&l, box[0] + 2 * cw, y, scale, icons[j], UI_PAGE);
        g_box.cells[j] =
            ui_cells_backed(&l, box[0] + 5 * cw, y, scale, j == 1 ? STATS_CPU_CELLS : STATS_CELLS, UI_PAGE);
    }
    icon_backed(&l, box[0] + STATS_GPU_AT * cw, box[1] + ch / 4 + ch, scale, GUI_ICON_GPU, UI_PAGE);
    g_box.gpu =
        ui_cells_backed(&l, box[0] + (STATS_GPU_AT + 3) * cw, box[1] + ch / 4 + ch, scale, STATS_GPU_CELLS, UI_PAGE);
    /* the graph right under the lines, as wide as their bands (STATS_GRAPH_W): each frame's time a bar up from its
       foot, the newest at the right, over the two guides; the last line says their quickest and slowest */
    g_box.gx = box[0] + cw;
    g_box.gw = STATS_GRAPH_W * cw;
    g_box.gy = box[1] + ch / 4 + STATS_LINES * ch;
    g_box.gh = STATS_GRAPH_LINES * ch;
    ui_fill(&l, g_box.gx, g_box.gy, g_box.gw, g_box.gh, UI_INK);
    for (j = 0; j < 2; ++j) {
        top = g_box.gy + g_box.gh - g_box.gh * guides[j] / STATS_TOP_MS;
        ui_fill(&l, g_box.gx, top, g_box.gw, (float)scale, UI_SOFT);
    }
    g_box.bars = l.n;
    ui_ring_bars(&l, g_box.gx, g_box.gy, g_box.gw, g_box.gh, STATS_BARS, UI_PAGE); /* none yet */
    memset(g_box.height, 0, sizeof g_box.height);
    ++g_box.ring_version;
    /* the lines under it, half a line down, each after a blank and its icon as the lines above, its band filled to
       the box's right; the half line above them and a quarter line under them the ink's */
    if (notes.n) {
        float right = box[0] + box[2] - cw;
        ui_fill(&l, box[0] + cw, g_box.gy + g_box.gh, right - box[0] - cw, ch / 2, UI_INK);
        ui_fill(&l, box[0] + cw, g_box.gy + g_box.gh + ch / 2 + (float)notes.n * ch, right - box[0] - cw, ch / 4,
                UI_INK);
        for (j = 0; j < (unsigned)notes.n; ++j) {
            float y = g_box.gy + g_box.gh + ch / 2 + (float)j * ch, at;
            ui_text_backed(&l, box[0] + cw, y, scale, " ", UI_PAGE);
            icon_backed(&l, box[0] + 2 * cw, y, scale, notes.icon[j], notes.rgba[j]);
            at = box[0] + 5 * cw + ui_text_backed(&l, box[0] + 5 * cw, y, scale, notes.text[j], notes.rgba[j]);
            if (at < right) ui_fill(&l, at, y, right - at, ch, UI_INK);
        }
    }
    g_box.n = l.n;
    g_box.key = *key;
    snprintf(g_box.shortcut, sizeof g_box.shortcut, "%s", g_settings.render_doc_capture_shortcut);
    g_box.laid = 1;
    /* nothing written yet: each value differs from these */
    g_box.fps10 = g_box.ms10 = g_box.low10 = g_box.peak10 = -2;
    g_box.measures = g_stats.measures - 1;
    g_box.full = 1;
    ++g_box.version;
    ui_span_add(&g_box.changed, 0, g_box.n);
    g_box.head = 0;
}
/* the bar of a frame of t ms into its slot of the ring: its height in 255ths of the graph's, rounded */
static void bar_set(unsigned slot, float t) {
    u8 h = (u8)(255.0f * (t < STATS_TOP_MS ? t : STATS_TOP_MS) / STATS_TOP_MS + 0.5f);
    if (g_box.height[slot] != h) ++g_box.ring_version;
    g_box.height[slot] = h;
}
void stats_draw(SnxUiList* l, const int safe[4], int menu_scale, u64 now_ns) {
    int scale = stats_scale(menu_scale);
    char text[64], *at;
    StatsKey key;
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
    /* the place and the lines under the graph: the layout's key, whose bytes are compared (its padding zeroed) */
    memset(&key, 0, sizeof key);
    memcpy(key.safe, safe, sizeof key.safe);
    key.scale = scale;
    key.showcase = __atomic_load_n(&g_settings.showcase, __ATOMIC_RELAXED);
    key.place = vpad_showcase_place_version();
    key.layout = __atomic_load_n(&g_layout, __ATOMIC_RELAXED) + g_stats.notes;
    /* the capture key only while RenderDoc's line names it */
    if (!g_box.laid || memcmp(&key, &g_box.key, sizeof key) ||
        (g_stats.capture.ready && strcmp(g_box.shortcut, g_settings.render_doc_capture_shortcut)))
        box_layout(safe, scale, &key);
    if (g_box.full || g_box.frames != g_stats.frames) { /* frames kept: the last one's time, and the new bars */
        int ms10 = g_stats.count >= 2 ? (int)(frame_ms(0) * 10 + 0.5f) : -1;
        /* the times kept, frame j back from the newest in slot (frames - 2 - j) mod STATS_BARS: the frames since the
           last draw, or all after a layout */
        unsigned kept = g_stats.count ? g_stats.count - 1 : 0, fresh = kept;
        if (!g_box.full && g_stats.frames - g_box.frames < kept) fresh = (unsigned)(g_stats.frames - g_box.frames);
        if (g_box.full) g_box.low.n = g_box.peak.n = 0;
        for (j = fresh; j-- > 0;) { /* oldest first, each into the ring and the quickest and slowest's windows */
            u64 at_frame = g_stats.frames - 2 - j;
            float t = frame_ms(j);
            int t10 = (int)(t * 10 + 0.5f);
            bar_set((unsigned)(at_frame % STATS_BARS), t);
            window_push(&g_box.low, at_frame, -t10);
            window_push(&g_box.peak, at_frame, t10);
        }
        if (kept) {
            g_box.head = (unsigned)((g_stats.frames - 2) % STATS_BARS);
            window_drop(&g_box.low, g_stats.frames - 1 - kept);
            window_drop(&g_box.peak, g_stats.frames - 1 - kept);
        }
        g_box.frames = g_stats.frames;
        g_box.full = 0;
        if (ms10 != g_box.ms10) {
            g_box.ms10 = ms10;
            g_box.fps10 = -2; /* the first line written again below: no frame rate is -2 */
        }
        { /* the graph's quickest and slowest frames, in tenths of a millisecond, on its own line: each window's
             first */
            int low10 = kept ? -g_box.low.v[g_box.low.head] : -1, peak10 = kept ? g_box.peak.v[g_box.peak.head] : -1;
            if (low10 != g_box.low10 || peak10 != g_box.peak10) {
                at = ui_put(text, "frame time ");
                if (kept) {
                    at = ui_put(put_tenths(ui_put(put_tenths(at, (u64)low10), " to "), (u64)peak10), " ms");
                } else
                    at = ui_put(at, "?");
                *at = 0;
                cells(g_box.cells[4], STATS_CELLS, text);
                g_box.low10 = low10;
                g_box.peak10 = peak10;
            }
        }
    }
    if (g_box.fps10 != g_stats.fps10) {
        at = g_stats.fps10 >= 0 ? put_tenths(text, (u64)g_stats.fps10) : ui_put(text, "?");
        at = ui_put(at, " fps  ");
        at = g_box.ms10 >= 0 ? put_tenths(at, (u64)g_box.ms10) : ui_put(at, "?");
        *ui_put(at, " ms") = 0;
        cells(g_box.cells[0], STATS_CELLS, text);
        g_box.fps10 = g_stats.fps10;
    }
    if (g_box.measures != g_stats.measures) { /* once a second */
        const SnxStatsMeasure* m = &g_stats.shown;
        *put_percent(ui_put(text, "cpu "), m->cpu) = 0;
        cells(g_box.cells[1], STATS_CPU_CELLS, text);
        if (m->gpu >= 0) { /* the busy time a frame in tenths of a millisecond, as the frame's own */
            at = ui_put(put_percent(ui_put(text, "gpu "), m->gpu), "  ");
            *ui_put(put_tenths(at, (m->gpu_ns + 50000) / 100000), " ms") = 0;
        } else
            *ui_put(text, "gpu ?  ? ms") = 0;
        cells(g_box.gpu, STATS_GPU_CELLS, text);
        *ui_put(put_mib(ui_put(put_mib(ui_put(text, "ram "), m->memory), "/"), m->memory_total), " MiB") = 0;
        cells(g_box.cells[2], STATS_CELLS, text);
        /* our use and the heap's size, said to be them, where the driver gives neither its use nor a budget */
        *ui_put(put_mib(ui_put(put_mib(ui_put(text, "vram "), m->vram), "/"), m->vram_budget),
                m->vram_ours ? " MiB ours/heap" : " MiB") = 0;
        cells(g_box.cells[3], STATS_CELLS, text);
        g_box.measures = g_stats.measures;
    }
    ui_append_kept(l, &(SnxUiList){g_box.q, g_box.n, STATS_QUADS}, g_box.version, &g_box, &g_box.changed);
    ui_ring_set(g_box.height, STATS_BARS, (int)g_box.head, STATS_SLOW_FROM, UI_FAILURE, g_box.ring_version);
}
