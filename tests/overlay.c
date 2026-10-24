/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's menu (runtime/platform/overlay.c): where it goes, its pages, what a tap, a move or a step does, its look;
   and ui.c's atlas and text */
#include "nx/runtime/platform/overlay.h"
#include "nx/runtime/platform/prepare_screen.h"
#include "nx/runtime/platform/fatal_screen.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/gamepad.h"
#include "nx/runtime/platform/touch.h"
#include "nx/runtime/platform/vpad.h"
#include "nx/runtime/platform/controls_atlas.h"
#include "nx/runtime/platform/plat.h"
#include "common/utilities/font.h"
#include "gui_theme_colors.h"
#include "common/utilities/users.h"
#include "gui/splash.h"
#include "common/utilities/endian.h"
/* the menu's page colour, its letters on the head's band, as R G B A bytes */
#define LIGHT_RGBA                                                                                                     \
    (0xFFu << 24 | (SNX_GUI_THEME_YORHA_PAGE & 0xFFu) << 16 | (SNX_GUI_THEME_YORHA_PAGE & 0xFF00u) |                   \
     (SNX_GUI_THEME_YORHA_PAGE >> 16 & 0xFFu))
/* the menu's colour of a held control, as R G B A bytes */
#define FAILURE                                                                                                        \
    (0xFFu << 24 | (SNX_GUI_THEME_YORHA_FAILURE & 0xFFu) << 16 | (SNX_GUI_THEME_YORHA_FAILURE & 0xFF00u) |             \
     (SNX_GUI_THEME_YORHA_FAILURE >> 16 & 0xFFu))
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* vpad.c's check of settings.txt's keys against its controls: a mismatch fails the test */
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    abort();
}
/* the platform's lock and the backend's touch screen, without either */
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
static int touch = 1;
void backend_touch_set(int mode) { touch = mode; }
/* and the gamepads' settings and names, as backend/gamepad.c keeps them; their rows and touch's name them */
static SnxGamepadConfig pads; /* the maps; the settings are g_settings' */
static const int* pad_stored;
static const char* pad_names[3];
SnxGamepadConfig* gamepad_config(void) { return &pads; }
void gamepad_store(const int* setting) { pad_stored = setting; }
static int maps_stored;
static unsigned pads_held[3];               /* what each holds */
static int pad_of_player[3] = {-1, -1, -1}; /* each player's pad, as gamepad_name counts them */
void gamepad_store_map(void) { ++maps_stored; }
int gamepad_layout(int i, int labels[4], int* symmetric) {
    static const int sony[4] = {SNX_PAD_LABEL_CROSS, SNX_PAD_LABEL_CIRCLE, SNX_PAD_LABEL_SQUARE,
                                SNX_PAD_LABEL_TRIANGLE};
    if (i != 1 || !pad_names[1]) return 0; /* no labels known: an Xbox pad's; the second a PlayStation one */
    memcpy(labels, sony, sizeof sony);
    *symmetric = 1;
    return 1;
}
unsigned gamepad_held(int i) { return i < 0 ? pads_held[0] | pads_held[1] | pads_held[2] : i < 3 ? pads_held[i] : 0; }
const char* gamepad_name(int i) { return i >= 0 && i < 3 ? pad_names[i] : NULL; }
int gamepad_of_player(int slot) { return slot >= 0 && slot < 3 ? pad_of_player[slot] : -1; }
const IniOpt g_backend_ini[] = {
    {"touch", "2", NULL, INI_INT, 0, &touch, 0, 2, NULL, "Touch screen", "Off|Fingers|Fingers and mouse", 0, 0, 0},
    {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, 0, 0}};
/* each player's own, two of them here: swap and vibration, every player's until picked, and a map */
static SnxPlayerConfig players[2];
static int player_maps_stored;
SnxPlayerConfig* gamepad_player(int p) { return p >= 0 && p < 2 ? &players[p] : NULL; }
void gamepad_store_player_map(int p) {
    (void)p;
    ++player_maps_stored;
}
const IniOpt g_gamepad_players_ini[] = {
    {"gamepad_p1_swap_ab", "-1", NULL, INI_INT, 0, &g_settings.player_swap_a_b[0], -1, 1, NULL, "Swap A/B", "As All|Off|On", 0, 0, 0},
    {"gamepad_p1_rumble", "-10", NULL, INI_INT, 0, &g_settings.player_vibration[0], -10, 100, NULL, "Vibration", "As All|Off|#%", 10,
     0, 0},
    {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, 0, 0}};
const IniOpt g_gamepad_ini[] = {
    {"gamepad_swap_ab", "0", NULL, INI_INT, 0, &g_settings.swap_a_b, 0, 1, NULL, "Swap A/B", "Off|On", 0, 0, 0},
    {"gamepad_deadzone", "20", NULL, INI_INT, 0, &g_settings.stick_dead_zone, 0, 90, NULL, "Stick dead zone", "#%", 5, 0, 0},
    {"gamepad_rumble", "100", NULL, INI_INT, 0, &g_settings.vibration, 0, 100, NULL, "Vibration", "Off|#%", 10, 0, 0},
    {"gamepad_background", "0", NULL, INI_INT, 0, &g_settings.background_input, 0, 1, NULL, "Background input", "Off|On", 0, 0, 0},
    {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, 0, 0}};
static char stored[64], log_[512]; /* the last key kept, and every key=value since log_ was emptied */
static void store(const char* key, const char* value) {
    size_t n = strlen(log_);
    snprintf(stored, sizeof stored, "%s", key);
    snprintf(log_ + n, sizeof log_ - n, "%s=%s;", key, value);
}

