/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/platform/prepare_screen.h"
#include "nx/runtime/platform/fatal_screen.h"
#include "nx/runtime/platform/overlay.h"
#include "gui/splash.h"
#include "nx/gpu/renderer.h"
#include "nx/runtime/platform/ui_colors.h"
#include <stdio.h>
#include <string.h>

/* a step as kept: its own copy of the label */
typedef struct {
    char label[32], detail[SNX_PREPARE_DETAIL_SIZE];
    u32 value, total;
    u64 started_ms;
    int done;
} Row;
static struct {
    SnxRunTheme theme;
    char name[96], foot[96];
    Row row[SNX_PREPARE_STEPS];
    char note[SNX_PREPARE_NOTES][SNX_PREPARE_NOTE_SIZE];
    int notes;
    int rows;   /* the steps: 0 for none */
    u64 now_ms; /* the last update's, for the dots and the time left */
    /* a program a collection's program started (prepare_screen_switch): up while switching, program the one that
       starts; the game's cover (cover), in the left square of the layer's picture (picture), given to the layer once
       drawn (made), its version then picture_version */
    int switching, program, cover, made;
    u32 picture_version;
    u8 picture[SNX_UI_IMAGE_W * SNX_UI_IMAGE_H * 4];
} g_prepare_screen;
typedef char prepare_cover_fits[SNX_PREPARE_COVER == SNX_UI_IMAGE_H && SNX_UI_IMAGE_W >= SNX_UI_IMAGE_H ? 1 : -1];

/* the YoRHa theme's [run] look, all of it from its theme.ini (gui_theme_colors.h): the launcher's own theme, for a
   runtime without the launcher to ask */
static const SnxRunTheme yorha = {.page = SNX_GUI_THEME_YORHA_RUN_PAGE,
                                  .ink = SNX_GUI_THEME_YORHA_RUN_INK,
                                  .soft = SNX_GUI_THEME_YORHA_RUN_SOFT,
                                  .dim = SNX_GUI_THEME_YORHA_RUN_DIM,
                                  .name = SNX_GUI_THEME_YORHA_NAME,
                                  .title = SNX_GUI_THEME_YORHA_LOADING,
                                  .steps = SNX_GUI_THEME_YORHA_RUN_STEPS,
                                  .texture = SNX_GUI_THEME_YORHA_RUN_TEXTURE,
                                  .progress = SNX_GUI_THEME_YORHA_RUN_PROGRESS};

