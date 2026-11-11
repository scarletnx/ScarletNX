/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's menu (runtime/platform/overlay.c): where it goes, its pages, what a tap, a move or a step does, its look;
   and ui.c's atlas and text */
#include "nx/runtime/platform/overlay.h"
#include "nx/runtime/platform/stats.h"
#include "nx/runtime/platform/prepare_screen.h"
#include "nx/runtime/platform/fatal_screen.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/gamepad.h"
#include "nx/runtime/platform/touch.h"
#include "nx/runtime/platform/vpad.h"
#include "nx/runtime/platform/hotkeys.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/renderer.h"
#include "common/utilities/font.h"
#include "gui/icons.h"
#include "gui_theme_colors.h"
#include "common/utilities/users.h"
#include "common/utilities/mii_db.h"
#include "common/utilities/mii_model.h"
#include "common/utilities/common.h"
#include "gui/splash.h"
#include "common/utilities/endian.h"
/* the menu's page colour, its letters on the head's band, as R G B A bytes */
#define LIGHT_RGBA                                                                                                     \
    (0xFFu << 24 | (SNX_GUI_THEME_YORHA_PAGE & 0xFFu) << 16 | (SNX_GUI_THEME_YORHA_PAGE & 0xFF00u) |                   \
     (SNX_GUI_THEME_YORHA_PAGE >> 16 & 0xFFu))
/* a panel in a dialog (the keyboard's line): the soft colour over the page at a quarter */
#define PANEL_RGB ui_over(SNX_GUI_THEME_YORHA_SOFT, SNX_GUI_THEME_YORHA_PAGE, 0x40u) /* the soft colour over the page */
#define PANEL_RGBA (0xFFu << 24 | (PANEL_RGB & 0xFFu) << 16 | (PANEL_RGB & 0xFF00u) | (PANEL_RGB >> 16 & 0xFFu))
/* the menu's colour of a held control, as R G B A bytes */
#define FAILURE                                                                                                        \
    (0xFFu << 24 | (SNX_GUI_THEME_YORHA_FAILURE & 0xFFu) << 16 | (SNX_GUI_THEME_YORHA_FAILURE & 0xFF00u) |             \
     (SNX_GUI_THEME_YORHA_FAILURE >> 16 & 0xFFu))
#include <SDL3/SDL_keycode.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* a quad as it holds it (ui.h's ui_view): its place in pixels, its colour widened back to 8 bits */
static SnxUiQuadView qv(const SnxUiQuad* q) { return ui_view(q); }

/* vpad.c's check of settings.txt's keys against its controls: a mismatch fails the test */
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    abort();
}
/* settings.txt's Display table, which holds the menu's Stats, names options.c's setter of the game's language */
void options_language_set(const IniOpt* o, const char* v) {
    (void)o;
    (void)v;
}
void options_tv_mode_set(const IniOpt* o, const char* v) {
    (void)o;
    (void)v;
}
/* options.c's TV mode as the menu's row sees it: the choice it shows, and the steps the row asked for. The ring is
   options.c's, which tests/options_tv_mode.c checks; here any next choice will do */