enum { SW = 2424, SH = 1080 };
static int px, py, s, C; /* C: the panel's columns, overlay_cols' */
/* a tap on a row of items, at a column, where the panel is for its page */
static int tap(int row, int col) {
    overlay_place(SW, SH, &px, &py, &s);
    C = overlay_cols(SW, s);
    return overlay_tap((float)(px + col * 8 * s + 2), (float)(py + (3 + row) * 16 * s + 5), SW, SH);
}
/* ui.c's warning for a picture that does not draw */
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
}
static SnxUiQuad quads[2048];
static SnxUiList list;
static void draw(void) {
    overlay_place(SW, SH, &px, &py, &s);
    C = overlay_cols(SW, s);
    list.q = quads;
    list.n = 0;
    list.max = 2048;
    overlay_draw(&list, SW, SH, 0);
}
/* whether text shows anywhere on the picture, its letters a cell apart on one row (spaces draw nothing) */
static int shows(const char* text) {
    int i, k;
    for (i = 0; i < list.n; ++i) {
        float x = list.q[i].x, y = list.q[i].y, step = 8.0f * (float)s;
        for (k = 0; text[k]; ++k) {
            unsigned char c = (unsigned char)text[k];
            int j, found = c == ' ';
            for (j = 0; j < list.n && !found; ++j)
                found = list.q[j].x == x + (float)k * step && list.q[j].y == y &&
                        list.q[j].u == SNX_CONTROLS_FONT_X + c % 16 * 8 &&
                        list.q[j].v == SNX_CONTROLS_FONT_Y + c / 16 * 16;
            if (!found) break;
        }
        if (!text[k]) return 1;
    }
    return 0;
}
/* the quads filled with rgba */
static int quads_of(u32 rgba) {
    int i, n = 0;
    for (i = 0; i < list.n; ++i) n += list.q[i].rgba == rgba && list.q[i].w > 0;
    return n;
}
/* the quad of a glyph at a cell, or -1 */
static int glyph_at(int col, int row, u8 c) {
    int i;
    for (i = 0; i < list.n; ++i)
        if (list.q[i].x == (float)(px + col * 8 * s) && list.q[i].y == (float)(py + row * 16 * s) &&
            list.q[i].u == SNX_CONTROLS_FONT_X + c % 16 * 8 && list.q[i].v == SNX_CONTROLS_FONT_Y + c / 16 * 16)
            return i;
    return -1;
}
/* whether a line of text is drawn from that cell on, in the font's glyphs (a space draws none) */
static int text_at(int col, int row, const char* text) {
    int k;
    for (k = 0; text[k]; ++k)
        if (text[k] != ' ' && glyph_at(col + k, row, (u8)text[k]) < 0) return 0;
    return 1;
}

/* a stopped panel's texts, Close in focus */
static SnxOverlayError panel_of(const char* name, const char* msg, const char* how, const char* log, const char* said) {
    SnxOverlayError e;
    memset(&e, 0, sizeof e);
    e.name = name;
    e.msg = msg;
    e.how = how;
    e.log = log;
    e.said = said;
    e.focus = SNX_OVERLAY_ERROR_CLOSE;
    return e;
}
/* The screen before the game (prepare_screen.c): hidden until told; then the page over the window, the steps, bar
   and foot within its safe part, the scrolling lines last and only as many as leave the menu room; a line cut to the
   width by glyphs, not UTF-8 bytes; and nothing once hidden again */