void prepare_screen_set(const SnxRunTheme* theme, const char* name, const SnxPrepareStep* steps, int n,
                        const char (*notes)[SNX_PREPARE_NOTE_SIZE], int nnotes, const char* foot, u64 now_ms) {
    int i, from;
    g_prepare_screen.rows = n < 0 || !steps ? 0 : n > SNX_PREPARE_STEPS ? SNX_PREPARE_STEPS : n;
    if (!g_prepare_screen.rows) return;
    g_prepare_screen.theme = theme ? *theme : yorha;
    g_prepare_screen.now_ms = now_ms;
    snprintf(g_prepare_screen.name, sizeof g_prepare_screen.name, "%s", name ? name : "");
    snprintf(g_prepare_screen.foot, sizeof g_prepare_screen.foot, "%s", foot ? foot : "");
    /* the newest, when there are more than the screen keeps */
    from = !notes || nnotes < 0 ? 0 : nnotes > SNX_PREPARE_NOTES ? nnotes - SNX_PREPARE_NOTES : 0;
    g_prepare_screen.notes = !notes || nnotes < 0 ? 0 : nnotes - from;
    for (i = 0; i < g_prepare_screen.notes; ++i)
        snprintf(g_prepare_screen.note[i], sizeof g_prepare_screen.note[i], "%s", notes[from + i]);
    for (i = 0; i < g_prepare_screen.rows; ++i) {
        Row* r = &g_prepare_screen.row[i];
        snprintf(r->label, sizeof r->label, "%s", steps[i].label ? steps[i].label : "");
        snprintf(r->detail, sizeof r->detail, "%s", steps[i].detail);
        r->value = steps[i].value;
        r->total = steps[i].total;
        r->started_ms = steps[i].started_ms;
        r->done = steps[i].done;
    }
}
void prepare_screen_switch(const SnxRunTheme* theme, const char* name, int program, const char* foot, u64 now_ms) {
    u32 version;
    if (program < 0) {
        /* the picture back to none, unless the menu gave it one of its own since */
        if (g_prepare_screen.switching && g_prepare_screen.made && ui_image_get(&version) &&
            version == g_prepare_screen.picture_version)
            ui_image_set(NULL);
        g_prepare_screen.switching = 0;
        g_prepare_screen.made = 0;
        return;
    }
    g_prepare_screen.switching = 1;
    g_prepare_screen.program = program;
    g_prepare_screen.theme = theme ? *theme : yorha;
    g_prepare_screen.now_ms = now_ms;
    snprintf(g_prepare_screen.name, sizeof g_prepare_screen.name, "%s", name ? name : "");
    snprintf(g_prepare_screen.foot, sizeof g_prepare_screen.foot, "%s", foot ? foot : "");
}
int prepare_screen_cover(const void* jpeg, size_t size) {
    const int side = SNX_PREPARE_COVER, stride = SNX_UI_IMAGE_W * 4;
    g_prepare_screen.made = 0;
    g_prepare_screen.cover = 0;
    if (!jpeg) return 0;
    if (snx_splash_picture(jpeg, size, side, 0, g_prepare_screen.picture, stride)) return 1;
    for (int y = 0; y < side; ++y) { /* ARGB8888, opaque, to the bytes R G B A */
        u8* q = &g_prepare_screen.picture[(size_t)y * (size_t)stride];
        for (int x = 0; x < side; ++x, q += 4) {
            u32 p = (u32)q[0] | (u32)q[1] << 8 | (u32)q[2] << 16 | (u32)q[3] << 24;
            q[0] = (u8)(p >> 16);
            q[1] = (u8)(p >> 8);
            q[2] = (u8)p;
            q[3] = 0xFF;
        }
    }
    g_prepare_screen.cover = 1;
    return 0;
}
void prepare_screen_game_drew(void) {
    if (g_prepare_screen.switching) prepare_screen_switch(NULL, NULL, -1, NULL, 0);
}
int prepare_screen_shown(void) { return g_prepare_screen.rows > 0 || g_prepare_screen.switching; }

/* the cover as the layer's picture, given again when the menu gave one of its own since */
static void cover_picture(void) {
    u32 version;
    if (!g_prepare_screen.made || !ui_image_get(&version) || version != g_prepare_screen.picture_version) {
        ui_image_set(g_prepare_screen.picture);
        ui_image_get(&g_prepare_screen.picture_version);
        g_prepare_screen.made = 1;
    }
}

typedef char prepare_step_room[SNX_PREPARE_KINDS <= SNX_PREPARE_STEPS ? 1 : -1];
/* the next row of the screen, a step of a kind first seen at work at *began (set to now_ms the first time) */
static SnxPrepareStep* prepare_step(SnxPrepareStep* st, int* n, u64* began, u64 now_ms, const char* label, u32 value,
                                    u32 total, int done) {
    SnxPrepareStep* s = &st[(*n)++];
    *s = (SnxPrepareStep){0};
    if (!*began) *began = now_ms;
    s->label = label;
    s->value = value;
    s->total = total;
    s->started_ms = *began; /* for the theme's time left */
    s->done = done;
    return s;
}
int prepare_screen_steps(const RendererPrepare* p, SnxPrepareStep* st, u64 began[SNX_PREPARE_KINDS], u64 now_ms) {
    SnxPrepareStep* s;
    int n = 0;
    if (p->files) { /* the walk that finds the programs, then their translation */
        s = prepare_step(st, &n, &began[SNX_PREPARE_FIND], now_ms, "FINDING SHADERS", p->files_seen, p->files,
                         p->programs || !p->scanning);
        /* how many it looked in, whatever the theme's progress says (a percentage, a time left), and until it is
           done the one it looks in now, so that a file it takes long over shows by its name */
        snprintf(s->detail, sizeof s->detail, "%u of %u file%s%s%s", p->files_seen, p->files, p->files == 1 ? "" : "s",
                 !s->done && p->file[0] ? ", " : "", s->done ? "" : p->file);
    }
    if (p->programs) {
        s = prepare_step(st, &n, &began[SNX_PREPARE_TRANSLATE], now_ms, "PREPARING SHADERS", p->programs_done,
                         p->programs, !p->scanning);
        snprintf(s->detail, sizeof s->detail, "(%u new)", p->shipped);
    }
    if (p->pipes) {
        /* the pipelines of earlier runs, made from the shader cache: a player knows them by that name */
        s = prepare_step(st, &n, &began[SNX_PREPARE_PIPES], now_ms, "LOADING SHADER CACHE", p->pipes_made, p->pipes,
                         p->pipes_made >= p->pipes);
        snprintf(s->detail, sizeof s->detail, "%s", "from earlier sessions");
    }
    return n;
}