static struct {
    int choice, steps, last;
} tv = {1, 0, 0};
int options_tv_mode_choice(void) { return tv.choice; }
int options_tv_mode_step(int step, char* text, size_t size) {
    tv.steps++;
    tv.last = step;
    tv.choice = (tv.choice + 1) % 3;
    if (size) text[0] = 0;
    return tv.choice;
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
/* the names change only as the tests set them: their pointers, mixed */
u32 gamepad_names_version(void) {
    return (u32)(uintptr_t)pad_names[0] ^ (u32)(uintptr_t)pad_names[1] * 3u ^ (u32)(uintptr_t)pad_names[2] * 7u;
}
int gamepad_of_player(int slot) { return slot >= 0 && slot < 3 ? pad_of_player[slot] : -1; }
/* the renderer's RenderDoc: whether a capture can be taken, which shows the menu's Capture frame */
static int can_capture;
static int capture(int ask) {
    (void)ask;
    return can_capture;
}
static const Renderer renderer = {.capture = capture};
const Renderer* g_renderer = &renderer;
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
const IniOpt g_gamepad_players_ini[] = {{"gamepad_p1_swap_ab", "-1", NULL, INI_INT, 0, &g_settings.player_swap_a_b[0],
                                         -1, 1, NULL, "Swap A/B", "As All|Off|On", 0, 0, 0},
                                        {"gamepad_p1_rumble", "-10", NULL, INI_INT, 0, &g_settings.player_vibration[0],
                                         -10, 100, NULL, "Vibration", "As All|Off|#%", 10, 0, 0},
                                        {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, 0, 0}};
const IniOpt g_gamepad_ini[] = {
    {"gamepad_swap_ab", "0", NULL, INI_INT, 0, &g_settings.swap_a_b, 0, 1, NULL, "Swap A/B", "Off|On", 0, 0, 0},
    {"gamepad_deadzone", "20", NULL, INI_INT, 0, &g_settings.stick_dead_zone, 0, 90, NULL, "Stick dead zone", "#%", 5,
     0, 0},
    {"gamepad_rumble", "100", NULL, INI_INT, 0, &g_settings.vibration, 0, 100, NULL, "Vibration", "Off|#%", 10, 0, 0},
    {"gamepad_background", "0", NULL, INI_INT, 0, &g_settings.background_input, 0, 1, NULL, "Background input",
     "Off|On", 0, 0, 0},
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
/* ui.c's warning for a picture that does not draw: the last one */
static char warned[512];
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    vsnprintf(warned, sizeof warned, fmt, ap);
    va_end(ap);
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
/* whether text shows anywhere on the picture, its letters a cell apart on one row (spaces draw nothing), each in rgba
   unless any */
static int shows_as(const char* text, int any, u32 rgba) {
    int i, k;
    for (i = 0; i < list.n; ++i) {
        float x = qv(&list.q[i]).x, y = qv(&list.q[i]).y, step = 8.0f * (float)s;
        for (k = 0; text[k]; ++k) {
            unsigned char c = (unsigned char)text[k];
            int j, found = c == ' ';
            for (j = 0; j < list.n && !found; ++j)
                found = qv(&list.q[j]).x == x + (float)k * step && qv(&list.q[j]).y == y &&
                        list.q[j].u == SNX_UI_FONT_X + c % 16 * 8 &&
                        list.q[j].v == SNX_UI_FONT_Y + c / 16 * 16 && (any ||
                            qv(&list.q[j]).rgba == ui_rgba(rgba));
            if (!found) break;
        }
        if (!text[k]) return 1;
    }
    return 0;
}
static int shows(const char* text) { return shows_as(text, 1, 0); }
/* the quads filled with rgba */
static int quads_of(u32 rgba) {
    int i, n = 0;
    for (i = 0; i < list.n; ++i) n += qv(&list.q[i]).rgba == ui_rgba(rgba) && qv(&list.q[i]).w > 0;
    return n;
}
/* the quad of a glyph at a cell, or -1 */
static int glyph_at(int col, int row, u8 c) {
    int i;
    for (i = 0; i < list.n; ++i)
        if (qv(&list.q[i]).x == (float)(px + col * 8 * s) && qv(&list.q[i]).y == (float)(py + row * 16 * s) &&
            list.q[i].u == SNX_UI_FONT_X + c % 16 * 8 && list.q[i].v == SNX_UI_FONT_Y + c / 16 * 16)
            return i;
    return -1;
}
/* whether an icon of gui/icons.h is drawn from that cell on, two cells wide, from its texels beside the picture */
static int icon_at(int col, int row, unsigned icon) {
    int i;
    for (i = 0; i < list.n; ++i)
        if (qv(&list.q[i]).x == (float)(px + col * 8 * s) && qv(&list.q[i]).y == (float)(py + row * 16 * s) &&
            qv(&list.q[i]).w == (float)(16 * s) && qv(&list.q[i]).h == (float)(16 * s) &&
            list.q[i].u == SNX_UI_ICONS_X + 16 * (icon - GUI_ICON_FIRST) && list.q[i].v == SNX_UI_PICTURE_Y &&
            list.q[i].uw == 16 && list.q[i].vh == 16 && qv(&list.q[i]).sample == UI_SHARP)
            return 1;
    return 0;
}
/* whether a line of text is drawn from that cell on, in the font's glyphs (a space draws none) */
static int text_at(int col, int row, const char* text) {
    int k;
    for (k = 0; text[k]; ++k)
        if (text[k] != ' ' && glyph_at(col + k, row, (u8)text[k]) < 0) return 0;
    return 1;
}

/* whether an icon of gui/icons.h is drawn anywhere */
static int icon_drawn(unsigned icon) {
    int i;
    for (i = 0; i < list.n; ++i)
        if (list.q[i].u == SNX_UI_ICONS_X + 16 * (icon - GUI_ICON_FIRST) && list.q[i].v == SNX_UI_PICTURE_Y)
            return 1;
    return 0;
}
/* F1's page, from the page showing: the runtime's keys and the game's buttons, a row each with its icon and its keys
   on a band of ink in the page's colour, none in focus; Up/Down, the D-pad and the wheel go over its rows, each of
   which fits the narrowest panel; any other key, button or tap goes back to the game, F1 again too, and the other
   pages keep those keys as they were */
/* the keys page's first screen: a build with the client holds its capture's row, which takes the last one's place */
#if defined(SNX_TRACY)
enum { KEYS_TRACY_ROWS = 1 };
static const char* const KEYS_LAST = "Tracy, 60 s";
static const char* const KEYS_UNDER = "quit the game"; /* under the fold, as its icon is */
#else
enum { KEYS_TRACY_ROWS = 0 };
static const char* const KEYS_LAST = "quit the game";
static const char* const KEYS_UNDER = NULL;
#endif
static void keys_page(void) {
    const u64 x_button = 1ull << 2, down = 1ull << 15;
    const i32 still[4] = {0, 0, 0, 0};
    int was = overlay_shown(), i, n;
    u32 light, ink;
    if (!was) overlay_toggle();
    assert(overlay_keys() == OVERLAY_NONE && overlay_shown());
    draw();
    light = qv(&list.q[0]).rgba; /* the panel, and its head's band */
    ink = qv(&list.q[1]).rgba;
    assert(shows("SCARLETNX  /  KEYS") && shows("Up/Down: the rest. Any other key or button: back to the game."));
    assert(shows_as("F2, Home", 0, light) && shows_as("the game's menu", 0, ink) && shows_as("F3", 0, light) &&
           shows_as("a screenshot", 0, ink) && shows_as("F11, Alt+Return", 0, light) && shows(KEYS_LAST));
    assert(icon_drawn(GUI_ICON_APP_WINDOW) && icon_drawn(GUI_ICON_IMAGE) &&
           icon_drawn(GUI_ICON_LOG_OUT) == !KEYS_UNDER);
    for (i = 2, n = 0; i < list.n; ++i) /* the widest keys' band: a pixel of the scale past them either side */
        n += qv(&list.q[i]).rgba == ui_rgba(ink) && qv(&list.q[i]).h == (float)(16 * s) &&
             qv(&list.q[i]).w == (float)(8 * s * (int)strlen("F11, Alt+Return") + 2 * s);
    assert(n == 1);
    assert(shows("\x19") && !shows("\x18") && !shows("right stick")); /* twelve rows show, the rest under them */
    for (i = 2; i < list.n; ++i) assert(!(qv(&list.q[i]).rgba == ui_rgba(ink) &&
                                        qv(&list.q[i]).w == qv(&list.q[0]).w)); /* none in focus */
    overlay_move(1);
    draw();
    assert(!shows("the game's menu") && shows("a screenshot") && shows("\x18") && shows("\x19"));
    for (i = 0; i < 40; ++i) assert(overlay_pad(down) == OVERLAY_NONE);
    draw();
    assert(shows("right stick") && shows_as("I, J, K, L", 0, light) && icon_drawn(GUI_ICON_JOYSTICK));
    assert(shows_as("Ctrl+D", 0, light) && shows("TV on/off/auto")); /* the last runtime key's, after Esc's */
    assert(!shows("\x19") && shows("\x18") && !shows("a screenshot"));
    overlay_scroll(-100);
    draw();
    assert(shows("the game's menu") && !shows("\x18"));
    /* the narrowest panel, 40 columns: every row inside it, a column short of its edge, where the arrows go */
    for (n = 0; n < 3; ++n) {
        int nx, ny, ns;
        float right;
        list.n = 0;
        overlay_draw(&list, 400, 600, 0);
        overlay_place(400, 600, &nx, &ny, &ns);
        assert(overlay_cols(400, ns) == OVERLAY_COLS);
        right = qv(&list.q[0]).x + qv(&list.q[0]).w - 8.0f * (float)ns;
        for (i = 2; i < list.n; ++i) {
            const SnxUiQuadView q_v = qv(&list.q[i]), *q = &q_v;
            int arrow = q->u == SNX_UI_FONT_X + 0x18 % 16 * 8 || q->u == SNX_UI_FONT_X + 0x19 % 16 * 8;
            if (q->w == 8.0f * (float)ns && q->h == 16.0f * (float)ns && q->v >= SNX_UI_FONT_Y && !arrow)
                assert(q->x + q->w <= right);
        }
        overlay_move(12);
    }
    overlay_scroll(-100);
    /* with RenderDoc in the process, the capture key's row right after F12's; a debugger, its paragraph; neither by
       default */
    assert(!shows("capture a frame") && !shows("Debugger") && !shows("let the mouse go, hold it"));
    overlay_keys_mouse(1); /* a game holds the mouse: its key's row, Ctrl+G by default */
    draw();
    assert(shows_as("Ctrl+G", 0, light) && shows("let the mouse go, hold it"));
    overlay_keys_mouse(0);
    overlay_keys_extra(1, "gdb");
    draw();
    for (i = 0; i < KEYS_TRACY_ROWS; ++i) overlay_move(1); /* the capture's row takes the last one's place */
    draw();
    assert(shows_as("Ctrl+Shift+F12", 0, light) && shows("capture a frame") && !shows("Debugger"));
    for (i = 0; i < 40; ++i) overlay_move(1);
    draw();
    assert(shows("right stick") && shows_as("Debugger", 0, light) && shows("gdb")); /* the last row, past the keys */
    overlay_keys_extra(0, "");
    overlay_scroll(-100);
    draw();
    assert(!shows("capture a frame") && !shows("Debugger"));
    /* the page kept from frame to frame is laid out again for RenderDoc found alone, and for a key set elsewhere (the
       .ini read again) with no call of the menu's between */
    overlay_keys_extra(1, "");
    draw();
    assert(shows("capture a frame"));
    overlay_keys_extra(0, "");
    draw();
    assert(!shows("capture a frame"));
    assert(hotkeys_ini("hotkey_screenshot", "Ctrl+S"));
    draw();
    assert(shows_as("Ctrl+S", 0, light));
    assert(hotkeys_ini("hotkey_screenshot", "F3"));
    draw();
    assert(!shows("Ctrl+S"));
    /* the keys as set: one moved; one with no key left out; the menu's Home without its key; fullscreen's second
       alone; the capture key's row, with RenderDoc in the process, gone with its key */
    assert(hotkeys_ini("hotkey_screenshot", "Ctrl+S") && hotkeys_ini("hotkey_quit", "") &&
           hotkeys_ini("hotkey_menu", "") && hotkeys_ini("hotkey_fullscreen", "") && hotkeys_ini("hotkey_capture", ""));
    overlay_keys_extra(1, "");
    draw();
    assert(shows_as("Ctrl+S", 0, light) && shows("a screenshot") && !shows("F3") && shows_as("Home", 0, light));
    assert(!shows("F2") && shows_as("Alt+Return", 0, light) && !shows("F11") && !shows("capture a frame"));
    for (i = 0; i < 40; ++i) overlay_move(1);
    draw();
    assert(shows("right stick") && !shows("quit the game"));
    overlay_scroll(-100);
    assert(hotkeys_ini("hotkey_screenshot", "F3") && hotkeys_ini("hotkey_quit", "Escape") &&
           hotkeys_ini("hotkey_menu", "F2") && hotkeys_ini("hotkey_fullscreen", "F11") &&
           hotkeys_ini("hotkey_capture", "Ctrl+Shift+F12"));
    overlay_keys_extra(0, "");
    /* back to the game: any key, button or tap, F1 again */
    assert(overlay_select() == OVERLAY_CLOSE && overlay_back() == OVERLAY_CLOSE && overlay_side(-1) == OVERLAY_CLOSE);
    assert(overlay_other() == OVERLAY_CLOSE && overlay_pad(x_button) == OVERLAY_CLOSE &&
           overlay_keys() == OVERLAY_CLOSE);
    draw();
    assert(overlay_tap(5.0f, 5.0f, SW, SH) == OVERLAY_CLOSE &&
           overlay_tap(qv(&list.q[0]).x + qv(&list.q[0]).w / 2, qv(&list.q[0]).y + qv(&list.q[0]).h / 2, SW,
               SH) == OVERLAY_CLOSE &&
           overlay_tap(qv(&list.q[1]).x + qv(&list.q[1]).w / 2, qv(&list.q[1]).y + qv(&list.q[1]).h / 2, SW,
               SH) == OVERLAY_CLOSE);
    overlay_controls(0, still, 1);
    assert(overlay_controls(x_button, still, 2) == OVERLAY_CLOSE);
    overlay_controls(0, still, 3);
    /* the first page, as the menu opens: X, Left and a key of no use there do nothing */
    overlay_toggle();
    overlay_toggle();
    assert(overlay_other() == OVERLAY_NONE && overlay_pad(x_button) == OVERLAY_NONE && overlay_side(1) == OVERLAY_NONE);
    draw();
    assert(shows("SCARLETNX  /  MENU"));
    if (!was) overlay_toggle();
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
    assert(qv(&many[0]).x == 0 && qv(&many[0]).w == 3840 && qv(&many[0]).h == 1080); /* the page, the whole window */
    for (i = 1; i < l.n; ++i) {
        if (qv(&many[i]).y == 32.0f * 32 && qv(&many[i]).x == 40 + 16) at_foot = 1; /* the foot's F, at its row */
        if (qv(&many[i]).h == 32 && qv(&many[i]).x < 40) in_cutout = 1;             /* a letter left of the safe part */
        if (qv(&many[i]).y == 32.0f * 6) on_note = 1;                          /* the first line's row,
            under three steps */
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
    for (i = 0; i < l.n; ++i) on_note += qv(&many[i]).y == 32.0f * 5 && qv(&many[i]).h == 32;
    assert(on_note == 96);
    prepare_screen_set(NULL, NULL, NULL, 0, NULL, 0, NULL, 0);
    assert(!prepare_screen_shown());
}
/* the height of the letters the screen before the game draws on a picture w x h with the theme's text_size (the
   launcher's UI scale, 0 for its Auto), for a display at display_percent: its glyphs are 8 by 16 pixels times it */
static float prepare_letters(int text_size, int w, int h, int display_percent) {
    static SnxUiQuad q[4096];
    SnxUiList l = {q, 0, 4096};
    SnxPrepareStep st[1] = {{0}};
    SnxRunTheme t = {0};
    float letters = 0;
    t.ink = 0xFFFFFFu;
    t.text_size = text_size;
    st[0].label = "PREPARING SHADERS";
    st[0].value = 1;
    st[0].total = 2;
    prepare_screen_set(&t, "GAME", st, 1, NULL, 0, "FOOT", 1000);
    prepare_screen_draw(&l, w, h, NULL, display_percent);
    for (int i = 0; i < l.n; ++i)
        if (qv(&q[i]).h >= 16 && qv(&q[i]).w * 2 == qv(&q[i]).h) { /* a glyph */
            assert(!letters || qv(&q[i]).h == letters);  /* one size for all of them */
            letters = qv(&q[i]).h;
        }
    prepare_screen_set(NULL, NULL, NULL, 0, NULL, 0, NULL, 0);
    return letters;
}
/* Its letters are the launcher's: its UI scale, else twice the display's scale, smaller only while 16 rows don't fit
   the height; a 1280x720 window at 100% shows them 2x as the launcher does, not 1x */
static void prepare_screen_size(void) {
    assert(prepare_letters(0, 1280, 720, 100) == 32);  /* Auto at 100%: 2x, 22 rows */
    assert(prepare_letters(0, 1920, 1080, 150) == 48); /* Auto at 150%: 3x */
    assert(prepare_letters(1, 1920, 1080, 100) == 16); /* the launcher's 1x */
    assert(prepare_letters(3, 1920, 1080, 100) == 48);
    assert(prepare_letters(4, 1920, 1080, 100) == 64); /* 16 rows of 64 fit 1080 */
    assert(prepare_letters(4, 1280, 720, 100) == 32);  /* 4x and 3x leave under 16 rows of 720: 2x */
    assert(prepare_letters(2, 640, 200, 100) == 16);   /* never under 1x */
}
/* The steps the screen shows for the renderer's progress (prepare_screen_steps): none before there is work; the files
   looked in for the game's shaders counted, whatever the theme's progress says, and the one looked in now named until
   that step is done; each kind's start kept from the first time it was seen at work */
static void prepare_screen_steps_from(void) {
    RendererPrepare p = {0};
    SnxPrepareStep st[SNX_PREPARE_STEPS];
    u64 began[SNX_PREPARE_KINDS] = {0};
    assert(prepare_screen_steps(&p, st, began, 500) == 0 && began[SNX_PREPARE_FIND] == 0);
    p.files = 51234;
    p.files_seen = 1234;
    p.scanning = 1;
    snprintf(p.file, sizeof p.file, "%s", "effect.bnsh");
    assert(prepare_screen_steps(&p, st, began, 1000) == 1);
    assert(!strcmp(st[0].label, "FINDING SHADERS") && st[0].value == 1234 && st[0].total == 51234 && !st[0].done);
    assert(!strcmp(st[0].detail, "1234 of 51234 files, effect.bnsh") && st[0].started_ms == 1000);
    p.files_seen = 1235; /* between two files */
    p.file[0] = 0;
    assert(prepare_screen_steps(&p, st, began, 2000) == 1);
    assert(!strcmp(st[0].detail, "1235 of 51234 files") && st[0].started_ms == 1000);
    /* every file counted: the step done, a name left behind not shown; the translation's */
    p.files_seen = 51234;
    p.programs = 300;
    p.programs_done = 12;
    p.shipped = 5;
    snprintf(p.file, sizeof p.file, "%s", "last.bin");
    assert(prepare_screen_steps(&p, st, began, 3000) == 2);
    assert(st[0].done && !strcmp(st[0].detail, "51234 of 51234 files") && st[0].started_ms == 1000);
    assert(!strcmp(st[1].label, "PREPARING SHADERS") && st[1].value == 12 && st[1].total == 300 && !st[1].done);
    assert(!strcmp(st[1].detail, "(5 new)") && st[1].started_ms == 3000);
    /* one file, an executable's segment; the pipelines of earlier runs, made */
    p = (RendererPrepare){0};
    p.files = p.files_seen = 1;
    p.scanning = 1;
    snprintf(p.file, sizeof p.file, "%s", "main.ro");
    p.pipes = p.pipes_made = 10;
    memset(began, 0, sizeof began);
    assert(prepare_screen_steps(&p, st, began, 4000) == 2 && !strcmp(st[0].detail, "1 of 1 file, main.ro"));
    assert(!strcmp(st[1].label, "LOADING SHADER CACHE") && !strcmp(st[1].detail, "from earlier sessions"));
    assert(st[1].done && st[1].value == 10 && st[1].total == 10);
    /* the longest counts and name fit whole */
    p.files = p.files_seen = 0xFFFFFFFFu;
    memset(p.file, 'n', sizeof p.file - 1);
    assert(prepare_screen_steps(&p, st, began, 5000) == 2);
    assert(strlen(st[0].detail) == strlen("4294967295 of 4294967295 files, ") + sizeof p.file - 1);
    assert(st[0].detail[strlen(st[0].detail) - 1] == 'n');
}
/* the text the screen before the game draws on its row at y, a glyph a column of 8 * s pixels from the left (a space
   where none is), up to its last glyph */
static void prepare_row(const SnxUiList* l, float y, int s, char* out, int size) {
    int i, end = 0;
    memset(out, ' ', (size_t)size);
    for (i = 0; i < l->n; ++i) {
        const SnxUiQuadView q_v = qv(&l->q[i]), *q = &q_v;
        int col = (int)(q->x / (8.0f * (float)s));
        if (q->y != y || q->uw != 8 || q->vh != 16 || q->u < SNX_UI_FONT_X || q->v < SNX_UI_FONT_Y ||
            col >= size - 1)
            continue;
        out[col] = (char)((q->u - SNX_UI_FONT_X) / 8 + (q->v - SNX_UI_FONT_Y) / 16 * 16);
        if (col >= end) end = col + 1;
    }
    out[end] = 0;
}
/* A step's row on a window 80 columns wide (1280 pixels at 2x): a detail too long for it (a file's name) cut before
   its progress, which stays on the row, in a theme of steps and in a console's; a done one's as it was */
static void prepare_screen_cut(void) {
    static SnxUiQuad q[4096];
    RendererPrepare p = {0};
    SnxUiList l = {q, 0, 4096};
    SnxPrepareStep st[SNX_PREPARE_STEPS];
    u64 began[SNX_PREPARE_KINDS] = {0};
    SnxRunTheme t = {0};
    char row[128];
    int n;
    t.ink = 0xFFFFFFu;
    t.steps = 1;
    t.progress = SNX_RUN_PROGRESS_PERCENT;
    p.files = 51234;
    p.files_seen = 14858; /* 29% */
    p.scanning = 1;
    snprintf(p.file, sizeof p.file, "%s", "a_name_long_enough_to_run_past_the_row_of_a_narrow_window.bnsh");
    n = prepare_screen_steps(&p, st, began, 1000);
    prepare_screen_set(&t, "GAME", st, n, NULL, 0, "FOOT", 1200); /* 1200 ms: three dots */
    prepare_screen_draw(&l, 1280, 720, NULL, 100);
    prepare_row(&l, 32.0f * 3, 2, row, sizeof row); /* the first step's row, under the head */
    assert(!strcmp(row, " FINDING SHADERS: 14858 OF 51234 FILES, A_NAME_LONG_ENOUGH_TO_RUN_PAS... 29%..."));
    /* a console's line: the label, the detail cut, then the progress as the theme says it */
    t.steps = 0;
    t.progress = SNX_RUN_PROGRESS_AMOUNT;
    prepare_screen_set(&t, "GAME", st, n, NULL, 0, "FOOT", 1200);
    l.n = 0;
    prepare_screen_draw(&l, 1280, 720, NULL, 100);
    prepare_row(&l, 32.0f * 3, 2, row, sizeof row);
    assert(!strcmp(row, " FINDING SHADERS 14858 of 51234 files, a_name_long_enough_to_...: 14858 / 51234"));
    /* every file counted, the shaders being translated: the done row whole, the next one's progress */
    p.files_seen = 51234;
    p.programs = 300;
    t.steps = 1;
    t.progress = SNX_RUN_PROGRESS_PERCENT;
    n = prepare_screen_steps(&p, st, began, 2000);
    prepare_screen_set(&t, "GAME", st, n, NULL, 0, "FOOT", 1200);
    l.n = 0;
    prepare_screen_draw(&l, 1280, 720, NULL, 100);
    prepare_row(&l, 32.0f * 3, 2, row, sizeof row);
    assert(!strcmp(row, " FINDING SHADERS: 51234 OF 51234 FILES ...OK"));
    prepare_row(&l, 32.0f * 4, 2, row, sizeof row);
    assert(!strcmp(row, " PREPARING SHADERS: (0 NEW) 0%..."));
    prepare_screen_set(NULL, NULL, NULL, 0, NULL, 0, NULL, 0);
}
/* a tap on a row of the dialog drawn last, list.q[0] its page: rows from its top, items counted from its foot */
static int tap_row(int row) {
    return overlay_tap(qv(&list.q[0]).x + 10.0f, qv(&list.q[0]).y + (float)(row * 16 * s) + 5.0f, SW, SH);
}
static int rows_drawn(void) { return (int)(qv(&list.q[0]).h / (float)(16 * s)); }
/* The software keyboard's box: over the game, not under the menu; the game's texts; what typing, Backspace, Enter short
   of the least and Enter, Esc, a pad's A and B, and taps do; the most a line holds, in UTF-16 units; the answer, taken
   once; a password; a long line's end */
static void keyboard_box(void) {
    OverlayKeyboard k = {0};
    char out[OVERLAY_KB_TEXT];
    const i32 still[4] = {0};
    OverlayTyping typing;
    u32 ticket = 0;
    int i, xs;
    if (overlay_shown()) overlay_toggle();
    assert(!overlay_keyboard_shown() && overlay_keyboard_take(&ticket, out, sizeof out) == -1);
    snprintf(k.text, sizeof k.text, "Ann");
    snprintf(k.header, sizeof k.header, "Your rival's name");
    snprintf(k.sub, sizeof k.sub, "Up to six letters");
    snprintf(k.guide, sizeof k.guide, "Pick a name");
    snprintf(k.ok, sizeof k.ok, "Done");
    k.min = 4;
    k.max = 6;
    k.ticket = 7;
    overlay_keyboard_show(&k);
    assert(overlay_keyboard_shown());
    draw();
    assert(shows("SCARLETNX  /  KEYBOARD") && shows("Your rival's name") && shows("Up to six letters") &&
           shows("Pick a name"));
    assert(shows("Ann\xDD") && shows("3 of 6 characters, 4 at least.") && shows("Enter or A: Done. Esc or B: Cancel."));
    assert(shows("\xFE Done") && shows("\xFE Cancel") && quads_of(PANEL_RGBA) == 1); /* the line on its panel */
    /* Enter short of the least: still there, saying how many more in the failure colour, until the line changes */
    assert(overlay_keyboard_key(OVERLAY_KEY_ENTER) == OVERLAY_NONE && overlay_keyboard_shown());
    draw();
    assert(shows_as("1 more character needed, 4 at least.", 0, FAILURE) && !shows("3 of 6"));
    assert(overlay_keyboard_take(&ticket, out, sizeof out) == -1);
    /* typed: the controls left out, up to the most */
    overlay_keyboard_type("i\te!x");
    draw();
    assert(shows("Annie!\xDD") && shows("6 of 6 characters, 4 at least.") && !shows("more character"));
    /* Backspace a character at a time; one past U+FFFF is two units, and doesn't fit in one */
    assert(overlay_keyboard_key(OVERLAY_KEY_BACKSPACE) == OVERLAY_NONE);
    overlay_keyboard_key(OVERLAY_KEY_BACKSPACE);
    overlay_keyboard_type("\xC3\xA9\xF0\x9F\x98\x80");
    draw();
    assert(shows("Anni\x82\xDD") && shows("5 of 6 characters"));
    overlay_keyboard_key(OVERLAY_KEY_BACKSPACE);
    overlay_keyboard_type("\xF0\x9F\x98\x80");
    draw();
    assert(shows("Anni?\xDD") && shows("6 of 6 characters"));
    overlay_keyboard_key(OVERLAY_KEY_BACKSPACE);
    draw();
    assert(shows("Anni\xDD") && shows("4 of 6 characters"));
    /* Enter gives the line: the box goes, and its answer is taken once */
    assert(overlay_keyboard_key(OVERLAY_KEY_ENTER) == OVERLAY_KEYBOARD && !overlay_keyboard_shown());
    draw();
    assert(!shows("KEYBOARD"));
    assert(overlay_keyboard_take(&ticket, out, sizeof out) == 0 && ticket == 7 && !strcmp(out, "Anni"));
    assert(overlay_keyboard_take(&ticket, out, sizeof out) == -1);
    /* nothing is typed into a box that doesn't show */
    overlay_keyboard_type("zz");
    assert(overlay_keyboard_key(OVERLAY_KEY_ENTER) == OVERLAY_NONE && overlay_keyboard_take(&ticket, out, 8) == -1);
    /* Esc cancels */
    k.ticket = 8;
    overlay_keyboard_show(&k);
    assert(overlay_keyboard_key(OVERLAY_KEY_CANCEL) == OVERLAY_KEYBOARD);
    assert(overlay_keyboard_take(&ticket, out, sizeof out) == 1 && ticket == 8);
    /* a pad's A gives the line, B cancels; A held as the box comes isn't a press */
    overlay_controls(1, still, 1000);
    snprintf(k.text, sizeof k.text, "Anna");
    k.ticket = 9;
    overlay_keyboard_show(&k);
    assert(overlay_controls(1, still, 1016) == OVERLAY_NONE && overlay_keyboard_shown());
    assert(overlay_controls(0, still, 1032) == OVERLAY_NONE);
    assert(overlay_controls(1, still, 1048) == OVERLAY_KEYBOARD && !overlay_keyboard_shown());
    assert(overlay_keyboard_take(&ticket, out, sizeof out) == 0 && ticket == 9 && !strcmp(out, "Anna"));
    k.ticket = 10;
    overlay_keyboard_show(&k);
    overlay_controls(0, still, 1064);
    assert(overlay_controls(1ull << 13, still, 1080) == OVERLAY_NONE); /* up: nothing */
    assert(overlay_controls(2, still, 1096) == OVERLAY_KEYBOARD && overlay_keyboard_take(&ticket, out, 8) == 1);
    /* taps: the line asks for the system's keyboard, OK does as Enter (short of the least: nothing yet), Cancel as
       Esc, the rest nothing */
    snprintf(k.text, sizeof k.text, "Bo");
    k.ticket = 11;
    overlay_keyboard_show(&k);
    assert(overlay_keyboard_typing(SW, SH, &typing) && typing.ticket == 11 && !typing.password && !typing.numbers);
    assert(typing.line.w > 0 && typing.line.h == (float)(16 * s));
    assert(overlay_tap(typing.line.x + 4, typing.line.y + 4, SW, SH) == OVERLAY_TYPE);
    assert(overlay_tap(5.0f, 5.0f, SW, SH) == OVERLAY_NONE && overlay_keyboard_shown());
    draw();
    assert(rows_drawn() > 3 && tap_row(rows_drawn() - 3) == OVERLAY_NONE && overlay_keyboard_shown());
    overlay_keyboard_type("bby");
    assert(tap_row(rows_drawn() - 3) == OVERLAY_KEYBOARD);
    assert(overlay_keyboard_take(&ticket, out, sizeof out) == 0 && !strcmp(out, "Bobby"));
    k.ticket = 12;
    overlay_keyboard_show(&k);
    draw();
    assert(tap_row(rows_drawn() - 2) == OVERLAY_KEYBOARD && overlay_keyboard_take(&ticket, out, 8) == 1);
    /* the menu goes over the box, and takes the pad: the box comes back as it was */
    k.ticket = 13;
    overlay_keyboard_show(&k);
    overlay_toggle();
    draw();
    assert(shows("SCARLETNX  /  MENU") && !shows("SCARLETNX  /  KEYBOARD"));
    overlay_controls(0, still, 1112);
    assert(overlay_controls(2, still, 1128) == OVERLAY_CLOSE && overlay_keyboard_shown());
    overlay_toggle();
    draw();
    assert(shows("SCARLETNX  /  KEYBOARD") && shows("Bo\xDD"));
    /* the game takes its box away: another's ticket leaves it, its own hides it unanswered */
    overlay_keyboard_hide(99);
    assert(overlay_keyboard_shown());
    overlay_keyboard_hide(13);
    assert(!overlay_keyboard_shown() && overlay_keyboard_take(&ticket, out, sizeof out) == -1);
    /* a password's characters are asterisks, and the system's keyboard is told */
    snprintf(k.text, sizeof k.text, "abc");
    k.password = 1;
    overlay_keyboard_show(&k);
    draw();
    assert(shows("***\xDD") && !shows("abc") && overlay_keyboard_typing(SW, SH, &typing) && typing.password);
    overlay_keyboard_hide(13);
    /* no most: OVERLAY_KB_UNITS; a line past the box's four rows shows its end (70 columns a row here: 400 x, END and
       the cursor take six, and the last four hold 260 of the x) */
    k.password = 0;
    k.min = 0;
    k.max = 0;
    k.sub[0] = 0; /* its "six" would be one more x */
    memset(k.text, 'x', 400);
    memcpy(k.text + 400, "END", 4);
    overlay_keyboard_show(&k);
    draw();
    for (i = 0, xs = 0; i < list.n; ++i)
        xs += list.q[i].u == SNX_UI_FONT_X + 'x' % 16 * 8 && list.q[i].v == SNX_UI_FONT_Y + 'x' / 16 * 16;
    assert(shows("xEND\xDD") && shows("403 of 1024 characters.") && xs == 260);
    overlay_keyboard_hide(13);
}
/* the band behind the item in focus: the widest fill of the ink that is not the head's (the first quads are the page
   and the head), or NULL */
static const SnxUiQuad* focus_band(u32 ink) {
    const SnxUiQuad* band = NULL;
    int i;
    for (i = 2; i < list.n; ++i)
        if (qv(&list.q[i]).ink == UI_SOLID && qv(&list.q[i]).rgba == ui_rgba(ink) &&
            (!band || qv(&list.q[i]).w > qv(band).w))
            band = &list.q[i];
    return band;
}
/* the markers of the items showing (the font's square, a quad each) */
static int items_shown(void) {
    int i, n = 0;
    for (i = 0; i < list.n; ++i) n += qv(&list.q[i]).ink == UI_GLYPH && qv(&list.q[i]).glyph == 0xFE;
    return n;
}
/* A screen of fingers (overlay_set_fingers, a phone's 2424 by 1080 at a display scale of 2.6, the menu's letters at
   3): an item's row is 48 pixels at the display's scale, 125, where its letters' is 48, the letters in its middle;
   a finger anywhere in a row takes that row's item; and a page shows the items the picture has room for, seven,
   scrolling to the one in focus. Off, the rows are the letters' again */
static void finger_rows(void) {
    const u32 ink = 0xFF3A4145u;
    const SnxUiQuad* band;
    float first, x;
    int stats = g_settings.stats, i, marked = 0;
    if (overlay_shown()) overlay_toggle();
    overlay_set_display_scale(2.6f);
    overlay_toggle();
    draw();
    assert(s == 3 && (band = focus_band(ink)) && qv(band).h == 48 && items_shown() == 7);
    overlay_set_fingers(1);
    draw();
    assert(s == 3 && (band = focus_band(ink)) && qv(band).h == 125 && items_shown() == 7);
    first = qv(band).y;
    x = qv(band).x;
    assert(qv(&list.q[0]).h == 4 * 48 + 7 * 125); /* the page: the head, the hint, a blank row, the items, a blank */
    assert(qv(&list.q[0]).y + qv(&list.q[0]).h <= SH);
    for (i = 0; i < list.n; ++i) /* each marker 38.5 pixels down its row, a row of 125 apart */
        if (qv(&list.q[i]).ink == UI_GLYPH && qv(&list.q[i]).glyph == 0xFE) {
            assert(qv(&list.q[i]).y == first + 125 * (float)marked + (125 - 48) / 2.0f && qv(&list.q[i]).h == 48);
            ++marked;
        }
    assert(marked == 7);
    /* Stats is the sixth item: a finger near its row's foot, far under its letters, switches it */
    assert(overlay_tap(x + 40, first + 5 * 125 + 118, SW, SH) == OVERLAY_NONE && g_settings.stats == !stats);
    assert(overlay_tap(x + 40, first + 5 * 125 + 3, SW, SH) == OVERLAY_NONE && g_settings.stats == stats);
    /* the panel covers most of the picture now, an on-screen control under some of its rows (the D-pad, as a phone
       lays them out): a finger on it there is the menu's, not the control's, and its lift takes the row it is on */
    {
        const int area[4] = {0, 0, SW, SH};
        const SnxVpadConfig* v = vpad_config();
        const SnxOverlayRect panel = {qv(&list.q[0]).x, qv(&list.q[0]).y, qv(&list.q[0]).w, qv(&list.q[0]).h};
        float dx = v->at[SNX_VPAD_DPAD].x * SW, dy = v->at[SNX_VPAD_DPAD].y * SH;
        int row = (int)((dy - first) / 125);
        u64 b;
        i32 st[4];
        assert(vpad_ini("controls", "1") && overlay_error_hit(&panel, dx, dy) && row > 0 && row < 7);
        overlay_move(-5); /* to the first item, away from the row */
        overlay_pointer_down(11, dx, dy, area);
        vpad_state(&b, st);
        assert(!b);
        assert(overlay_pointer_up(11, dx, dy, SW, SH) != OVERLAY_CLOSE);
        draw();
        assert((band = focus_band(ink)) && qv(band).y == first + 125 * (float)row);
        if (row == 5) assert(g_settings.stats == !stats); /* Stats, switched */
        g_settings.stats = stats;
        assert(vpad_ini("controls", "0"));
    }
    /* On-screen controls' page has eleven items: seven show, and Back, the last, comes into view when in focus */
    assert(overlay_tap(x + 40, first + 60, SW, SH) == OVERLAY_NONE);
    draw();
    assert(items_shown() == 7 && !shows("\xFE Back"));
    /* a finger that slips less than a finger's slip (8 pixels at the scale, 20.8) taps the row it went down on, though
       it lifts on the next: Snap to grid switches, Scale under it keeps its value. One dragged up four rows scrolls the
       page four items on, Back into view, and acts on none; dragged down again, the page comes back */
    {
        const int area[4] = {0, 0, SW, SH};
        int snap = g_settings.snap_to_grid, scale = g_settings.controls_scale;
        float y0 = first + 2 * 125 + 120, y1 = first + 6 * 125 + 60;
        overlay_pointer_down(12, x + 40, y0, area);
        overlay_pointer_move(12, x + 40, y0 + 12, area);
        assert(overlay_pointer_up(12, x + 40, y0 + 12, SW, SH) == OVERLAY_NONE);
        draw();
        assert(g_settings.snap_to_grid == !snap && g_settings.controls_scale == scale);
        g_settings.snap_to_grid = snap;
        overlay_pointer_down(13, x + 40, y1, area);
        overlay_pointer_move(13, x + 40, y1 - 2 * 125, area);
        overlay_pointer_move(13, x + 40, y1 - 4 * 125 - 10, area);
        assert(overlay_pointer_up(13, x + 40, y1 - 4 * 125 - 10, SW, SH) == OVERLAY_NONE);
        draw();
        assert(shows("SCARLETNX  /  CONTROLS") && items_shown() == 7 && shows("\xFE Back"));
        assert(g_settings.snap_to_grid == snap && g_settings.controls_scale == scale);
        overlay_pointer_down(14, x + 40, first + 60, area);
        overlay_pointer_move(14, x + 40, first + 60 + 4 * 125 + 10, area);
        assert(overlay_pointer_up(14, x + 40, first + 60 + 4 * 125 + 10, SW, SH) == OVERLAY_NONE);
        draw();
        assert(items_shown() == 7 && !shows("\xFE Back") && shows("\xFE Show controls"));
        assert((band = focus_band(ink)) && qv(band).y == first + 4 * 125); /* the focus kept on the page */
        overlay_move(-4);                                                  /* back to the first item */
    }
    overlay_move(-1);
    draw();
    assert(items_shown() == 7 && shows("\xFE Back") && (band = focus_band(ink)) && qv(band).h == 125);
    assert(qv(band).y + qv(band).h <= SH && qv(&list.q[0]).y >= 0);
    overlay_set_fingers(0);
    overlay_move(1); /* round to the first: the page from its top */
    draw();
    assert(items_shown() == 11 && (band = focus_band(ink)) && qv(band).h == 48);
    overlay_toggle();
    overlay_set_display_scale(1.0f);
    draw();
    assert(!overlay_shown() && s == 2);
}

int main(void) {
    const u32 ink = 0xFF3A4145u;
    int x, y, sc;
    vpad_set_store(store);
    snx_pad_map_unset(&pads.own); /* as gamepad.c starts it: every control the button in its place */
    snx_pad_map_unset(&players[0].own);
    prepare_screen();
    prepare_screen_size();
    prepare_screen_steps_from();
    prepare_screen_cut();
    assert(!overlay_shown());
    overlay_toggle();
    assert(overlay_shown());
    /* letters half the screen's height at most, in whole steps, in the middle: a phone held wide, a small window. The
       page is as tall as its items (seven here, under three rows of head, over a blank one), at the scale of the
       tallest page, and as wide as three quarters of the picture hold, between 40 and 72 columns */
    overlay_place(SW, SH, &px, &py, &s);
    C = overlay_cols(SW, s);
    assert(s == 2 && C == OVERLAY_COLS_MAX && px == (SW - C * 16) / 2 && py == (SH - 11 * 32) / 2);
    assert(overlay_cols(1280, 2) == 60 && overlay_cols(500, 1) == 46 && overlay_cols(400, 1) == OVERLAY_COLS &&
           overlay_cols(3840, 4) == OVERLAY_COLS_MAX);
    overlay_place(500, 168, &x, &y, &sc);
    assert(sc == 1 && x == (500 - 46 * 8) / 2 && y == (168 - 11 * 16) / 2); /* taller than the picture */
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
    assert(qv(&list.q[0]).w == (float)(C * 16) && qv(&list.q[0]).h == 11 * 32 &&
           qv(&list.q[1]).rgba == ui_rgba(ink) && qv(&list.q[1]).h == 32);
    assert(qv(&list.q[0]).rgba >> 24 == 0xFFu);
    assert(text_at(C - 2, 3, ">") && text_at(C - 2, 4, ">") && text_at(C - 2, 6, ">")); /* a page under each */
    /* each item its marker, its icon in two cells and one, its name */
    assert(text_at(1, 0, "SCARLETNX  /  MENU") && text_at(1, 3, "\xFE") && icon_at(3, 3, GUI_ICON_POINTER) &&
           text_at(6, 3, "On-screen controls") && icon_at(3, 4, GUI_ICON_GAMEPAD_2) && text_at(6, 4, "Controllers") &&
           icon_at(3, 5, GUI_ICON_SAVE) && text_at(6, 5, "Save states") && icon_at(3, 6, GUI_ICON_KEYBOARD) &&
           text_at(6, 6, "Shortcuts") && icon_at(3, 7, GUI_ICON_MONITOR) && text_at(6, 7, "TV mode") &&
           icon_at(3, 8, GUI_ICON_ACTIVITY) && text_at(6, 8, "Stats") && icon_at(3, 9, GUI_ICON_LOG_OUT) &&
           text_at(1, 9, "\xFE") && text_at(6, 9, "Exit"));
    assert(!icon_at(3, 3, GUI_ICON_SAVE) && !text_at(1, 3, "\xFE On-screen controls"));
    /* Stats: a tap turns it on and keeps it in the game's .ini, as F12 does, and again off */
    assert(text_at(C - 4, 8, "Off") && !g_settings.stats);
    assert(tap(5, 3) == OVERLAY_NONE && g_settings.stats && !strcmp(stored, "stats"));
    draw();
    assert(text_at(C - 3, 8, "On"));
    assert(tap(5, 3) == OVERLAY_NONE && !g_settings.stats);
    /* TV mode: the choice in effect in docked's words, no page under it. A tap or a select steps it on and Left or
       Right either way, each press once: a direction held steps it no further, as each step docks or undocks the
       console under the game. Nothing goes to the .ini, the last key stored still Stats' */
    assert(text_at(C - 3, 7, "On") && !text_at(C - 2, 7, ">"));
    assert(tap(4, 3) == OVERLAY_NONE && tv.steps == 1 && tv.last == 1);
    draw();
    assert(text_at(C - 5, 7, "Auto"));
    assert(overlay_side(-1) == OVERLAY_NONE && tv.steps == 2 && tv.last == -1);
    assert(overlay_side_held(-1) == OVERLAY_NONE && overlay_side_held(1) == OVERLAY_NONE && tv.steps == 2);
    assert(overlay_select() == OVERLAY_NONE && tv.steps == 3 && tv.last == 1);
    draw();
    assert(text_at(C - 3, 7, "On") && !strcmp(stored, "stats"));
    /* the page kept from frame to frame, laid out again for what changes elsewhere with no call of the menu's between:
       a setting a row shows (F12's Stats), the TV mode (its key), RenderDoc's capture */
    g_settings.stats = 1;
    draw();
    assert(text_at(C - 3, 8, "On"));
    g_settings.stats = 0;
    draw();
    assert(text_at(C - 4, 8, "Off"));
    tv.choice = 2;
    draw();
    assert(text_at(C - 5, 7, "Auto"));
    tv.choice = 1;
    can_capture = 1;
    draw();
    assert(text_at(6, 9, "Capture frame") && text_at(C - 3, 7, "On"));
    can_capture = 0;
    draw();
    assert(text_at(6, 9, "Exit"));
    /* a tap on Exit exits; on the title, nothing; outside the panel, it closes */
    assert(tap(6, 3) == OVERLAY_EXIT && tap(-3, 3) == OVERLAY_NONE);
    assert(overlay_tap(5.0f, 5.0f, SW, SH) == OVERLAY_CLOSE);
    /* the focus goes round, from Exit to the first item and back over the top; Back from here closes */
    overlay_move(1);
    overlay_move(-8);
    assert(overlay_select() == OVERLAY_EXIT && overlay_back() == OVERLAY_CLOSE);
    /* RenderDoc in the game: Capture frame between Stats and Exit, with the stats box's RenderDoc icon and no page
       under it, the panel a row taller; a tap is the caller's capture, Exit a row down */
    can_capture = 1;
    draw();
    assert(qv(&list.q[0]).h == 12 * 32 && icon_at(3, 9, GUI_ICON_IMAGE) && text_at(6, 9, "Capture frame") &&
           !text_at(C - 2, 9, ">") && text_at(6, 10, "Exit") && icon_at(3, 10, GUI_ICON_LOG_OUT));
    assert(tap(7, 3) == OVERLAY_EXIT && tap(6, 3) == OVERLAY_CAPTURE && overlay_select() == OVERLAY_CAPTURE);
    overlay_move(1);
    assert(overlay_select() == OVERLAY_EXIT);
    overlay_move(-1);
    /* without it, none: Exit where it was */
    can_capture = 0;
    draw();
    assert(qv(&list.q[0]).h == 11 * 32 && text_at(6, 9, "Exit") && !shows("Capture frame"));
    assert(overlay_select() == OVERLAY_EXIT);

    /* the Shortcuts page: a row a runtime key, its key at the right. Enter (a select) waits for a key, which with its
       modifiers becomes the row's key, saved in the .ini; a modifier alone waits on; Esc cancels; a key another row
       has is refused, said in the failure colour with the key the row keeps; Backspace puts the default back, as a
       key given */
    log_[0] = 0;
    assert(tap(3, 3) == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  SHORTCUTS") && text_at(1, 3, "\xFE Help") && text_at(C - 3, 3, "F1"));
    assert(text_at(1, 5, "\xFE Screenshot") && text_at(C - 3, 5, "F3") && qv(&list.q[0]).h == 16 * 32);
    assert(shows("Enter, then a key: its new key. Backspace: its default."));
    overlay_move(2);
    assert(overlay_select() == OVERLAY_NONE && overlay_shortcut_listening());
    draw();
    assert(text_at(C - 12, 5, "Press a key") && shows("Press the key for Screenshot. Esc or B cancels."));
    overlay_shortcut_key(SDLK_LCTRL, HOTKEY_CTRL);
    assert(overlay_shortcut_listening() && !log_[0]);
    overlay_shortcut_key(SDLK_S, HOTKEY_CTRL);
    assert(!overlay_shortcut_listening() && !strcmp(hotkey_text(HOTKEY_SCREENSHOT), "Ctrl+S"));
    assert(!strcmp(log_, "hotkey_screenshot=Ctrl+S;"));
    {
        int also;
        assert(hotkey_find(SDLK_S, HOTKEY_CTRL, &also) == HOTKEY_SCREENSHOT && hotkey_find(SDLK_F3, 0, &also) == -1);
    }
    draw();
    assert(text_at(C - 7, 5, "Ctrl+S") && shows("Enter, then a key"));
    /* Esc, and Android's Back: nothing set, nothing saved, nothing refused (Escape is Quit's) */
    log_[0] = 0;
    overlay_select();
    overlay_shortcut_key(SDLK_ESCAPE, 0);
    assert(!overlay_shortcut_listening() && !strcmp(hotkey_text(HOTKEY_SCREENSHOT), "Ctrl+S") && !log_[0]);
    draw();
    assert(!shows("already does"));
    overlay_select();
    overlay_shortcut_key(SDLK_AC_BACK, 0);
    assert(!overlay_shortcut_listening() && !strcmp(hotkey_text(HOTKEY_SCREENSHOT), "Ctrl+S") && !log_[0]);
    /* the stats' key: refused, said */
    overlay_select();
    overlay_shortcut_key(SDLK_F12, 0);
    assert(!overlay_shortcut_listening() && !strcmp(hotkey_text(HOTKEY_SCREENSHOT), "Ctrl+S") && !log_[0]);
    draw();
    /* (the line under the hint moves the rows a row down: the row in focus by its colour) */
    assert(shows_as("F12 already does Stats: Screenshot keeps Ctrl+S.", 0, FAILURE) &&
           shows_as("Ctrl+S", 0, LIGHT_RGBA));
    /* Help on F3, Screenshot's default: Backspace on Screenshot is refused as that key would be */
    overlay_move(-2);
    overlay_select();
    overlay_shortcut_key(SDLK_F3, 0);
    assert(!strcmp(hotkey_text(HOTKEY_HELP), "F3") && !strcmp(log_, "hotkey_help=F3;"));
    overlay_move(2);
    assert(overlay_backspace() == OVERLAY_NONE && !strcmp(hotkey_text(HOTKEY_SCREENSHOT), "Ctrl+S"));
    draw();
    assert(shows("F3 already does Help: Screenshot keeps Ctrl+S."));
    /* Help back to F1, then Screenshot to F3, each saved */
    log_[0] = 0;
    overlay_move(-2);
    assert(overlay_backspace() == OVERLAY_NONE && !strcmp(hotkey_text(HOTKEY_HELP), "F1"));
    overlay_move(2);
    assert(overlay_backspace() == OVERLAY_NONE && !strcmp(hotkey_text(HOTKEY_SCREENSHOT), "F3"));
    assert(!strcmp(log_, "hotkey_help=F1;hotkey_screenshot=F3;"));
    draw();
    assert(!shows("already does"));
    /* a pad's B, a move and a tap cancel the wait; on the page the next B goes back to the menu, on Shortcuts */
    overlay_select();
    assert(overlay_pad(1ull << 1) == OVERLAY_NONE && !overlay_shortcut_listening());
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  SHORTCUTS"));
    overlay_select();
    overlay_move(1);
    assert(!overlay_shortcut_listening());
    overlay_select();
    assert(tap(0, 3) == OVERLAY_NONE && !overlay_shortcut_listening() && !strcmp(hotkey_text(HOTKEY_HELP), "F1"));
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  SHORTCUTS"));
    /* Backspace on Back, as Back */
    overlay_scroll(100);
    assert(overlay_backspace() == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  MENU") && overlay_select() == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  SHORTCUTS") && overlay_back() == OVERLAY_NONE);

    /* the controls' page: each setting shown with its value, changed and kept */
    assert(tap(0, 3) == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  CONTROLS") && text_at(1, 3, "\xFE Show controls"));
    assert(qv(&list.q[0]).h == 15 * 32 && text_at(C - 2, 11, ">")); /* ten items; Buttons opens a page */
    /* named as the .ini's rows name them, in the launcher's order, the game's own actions among them */
    assert(text_at(1, 4, "\xFE Edit layout") && text_at(1, 5, "\xFE Snap to grid") &&
           text_at(1, 7, "\xFE Relative stick center"));
    assert(text_at(1, 10, "\xFE Touch screen") && text_at(1, 11, "\xFE Buttons") &&
           text_at(1, 12, "\xFE Reset layout"));
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
    /* the toggles; the touch screen goes round its three */
    tap(2, 3);
    assert(g_settings.snap_to_grid && !strcmp(stored, "controls_snap"));
    tap(2, 3);
    tap(4, 3);
    assert(!g_settings.relative_stick_center && !strcmp(stored, "controls_rel_center"));
    tap(4, 3);
    tap(7, 3);
    assert(touch == 2);
    draw();
    assert(text_at(C - 1 - (int)strlen("Fingers and mouse"), 10, "Fingers and mouse"));
    tap(7, 3);
    assert(touch == 0);
    draw();
    assert(text_at(C - 4, 10, "Off"));
    tap(7, 3);
    /* Edit layout is the caller's to do */
    assert(tap(1, 3) == OVERLAY_EDIT);

    /* the buttons' page: one hidden and kept, All shows them again; it scrolls to its last item */
    assert(tap(8, 3) == OVERLAY_NONE);
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
        overlay_move(2);                                          /* Shortcuts */
        assert(overlay_controls(0, down, 1000) == OVERLAY_NONE);  /* to TV mode */
        assert(overlay_controls(0, still, 1000) == OVERLAY_NONE); /* let go */
        assert(overlay_controls(0, down, 1000) == OVERLAY_NONE);  /* to Stats */
        assert(overlay_controls(0, still, 1000) == OVERLAY_NONE); /* let go */
        assert(overlay_controls(0, down, 1000) == OVERLAY_NONE);  /* to Exit */
        assert(overlay_controls(0, down, 1000 + SNX_OVERLAY_REPEAT_DELAY - 1) == OVERLAY_NONE);
        assert(overlay_controls(1ull << 0, down, 1000 + SNX_OVERLAY_REPEAT_DELAY - 1) == OVERLAY_EXIT); /* not yet */
        assert(overlay_controls(0, down, 1000 + SNX_OVERLAY_REPEAT_DELAY) == OVERLAY_NONE); /* round to the top */
        assert(overlay_controls(1ull << 0, down, 1001 + SNX_OVERLAY_REPEAT_DELAY) == OVERLAY_NONE); /* a page */
        draw();
        assert(text_at(1, 0, "SCARLETNX  /  CONTROLS"));
        assert(overlay_controls(1ull << 1, still, 2000) == OVERLAY_NONE);  /* B: back to the menu, on its first */
        assert(overlay_controls(1ull << 15, still, 2001) == OVERLAY_NONE); /* the D-pad down: Controllers */
        assert(overlay_controls(1ull << 15, still, 2001 + SNX_OVERLAY_REPEAT_DELAY) == OVERLAY_NONE); /* Save states */
        assert(overlay_controls(1ull << 15, still, 2001 + SNX_OVERLAY_REPEAT_DELAY + SNX_OVERLAY_REPEAT_MS - 1) ==
               OVERLAY_NONE);
        overlay_move(2); /* past Save states and Shortcuts, to TV mode */
        { /* the D-pad's right held on it, past three repeats: one step, the press's; the left stick's left, one back */
            int steps = tv.steps;
            u64 t;
            assert(overlay_controls(1ull << 14, still, 2530) == OVERLAY_NONE && tv.steps == steps + 1);
            for (t = 2530; t <= 2530 + SNX_OVERLAY_REPEAT_DELAY + 3 * SNX_OVERLAY_REPEAT_MS; t += 10)
                assert(overlay_controls(1ull << 14, still, t) == OVERLAY_NONE);
            assert(tv.steps == steps + 1 && tv.last == 1);
            assert(overlay_controls(0, still, t) == OVERLAY_NONE); /* let go */
            assert(overlay_controls(0, left, t + 1) == OVERLAY_NONE && tv.steps == steps + 2 && tv.last == -1);
            assert(overlay_controls(0, still, t + 2) == OVERLAY_NONE);
        }
        overlay_move(1);                                                                      /* to Stats */
        assert(overlay_controls(1ull << 0, still, 3500) == OVERLAY_NONE && g_settings.stats); /* A on it */
        g_settings.stats = 0;
        assert(overlay_controls(1ull << 13, still, 3501) == OVERLAY_NONE &&
               overlay_controls(0, left, 3502) == OVERLAY_NONE);
        assert(overlay_controls(1ull << 1, still, 3503) == OVERLAY_CLOSE);
        overlay_toggle(); /* closed: the pads are the game's */
        assert(!overlay_shown() && overlay_controls(1ull << 1, still, 3504) == OVERLAY_NONE);
        overlay_toggle(); /* B still held as it opens */
        assert(overlay_controls(1ull << 1, still, 3505) == OVERLAY_NONE);
        assert(overlay_controls(0, still, 3506) == OVERLAY_NONE);
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
    assert(qv(&list.q[0]).h == 11 * 32 && text_at(1, 3, "\xFE Player") && shows("< All >"));
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
    { /* its round parts, the two sticks and the four face buttons: each one circle (ui.h's ui_shape), filled */
        int i, round = 0;
        for (i = 0; i < list.n; ++i)
            round += qv(&list.q[i]).ink == UI_SHAPE && list.q[i].u == UI_SHAPE_CIRCLE && !list.q[i].v &&
                     list.q[i].uw == (0x8000 | UI_SHAPE_FILLED) && qv(&list.q[i]).w == qv(&list.q[i]).h;
        assert(round == 6);
    }
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
        assert(shows("SCARLETNX  /  SAVE STATES") && shows("\xFE Auto") && shows("\xFE Slot 1") &&
               shows("\xFE Slot 2") && shows("\xFE Back"));
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
    /* over the game it is its letters and a blank cell at either end, a row from the corner, each backed by the
       layer's ground, the menu's ink, in its page's colour: a band of cells side by side with no fill under them */
    {
        static const char said[] = "Saved to slot 2";
        const u32 page = 0xFF000000u | (SNX_GUI_THEME_YORHA_PAGE & 0xFFu) << 16 | (SNX_GUI_THEME_YORHA_PAGE & 0xFF00u) |
                         (SNX_GUI_THEME_YORHA_PAGE >> 16 & 0xFFu);
        int i;
        assert(list.n == (int)strlen(said) + 2 && ui_ground() == ui_rgba(ink));
        for (i = 0; i < list.n; ++i) {
            SnxUiQuadView q = qv(&list.q[i]);
            assert(q.ink == UI_GLYPH && q.backed && q.glyph == (u8)(i && i < list.n - 1 ? said[i - 1] : ' '));
            assert(q.uw == 8 && q.vh == 16 && q.rgba == ui_rgba(page));
            assert(q.x == (float)(8 * s * (1 + i)) && q.y == (float)(16 * s));
            assert(q.w == (float)(8 * s) && q.h == (float)(16 * s));
        }
    }
    /* the stats' box in that corner too: the band goes under it, from the box's left, half a line apart; back in its
       corner with the box in another, or off; and in the picture's top right where there is no room under the box */
    {
        static const int all[4] = {0, 0, SW, SH};
        static SnxUiQuad kept[1024];
        SnxUiList box_quads = {kept, 0, 1024};
        const IniOpt* corner = ini_find(g_display_ini, "stats_corner");
        float box[4], cw = (float)(8 * s), ch = (float)(16 * s);
        int short_s, short_x, short_y, h;
        assert(corner && !stats_box_drawn(box));
        g_settings.stats = 1;
        stats_corner_set(corner, "tl");
        stats_draw(&box_quads, all, s, 1000);
        assert(stats_box_drawn(box) && box[0] < cw + 16 * cw && box[1] < 2 * ch);
        { /* the box from its first quad that draws, the notification's band lined up with it */
            float left = box[0] + 1000;
            int k;
            for (k = 0; k < box_quads.n; ++k)
                if (qv(&box_quads.q[k]).x < left) left = qv(&box_quads.q[k]).x;
            assert(left == box[0]);
        }
        draw();
        assert(list.n == (int)strlen("Saved to slot 2") + 2 && shows("Saved to slot 2"));
        assert(qv(&list.q[0]).x == box[0] && qv(&list.q[0]).y == box[1] + box[3] + ch / 2);
        h = (int)(box[1] + box[3]) + 4; /* a picture that ends four pixels under the box */
        overlay_place(SW, h, &short_x, &short_y, &short_s);
        list.n = 0;
        overlay_draw(&list, SW, h, 0);
        assert(list.n == (int)strlen("Saved to slot 2") + 2);
        assert(qv(&list.q[0]).y == (float)(16 * short_s) &&
               qv(&list.q[list.n - 1]).x + qv(&list.q[list.n - 1]).w == (float)(SW - 8 * short_s));
        stats_corner_set(corner, "br");
        box_quads.n = 0;
        stats_draw(&box_quads, all, s, 1000);
        draw();
        assert(qv(&list.q[0]).x == cw && qv(&list.q[0]).y == ch);
        stats_corner_set(corner, "tl");
        box_quads.n = 0;
        stats_draw(&box_quads, all, s, 1000);
        draw();
        assert(qv(&list.q[0]).x == box[0] && qv(&list.q[0]).y == box[1] + box[3] + ch / 2);
        g_settings.stats = 0;
        draw();
        assert(qv(&list.q[0]).x == cw && qv(&list.q[0]).y == ch);
    }
    overlay_toast("", 0); /* none */
    /* a notice from any thread (RenderDoc's): the notification once the next frame draws it, the last given before
       then, and taken once */
    overlay_notice("RenderDoc: written in 2 s: one.rdc (2 KiB)");
    overlay_notice("RenderDoc: written in 3 s: two.rdc (2 KiB)");
    assert(!overlay_toasting(0));
    draw();
    assert(shows("RenderDoc: written in 3 s: two.rdc (2 KiB)") && !shows("one.rdc") && overlay_toasting(0));
    overlay_toast("", 0);
    draw();
    assert(!list.n);
    overlay_toggle();
    /* closed, it draws nothing; opened again, its first page */
    overlay_toggle();
    draw();
    assert(!overlay_shown() && !list.n);
    overlay_toggle();
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  MENU"));

    /* ui.c: the atlas with the font's glyphs, white where a glyph's bit is, the white block and the icons */
    {
        const u32* a;
        int w, h;
        u32 version;
        assert(!ui_atlas(&w, &h, &version) && !version);
        ui_atlas_build();
        a = ui_atlas(&w, &h, &version);
        assert(a && version == 1 && w == SNX_UI_ATLAS_W && h == SNX_UI_PICTURE_Y + SNX_UI_PICTURE);
        /* no user read yet: the picture's rows are transparent */
        for (y = 0; y < SNX_UI_PICTURE; ++y)
            for (x = 0; x < SNX_UI_PICTURE; ++x) assert(!a[(SNX_UI_PICTURE_Y + y) * w + x]);
        /* every glyph, white where its bit is: 'A' has both */
        for (sc = 0; sc < 256; ++sc)
            for (y = 0; y < 16; ++y)
                for (x = 0; x < 8; ++x)
                    assert(a[(SNX_UI_FONT_Y + sc / 16 * 16 + y) * w + SNX_UI_FONT_X + sc % 16 * 8 + x] ==
                           (gui_vga_font[sc * 16 + y] & (0x80 >> x) ? 0xFFFFFFFFu : 0));
        assert(gui_vga_font['A' * 16 + 5] && gui_vga_font['A' * 16 + 5] != 0xFF);
        assert(a[(SNX_UI_WHITE_Y + 1) * w + SNX_UI_WHITE_X + 1] == 0xFFFFFFFFu);
        /* each icon beside the picture, white where its glyph's bit is (gui_icon_font: its halves, left then right) */
        for (sc = 0; sc < GUI_ICON_COUNT; ++sc)
            for (y = 0; y < 16; ++y)
                for (x = 0; x < 16; ++x)
                    assert(a[(SNX_UI_PICTURE_Y + y) * w + SNX_UI_ICONS_X + sc * 16 + x] ==
                           (gui_icon_font[(sc * 2 + x / 8) * 16 + y] & (0x80 >> x % 8) ? 0xFFFFFFFFu : 0));
        ui_atlas_done();
        assert(!ui_atlas(&w, &h, &version) && version == 1);
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
        top = qv(&list.q[0]).y;
        total = (int)(qv(&list.q[0]).h / row_h);
        C = overlay_cols(SW, s);
        assert(list.n > 4 && qv(&list.q[0]).x == (float)px && qv(&list.q[0]).w == (float)(C * 8 * s) &&
               qv(&list.q[0]).rgba >> 24 == 0xFFu);
        assert(top >= 0 && top + qv(&list.q[0]).h <= (float)SH && qv(&list.q[1]).y == top && qv(&list.q[1]).h == row_h);
        py = (int)top;
        assert(text_at(1, 0, "SCARLETNX  /  STOPPED") && text_at(1, 1, "ENDER MAGNOLIA"));
        assert(
            text_at(1, 3, "vulkan: the GPU device was lost (driver reset or crash)")); /* one row, as wide as it is */
        i = glyph_at(1, 3, 'v');
        assert(i >= 0 && qv(&list.q[i]).rgba == ui_rgba(0xFF3C47A5u));
        assert(text_at(1, 5, "The GPU stopped answering and its"));
        for (i = 0; i < list.n; ++i)
            if (qv(&list.q[i]).y == top + row_h * (float)(total - 2) && qv(&list.q[i]).h == row_h &&
                qv(&list.q[i]).w == qv(&list.q[0]).w)
                close_band = i;
        assert(close_band >= 0 && text_at(1, total - 2, "\xFE Close") && text_at(1, total - 3, "\xFE Copy path") &&
               text_at(1, total - 4, "\xFE Share Log"));
        i = glyph_at(3, total - 4, 'S');
        assert(i >= 0);
        plain = qv(&list.q[i]).rgba;
        i = glyph_at(3, total - 2, 'C');
        assert(i >= 0 && qv(&list.q[i]).rgba != ui_rgba(plain));
        focused = qv(&list.q[i]).rgba;
        for (i = 0; i < SNX_OVERLAY_ERROR_ITEMS; ++i)
            assert(panel.items[i].y == top + row_h * (float)(total - 1 - SNX_OVERLAY_ERROR_ITEMS + i) &&
                   panel.items[i].h == row_h && panel.items[i].x == qv(&list.q[0]).x &&
                       panel.items[i].w == qv(&list.q[0]).w);
        assert(overlay_error_hit(&panel.items[SNX_OVERLAY_ERROR_SHARE], panel.items[0].x + 1, panel.items[0].y + 1));
        assert(!overlay_error_hit(&panel.items[SNX_OVERLAY_ERROR_SHARE], panel.items[1].x + 1, panel.items[1].y + 1));
        /* the band follows the focus */
        panel.focus = SNX_OVERLAY_ERROR_SHARE;
        list.n = 0;
        overlay_error_draw(&list, SW, SH, &panel);
        for (i = 0; i < list.n; ++i)
            if (qv(&list.q[i]).y == top + row_h * (float)(total - 4) && qv(&list.q[i]).h == row_h &&
                qv(&list.q[i]).w == qv(&list.q[0]).w)
                share_band = i;
        assert(share_band >= 0);
        i = glyph_at(3, total - 2, 'C');
        assert(i >= 0 && qv(&list.q[i]).rgba == ui_rgba(plain));
        i = glyph_at(3, total - 4, 'S');
        assert(i >= 0 && qv(&list.q[i]).rgba == ui_rgba(focused));
        /* what Share Log did, over the items, and kept when the picture is short */
        panel.said = "Log saved to /home/player/.local/state/scarletnx/games/0100ABCD12345000/scarletnx.log.";
        list.n = 0;
        overlay_error_draw(&list, SW, 260, &panel);
        overlay_place(SW, 260, &px, &py, &s);
        py = (int)qv(&list.q[0]).y;
        assert(qv(&list.q[0]).y >= 0 && qv(&list.q[0]).y + qv(&list.q[0]).h <= 260.0f);
        total = (int)(qv(&list.q[0]).h / (16.0f * (float)s));
        for (i = 1, share_band = 0; i < total - 5; ++i) share_band |= text_at(1, i, "Log saved to");
        assert(share_band);
        list.n = 0;
        panel = panel_of("CAF\xC3\x89", "x", "y", NULL, NULL);
        overlay_error_draw(&list, SW, SH, &panel);
        overlay_place(SW, SH, &px, &py, &s);
        py = (int)qv(&list.q[0]).y;
        assert(text_at(1, 1, "CAF\x90"));
        list.n = 0;
        panel =
            panel_of("N", "message",
                     "a b c d e f g h i j k l m n o p q r s t u v w x y z a b c d e f g h i j k l m n o p q r s t u "
                     "v w x y z a b c d e f g h i j k l m n o p q r s t u v w x y z",
                     NULL, NULL);
        overlay_error_draw(&list, SW, 200, &panel);
        assert(qv(&list.q[0]).y >= 0 && qv(&list.q[0]).y + qv(&list.q[0]).h <= 200.0f);
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
        py = (int)qv(&list.q[0]).y;
        assert(text_at(1, (int)((link.y - (float)py) / (16.0f * (float)s)) - 1, "The game's log:"));
        assert(overlay_error_hit(&link, link.x, link.y));
        assert(overlay_error_hit(&link, link.x + link.w - 1, link.y + link.h - 1));
        assert(!overlay_error_hit(&link, link.x - 1, link.y));
        assert(!overlay_error_hit(&link, link.x + link.w, link.y));
        assert(!overlay_error_hit(&link, link.x, link.y + link.h));
        for (i = 0; i < list.n; ++i)
            if (qv(&list.q[i]).x == link.x && qv(&list.q[i]).h == (float)s && qv(&list.q[i]).w > 0 &&
                qv(&list.q[i]).y >= link.y &&
                qv(&list.q[i]).y < link.y + link.h)
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
                py = (int)qv(&list.q[0]).y;
                assert(qv(&list.q[0]).y >= 0 && qv(&list.q[0]).y + qv(&list.q[0]).h <= (float)height);
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
    /* the user playing on the head's band: Player, without users.ini, as the YoRHa theme has it (square), in the
       atlas under the font: Wimsy (no picture= and no picture file), and its Mii's (Baguette's) with
       picture=mii; on the right of the band, its frame in the page colour
       and the nickname before it; none on a stopped game's panel */
    {
        char why[256];
        const u32* a;
        int w, h, i, pictures = 0, frames = 0, white = 0;
        u32 version;
        float right, band;
        SnxOverlayError e = panel_of("Game", "It stopped", "Look at the log", NULL, NULL);
        assert(!snx_users_load("/nonexistent/users", 1, why, sizeof why));
        assert(g_user_count == 1 && !strcmp(snx_users_selected()->nickname, "Player"));
        assert(SNX_GUI_THEME_YORHA_PICTURE == 1); /* square: the frame is the fills' */
        assert(!snx_users_selected()->picture_mii && snx_users_selected()->mii == 0);
        g_users[0].picture_mii = 1; /* picture=mii: Baguette's face */
        ui_atlas_build();
        a = ui_atlas(&w, &h, &version);
        assert(a && h == SNX_UI_PICTURE_Y + SNX_UI_PICTURE);
        { /* texel for texel the Mii's picture (mii_model.h), R G B A from the low byte, opaque */
            static uint8_t rgba[SNX_UI_PICTURE * SNX_UI_PICTURE * 4];
            uint8_t charinfo[SNX_MII_CHARINFO];
            SnxMiiLook look;
            snx_mii_db_charinfo(0, charinfo);
            snx_mii_db_look(0, &look);
            assert(!snx_mii_picture(charinfo, &look, SNX_UI_PICTURE, rgba));
            for (y = 0; y < SNX_UI_PICTURE; ++y)
                for (x = 0; x < SNX_UI_PICTURE; ++x) {
                    const uint8_t* p = rgba + 4 * (y * SNX_UI_PICTURE + x);
                    assert(snx_le32(a[(SNX_UI_PICTURE_Y + y) * w + x]) ==
                           ((u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | 0xFF000000u));
                }
        }
        ui_atlas_done();
        g_users[0].picture_mii = 0; /* the default again: no users/<uid>.jpg, Wimsy */
        ui_atlas_build();
        a = ui_atlas(&w, &h, &version);
        assert(a && h == SNX_UI_PICTURE_Y + SNX_UI_PICTURE);
        for (y = 0; y < SNX_UI_PICTURE; ++y)
            for (x = 0; x < SNX_UI_PICTURE; ++x) {
                u32 p = snx_le32(a[(SNX_UI_PICTURE_Y + y) * w + x]); /* the GPU's bytes R G B A */
                assert(p >> 24 == 0xFF);                                      /* square: every texel opaque */
                white += p == 0xFFFFFFFFu;
            }
        assert(a[SNX_UI_PICTURE_Y * w] == 0xFFFFFFFFu && white < SNX_UI_PICTURE * SNX_UI_PICTURE * 9 / 10);
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
                    assert(snx_le32(a[(SNX_UI_PICTURE_Y + y) * w + x]) ==
                           ((p & 0xFF00FF00u) | (p >> 16 & 0xFFu) | (p & 0xFFu) << 16));
                }
            assert(unlike > 0);
        }
        ui_atlas_done();
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
                const SnxUiQuadView q_v = qv(&list.q[i]), *q = &q_v;
                if (q->v == SNX_UI_PICTURE_Y && q->uw == SNX_UI_PICTURE && q->vh == SNX_UI_PICTURE) {
                    ++pictures;
                    assert(q->y == band + 2 * edge && q->h == side && q->w == side && q->sample == UI_BLEND);
                    assert(q->x + q->w == right - 2 * edge);
                }
                frames += q->rgba == ui_rgba(LIGHT_RGBA) && q->sample == UI_SHARP && (q->w == edge || q->h == edge) &&
                          q->x >= boxx && q->x < right && q->y >= band && q->y < band + 16.0f * (float)s;
                if (q->u == SNX_UI_FONT_X + 'P' % 16 * 8 && q->v == SNX_UI_FONT_Y + 'P' / 16 * 16 &&
                    q->y == band && q->x > boxx)
                    assert(q->x == boxx + 8.0f * (float)s); /* the nickname a column into the box */
            }
        }
        assert(pictures == 1 && frames == 4 && shows("Player"));
        list.n = 0;
        overlay_error_draw(&list, SW, SH, &e);
        for (i = 0, pictures = 0; i < list.n; ++i) pictures += list.q[i].v == SNX_UI_PICTURE_Y;
        assert(!pictures && !shows("Player"));
    }
    /* a picture that does not draw: Wimsy shows, and the warning names the user by number, never by nickname, which
       players attach to public reports with the log */
    {
        static const unsigned char broken[] = {0xFF, 0xD8, 'n', 'o', 't', 0xFF, 0xD9}; /* a JPEG's ends alone */
        static const char ini[] = "[user1]\nnickname=Wanda Maximoff\n";
        const uint64_t first[2] = {SNX_USERS_FIRST_UID0, SNX_USERS_FIRST_UID1};
        char why[256], uid[33], *jpg;
        snx_remove_tree("overlay_users");
        assert(!snx_mkdirs("overlay_users"));
        assert(!snx_write_file("overlay_users/users.ini", ini, sizeof ini - 1));
        snx_users_uid_text(first, uid);
        jpg = snx_format("overlay_users/%s.jpg", uid);
        assert(jpg && !snx_write_file(jpg, broken, sizeof broken));
        assert(!snx_users_load("overlay_users", 1, why, sizeof why));
        assert(!strcmp(snx_users_selected()->nickname, "Wanda Maximoff"));
        warned[0] = 0;
        ui_atlas_build();
        ui_atlas_done();
        assert(!strcmp(warned, "ui: user 1's picture could not be drawn, Wimsy shows in the menu\n"));
        free(jpg);
        assert(!snx_users_load("/nonexistent/users", 1, why, sizeof why)); /* Player again */
        snx_remove_tree("overlay_users");
    }
    keys_page();
    keyboard_box();
    finger_rows();
    puts(
        "overlay: placing, pages, taps, keys, values, scrolling, look, the error's panel, the atlas, the user, a picture "
        "that does not draw, F1's page and the keyboard's box passed");
    return 0;
}