static void prepare_screen(void) {
    static SnxUiQuad many[4096];
    static char notes[SNX_PREPARE_NOTES][SNX_PREPARE_NOTE_SIZE];
    SnxPrepareStep st[3] = {{0}};
    SnxUiList l = {many, 0, 4096};
    const int safe[4] = {40, 0, 3760, 1080}; /* a cutout at each side */
    int i, at_foot = 0, in_cutout = 0, on_note = 0;
    assert(!prepare_screen_shown());
    prepare_screen_draw(&l, 3840, 1080, safe, 100);
    assert(l.n == 0);
    st[0].label = "FINDING SHADERS";
    st[0].value = st[0].total = 10;
    st[0].done = 1;
    st[1].label = "PREPARING SHADERS";
    st[1].value = 5;
    st[1].total = 20;
    st[2].label = NULL; /* drawn as no label */
    for (i = 0; i < SNX_PREPARE_NOTES; ++i) memset(notes[i], 'x', SNX_PREPARE_NOTE_SIZE - 1);
    prepare_screen_set(NULL, "GAME", st, 3, (const char (*)[SNX_PREPARE_NOTE_SIZE])notes, SNX_PREPARE_NOTES, "FOOT",
                       1000);
    assert(prepare_screen_shown());
    prepare_screen_draw(&l, 3840, 1080, safe, 100);
    /* scale 2 (1080 / 480), 33 rows of 32 pixels and 235 columns of 16 in the safe part */
    assert(l.n > 0 && l.n <= 4096 - PREPARE_SCREEN_MENU_QUADS);
    assert(many[0].x == 0 && many[0].w == 3840 && many[0].h == 1080); /* the page, the whole window */
    for (i = 1; i < l.n; ++i) {
        if (many[i].y == 32.0f * 32 && many[i].x == 40 + 16) at_foot = 1; /* the foot's F, at its row */
        if (many[i].h == 32 && many[i].x < 40) in_cutout = 1;             /* a letter left of the safe part */
        if (many[i].y == 32.0f * 6) on_note = 1;                          /* the first line's row, under three steps */
    }
    assert(at_foot && !in_cutout && on_note);
    /* one line of a glyph that takes two bytes in UTF-8 (127 of them), on a picture 100 columns wide: cut to 96
       glyphs (the width less 4), the last three of them dots, as a cut by bytes would not */
    for (i = 0; i < 127; ++i) memcpy(notes[0] + 2 * i, "\xC3\xA9", 2);
    notes[0][254] = 0;
    prepare_screen_set(NULL, "GAME", st, 2, (const char (*)[SNX_PREPARE_NOTE_SIZE])notes, 1, "", 1000);
    l.n = 0;
    prepare_screen_draw(&l, 1600, 1080, NULL, 100);
    on_note = 0;
    for (i = 0; i < l.n; ++i) on_note += many[i].y == 32.0f * 5 && many[i].h == 32;
    assert(on_note == 96);
    prepare_screen_set(NULL, NULL, NULL, 0, NULL, 0, NULL, 0);
    assert(!prepare_screen_shown());
}
int main(void) {
    const u32 ink = 0xFF3A4145u;
    int x, y, sc;
    vpad_set_store(store);
    snx_pad_map_unset(&pads.own); /* as gamepad.c starts it: every control the button in its place */
    snx_pad_map_unset(&players[0].own);
    prepare_screen();
    assert(!overlay_shown());
    overlay_toggle();
    assert(overlay_shown());
    /* letters half the screen's height at most, in whole steps, in the middle: a phone held wide, a small window. The
       page is as tall as its items (four here, under three rows of head, over a blank one), at the scale of the
       tallest page, and as wide as three quarters of the picture hold, between 40 and 72 columns */
    overlay_place(SW, SH, &px, &py, &s);
    C = overlay_cols(SW, s);
    assert(s == 2 && C == OVERLAY_COLS_MAX && px == (SW - C * 16) / 2 && py == (SH - 8 * 32) / 2);
    assert(overlay_cols(1280, 2) == 60 && overlay_cols(500, 1) == 46 && overlay_cols(400, 1) == OVERLAY_COLS &&
           overlay_cols(3840, 4) == OVERLAY_COLS_MAX);
    overlay_place(500, 120, &x, &y, &sc);
    assert(sc == 1 && x == (500 - 46 * 8) / 2 && y == (120 - 8 * 16) / 2);
    /* a phone's display scale: the Poco F8 (408 dpi, 2510x1156) and the Pixel 9 (420 dpi) at 3x, where half the
       picture gave 2x; a 4K picture at 100% keeps the 4x half of it gives */
    overlay_set_display_scale(408.0f / 160);
    overlay_place(2510, 1156, &x, &y, &sc);
    assert(sc == 3);
    overlay_set_display_scale(420.0f / 160);
    overlay_place(SW, SH, &x, &y, &sc);
    assert(sc == 3);
    overlay_set_display_scale(1.0f);
    overlay_place(3840, 2160, &x, &y, &sc);
    assert(sc == 4);

    /* the first page: the title on a band of ink, the items under it, the focused one on a band; the page hides the
       game, whose text would show through it */
    draw();
    assert(list.q[0].w == (float)(C * 16) && list.q[0].h == 8 * 32 && list.q[1].rgba == ink && list.q[1].h == 32);
    assert(list.q[0].rgba >> 24 == 0xFFu);
    assert(text_at(C - 2, 3, ">") && text_at(C - 2, 4, ">")); /* a page under each */
    assert(text_at(1, 0, "SCARLETNX  /  MENU") && text_at(1, 3, "\xFE On-screen controls") &&
           text_at(1, 4, "\xFE Controllers") && text_at(1, 5, "\xFE Save states") && text_at(1, 6, "\xFE Exit"));
    /* a tap on Exit exits; on the title, nothing; outside the panel, it closes */
    assert(tap(3, 3) == OVERLAY_EXIT && tap(-3, 3) == OVERLAY_NONE);
    assert(overlay_tap(5.0f, 5.0f, SW, SH) == OVERLAY_CLOSE);
    /* the focus goes round, from Exit to the first item and back over the top; Back from here closes */
    overlay_move(1);
    overlay_move(-5);
    assert(overlay_select() == OVERLAY_EXIT && overlay_back() == OVERLAY_CLOSE);

    /* the controls' page: each setting shown with its value, changed and kept */
    assert(tap(0, 3) == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  CONTROLS") && text_at(1, 3, "\xFE Show controls"));
    assert(list.q[0].h == 16 * 32 && text_at(C - 2, 12, ">")); /* twelve items: the tallest; Buttons opens a page */
    /* named as the .ini's rows name them, in the launcher's order, the game's own actions among them */
    assert(text_at(1, 4, "\xFE Edit layout") && text_at(1, 5, "\xFE Snap to grid") &&
           text_at(1, 8, "\xFE Relative stick center"));
    assert(text_at(1, 11, "\xFE Touch screen") && text_at(1, 12, "\xFE Buttons") &&
           text_at(1, 13, "\xFE Reset layout"));
    g_settings.show_controls = 0;
    draw();
    assert(text_at(C - 4, 3, "Off"));
    assert(tap(0, 3) == OVERLAY_NONE && g_settings.show_controls && !strcmp(stored, "controls"));
    draw();
    assert(text_at(C - 3, 3, "On"));
    /* Scale's arrows: the left one down, the right one up, by tens, between 50% and 150% */
    assert(g_settings.controls_scale == 50);
    draw();
    assert(text_at(C - 9, 6, "< 100% >"));
    tap(3, C - 2);
    assert(g_settings.controls_scale == 60 && !strcmp(stored, "controls_scale"));
    tap(3, C - 9);
    tap(3, C - 9);
    assert(g_settings.controls_scale == 40);
    for (x = 0; x < 9; ++x) tap(3, C - 9);
    assert(g_settings.controls_scale == 0);
    g_settings.controls_scale = 50;
    /* the keys: down from Scale to Opacity, left steps it */
    overlay_move(1);
    overlay_side(-1);
    assert(g_settings.controls_opacity == 90 && !strcmp(stored, "controls_opacity"));
    overlay_side(1);
    /* the toggles; the touch screen goes round its three */
    tap(2, 3);
    assert(g_settings.snap_to_grid && !strcmp(stored, "controls_snap"));
    tap(2, 3);
    tap(5, 3);
    assert(!g_settings.relative_stick_center && !strcmp(stored, "controls_rel_center"));
    tap(5, 3);
    tap(8, 3);
    assert(touch == 2);
    draw();
    assert(text_at(C - 1 - (int)strlen("Fingers and mouse"), 11, "Fingers and mouse"));
    tap(8, 3);
    assert(touch == 0);
    draw();
    assert(text_at(C - 4, 11, "Off"));
    tap(8, 3);
    /* Edit layout is the caller's to do */
    assert(tap(1, 3) == OVERLAY_EDIT);

    /* the buttons' page: one hidden and kept, All shows them again; it scrolls to its last item */
    assert(tap(9, 3) == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  BUTTONS") && text_at(1, 4, "\xFE L"));
    log_[0] = 0;
    tap(1, 3);
    assert(!g_settings.control_shown[SNX_VPAD_L] && !strcmp(log_, "controls_l_shown=0;")); /* its place stays */
    draw();
    assert(text_at(C - 7, 4, "Hidden") && text_at(C - 12, 3, "Some hidden"));
    tap(0, 3);
    assert(g_settings.control_shown[SNX_VPAD_L]);
    tap(0, 3); /* all shown: All hides them */
    assert(!g_settings.control_shown[SNX_VPAD_A] && !g_settings.control_shown[SNX_VPAD_L]);
    tap(0, 3);
    overlay_move(-1); /* round to Back, at the bottom */
    draw();
    assert(text_at(1, 3 + 11, "\xFE Back") && text_at(C - 1, 3, "\x18") && !text_at(C - 1, 14, "\x19"));
    /* the wheel stops at the ends: up past All, and down past Back, where Back still goes up a page */
    overlay_scroll(-20);
    draw();
    assert(text_at(1, 3, "\xFE All") && !text_at(C - 1, 3, "\x18") && text_at(C - 1, 14, "\x19"));
    overlay_scroll(20);
    overlay_scroll(1);
    /* Back on it, and the key Back, go up a page each */
    assert(overlay_select() == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  CONTROLS"));
    assert(overlay_back() == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  MENU"));
    /* a pad's buttons, as the game reads them: the D-pad and the left stick move, A selects, B goes back, the rest do
       nothing */
    {
        static const i32 still[4], down[4] = {0, -20000, 0, 0}, left[4] = {-20000, 0, 0, 0};
        assert(overlay_pad(1ull << 15) == OVERLAY_NONE && overlay_pad(1ull << 19) == OVERLAY_NONE);
        assert(overlay_pad(1ull << 13) == OVERLAY_NONE && overlay_pad(1ull << 2) == OVERLAY_NONE); /* X */
        assert(overlay_pad(1ull << 0) == OVERLAY_NONE);                                            /* Controllers */
        draw();
        assert(text_at(1, 0, "SCARLETNX  /  CONTROLLERS"));
        assert(overlay_pad(1ull << 1) == OVERLAY_NONE);
        draw();
        assert(text_at(1, 0, "SCARLETNX  /  MENU"));
        /* the pads each poll (the on-screen controls and the connected ones): a press acts once; a direction held, the
           left stick past half way as one, goes on after a while, then on and on; with the menu closed, nothing, and
           a press held as it opens does not act */
        assert(overlay_controls(0, still, 1000) == OVERLAY_NONE); /* the focus back on Controllers */
        overlay_move(1);                                          /* Save states */
        assert(overlay_controls(0, down, 1000) == OVERLAY_NONE);  /* to Exit */
        assert(overlay_controls(0, down, 1000 + SNX_OVERLAY_REPEAT_DELAY - 1) == OVERLAY_NONE);
        assert(overlay_controls(1ull << 0, down, 1000 + SNX_OVERLAY_REPEAT_DELAY - 1) == OVERLAY_EXIT); /* not yet */
        assert(overlay_controls(0, down, 1000 + SNX_OVERLAY_REPEAT_DELAY) == OVERLAY_NONE); /* round to the top */
        assert(overlay_controls(1ull << 0, down, 1001 + SNX_OVERLAY_REPEAT_DELAY) == OVERLAY_NONE); /* a page */
        draw();
        assert(text_at(1, 0, "SCARLETNX  /  CONTROLS"));
        assert(overlay_controls(1ull << 1, still, 2000) == OVERLAY_NONE);  /* B: back to the menu, on its first */
        assert(overlay_controls(1ull << 15, still, 2001) == OVERLAY_NONE); /* the D-pad down: Controllers */
        assert(overlay_controls(1ull << 15, still, 2001 + SNX_OVERLAY_REPEAT_DELAY) == OVERLAY_NONE); /* Exit */
        assert(overlay_controls(1ull << 15, still, 2001 + SNX_OVERLAY_REPEAT_DELAY + SNX_OVERLAY_REPEAT_MS - 1) ==
               OVERLAY_NONE);
        overlay_move(1); /* past Save states */
        assert(overlay_controls(1ull << 0, still, 3000) == OVERLAY_EXIT);
        assert(overlay_controls(1ull << 13, still, 3001) == OVERLAY_NONE &&
               overlay_controls(0, left, 3002) == OVERLAY_NONE);
        assert(overlay_controls(1ull << 1, still, 3003) == OVERLAY_CLOSE);
        overlay_toggle(); /* closed: the pads are the game's */
        assert(!overlay_shown() && overlay_controls(1ull << 1, still, 3004) == OVERLAY_NONE);
        overlay_toggle(); /* B still held as it opens */
        assert(overlay_controls(1ull << 1, still, 3005) == OVERLAY_NONE);
        assert(overlay_controls(0, still, 3006) == OVERLAY_NONE);
        /* a finger or the mouse on the open menu: down on an on-screen control it is the controls', whose D-pad then
           moves the menu and whose lift elsewhere taps nothing; off them its lift taps the menu, outside the panel
           closing it; one the menu forgot (it opened, the window lost the focus) or the system cancelled taps nothing */
        {
            const int area[4] = {0, 0, SW, SH};
            const SnxVpadConfig* v = vpad_config();
            float dx = v->at[SNX_VPAD_DPAD].x * SW, dy = v->at[SNX_VPAD_DPAD].y * SH + 0.6f * 0.25f * SH / 2;
            u64 b;
            i32 st[4];
            assert(vpad_ini("controls", "1"));
            overlay_pointer_down(SNX_TOUCH_MOUSE, dx, dy, area);
            vpad_state(&b, st);
            assert(b == 1ull << 15 && overlay_controls(b, still, 4000) == OVERLAY_NONE); /* to Controllers */
            assert(overlay_pointer_up(SNX_TOUCH_MOUSE, 5, 5, SW, SH) == OVERLAY_NONE && overlay_shown());
            vpad_state(&b, st);
            assert(!b && overlay_controls(1ull << 0, still, 4001) == OVERLAY_NONE);
            draw();
            assert(text_at(1, 0, "SCARLETNX  /  CONTROLLERS"));
            assert(overlay_controls(1ull << 1, still, 4002) == OVERLAY_NONE); /* B: the menu */
            overlay_pointer_down(8, 5, 5, area);
            assert(overlay_pointer_up(8, 5, 5, SW, SH) == OVERLAY_CLOSE);
            overlay_pointer_down(9, 5, 5, area);
            overlay_pointers_reset();
            assert(overlay_pointer_up(9, 5, 5, SW, SH) == OVERLAY_NONE);
            overlay_pointer_down(10, 5, 5, area);
            overlay_pointer_cancel(10);
            assert(overlay_pointer_up(10, 5, 5, SW, SH) == OVERLAY_NONE);
            assert(vpad_ini("controls", "0"));
        }
    }

    /* the controllers' page: the ones connected, the Player row (every player's, or one's own), Swap A/B, the dead
       zone by fives, the vibration by tens down to Off and the background input, each kept */
    assert(tap(1, 3) == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  CONTROLLERS") && text_at(1, 1, "None connected."));
    assert(list.q[0].h == 11 * 32 && text_at(1, 3, "\xFE Player") && shows("< All >"));
    assert(text_at(1, 4, "\xFE Swap A/B") && text_at(C - 4, 4, "Off"));
    tap(1, 3);
    assert(g_settings.swap_a_b && pad_stored == &g_settings.swap_a_b);
    draw();
    assert(text_at(C - 3, 4, "On"));
    tap(1, 3);
    assert(!g_settings.swap_a_b);
    overlay_side(1);
    assert(g_settings.swap_a_b);
    g_settings.swap_a_b = 0;
    draw();
    assert(text_at(C - 8, 5, "< 20% >"));
    tap(2, C - 2);
    assert(g_settings.stick_dead_zone == 25 && pad_stored == &g_settings.stick_dead_zone);
    tap(2, C - 9);
    tap(2, C - 9);
    assert(g_settings.stick_dead_zone == 15);
    for (x = 0; x < 20; ++x) tap(2, C - 2);
    assert(g_settings.stick_dead_zone == 90);
    g_settings.stick_dead_zone = 20;
    for (x = 0; x < 10; ++x) tap(3, C - 9);
    assert(g_settings.vibration == 0 && pad_stored == &g_settings.vibration);
    draw();
    assert(text_at(C - 8, 6, "< Off >"));
    assert(overlay_select() == OVERLAY_NONE && g_settings.vibration == 10); /* the focus is on it: a select steps up */
    g_settings.vibration = 100;
    draw();
    assert(text_at(1, 7, "\xFE Background input") && text_at(C - 4, 7, "Off"));
    tap(4, 3);
    assert(g_settings.background_input && pad_stored == &g_settings.background_input);
    draw();
    assert(text_at(C - 3, 7, "On"));
    tap(4, 3);
    assert(!g_settings.background_input);
    pad_names[0] = "Xbox Wireless Controller";
    draw();
    assert(text_at(1, 1, "Xbox Wireless Controller") && !text_at(26, 1, "and"));
    pad_names[1] = "Pro Controller";
    pad_names[2] = "A controller with a very long name indeed";
    draw();
    assert(text_at(1, 1, "Xbox Wireless Controller and 2 more"));
    pad_names[0] = pad_names[2];
    draw();
    assert(text_at(1, 1, "A controller with a very lo and 2 more"));
    /* Button map: NieR's CONTROLS over the picture, no presets, the first control focused; its button changed (the map
       the player's from then on), a press on the pad while waiting swapping two, that press no menu key; Back goes up
       to its item */
    pad_names[0] = "Xbox Wireless Controller";
    pad_names[1] = pad_names[2] = NULL;
    pad_of_player[0] = 0;
    draw();
    assert(text_at(1, 8, "\xFE Button map") && text_at(C - 2, 8, ">"));
    assert(tap(5, 3) == OVERLAY_NONE);
    pads_held[0] = 1u << SNX_PAD_RT;
    draw();
    assert(shows("SCARLETNX  /  BUTTON MAP") && !shows("By label") && shows("Xbox Wireless Controller"));
    assert(shows("LT sends ZL") && quads_of(FAILURE) > 0); /* RT held: lit */
    overlay_side(1);                                       /* LT's button, ZL, up one: ZR */
    assert(pads.own.bit[SNX_PAD_LT] == 9 && maps_stored == 1);
    draw();
    assert(shows("LT sends ZR"));
    assert(overlay_select() == OVERLAY_NONE && overlay_listening());
    overlay_press(SNX_PAD_SOUTH); /* the bottom one, B: LT and it swap */
    assert(!overlay_listening() && pads.own.bit[SNX_PAD_LT] == 1 && pads.own.bit[SNX_PAD_SOUTH] == 9 &&
           maps_stored == 2);
    {
        static const i32 rest[4];
        assert(overlay_controls(1ull << 0, rest, 5000) == OVERLAY_NONE && !overlay_listening()); /* taken: no A */
        assert(overlay_controls(1ull << 0, rest, 5001) == OVERLAY_NONE && !overlay_listening());
        assert(overlay_controls(0, rest, 5002) == OVERLAY_NONE);
    }
    assert(overlay_back() == OVERLAY_NONE); /* no presets to go back to: up to its item at once */
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  CONTROLLERS") && text_at(1, 8, "\xFE Button map"));
    pads_held[0] = 0;
    /* the Player row steps to player 1: its Swap A/B and Vibration its own, As All until picked; its Button map
       its own map, kept as its key; round the eight back to All */
    tap(0, 3);
    draw();
    assert(shows("< 1 >") && text_at(1, 4, "\xFE Swap A/B") && shows("As All"));
    tap(1, 3);
    assert(g_settings.player_swap_a_b[0] == 0 && pad_stored == &g_settings.player_swap_a_b[0] && !g_settings.swap_a_b);
    tap(5, 3);
    draw();
    assert(shows("SCARLETNX  /  PLAYER 1  /  BUTTON MAP"));
    overlay_side(1); /* from every player's map, where LT sends B now: up one, X; every player's stays */
    assert(players[0].has_map && players[0].own.bit[SNX_PAD_LT] == 2 && pads.own.bit[SNX_PAD_LT] == 1 &&
           player_maps_stored == 1 && maps_stored == 2);
    assert(overlay_back() == OVERLAY_NONE);
    /* a player's map draws that player's pad, its labels and what it holds, not the first pad; one whose pad is not
       there, none */
    pad_names[1] = "DualSense Wireless Controller";
    pad_of_player[1] = 1;
    pads_held[0] = 1u << SNX_PAD_RT;
    tap(0, 3);
    tap(5, 3);
    draw();
    assert(shows("SCARLETNX  /  PLAYER 2  /  BUTTON MAP") && shows("DualSense Wireless Controller"));
    assert(!shows("Xbox Wireless Controller") && shows("Cross") && !quads_of(FAILURE));
    pads_held[1] = 1u << SNX_PAD_RT;
    draw();
    assert(quads_of(FAILURE) > 0);
    assert(overlay_back() == OVERLAY_NONE);
    tap(0, 3);
    tap(5, 3);
    draw();
    assert(shows("SCARLETNX  /  PLAYER 3  /  BUTTON MAP") && shows("No controller"));
    assert(!shows("Xbox Wireless Controller") && !shows("DualSense Wireless Controller") && !quads_of(FAILURE));
    assert(overlay_back() == OVERLAY_NONE);
    /* every player's: the first pad, lit by any */
    for (x = 0; x < 6; ++x) tap(0, 3);
    tap(5, 3);
    pads_held[0] = 0;
    draw();
    assert(shows("SCARLETNX  /  BUTTON MAP") && shows("Xbox Wireless Controller") && quads_of(FAILURE) > 0);
    assert(overlay_back() == OVERLAY_NONE);
    pads_held[1] = 0;
    pad_names[1] = NULL;
    draw();
    assert(shows("< All >"));
    /* Back, on its item or the key, comes up to the controllers' item */
    assert(tap(6, 3) == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  MENU") && overlay_select() == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  CONTROLLERS") && overlay_back() == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  MENU"));
    /* the save states' pages: a row per slot, the auto one first, then a slot's Save, Load and Delete, Load and
       Delete asked first. The list is read again each time the page opens */
    {
        OverlaySlot sl[3];
        u8* thumb = (u8*)calloc((size_t)SNX_UI_IMAGE_W * SNX_UI_IMAGE_H, 4);
        u32 version;
        memset(sl, 0, sizeof sl);
        snprintf(sl[2].when, sizeof sl[2].when, "2026-10-22 14:05  0:02:00");
        sl[2].thumb = thumb;
        assert(!overlay_states_shown());
        overlay_move(1);
        assert(overlay_select() == OVERLAY_NONE && overlay_states_shown() && overlay_states_stale());
        overlay_slots_set(sl, 3);
        assert(!overlay_states_stale());
        draw();
        assert(shows("SCARLETNX  /  SAVE STATES") && shows("\xFE Auto") &&
               shows("\xFE Slot 1") && shows("\xFE Slot 2") && shows("\xFE Back"));
        assert(shows("Empty") && shows("2026-10-22 14:05  0:02:00"));
        /* the focused slot's thumbnail beside the list: none for an empty one, slot 2's once in focus */
        assert(!ui_image_get(&version));
        overlay_move(2);
        draw();
        assert(ui_image_get(&version) != NULL);
        assert(overlay_select() == OVERLAY_NONE);
        draw();
        assert(shows("SCARLETNX  /  SLOT 2") && shows("\xFE Save"));
        assert(overlay_select() == OVERLAY_SAVE && overlay_slot() == 2);
        overlay_move(1); /* Load: asked, Cancel in focus */
        assert(overlay_select() == OVERLAY_NONE);
        draw();
        assert(shows("SCARLETNX  /  LOAD SLOT 2?"));
        assert(overlay_select() == OVERLAY_NONE); /* Cancel */
        draw();
        assert(shows("SCARLETNX  /  SLOT 2"));
        assert(overlay_select() == OVERLAY_NONE); /* Load again, the focus back on it, then yes */
        overlay_move(-1);
        assert(overlay_select() == OVERLAY_LOAD && overlay_slot() == 2);
        /* an empty slot neither loads nor goes */
        assert(overlay_back() == OVERLAY_NONE && overlay_states_shown());
        overlay_move(-1);
        assert(overlay_select() == OVERLAY_NONE);
        overlay_move(1);
        assert(overlay_select() == OVERLAY_NONE);
        draw();
        assert(shows("SCARLETNX  /  SLOT 1"));
        /* the auto slot has no Save */
        assert(overlay_back() == OVERLAY_NONE);
        overlay_move(-1);
        assert(overlay_select() == OVERLAY_NONE);
        draw();
        assert(shows("SCARLETNX  /  AUTO SLOT") && shows("\xFE Load"));
        assert(overlay_back() == OVERLAY_NONE && overlay_back() == OVERLAY_NONE && !overlay_states_shown());
        draw();
        assert(shows("SCARLETNX  /  MENU"));
    }
    /* a notification over the game, closed or open, until its time is up */
    overlay_toast("Saved to slot 2", 1000);
    assert(overlay_toasting(1000) && !overlay_toasting(1000 + (u64)SNX_OVERLAY_TOAST_MS * 1000000u));
    overlay_toggle();
    draw();
    assert(!overlay_shown() && shows("Saved to slot 2"));
    overlay_toast("", 0); /* none */
    overlay_toggle();
    /* closed, it draws nothing; opened again, its first page */
    overlay_toggle();
    draw();
    assert(!overlay_shown() && !list.n);
    overlay_toggle();
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  MENU"));

    /* ui.c: the atlas with the font's glyphs, white where a glyph's bit is, and sprites' alpha premultiplied */
    {
        u32* sprites = calloc((size_t)SNX_CONTROLS_ATLAS_WIDTH * SNX_CONTROLS_ATLAS_HEIGHT, 4);
        const u32* a;
        int w, h;
        u32 version;
        assert(sprites);
        sprites[0] = snx_le32(0x80FF8040u); /* half alpha, in the bytes R G B A as SDL's RGBA32 has them */
        assert(!ui_atlas(&w, &h, &version) && !version);
        ui_atlas_set(sprites, 4, 4); /* not the atlas the header is for */
        assert(!ui_atlas(&w, &h, &version));
        ui_atlas_set(sprites, SNX_CONTROLS_ATLAS_WIDTH, SNX_CONTROLS_ATLAS_HEIGHT);
        a = ui_atlas(&w, &h, &version);
        assert(a && version == 1 && w == SNX_CONTROLS_ATLAS_WIDTH && h == SNX_CONTROLS_ATLAS_HEIGHT + SNX_UI_PICTURE);
        /* no user read yet: the picture's rows are transparent */
        for (y = 0; y < SNX_UI_PICTURE; ++y)
            for (x = 0; x < SNX_UI_PICTURE; ++x) assert(!a[(SNX_CONTROLS_ATLAS_HEIGHT + y) * w + x]);
        assert(snx_le32(a[0]) == 0x80804020u); /* the texels are the GPU's bytes */
        /* every glyph, white where its bit is: 'A' has both */
        for (sc = 0; sc < 256; ++sc)
            for (y = 0; y < 16; ++y)
                for (x = 0; x < 8; ++x)
                    assert(a[(SNX_CONTROLS_FONT_Y + sc / 16 * 16 + y) * w + SNX_CONTROLS_FONT_X + sc % 16 * 8 + x] ==
                           (gui_vga_font[sc * 16 + y] & (0x80 >> x) ? 0xFFFFFFFFu : 0));
        assert(gui_vga_font['A' * 16 + 5] && gui_vga_font['A' * 16 + 5] != 0xFF);
        assert(a[(SNX_CONTROLS_WHITE_Y + 1) * w + SNX_CONTROLS_WHITE_X + 1] == 0xFFFFFFFFu);
        ui_atlas_done();
        assert(!ui_atlas(&w, &h, &version) && version == 1);
        free(sprites);
    }
    /* a fatal error's panel: the menu's, centred on its own rows; the head, the game's name, the error in the
       failure's colour, its hint, then Share Log and Close at its foot, Close in focus. UTF-8 comes out in the font's
       bytes, and on a short picture the last paragraphs give up rows so the panel stays on it, what Share Log did last */
    {
        float top, row_h;
        int total, i, close_band = -1, share_band = -1;
        unsigned plain, focused;
        SnxOverlayError panel = panel_of("ENDER MAGNOLIA", "vulkan: the GPU device was lost (driver reset or crash)",
                                         "Share Log sends the game's log, which has what ran last.", NULL, NULL);
        list.n = 0;
        overlay_error_draw(&list, SW, SH, &panel);
        overlay_place(SW, SH, &px, &py, &s);
        row_h = 16.0f * (float)s;
        top = list.q[0].y;
        total = (int)(list.q[0].h / row_h);
        C = overlay_cols(SW, s);
        assert(list.n > 4 && list.q[0].x == (float)px && list.q[0].w == (float)(C * 8 * s) &&
               list.q[0].rgba >> 24 == 0xFFu);
        assert(top >= 0 && top + list.q[0].h <= (float)SH && list.q[1].y == top && list.q[1].h == row_h);
        py = (int)top;
        assert(text_at(1, 0, "SCARLETNX  /  STOPPED") && text_at(1, 1, "ENDER MAGNOLIA"));
        assert(
            text_at(1, 3, "vulkan: the GPU device was lost (driver reset or crash)")); /* one row, as wide as it is */
        i = glyph_at(1, 3, 'v');
        assert(i >= 0 && list.q[i].rgba == 0xFF3C47A5u);
        assert(text_at(1, 5, "The GPU stopped answering and its"));
        for (i = 0; i < list.n; ++i)
            if (list.q[i].y == top + row_h * (float)(total - 2) && list.q[i].h == row_h && list.q[i].w == list.q[0].w)
                close_band = i;
        assert(close_band >= 0 && text_at(1, total - 2, "\xFE Close") && text_at(1, total - 3, "\xFE Copy path") &&
               text_at(1, total - 4, "\xFE Share Log"));
        i = glyph_at(3, total - 4, 'S');
        assert(i >= 0);
        plain = list.q[i].rgba;
        i = glyph_at(3, total - 2, 'C');
        assert(i >= 0 && list.q[i].rgba != plain);
        focused = list.q[i].rgba;
        for (i = 0; i < SNX_OVERLAY_ERROR_ITEMS; ++i)
            assert(panel.items[i].y == top + row_h * (float)(total - 1 - SNX_OVERLAY_ERROR_ITEMS + i) &&
                   panel.items[i].h == row_h && panel.items[i].x == list.q[0].x && panel.items[i].w == list.q[0].w);
        assert(overlay_error_hit(&panel.items[SNX_OVERLAY_ERROR_SHARE], panel.items[0].x + 1, panel.items[0].y + 1));
        assert(!overlay_error_hit(&panel.items[SNX_OVERLAY_ERROR_SHARE], panel.items[1].x + 1, panel.items[1].y + 1));
        /* the band follows the focus */
        panel.focus = SNX_OVERLAY_ERROR_SHARE;
        list.n = 0;
        overlay_error_draw(&list, SW, SH, &panel);
        for (i = 0; i < list.n; ++i)
            if (list.q[i].y == top + row_h * (float)(total - 4) && list.q[i].h == row_h && list.q[i].w == list.q[0].w)
                share_band = i;
        assert(share_band >= 0);
        i = glyph_at(3, total - 2, 'C');
        assert(i >= 0 && list.q[i].rgba == plain);
        i = glyph_at(3, total - 4, 'S');
        assert(i >= 0 && list.q[i].rgba == focused);
        /* what Share Log did, over the items, and kept when the picture is short */
        panel.said = "Log saved to /home/player/.local/state/scarletnx/games/0100ABCD12345000/scarletnx.log.";
        list.n = 0;
        overlay_error_draw(&list, SW, 260, &panel);
        overlay_place(SW, 260, &px, &py, &s);
        py = (int)list.q[0].y;
        assert(list.q[0].y >= 0 && list.q[0].y + list.q[0].h <= 260.0f);
        total = (int)(list.q[0].h / (16.0f * (float)s));
        for (i = 1, share_band = 0; i < total - 5; ++i) share_band |= text_at(1, i, "Log saved to");
        assert(share_band);
        list.n = 0;
        panel = panel_of("CAF\xC3\x89", "x", "y", NULL, NULL);
        overlay_error_draw(&list, SW, SH, &panel);
        overlay_place(SW, SH, &px, &py, &s);
        py = (int)list.q[0].y;
        assert(text_at(1, 1, "CAF\x90"));
        list.n = 0;
        panel =
            panel_of("N", "message",
                     "a b c d e f g h i j k l m n o p q r s t u v w x y z a b c d e f g h i j k l m n o p q r s t u "
                     "v w x y z a b c d e f g h i j k l m n o p q r s t u v w x y z",
                     NULL, NULL);
        overlay_error_draw(&list, SW, 200, &panel);
        assert(list.q[0].y >= 0 && list.q[0].y + list.q[0].h <= 200.0f);
    }
    /* Only visible log rows are a link. Wrapped paths remain one target, and a
       short screen that hides the path leaves no invisible click target. */
    {
        int i, underlines = 0;
        const char* path = "/home/player/.local/state/scarletnx/A game with a long name.log";
        SnxOverlayError panel = panel_of("GAME", "message", "The game's log:", path, NULL);
        SnxOverlayRect link;
        list.n = 0;
        overlay_error_draw(&list, SW, SH, &panel);
        link = panel.link;
        assert(link.w > 0 && link.h >= 16.0f * (float)s);
        /* right under its label, no blank row between */
        overlay_place(SW, SH, &px, &py, &s);
        py = (int)list.q[0].y;
        assert(text_at(1, (int)((link.y - (float)py) / (16.0f * (float)s)) - 1, "The game's log:"));
        assert(overlay_error_hit(&link, link.x, link.y));
        assert(overlay_error_hit(&link, link.x + link.w - 1, link.y + link.h - 1));
        assert(!overlay_error_hit(&link, link.x - 1, link.y));
        assert(!overlay_error_hit(&link, link.x + link.w, link.y));
        assert(!overlay_error_hit(&link, link.x, link.y + link.h));
        for (i = 0; i < list.n; ++i)
            if (list.q[i].x == link.x && list.q[i].h == (float)s && list.q[i].w > 0 && list.q[i].y >= link.y &&
                list.q[i].y < link.y + link.h)
                ++underlines;
        assert(underlines == (int)(link.h / (16.0f * (float)s)));
        list.n = 0;
        overlay_error_draw(&list, SW, 160, &panel);
        assert(!overlay_error_hit(&panel.link, panel.link.x, panel.link.y) && !panel.link.h);
        list.n = 0;
        panel = panel_of("GAME", "message", "Share Log", NULL, NULL);
        overlay_error_draw(&list, SW, SH, &panel);
        assert(!panel.link.w && !panel.link.h && !overlay_error_hit(NULL, 0, 0));
        /* Share Log's line takes room from the log's label and the hint first: on a picture the panel just fills with
           both, the whole path is still there and still a link */
        {
            const char* said =
                "Log saved to /home/player/.local/state/scarletnx/games/0100ABCD12345000/scarletnx.log (path copied).";
            int whole;
            panel = panel_of("GAME", "Unsupported instruction ce62c820 at 801b67c",
                             "The game's log has what ran last:", path, NULL);
            list.n = 0;
            overlay_error_draw(&list, 1280, 720, &panel);
            overlay_place(1280, 720, &px, &py, &s);
            whole = (int)(panel.link.h / (16.0f * (float)s));
            for (int height = 720, cut = 0; height >= 200; height -= 4) {
                int label = 0, rows;
                panel.said = NULL; /* the log whole at this size, its columns and scale */
                list.n = 0;
                overlay_error_draw(&list, 1280, height, &panel);
                overlay_place(1280, height, &px, &py, &s);
                whole = (int)(panel.link.h / (16.0f * (float)s));
                panel.said = said;
                list.n = 0;
                overlay_error_draw(&list, 1280, height, &panel);
                py = (int)list.q[0].y;
                assert(list.q[0].y >= 0 && list.q[0].y + list.q[0].h <= (float)height);
                rows = (int)(panel.link.h / (16.0f * (float)s));
                for (int r = 0; r < 64; ++r) label |= text_at(1, r, "The game's log has");
                assert(rows == whole || !label);
                cut |= !label;
                if (height == 200) assert(cut); /* the sweep got to where rows are cut */
            }
        }
    }
    {
        char url[256];
        assert(fatal_log_url("/tmp/a b#c%?.log", url, sizeof url));
        assert(!strcmp(url, "file:///tmp/a%20b%23c%25%3F.log"));
        assert(fatal_log_url("C:\\games\\a b.log", url, sizeof url));
        assert(!strcmp(url, "file:///C:/games/a%20b.log"));
        assert(fatal_log_url("\\\\server\\share\\a.log", url, sizeof url));
        assert(!strcmp(url, "file://server/share/a.log"));
        assert(fatal_log_url("/tmp/caf\xC3\xA9\\x.log", url, sizeof url));
        assert(!strcmp(url, "file:///tmp/caf%C3%A9%5Cx.log"));
        assert(!fatal_log_url("relative.log", url, sizeof url) && !*url);
        assert(!fatal_log_url("https://example.org", url, sizeof url) && !*url);
        assert(!fatal_log_url("/tmp/a.log", url, 8) && !*url);
        assert(!fatal_log_url(NULL, url, sizeof url) && !*url);
        assert(!fatal_log_url("", url, sizeof url) && !*url);
        assert(!fatal_log_url("/tmp/a.log", NULL, 0));
    }
    /* the user playing on the head's band: Player, without users.ini, and Wimsy, as the YoRHa theme has it (square),
       in the atlas under the sprites; on the right of the band, its frame in the page colour and the nickname before
       it; none on a stopped game's panel */
    {
        char why[256];
        u32* sprites = calloc((size_t)SNX_CONTROLS_ATLAS_WIDTH * SNX_CONTROLS_ATLAS_HEIGHT, 4);
        const u32* a;
        int w, h, i, pictures = 0, frames = 0, white = 0;
        u32 version;
        float right, band;
        SnxOverlayError e = panel_of("Game", "It stopped", "Look at the log", NULL, NULL);
        assert(sprites && !snx_users_load("/nonexistent/users", 1, why, sizeof why));
        assert(g_user_count == 1 && !strcmp(snx_users_selected()->nickname, "Player"));
        assert(SNX_GUI_THEME_YORHA_PICTURE == 1); /* square: the frame is the fills' */
        ui_atlas_set(sprites, SNX_CONTROLS_ATLAS_WIDTH, SNX_CONTROLS_ATLAS_HEIGHT);
        a = ui_atlas(&w, &h, &version);
        assert(a && h == SNX_CONTROLS_ATLAS_HEIGHT + SNX_UI_PICTURE);
        for (y = 0; y < SNX_UI_PICTURE; ++y)
            for (x = 0; x < SNX_UI_PICTURE; ++x) {
                u32 p = snx_le32(a[(SNX_CONTROLS_ATLAS_HEIGHT + y) * w + x]); /* the GPU's bytes R G B A */
                assert(p >> 24 == 0xFF); /* square: every texel opaque */
                white += p == 0xFFFFFFFFu;
            }
        assert(a[SNX_CONTROLS_ATLAS_HEIGHT * w] == 0xFFFFFFFFu && white < SNX_UI_PICTURE * SNX_UI_PICTURE * 9 / 10);
        { /* texel for texel the picture drawn ARGB, R G B A from the low byte: Wimsy has texels whose red and blue
             differ, which the order would swap */
            static u32 argb[SNX_UI_PICTURE * SNX_UI_PICTURE];
            int unlike = 0;
            assert(!snx_splash_picture(NULL, 0, SNX_UI_PICTURE, SNX_GUI_THEME_YORHA_PICTURE == 2, argb,
                                       SNX_UI_PICTURE * 4));
            for (y = 0; y < SNX_UI_PICTURE; ++y)
                for (x = 0; x < SNX_UI_PICTURE; ++x) {
                    u32 p = argb[y * SNX_UI_PICTURE + x];
                    unlike += (p >> 16 & 0xFFu) != (p & 0xFFu);
                    assert(snx_le32(a[(SNX_CONTROLS_ATLAS_HEIGHT + y) * w + x]) ==
                           ((p & 0xFF00FF00u) | (p >> 16 & 0xFFu) | (p & 0xFFu) << 16));
                }
            assert(unlike > 0);
        }
        ui_atlas_done();
        free(sprites);
        if (!overlay_shown()) overlay_toggle();
        while (overlay_back() != OVERLAY_CLOSE) {} /* the first page */
        assert(overlay_shown());
        draw();
        band = (float)py;
        right = (float)(px + C * 8 * s) - 8.0f * (float)s; /* a column in from the panel's right */
        {
            float edge = (float)s, side = 12.0f * (float)s, boxw = 8.0f * 8.0f * (float)s + side + 2 * edge;
            float boxx = right - boxw;
            for (i = 0; i < list.n; ++i) {
                const SnxUiQuad* q = &list.q[i];
                if (q->v == SNX_CONTROLS_ATLAS_HEIGHT && q->uw == SNX_UI_PICTURE && q->vh == SNX_UI_PICTURE) {
                    ++pictures;
                    assert(q->y == band + 2 * edge && q->h == side && q->w == side && q->sample == UI_BLEND);
                    assert(q->x + q->w == right - 2 * edge);
                }
                frames += q->rgba == LIGHT_RGBA && q->sample == UI_SHARP && (q->w == edge || q->h == edge) && q->x >= boxx &&
                          q->x < right && q->y >= band && q->y < band + 16.0f * (float)s;
                if (q->u == SNX_CONTROLS_FONT_X + 'P' % 16 * 8 && q->v == SNX_CONTROLS_FONT_Y + 'P' / 16 * 16 &&
                    q->y == band && q->x > boxx)
                    assert(q->x == boxx + 8.0f * (float)s); /* the nickname a column into the box */
            }
        }
        assert(pictures == 1 && frames == 4 && shows("Player"));
        list.n = 0;
        overlay_error_draw(&list, SW, SH, &e);
        for (i = 0, pictures = 0; i < list.n; ++i) pictures += list.q[i].v == SNX_CONTROLS_ATLAS_HEIGHT;
        assert(!pictures && !shows("Player"));
    }
    puts("overlay: placing, pages, taps, keys, values, scrolling, look, the error's panel, the atlas and the user "
         "passed");
    return 0;
}