/* text in the font's bytes into out, capitals when upper (as the launcher's loading screen writes the game's name and
   a step's detail), at most cols glyphs: a longer one cut, ending in "..." as the launcher cuts a row. Its glyphs */
static int screen_line(const char* text, int upper, int cols, char* out, size_t size) {
    int n;
    char* q;
    fatal_cp437(text, out, size); /* a glyph a byte from here: the cut counts glyphs, not UTF-8's bytes */
    for (q = out; upper && *q; ++q)
        if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 32);
    n = (int)strlen(out);
    if (cols < 0) cols = 0;
    if (n > cols) {
        n = cols;
        if (cols > 3) memcpy(out + cols - 3, "...", 3);
        out[n] = 0;
    }
    return n;
}
static float screen_text(SnxUiList* l, float x, float y, int s, const char* text, int upper, int cols, u32 rgb) {
    char line[SNX_PREPARE_NOTE_SIZE];
    screen_line(text, upper, cols, line, sizeof line);
    return ui_text(l, x, y, s, line, UI_RGB(rgb));
}
/* a step's progress as the theme's [run] progress says, as the launcher's (steps.c gui_step_progress): "3 / 6",
   "50%", "01:23 left" */
static void screen_progress(const Row* r, u64 now_ms, char* out, size_t size) {
    u32 done = r->value < r->total ? r->value : r->total;
    i64 left = snx_run_seconds_left(now_ms > r->started_ms ? now_ms - r->started_ms : 0, done, r->total);
    if (!r->total)
        out[0] = 0;
    else if (g_prepare_screen.theme.progress == SNX_RUN_PROGRESS_PERCENT)
        snprintf(out, size, "%u%%", (unsigned)((u64)done * 100 / r->total));
    else if (g_prepare_screen.theme.progress == SNX_RUN_PROGRESS_TIME && left < 0)
        snprintf(out, size, "--:-- left");
    else if (g_prepare_screen.theme.progress == SNX_RUN_PROGRESS_TIME && left >= 3600)
        snprintf(out, size, "%u:%02u:%02u left", (unsigned)(left / 3600), (unsigned)(left / 60 % 60),
                 (unsigned)(left % 60));
    else if (g_prepare_screen.theme.progress == SNX_RUN_PROGRESS_TIME)
        snprintf(out, size, "%02u:%02u left", (unsigned)(left / 60), (unsigned)(left % 60));
    else
        snprintf(out, size, "%u / %u", (unsigned)done, (unsigned)r->total);
}
/* the rows the launcher's UI scale keeps at the least (src/gui/main.c GUI_LEAST_ROWS), and those its Auto keeps
   (GUI_AUTO_ROWS: a phone's menu needs room for a finger's row each) */
#define SCREEN_LEAST_ROWS 16
#if defined(__ANDROID__)
#define SCREEN_AUTO_ROWS 30
#else
#define SCREEN_AUTO_ROWS 16
#endif
/* the largest scale from s down, 1 at the least, whose rows rows of letters fit h pixels (main.c scale_fits) */
static int screen_fits(int s, int h, int rows) {
    while (s > 1 && h < rows * 16 * s) --s;
    return s < 1 ? 1 : s;
}
/* the letters' scale, as the launcher's on a window h pixels high: its UI scale (text_size), or with none its Auto,
   twice the display's scale (main.c pick_auto_scale); smaller while SCREEN_LEAST_ROWS would not fit */
static int screen_scale(int h, int text_size, int display_percent) {
    int s = text_size > 0 ? text_size : screen_fits(2 * display_percent / 100, h, SCREEN_AUTO_ROWS);
    return screen_fits(s, h, SCREEN_LEAST_ROWS);
}

void prepare_screen_draw(SnxUiList* l, int sw, int sh, const int safe[4], int display_percent) {
    static const char* const turn[] = {"", ".", "..", "..."};
    const SnxRunTheme* t = &g_prepare_screen.theme;
    u64 now = g_prepare_screen.now_ms;
    int x0 = safe && safe[2] > 0 ? safe[0] : 0, y0 = safe && safe[3] > 0 ? safe[1] : 0;
    int w = safe && safe[2] > 0 ? safe[2] : sw, h = safe && safe[3] > 0 ? safe[3] : sh;
    int s = screen_scale(h, t->text_size, display_percent), i, k, all_done = 1, at_work = -1, percent = 100;
    int first_row = 3; /* the first step's: under the cover when there is one */
    float cw = 8.0f * (float)s, ch = 16.0f * (float)s, x, y, left = (float)x0 + cw;
    int rows = h / (int)ch, cols = w / (int)cw;
    char line[SNX_PREPARE_NOTE_SIZE], progress[32];
    float top = (float)y0;
    if (!prepare_screen_shown()) return;
    /* the page over the whole window, the rest within its safe part */
    ui_fill(l, 0, 0, (float)sw, (float)sh, UI_RGB(t->page));
    if (t->texture) { /* the launcher's faint lines, white at 14 of 255 over the page (opaque), every four pixels */
        const u32 faint = UI_RGB(ui_over(0xFFFFFFu, t->page, 14));
        for (y = 3; y < (float)sh; y += 4) ui_fill(l, 0, y, (float)sw, 1, faint);
    }
    for (i = 0; i < g_prepare_screen.rows; ++i)
        if (!g_prepare_screen.row[i].done) {
            all_done = 0;
            if (at_work < 0) at_work = i;
        }
    /* the head: the title, the game's name (and the program a switch starts) and dots that turn while the work goes on,
       still once it is done; a switch's work is the game's first frame */
    if (g_prepare_screen.switching) all_done = 0;
    x = left + screen_text(l, left, top + ch, s, t->title, 0, cols - 2, t->ink);
    if (t->title[0]) x += cw;
    if (g_prepare_screen.switching && g_prepare_screen.program > 0)
        snprintf(line, sizeof line, "%s (Program %d)%s", g_prepare_screen.name, g_prepare_screen.program,
                 all_done ? "..." : turn[now / 400 % 4]);
    else
        snprintf(line, sizeof line, "%s%s", g_prepare_screen.name, all_done ? "..." : turn[now / 400 % 4]);
    screen_text(l, x, top + ch, s, line, 1, cols - 1 - (int)((x - (float)x0) / cw), t->soft);
    /* a switch's cover under the head, a quarter of the rows high (3 to 8), at the text's left; the steps under it.
       Not while the menu is over the screen, which shows its own picture */
    if (g_prepare_screen.switching && g_prepare_screen.cover) {
        int span = rows / 4 < 3 ? 3 : rows / 4 > 8 ? 8 : rows / 4;
        if (!overlay_shown()) {
            cover_picture();
            ui_image_part(l, left, top + ch * 3, ch * (float)span, ch * (float)span, 0, 0, SNX_PREPARE_COVER,
                          SNX_PREPARE_COVER);
        }
        first_row = 3 + span + 1;
    }
    /* a row for each step, coloured as the launcher's: the last row's label and a step at work's detail in ink, the
       others soft */
    for (i = 0; i < g_prepare_screen.rows; ++i) {
        const Row* r = &g_prepare_screen.row[i];
        int last = i == g_prepare_screen.rows - 1, col = 1; /* col: the next one, from the safe part's left */
        u32 label_ink = last ? t->ink : t->soft, detail_ink = last || !r->done ? t->ink : t->soft;
        y = top + ch * (float)(first_row + i);
        screen_progress(r, now, progress, sizeof progress);
        if (t->steps) { /* LABEL: DETAIL progress, then dots while at work, ...OK once done */
            const char* status = r->done ? "...OK" : progress;
            int end = (int)strlen(status) + (r->done ? 0 : 3); /* its columns, with the dots' at most */
            if (r->label[0]) {                                 /* no label: no colon alone */
                snprintf(line, sizeof line, "%s:", r->label);
                col +=
                    (int)(screen_text(l, cw * (float)col + (float)x0, y, s, line, 0, cols - 1 - col, label_ink) / cw) +
                    1;
            }
            /* the detail cut to what the status leaves, as the launcher cuts a step's: a long one (a file's name)
               keeps the progress on the row */
            col += (int)(screen_text(l, cw * (float)col + (float)x0, y, s, r->detail, 1, cols - 2 - col - end,
                                     detail_ink) /
                         cw) +
                   1;
            col += (int)(screen_text(l, cw * (float)col + (float)x0, y, s, status, 0, cols - 1 - col, t->ink) / cw);
            if (!r->done)
                screen_text(l, cw * (float)col + (float)x0, y, s, turn[now / 400 % 4], 0, cols - 1 - col, t->ink);
        } else { /* the console's: the line as the runtime says it, its detail cut before its progress */
            char status[40];
            snprintf(status, sizeof status, "%s%s", progress[0] && !r->done ? ": " : "", r->done ? "" : progress);
            snprintf(line, sizeof line, "%s %s", r->label, r->detail);
            x = left + screen_text(l, left, y, s, line, 0, cols - 2 - (int)strlen(status), last ? t->ink : t->soft);
            screen_text(l, x, y, s, status, 0, cols - 2 - (int)((x - left) / cw), last ? t->ink : t->soft);
        }
    }
    /* the bar shows the first step not done, as the launcher's shows the step at work, full once every one is */
    if (at_work >= 0) {
        const Row* r = &g_prepare_screen.row[at_work];
        percent = r->total ? (int)((u64)(r->value < r->total ? r->value : r->total) * 100 / r->total) : 0;
    }
    if (g_prepare_screen.rows) { /* forty segments, filled for that step's part done, and the percentage: in every
         theme, as the screen is there for its progress (the launcher's loading screen draws one only when its theme
         says bar); none while a switch waits for the game with no step, which has no part done to show */
        enum { SEGMENTS = 40 };
        int filled = (percent * SEGMENTS + 50) / 100;
        float span = (float)(cols - 8) * cw, gap = 2.0f * (float)s, seg = (span - gap * (SEGMENTS - 1)) / SEGMENTS;
        float sy = top + ch * (float)(rows - 3) + ch * 0.3f, sh4 = ch * 0.4f;
        ui_fill(l, left, top + ch * (float)(rows - 4) + ch / 2, (float)(cols - 2) * cw, (float)s, UI_RGB(t->dim));
        for (k = 0; k < SEGMENTS; ++k) {
            float sx = left + (float)k * (seg + gap);
            if (k < filled)
                ui_fill(l, sx, sy, seg, sh4, UI_RGB(t->ink));
            else { /* framed in the dim colour */
                ui_fill(l, sx, sy, seg, (float)s, UI_RGB(t->dim));
                ui_fill(l, sx, sy + sh4 - (float)s, seg, (float)s, UI_RGB(t->dim));
                ui_fill(l, sx, sy, (float)s, sh4, UI_RGB(t->dim));
                ui_fill(l, sx + seg - (float)s, sy, (float)s, sh4, UI_RGB(t->dim));
            }
        }
        snprintf(line, sizeof line, "%3d%%", percent);
        screen_text(l, (float)x0 + cw * (float)(cols - 6), top + ch * (float)(rows - 3), s, line, 0, 5, t->ink);
    }
    screen_text(l, left, top + ch * (float)(rows - 1), s, g_prepare_screen.foot, 0, cols - 2, t->soft);
    if (g_prepare_screen.rows) { /* last, the latest lines under the steps, as the launcher's detailed loading screen
         shows a step's: as many as fit above the bar's divider and in the quads left, found from the newest back so it
         is never the one left out, then drawn the newest last, each cut to the width */
        int first = first_row + g_prepare_screen.rows, room = rows - 4 - first, from = g_prepare_screen.notes;
        int budget = l->max - l->n - PREPARE_SCREEN_MENU_QUADS;
        while (from > 0 && g_prepare_screen.notes - from < room) {
            int n = screen_line(g_prepare_screen.note[from - 1], 0, cols - 4, line, sizeof line);
            if (n > budget) break;
            budget -= n;
            from--;
        }
        for (k = from; k < g_prepare_screen.notes; ++k) {
            screen_line(g_prepare_screen.note[k], 0, cols - 4, line, sizeof line);
            ui_text(l, (float)x0 + 3 * cw, top + ch * (float)(first + k - from), s, line, UI_RGB(t->soft));
        }
    }
}
