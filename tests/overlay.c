/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's menu (runtime/platform/overlay.c): where it goes, its pages, what a tap, a move or a step does, its look;
   and ui.c's atlas and text */
#include "nx/runtime/platform/overlay.h"
#include <math.h>
#include "nx/runtime/platform/stats.h"
#include "nx/runtime/platform/prepare_screen.h"
#include "nx/runtime/platform/wait_screen.h"
#include "nx/runtime/platform/ui_colors.h"
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
/* the colour of a choice to take with care, as R G B A bytes */
#define WARNING_RGBA                                                                                                   \
    (0xFFu << 24 | (SNX_GUI_THEME_YORHA_WARNING & 0xFFu) << 16 | (SNX_GUI_THEME_YORHA_WARNING & 0xFF00u) |             \
     (SNX_GUI_THEME_YORHA_WARNING >> 16 & 0xFFu))
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
/* and the other set functions of g_ini_sections' tables, which the Settings page lists: none of them is read here */
#define NO_SET(name)                                                                                                   \
    void name(const IniOpt* o, const char* v) {                                                                        \
        (void)o;                                                                                                       \
        (void)v;                                                                                                       \
    }
NO_SET(gamepad_map_set)
NO_SET(options_fps_set)
NO_SET(options_heap_max_set)
NO_SET(options_ldn_set)
NO_SET(options_room_set)
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
/* options.c's picks of the game, which the menu marks with *: the keys picked, and the last one taken out */
static struct {
    const char* key; /* the one picked, NULL for none */
    char value[64];  /* its value, and the one the Settings page saved last (ini_set_apply) */
    char unpicked[32];
    char applied[128]; /* key=value, as the Settings page gave it */
    _Atomic int changes;
} picks;
int ini_picked(const char* k) { return picks.key && !strcmp(picks.key, k); }
void ini_unpick(const char* k) {
    snprintf(picks.unpicked, sizeof picks.unpicked, "%s", k);
    if (ini_picked(k)) picks.key = NULL;
    ++picks.changes;
}
const _Atomic int* ini_picks_changes(void) { return &picks.changes; }
int ini_next_start(const char* k, char* out, size_t n) {
    if (n) out[0] = 0;
    if (!ini_picked(k)) return 0;
    snprintf(out, n, "%s", picks.value);
    return 1;
}
void ini_set_apply(const char* k, const char* v) {
    snprintf(picks.applied, sizeof picks.applied, "%s=%s", k, v);
    picks.key = k; /* picked: what options.c's poll makes of it */
    snprintf(picks.value, sizeof picks.value, "%s", v);
    ++picks.changes;
}
/* and its Clean Old Settings: the old keys it counts, and the cleans asked */
static int old_keys, cleans;
int ini_old_settings(void) { return old_keys; }
void ini_clean_old(void) { ++cleans; }
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
   unless any. A cell is its first letter's width: the menu's letters, or a notification's smaller ones */
static int shows_as(const char* text, int any, u32 rgba) {
    int i, k;
    for (i = 0; i < list.n; ++i) {
        float x = qv(&list.q[i]).x, y = qv(&list.q[i]).y, step = qv(&list.q[i]).w;
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
/* where text shows, as shows finds it: its first letter's quad's corner in x, y; 1, else 0 */
static int text_xy(const char* text, float* x, float* y) {
    int i, k;
    for (i = 0; i < list.n; ++i) {
        float qx = qv(&list.q[i]).x, qy = qv(&list.q[i]).y, step = qv(&list.q[i]).w;
        for (k = 0; text[k]; ++k) {
            unsigned char c = (unsigned char)text[k];
            int j, found = c == ' ';
            for (j = 0; j < list.n && !found; ++j)
                found = qv(&list.q[j]).x == qx + (float)k * step && qv(&list.q[j]).y == qy &&
                        list.q[j].u == SNX_UI_FONT_X + c % 16 * 8 && list.q[j].v == SNX_UI_FONT_Y + c / 16 * 16;
            if (!found) break;
        }
        if (!text[k]) {
            *x = qx;
            *y = qy;
            return 1;
        }
    }
    return 0;
}
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
/* the icon of the notification that says text, after its band's blank cell, backed: GUI_ICON_NONE where it has none
   or text shows nowhere */
static unsigned toast_icon(const char* text) {
    float x, y;
    int i;
    if (!text_xy(text, &x, &y)) return GUI_ICON_NONE;
    for (i = 0; i < list.n; ++i) {
        const SnxUiQuadView q = qv(&list.q[i]);
        if (q.ink == UI_ICON && q.backed && q.y == y && q.x + q.w + q.w / 2 == x)
            return GUI_ICON_FIRST + (unsigned)(q.u - SNX_UI_ICONS_X) / 16;
    }
    return GUI_ICON_NONE;
}
/* every notification's time up: none shows from here */
static void toasts_gone(void) {
    static SnxUiQuad gone[2048];
    SnxUiList l = {gone, 0, 2048};
    overlay_draw(&l, SW, SH, 1ull << 62);
}
/* the list at now_ns as a renderer's frame makes it, from no faded span (ui_dirty_begin): the spans it gave, into
   fades, and how many */
static int draw_fades(u64 now_ns, SnxUiFade fades[SNX_UI_FADES]) {
    int first, end;
    ui_dirty_begin(NULL);
    ui_dirty_end(&first, &end);
    list.q = quads;
    list.n = 0;
    list.max = 2048;
    overlay_draw(&list, SW, SH, now_ns);
    return ui_fades(fades);
}
/* F1's page, from the page showing: the runtime's keys and the game's buttons, a row each with its icon and its keys
   on a band of ink in the page's colour, none in focus; Up/Down, the D-pad and the wheel go over its rows, each of
   which fits the narrowest panel; any other key, button or tap goes back to the game, F1 again too, and the other
   pages keep those keys as they were */
/* the keys page's first screen: a build with the client holds its capture's row, which takes the last one's place */
#if defined(SNX_TRACY)
enum { KEYS_TRACY_ROWS = 1 };
static const char* const KEYS_LAST = "Tracy capture";
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
    assert(shows_as("Ctrl+D", 0, light) && shows("TV on/off/auto"));
    /* the last runtime keys, after the TV mode's */
    assert(shows_as("Ctrl+U", 0, light) && shows("upscaler") && shows_as("Ctrl+Shift+A", 0, light) &&
           shows("anti-aliasing"));
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
    assert(!shows("capture a frame") && !shows("Debugger") && !shows("Joy-Con mouse on/off"));
    overlay_keys_mouse(1); /* the way in (game_mouse at Hotkey), or a game holds the mouse: its row, Ctrl+G */
    draw();
    for (i = 0; i < KEYS_TRACY_ROWS; ++i) overlay_scroll(1); /* the client's row is one further down */
    draw();
    assert(shows_as("Ctrl+G", 0, light) && shows("Joy-Con mouse on/off"));
    for (i = 0; i < KEYS_TRACY_ROWS; ++i) overlay_scroll(-1);
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
    for (i = 0; i < KEYS_TRACY_ROWS; ++i) overlay_scroll(1); /* its row is one further down */
    draw();
    assert(shows("capture a frame"));
    for (i = 0; i < KEYS_TRACY_ROWS; ++i) overlay_scroll(-1);
    draw();
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
/* a JPEG of side x side pixels of one colour, as a game's icon.jpg, into a buffer of the test's */
typedef struct {
    unsigned char bytes[65536];
    int n;
} TestJpeg;
static void jpeg_put(void* closure, void* data, int size) {
    TestJpeg* j = (TestJpeg*)closure;
    assert(j->n + size <= (int)sizeof j->bytes);
    memcpy(j->bytes + j->n, data, (size_t)size);
    j->n += size;
}
static void jpeg_of(TestJpeg* j, int side, u32 rgb) {
    static u8 px[256 * 256 * 4];
    for (int i = 0; i < side * side; ++i) {
        px[i * 4] = (u8)(rgb >> 16);
        px[i * 4 + 1] = (u8)(rgb >> 8);
        px[i * 4 + 2] = (u8)rgb;
        px[i * 4 + 3] = 0xFF;
    }
    j->n = 0;
    assert(snx_splash_jpeg_rgba(px, side, side, 90, jpeg_put, j) == 0 && j->n > 0);
}
/* the quads of l drawn from the layer's picture (ui_image), and the first's view in *first */
static int pictures_in(const SnxUiList* l, SnxUiQuadView* first) {
    int n = 0;
    for (int i = 0; i < l->n; ++i)
        if (qv(&l->q[i]).sample == UI_IMAGE && !n++ && first) *first = qv(&l->q[i]);
    return n;
}
/* A program a collection's program started (prepare_screen_switch): the screen up with no step, the program after the
   game's name (none for program 0) and its dots turning; the rows of what is known as done steps; the bar full with
   no step at work; the cover in the top right corner, a quarter of the rows high, its texels the left square of the
   picture, the rows cut before it and the steps under them; the cover given again after the menu took the picture,
   and the picture back to none when the switch's part goes */
static void prepare_screen_switching(void) {
    static SnxUiQuad q[4096];
    static TestJpeg jpeg;
    static u8 other[SNX_UI_IMAGE_W * SNX_UI_IMAGE_H * 4];
    SnxUiList l = {q, 0, 4096};
    SnxRunTheme t = {0};
    SnxPrepareStep st[1] = {{0}};
    SnxProgramStart f;
    SnxStartRow start[SNX_START_ROWS];
    SnxUiQuadView cover = {0};
    const u8* now;
    char row[160];
    u32 version;
    int i, n, bar = 0;
    t.ink = 0xFFFFFFu;
    t.steps = 1;
    snx_program_start_unknown(&f);
    f.program = 1;
    f.programs = 4;
    f.previous = 0;
    snprintf(f.gpu, sizeof f.gpu, "%s", "A GPU");
    n = snx_program_start_rows(&f, start); /* the rest not known: "?" */
    assert(!prepare_screen_shown());
    prepare_screen_switch(&t, "GAME", 1, start, n, "FOOT", 1200); /* 1200 ms: three dots */
    assert(prepare_screen_shown());
    prepare_screen_draw(&l, 1280, 720, NULL, 100); /* 2x: 22 rows of 32 pixels, 80 columns */
    prepare_row(&l, 32.0f, 2, row, sizeof row);
    assert(!strcmp(row, " GAME (PROGRAM 1)..."));
    prepare_row(&l, 32.0f * 3, 2, row, sizeof row);
    assert(!strcmp(row, " PROGRAM: 1 OF 4 ...OK"));
    prepare_row(&l, 32.0f * 4, 2, row, sizeof row);
    assert(!strcmp(row, " STARTED BY: PROGRAM 0 ...OK"));
    prepare_row(&l, 32.0f * 5, 2, row, sizeof row);
    assert(!strcmp(row, " USER: ? ...OK"));
    prepare_row(&l, 32.0f * 6, 2, row, sizeof row);
    assert(!strcmp(row, " VERSION: ? (UPDATE ?) ...OK"));
    prepare_row(&l, 32.0f * 9, 2, row, sizeof row);
    assert(!strcmp(row, " TITLE ID: ? ...OK"));
    prepare_row(&l, 32.0f * 21, 2, row, sizeof row);
    assert(!strcmp(row, " FOOT"));
    prepare_row(&l, 32.0f * 19, 2, row, sizeof row); /* the bar's percentage: full, no step at work */
    assert(strstr(row, "100%"));
    for (i = 0; i < l.n; ++i) bar += qv(&q[i]).y >= 32.0f * 18 && qv(&q[i]).y < 32.0f * 21;
    assert(bar > 40 && pictures_in(&l, NULL) == 0);               /* the bar's segments; no cover given */
    prepare_screen_switch(&t, "GAME", 1, start, n, "FOOT", 1600); /* the dots turn: none at 1600 ms */
    l.n = 0;
    prepare_screen_draw(&l, 1280, 720, NULL, 100);
    prepare_row(&l, 32.0f, 2, row, sizeof row);
    assert(!strcmp(row, " GAME (PROGRAM 1)"));
    prepare_screen_switch(&t, "GAME", 0, start, n, "FOOT", 1200); /* program 0, the collection's menu: the name alone */
    l.n = 0;
    prepare_screen_draw(&l, 1280, 720, NULL, 100);
    prepare_row(&l, 32.0f, 2, row, sizeof row);
    assert(!strcmp(row, " GAME..."));
    /* a JPEG that does not draw: 1, and no cover */
    assert(prepare_screen_cover("not a jpeg", 10) == 1);
    l.n = 0;
    prepare_screen_draw(&l, 1280, 720, NULL, 100);
    assert(pictures_in(&l, NULL) == 0);
    /* the cover: 5 rows of 32 at the top right, from the head's row 3, a column from the right edge; its texels the
       picture's left square, red; the rows cut before it; a step under the rows */
    jpeg_of(&jpeg, 256, 0xFF0000u);
    assert(prepare_screen_cover(jpeg.bytes, (size_t)jpeg.n) == 0);
    snprintf(f.build, sizeof f.build, "%s", "ScarletNX a-branch-name-long-enough-to-reach-past-the-cover abc1234");
    n = snx_program_start_rows(&f, start);
    st[0].label = "PREPARING SHADERS";
    st[0].value = 1;
    st[0].total = 2;
    prepare_screen_set(&t, "GAME", st, 1, NULL, 0, "STEPS FOOT", 1200);
    prepare_screen_switch(&t, "GAME", 2, start, n, "FOOT", 1200);
    l.n = 0;
    prepare_screen_draw(&l, 1280, 720, NULL, 100);
    assert(pictures_in(&l, &cover) == 1);
    assert(cover.w == 32.0f * 5 && cover.h == cover.w && cover.x == 1280 - 16 - cover.w && cover.y == 32.0f * 3);
    assert(cover.u == 0 && cover.v == 0 && cover.uw == SNX_PREPARE_COVER && cover.vh == SNX_PREPARE_COVER);
    now = ui_image_get(&version);
    assert(now && now[0] > 240 && now[1] < 16 && now[2] < 16 && now[3] == 0xFF);
    for (i = 0; i < l.n; ++i) /* no letter of the rows over the cover */
        assert(!(qv(&q[i]).h == 32 && qv(&q[i]).y >= 32.0f * 3 && qv(&q[i]).y < 32.0f * 8 &&
                 qv(&q[i]).x + qv(&q[i]).w > cover.x && qv(&q[i]).sample != UI_IMAGE));
    prepare_row(&l, 32.0f * (float)(3 + n), 2, row, sizeof row); /* the step under the rows */
    assert(!strncmp(row, " PREPARING SHADERS:", 19));
    prepare_row(&l, 32.0f * 21, 2, row, sizeof row); /* the steps' foot while they show, not the switch's */
    assert(!strcmp(row, " STEPS FOOT"));
    /* the menu's thumbnail took the picture: the cover is given again at the next draw */
    memset(other, 0x40, sizeof other);
    ui_image_set(other);
    l.n = 0;
    prepare_screen_draw(&l, 1280, 720, NULL, 100);
    now = ui_image_get(&version);
    assert(now && now[0] > 240 && now[1] < 16);
    /* the game's first frame: the switch's part gone, the steps alone, then nothing; the picture back to none */
    prepare_screen_game_drew();
    assert(prepare_screen_shown() && !ui_image_get(&version));
    l.n = 0;
    prepare_screen_draw(&l, 1280, 720, NULL, 100);
    assert(pictures_in(&l, NULL) == 0);
    prepare_row(&l, 32.0f, 2, row, sizeof row);
    assert(!strcmp(row, " GAME...")); /* no program after the name: the step's dots at 1200 ms */
    prepare_row(&l, 32.0f * 3, 2, row, sizeof row);
    assert(!strncmp(row, " PREPARING SHADERS:", 19));        /* the rows gone with the switch */
    prepare_screen_switch(NULL, NULL, -1, NULL, 0, NULL, 0); /* again: nothing to take away */
    prepare_screen_set(NULL, NULL, NULL, 0, NULL, 0, NULL, 0);
    assert(!prepare_screen_shown());
    assert(prepare_screen_cover(NULL, 0) == 0);
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
    assert(shows("Ann\xDD") && shows("3 of 6 characters, 4 at least.") && shows("Enter: Done. Esc: Cancel."));
    assert(shows("\xFE Done") && shows("\xFE Cancel") && quads_of(PANEL_RGBA) == 1); /* the line on its panel */
    /* the hint names what was pressed last: a controller's buttons by its face, the console's A and B by default,
       then the keyboard's keys again; never both */
    overlay_input_pad("Circle", "Cross");
    draw();
    assert(shows("Circle: Done. Cross: Cancel.") && !shows("Enter") && !shows("Esc"));
    overlay_input_pad(NULL, NULL);
    draw();
    assert(shows("A: Done. B: Cancel.") && !shows("Circle"));
    overlay_input_keys();
    draw();
    assert(shows("Enter: Done. Esc: Cancel.") && !shows("A: Done."));
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
/* the slider's knob (overlay.c's slider_draw) from a column of an item's row on, rows and columns as the letters':
   a cell wide, a pixel of the scale in two from its row's top and foot, in rgba */
static int knob_at(int col, int row, u32 rgba) {
    int i;
    for (i = 0; i < list.n; ++i)
        if (qv(&list.q[i]).ink == UI_SOLID && qv(&list.q[i]).x == (float)(px + col * 8 * s) &&
            qv(&list.q[i]).y == (float)(py + row * 16 * s + 2 * s) && qv(&list.q[i]).w == (float)(8 * s) &&
            qv(&list.q[i]).h == (float)(12 * s) && qv(&list.q[i]).rgba == ui_rgba(rgba))
            return 1;
    return 0;
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
    assert(s == 3 && (band = focus_band(ink)) && qv(band).h == 48 && items_shown() == 9); /* the first page's all */
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

/* the turning ring's quads in l (ui_loading_ring's: two circles and UI_RING_BAR_DOTS filled ones), and where the
   first is centred */
static int ring_in(const SnxUiList* l, float* cx, float* cy) {
    int n = 0;
    for (int i = 0; i < l->n; ++i) {
        const SnxUiQuadView v = qv(&l->q[i]);
        if (v.ink != UI_SHAPE || l->q[i].u != UI_SHAPE_CIRCLE) continue;
        if (!n++) {
            *cx = v.x + v.w / 2;
            *cy = v.y + v.h / 2;
        }
    }
    return n;
}
/* The screen's ring where the theme puts it, as the launcher's: at the right edge (YoRHa's), left of the title, which
   then starts three columns on, after the name and its dots' whole width, or none; 7 pixels a scale step in radius,
   its inner circle's edge 1.5 times the scale rounded up, its bar's dots on that edge, going round with the time and
   still once every step is done */
static void prepare_screen_ring(void) {
    static SnxUiQuad q[4096];
    SnxUiList l = {q, 0, 4096};
    SnxPrepareStep st[1] = {{0}};
    SnxRunTheme t;
    char row[160];
    float cx = 0, cy = 0, dx[2][UI_RING_BAR_DOTS];
    st[0].label = "PREPARING SHADERS";
    st[0].value = 1;
    st[0].total = 2;
    prepare_screen_theme(NULL, &t); /* YoRHa's: its ring at the right */
    assert(t.ring == SNX_RUN_RING_RIGHT && t.steps);
    prepare_screen_set(&t, "GAME", st, 1, NULL, 0, "FOOT", 1000);
    prepare_screen_draw(&l, 1280, 720, NULL, 100); /* 2x */
    assert(ring_in(&l, &cx, &cy) == 2 + UI_RING_BAR_DOTS && cx == 1280 - 16 * 2 && cy == 32 * 1.5f);
    for (int i = 0, k = 0; i < l.n; ++i) {
        const SnxUiQuadView v = qv(&q[i]);
        if (v.ink != UI_SHAPE || q[i].u != UI_SHAPE_CIRCLE) continue;
        if (k == 0) assert(v.w == 28 && q[i].vh == 1 && !(q[i].uw & UI_SHAPE_FILLED)); /* 7 x 2, its edge 1 */
        if (k == 1) { /* its size a place in quarter pixels */
            assert(fabsf(v.w - 28 * 0.62f) <= 0.25f && q[i].vh == 3 && v.rgba == ui_rgba(UI_RGB(t.soft)));
        }
        if (k >= 2) { /* on the inner circle's edge, 1.5 pixels in, in ink */
            const float ox = v.x + v.w / 2 - cx, oy = v.y + v.h / 2 - cy, r = 28 * 0.62f / 2 - 1.5f;
            assert((q[i].uw & UI_SHAPE_FILLED) && v.w == 3 && v.rgba == ui_rgba(UI_RGB(t.ink)));
            assert(fabsf(sqrtf(ox * ox + oy * oy) - r) <= 0.5f);
            dx[0][k - 2] = v.x;
        }
        ++k;
    }
    /* 100 ms on: the bar went round 25 degrees */
    prepare_screen_set(&t, "GAME", st, 1, NULL, 0, "FOOT", 1100);
    l.n = 0;
    prepare_screen_draw(&l, 1280, 720, NULL, 100);
    for (int i = 0, k = 0; i < l.n; ++i)
        if (qv(&q[i]).ink == UI_SHAPE && q[i].u == UI_SHAPE_CIRCLE && k++ >= 2) dx[1][k - 3] = qv(&q[i]).x;
    assert(memcmp(dx[0], dx[1], sizeof dx[0]));
    /* every step done: still, its bar from 3 o'clock whatever the time */
    st[0].done = 1;
    st[0].value = 2;
    prepare_screen_set(&t, "GAME", st, 1, NULL, 0, "FOOT", 1100);
    l.n = 0;
    prepare_screen_draw(&l, 1280, 720, NULL, 100);
    for (int i = 0, k = 0; i < l.n; ++i)
        if (qv(&q[i]).ink == UI_SHAPE && q[i].u == UI_SHAPE_CIRCLE && k++ == 2)
            assert(qv(&q[i]).y + qv(&q[i]).h / 2 == cy && qv(&q[i]).x > cx);
    /* left: the title three columns on, the ring two in */
    t.ring = SNX_RUN_RING_LEFT;
    snprintf(t.title, sizeof t.title, "%s", "BOOTING");
    prepare_screen_set(&t, "GAME", st, 1, NULL, 0, "FOOT", 1000);
    l.n = 0;
    prepare_screen_draw(&l, 1280, 720, NULL, 100);
    assert(ring_in(&l, &cx, &cy) == 2 + UI_RING_BAR_DOTS && cx == 16 * 2);
    prepare_row(&l, 32.0f, 2, row, sizeof row);
    assert(!strcmp(row, "    BOOTING GAME..."));
    /* after the name: its dots' columns and one more, then the ring's radius */
    t.ring = SNX_RUN_RING_NEAR_TEXT;
    prepare_screen_set(&t, "GAME", st, 1, NULL, 0, "FOOT", 1000);
    l.n = 0;
    prepare_screen_draw(&l, 1280, 720, NULL, 100);
    assert(ring_in(&l, &cx, &cy) == 2 + UI_RING_BAR_DOTS && cx == 16 * (1 + 8 + 4 + 4) + 14);
    /* at 1x (the launcher's UI scale 1): the inner circle's edge and the bar's dots 2 pixels, the 1.5 rounded up as the
       launcher's lines take it, the outer edge 1 */
    t.ring = SNX_RUN_RING_RIGHT;
    t.text_size = 1;
    prepare_screen_set(&t, "GAME", st, 1, NULL, 0, "FOOT", 1000);
    l.n = 0;
    prepare_screen_draw(&l, 1280, 720, NULL, 100);
    for (int i = 0, k = 0; i < l.n; ++i) {
        if (qv(&q[i]).ink != UI_SHAPE || q[i].u != UI_SHAPE_CIRCLE) continue;
        assert(k == 0 ? q[i].vh == 1 && qv(&q[i]).w == 14 : k == 1 ? q[i].vh == 2 : qv(&q[i]).w == 2);
        ++k;
    }
    t.text_size = 0;
    t.ring = SNX_RUN_RING_NONE;
    prepare_screen_set(&t, "GAME", st, 1, NULL, 0, "FOOT", 1000);
    l.n = 0;
    prepare_screen_draw(&l, 1280, 720, NULL, 100);
    assert(ring_in(&l, &cx, &cy) == 0);
    prepare_screen_set(NULL, NULL, NULL, 0, NULL, 0, NULL, 0);
}
/* The game closing's screen (main.c's save at exit) as the screen before the game draws it: its row says the MiB
   packed of the save's and the seconds since it began, its progress as the theme says it (the MiB for an amount), and
   the bar the share packed */
static void closing_screen(void) {
    static SnxUiQuad q[4096];
    SnxUiList l = {q, 0, 4096};
    SnxClosing w = {.began_ms = 10000};
    SnxPrepareStep st;
    SnxRunTheme t;
    char row[160];
    const u64 total = 166408602, packed = 100663296; /* 158.7 and 96.0 MiB */
    prepare_screen_theme(NULL, &t);
    snprintf(t.title, sizeof t.title, "%s", "CLOSING");
    snx_closing_step(&w, total, packed, &st);
    prepare_screen_set(&t, "GAME", &st, 1, NULL, 0, "FOOT", 14200);
    prepare_screen_draw(&l, 1280, 720, NULL, 100);
    prepare_row(&l, 32.0f, 2, row, sizeof row);
    assert(!strcmp(row, " CLOSING GAME...")); /* 14200 ms: three dots */
    prepare_row(&l, 32.0f * 3, 2, row, sizeof row);
    assert(!strcmp(row, " SAVING SHADERS: 96.0 OF 158.7 MIB 60%..."));
    prepare_row(&l, 32.0f * 19, 2, row, sizeof row);
    assert(strstr(row, " 60%"));
    t.steps = 0;
    t.progress = SNX_RUN_PROGRESS_AMOUNT;
    prepare_screen_set(&t, "GAME", &st, 1, NULL, 0, "FOOT", 14200);
    l.n = 0;
    prepare_screen_draw(&l, 1280, 720, NULL, 100);
    prepare_row(&l, 32.0f * 3, 2, row, sizeof row);
    assert(!strcmp(row, " SAVING SHADERS 96.0 of 158.7 MiB: 96.0 / 158.7 MiB"));
    prepare_screen_set(NULL, NULL, NULL, 0, NULL, 0, NULL, 0);
}
/* A save state's box (overlay_loading): nothing before it; then the whole picture shaded black, its alpha coming in
   from 0 to the launcher's 128 over OVERLAY_LOADING_FADE_MS, the first faded span; over it the box with the save
   states' head, its words and the slot's, and the ring at their right. It stays as the load ends well until the game's
   first frame after that, and goes at once when the load fails */
static void loading_box(void) {
    SnxUiFade fades[SNX_UI_FADES];
    const u64 t0 = 5000000000ull;
    float x, y, cx = 0, cy = 0;
    int n;
    assert(!overlay_loading_shown());
    n = draw_fades(t0, fades);
    assert(n == 0 && !shows("Loading the save state"));
    overlay_loading("Slot 2", t0);
    assert(overlay_loading_shown());
    n = draw_fades(t0, fades);
    {
        const SnxUiQuadView shade = qv(&list.q[fades[0].first]);
        assert(n == 1 && fades[0].end == fades[0].first + 1 && fades[0].alpha == 0);
        assert(shade.x == 0 && shade.y == 0 && shade.w == SW && shade.h == SH && (shade.rgba & 0xFFFFFFu) == 0);
    }
    n = draw_fades(t0 + 75000000ull, fades);
    assert(n == 1 && fades[0].alpha == 64);
    n = draw_fades(t0 + 900000000ull, fades);
    assert(n == 1 && fades[0].alpha == 128);
    assert(shows("SCARLETNX  /  SAVE STATES") && shows("Loading the save state") && shows("Slot 2"));
    assert(text_xy("Loading the save state", &x, &y));
    {
        SnxUiList shown = list;
        int s, px, py;
        overlay_place(SW, SH, &px, &py, &s);
        assert(ring_in(&shown, &cx, &cy) == 2 + UI_RING_BAR_DOTS);
        assert(cy == y + 16.0f * (float)s && cx > x + 8.0f * (float)s * (float)strlen("Loading the save state"));
    }
    overlay_game_drew(); /* the game's frames while it loads leave it */
    assert(overlay_loading_shown());
    overlay_loading_done();
    assert(overlay_loading_shown());
    overlay_game_drew();
    assert(!overlay_loading_shown());
    n = draw_fades(t0 + 900000000ull, fades);
    assert(n == 0 && !shows("Loading the save state"));
    /* a load that failed: gone at once */
    overlay_loading("The auto slot", t0);
    draw_fades(t0, fades);
    assert(shows("The auto slot"));
    overlay_loading_end();
    assert(!overlay_loading_shown());
    n = draw_fades(t0 + 900000000ull, fades);
    assert(n == 0 && !shows("The auto slot"));
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
    prepare_screen_switching();
    prepare_screen_ring();
    closing_screen();
    loading_box();
    assert(!overlay_shown());
    overlay_toggle();
    assert(overlay_shown());
    /* letters half the screen's height at most, in whole steps, in the middle: a phone held wide, a small window. The
       page is as tall as its items (nine here, under three rows of head, over a blank one), at the scale of the
       tallest page, and as wide as three quarters of the picture hold, between 40 and 72 columns */
    overlay_place(SW, SH, &px, &py, &s);
    C = overlay_cols(SW, s);
    assert(s == 2 && C == OVERLAY_COLS_MAX && px == (SW - C * 16) / 2 && py == (SH - 13 * 32) / 2);
    assert(overlay_cols(1280, 2) == 60 && overlay_cols(500, 1) == 46 && overlay_cols(400, 1) == OVERLAY_COLS &&
           overlay_cols(3840, 4) == OVERLAY_COLS_MAX);
    overlay_place(500, 168, &x, &y, &sc);
    assert(sc == 1 && x == (500 - 46 * 8) / 2 && y == (168 - 13 * 16) / 2); /* taller than the picture */
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
    assert(qv(&list.q[0]).w == (float)(C * 16) && qv(&list.q[0]).h == 13 * 32 &&
           qv(&list.q[1]).rgba == ui_rgba(ink) && qv(&list.q[1]).h == 32);
    assert(qv(&list.q[0]).rgba >> 24 == 0xFFu);
    assert(text_at(C - 2, 3, ">") && text_at(C - 2, 4, ">") && text_at(C - 2, 6, ">")); /* a page under each */
    /* each item its marker, its icon in two cells and one, its name */
    assert(text_at(1, 0, "SCARLETNX  /  MENU") && text_at(1, 3, "\xFE") && icon_at(3, 3, GUI_ICON_POINTER) &&
           text_at(6, 3, "On-screen controls") && icon_at(3, 4, GUI_ICON_GAMEPAD_2) && text_at(6, 4, "Controllers") &&
           icon_at(3, 5, GUI_ICON_SAVE) && text_at(6, 5, "Save states") && icon_at(3, 6, GUI_ICON_KEYBOARD) &&
           text_at(6, 6, "Shortcuts") && icon_at(3, 7, GUI_ICON_MONITOR) && text_at(6, 7, "TV mode") &&
           icon_at(3, 8, GUI_ICON_ACTIVITY) && text_at(6, 8, "Stats") && icon_at(3, 9, GUI_ICON_VOLUME_2) &&
           text_at(6, 9, "Volume") && icon_at(3, 10, GUI_ICON_SLIDERS_HORIZONTAL) && text_at(6, 10, "Settings") &&
           text_at(C - 2, 10, ">") && icon_at(3, 11, GUI_ICON_LOG_OUT) && text_at(1, 11, "\xFE") &&
           text_at(6, 11, "Exit"));
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
    /* Volume (settings.txt's volume, INI_SLIDER): its icon, its value at the right, a slider a column before the value
       at its widest with room for a game's own * after it, OVERLAY_SLIDER_COLS long, its knob a cell wide at the
       value, in the row's colour: the ink, the
       page's in focus. Left/Right step it 5 a press and a direction held goes on (no TV mode), Enter and A up, to its
       ends and never round, each step kept in the game's .ini; a tap on the slider takes the step under it, one left
       of it steps down, one on its value up; a press on it takes the step under it at once, the knob follows the drag
       past either end, and the lift taps nothing */
    {
        const int area[4] = {0, 0, SW, SH};
        const int bar = C - 8 - OVERLAY_SLIDER_COLS; /* the slider's first column */
        float row = (float)(py + 9 * 16 * s + 5), cell = (float)(8 * s);
        int i;
        draw();
        assert(g_settings.volume == 100 && text_at(C - 5, 9, "100%") && knob_at(bar + 20, 9, ink));
        assert(!knob_at(bar + 20, 8, ink) && !knob_at(bar + 20, 8, LIGHT_RGBA)); /* Stats: no slider */
        log_[0] = 0;
        assert(tap(6, C - 3) == OVERLAY_NONE && g_settings.volume == 100 && !strcmp(log_, "volume=100;"));
        draw();
        assert(knob_at(bar + 20, 9, LIGHT_RGBA) && !knob_at(bar + 20, 9, ink));
        assert(overlay_side(1) == OVERLAY_NONE && g_settings.volume == 100);
        assert(overlay_side(-1) == OVERLAY_NONE && g_settings.volume == 95);
        assert(overlay_side_held(-1) == OVERLAY_NONE && overlay_side_held(-1) == OVERLAY_NONE &&
               g_settings.volume == 85);
        draw();
        assert(text_at(C - 4, 9, "85%") && knob_at(bar + 17, 9, LIGHT_RGBA) && !knob_at(bar + 20, 9, LIGHT_RGBA));
        assert(overlay_select() == OVERLAY_NONE && g_settings.volume == 90);
        assert(overlay_pad(1ull << 0) == OVERLAY_NONE && g_settings.volume == 95); /* A */
        assert(tap(6, bar + 10) == OVERLAY_NONE && g_settings.volume == 50);
        assert(tap(6, bar - 2) == OVERLAY_NONE && g_settings.volume == 45);
        assert(tap(6, C - 3) == OVERLAY_NONE && g_settings.volume == 50);
        for (i = 0; i < 12; ++i) assert(overlay_side(-1) == OVERLAY_NONE);
        assert(g_settings.volume == 0 && !strcmp(stored, "volume"));
        draw();
        assert(text_at(C - 5, 9, "Mute") && knob_at(bar, 9, LIGHT_RGBA));
        log_[0] = 0;
        overlay_pointer_down(21, (float)px + cell * (float)(bar + 4) + cell / 2, row, area);
        assert(g_settings.volume == 20 && !strcmp(log_, "volume=20;"));
        overlay_pointer_move(21, (float)px + cell * (float)(bar + 30), row + 200, area); /* off its row: its x counts */
        assert(g_settings.volume == 100);
        overlay_pointer_move(21, (float)px - 100, row, area);
        assert(g_settings.volume == 0);
        overlay_pointer_move(21, (float)px + cell * (float)(bar + 15) + cell / 2, row, area);
        assert(g_settings.volume == 75);
        assert(overlay_pointer_up(21, (float)px + cell * (float)(bar + 15) + cell / 2, row, SW, SH) == OVERLAY_NONE &&
               g_settings.volume == 75 && overlay_shown());
        overlay_pointer_move(21, (float)px + cell * (float)(bar + 2) + cell / 2, row, area); /* lifted: it moves none */
        assert(g_settings.volume == 75);
        g_settings.volume = 100;
    }
    /* a setting the game's picks hold, set for this game alone: * after its value, room for which the slider keeps, and
       a second line of help saying so; Delete or Y on it takes it out, on a row not picked nothing; the * and the line
       go with it */
    {
        assert(tap(6, C - 3) == OVERLAY_NONE && g_settings.volume == 100); /* Volume in focus, at its end */
        picks.key = "volume";
        ++picks.changes; /* as options.c counts each change of the picks, which the menu watches */
        draw();
        assert(shows("100% *") && shows("*: set for this game alone.") && shows("back to Global.") && !shows("Off *"));
        assert(overlay_pad(1ull << 3) == OVERLAY_NONE && !strcmp(picks.unpicked, "volume") && !ini_picked("volume"));
        draw();
        assert(text_at(C - 5, 9, "100%") && !shows("100% *") && !shows("*: set for this game alone."));
        picks.key = "docked";
        ++picks.changes;
        picks.unpicked[0] = 0;
        draw();
        assert(shows("On *") && overlay_delete() == OVERLAY_NONE && !picks.unpicked[0]); /* Volume in focus: not picked */
        overlay_move(-2);                                                          /* to TV mode */
        assert(overlay_delete() == OVERLAY_NONE && !strcmp(picks.unpicked, "docked") && !ini_picked("docked"));
        picks.unpicked[0] = 0;
        overlay_move(2);
    }
    /* Clean Old Settings shows while the game's files keep old keys (options.h's ini_old_settings), before Exit, its
       eraser beside it: asked first, with how many, Cancel in focus and Back to its row; taken, the clean asked of
       the runtime's settings, the count said over the game, and the row gone with the keys */
    {
        old_keys = 2;
        ++picks.changes;
        draw();
        assert(text_at(6, 11, "Clean Old Settings") && text_at(6, 12, "Exit") && icon_drawn(GUI_ICON_ERASER));
        overlay_move(2); /* from Volume, past Settings */
        assert(overlay_select() == OVERLAY_NONE);
        draw();
        assert(shows("CLEAN OLD SETTINGS?") && shows("2 keys this version does not use"));
        assert(overlay_select() == OVERLAY_NONE && !cleans); /* Cancel, in focus: its row again */
        assert(overlay_select() == OVERLAY_NONE);
        overlay_move(-1); /* Clean */
        assert(overlay_select() == OVERLAY_NONE && cleans == 1);
        old_keys = 0;
        ++picks.changes;
        draw();
        assert(text_at(6, 11, "Exit") && shows("Clean Old Settings: 2 taken out")); /* its row gone, the toast */
        assert(toast_icon("Clean Old Settings: 2 taken out") == GUI_ICON_ERASER);   /* after the row's icon */
        overlay_move(6); /* Volume, as the first page was */
    }
    /* Settings, after Volume: the game's .ini's sections the menu has no page for (a section's own page after it), Debug
       last, each opening its rows as the launcher's Settings name them. A row read when the game starts says (next
       start) and shows the pick for it; a step saves to the game's picks, applied by the next tick (ini_set_apply): a
       float by a quarter, a text's words round; a text with no words is only read. Back to the entry, then to it */
    {
        const IniOpt* resolution = ini_find(g_graphics_ini, "resolution");
        const IniOpt* ldn = ini_find(g_network_ini, "ldn");
        assert(resolution && resolution->type == INI_FLOAT && ldn && ldn->type == INI_TEXT);
        *(float*)resolution->var = 1.0f;
        snprintf((char*)ldn->var, ldn->size, "lan");
        overlay_move(1);
        assert(overlay_select() == OVERLAY_NONE);
        draw();
        assert(text_at(1, 0, "SCARLETNX  /  SETTINGS") && text_at(1, 3, "\xFE Display") &&
               text_at(1, 4, "\xFE Graphics") && text_at(1, 5, "\xFE Vulkan Extensions") && text_at(1, 6, "\xFE Audio") &&
               text_at(1, 7, "\xFE Performance") && text_at(1, 8, "\xFE Network") && text_at(1, 9, "\xFE Debug") &&
               text_at(1, 10, "\xFE Back") && text_at(C - 2, 3, ">"));
        overlay_move(1); /* Graphics */
        assert(overlay_select() == OVERLAY_NONE);
        draw();
        assert(text_at(1, 0, "SCARLETNX  /  SETTINGS  /  GRAPHICS") && text_at(1, 3, "\xFE Resolution scale (next start)") &&
               shows("< 1 >"));
        picks.applied[0] = 0;
        assert(overlay_side(1) == OVERLAY_NONE && !strcmp(picks.applied, "resolution=1.25"));
        draw();
        assert(shows("< 1.25 * >") && *(float*)resolution->var == 1.0f); /* the next start's; the run keeps its 1 */
        assert(overlay_back() == OVERLAY_NONE);
        draw();
        assert(text_at(1, 0, "SCARLETNX  /  SETTINGS"));
        overlay_move(4); /* Network */
        assert(overlay_select() == OVERLAY_NONE);
        overlay_move(2); /* Local wireless */
        assert(overlay_select() == OVERLAY_NONE && !strcmp(picks.applied, "ldn=room"));
        overlay_move(1); /* Eden room: a text with no words */
        picks.applied[0] = 0;
        assert(overlay_select() == OVERLAY_NONE && overlay_side(1) == OVERLAY_NONE && !picks.applied[0]);
        assert(overlay_back() == OVERLAY_NONE && overlay_back() == OVERLAY_NONE);
        draw();
        assert(text_at(1, 0, "SCARLETNX  /  MENU") && overlay_select() == OVERLAY_NONE); /* on Settings again */
        assert(overlay_back() == OVERLAY_NONE);
        picks.key = NULL;
        ++picks.changes;
        overlay_move(-1); /* Volume */
    }
    /* with fingers the picks' line says to tap it, and a tap on it takes the focused row's pick out */
    {
        float lx, ly;
        picks.key = "volume";
        picks.unpicked[0] = 0;
        ++picks.changes;
        overlay_set_fingers(1);
        draw();
        assert(text_xy("Tap here", &lx, &ly) && !shows("Y or Delete"));
        assert(overlay_tap(lx + 2, ly + 2, SW, SH) == OVERLAY_NONE && !strcmp(picks.unpicked, "volume"));
        overlay_set_fingers(0);
        picks.unpicked[0] = 0;
        ++picks.changes;
    }
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
    assert(text_at(6, 11, "Capture frame") && text_at(C - 3, 7, "On"));
    can_capture = 0;
    draw();
    assert(text_at(6, 11, "Exit"));
    /* Exit ends the game, no way back: the failure colour, asked first with Cancel in focus; Back to the menu on Exit,
       then its Exit exits. On the title, nothing; outside the panel, it closes */
    assert(shows_as("Exit", 0, FAILURE));
    assert(tap(8, 3) == OVERLAY_NONE);
    draw();
    assert(shows("SCARLETNX  /  EXIT THE GAME?") && shows("what was played since it last saved is lost.") &&
           shows_as("\xFE Exit", 0, FAILURE));
    assert(overlay_back() == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  MENU") && overlay_select() == OVERLAY_NONE);
    overlay_move(-1);
    assert(overlay_select() == OVERLAY_EXIT && tap(-3, 3) == OVERLAY_NONE);
    assert(overlay_tap(5.0f, 5.0f, SW, SH) == OVERLAY_CLOSE);
    /* the focus goes round, from Exit to the first item and back over the top; Back from here closes */
    overlay_move(-1);
    overlay_move(1);
    overlay_move(-10);
    assert(overlay_select() == OVERLAY_NONE); /* asked */
    overlay_move(-1);
    assert(overlay_select() == OVERLAY_EXIT && overlay_back() == OVERLAY_CLOSE);
    /* RenderDoc in the game: Capture frame between Stats and Exit, with the stats box's RenderDoc icon and no page
       under it, the panel a row taller; a tap is the caller's capture, Exit a row down */
    can_capture = 1;
    draw();
    assert(qv(&list.q[0]).h == 14 * 32 && icon_at(3, 11, GUI_ICON_IMAGE) && text_at(6, 11, "Capture frame") &&
           !text_at(C - 2, 11, ">") && text_at(6, 12, "Exit") && icon_at(3, 12, GUI_ICON_LOG_OUT));
    assert(tap(9, 3) == OVERLAY_NONE && overlay_back() == OVERLAY_NONE); /* Exit asked; Back to it */
    assert(tap(8, 3) == OVERLAY_CAPTURE && overlay_select() == OVERLAY_CAPTURE);
    overlay_move(1);
    assert(overlay_select() == OVERLAY_NONE);
    overlay_move(-1);
    assert(overlay_select() == OVERLAY_EXIT);
    overlay_move(-2); /* round to Capture frame */
    /* without it, none: Exit where it was */
    can_capture = 0;
    draw();
    assert(qv(&list.q[0]).h == 13 * 32 && text_at(6, 11, "Exit") && !shows("Capture frame"));
    assert(overlay_select() == OVERLAY_NONE);
    overlay_move(-1);
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
    assert(text_at(C - 12, 5, "Press a key") && shows("Press the key for Screenshot. Esc cancels."));
    overlay_input_pad("B", "A"); /* an Xbox pad's: its B sends the console's A */
    draw();
    assert(shows("Press the key for Screenshot. A cancels."));
    overlay_input_keys();
    draw();
    assert(shows("Press the key for Screenshot. Esc cancels."));
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
    { /* Reset layout wants care, asked first with Cancel in focus: Cancel keeps the controls as they are, Reset puts
         them back, each shown again */
        g_settings.control_shown[0] = 0;
        assert(shows_as("\xFE Reset layout", 0, WARNING_RGBA) && tap(9, 3) == OVERLAY_NONE);
        draw();
        assert(shows("SCARLETNX  /  RESET THE CONTROLS?") &&
               shows("The on-screen controls go back where they start.") && shows_as("\xFE Reset", 0, WARNING_RGBA));
        assert(overlay_select() == OVERLAY_NONE); /* Cancel */
        draw();
        assert(text_at(1, 0, "SCARLETNX  /  CONTROLS") && !g_settings.control_shown[0]);
        assert(overlay_select() == OVERLAY_NONE); /* Reset layout, in focus again, then Reset */
        overlay_move(-1);
        assert(overlay_select() == OVERLAY_NONE);
        draw();
        assert(text_at(1, 0, "SCARLETNX  /  CONTROLS") && g_settings.control_shown[0]);
    }
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
        assert(overlay_controls(0, down, 1000) == OVERLAY_NONE);  /* to Volume */
        assert(overlay_controls(0, still, 1000) == OVERLAY_NONE); /* let go */
        assert(overlay_controls(0, down, 1000) == OVERLAY_NONE);  /* to Settings */
        assert(overlay_controls(0, still, 1000) == OVERLAY_NONE); /* let go */
        assert(overlay_controls(0, down, 1000) == OVERLAY_NONE);  /* to Exit */
        assert(overlay_controls(0, down, 1000 + SNX_OVERLAY_REPEAT_DELAY - 1) == OVERLAY_NONE);
        assert(overlay_controls(1ull << 0, down, 1000 + SNX_OVERLAY_REPEAT_DELAY - 1) == OVERLAY_NONE); /* not yet */
        assert(overlay_back() == OVERLAY_NONE); /* A on Exit asked first; Back to it */
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
        /* a finger or the mouse on the open menu: the on-screen controls hide under it (vpad_menu), so one down where
           the D-pad shows takes nothing of theirs and its lift there, off the panel, closes the menu; closed, they
           show and take fingers again. One the menu forgot (it opened, the window lost the focus) or the system
           cancelled taps nothing */
        {
            const int area[4] = {0, 0, SW, SH};
            const SnxVpadConfig* v = vpad_config();
            float dx = v->at[SNX_VPAD_DPAD].x * SW, dy = v->at[SNX_VPAD_DPAD].y * SH + 0.6f * 0.25f * SH / 2;
            u64 b;
            i32 st[4];
            assert(vpad_ini("controls", "1") && !vpad_shown()); /* on, but under the menu */
            overlay_pointer_down(SNX_TOUCH_MOUSE, dx, dy, area);
            vpad_state(&b, st);
            assert(!b && overlay_pointer_up(SNX_TOUCH_MOUSE, dx, dy, SW, SH) == OVERLAY_CLOSE);
            overlay_toggle();
            assert(!overlay_shown() && vpad_shown());
            overlay_toggle();
            assert(overlay_shown() && !vpad_shown());
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
    assert(qv(&list.q[0]).h == 12 * 32 && text_at(1, 3, "\xFE Player") && shows("< All >"));
    assert(text_at(1, 9, "\xFE Assign players") && tap(6, 3) == OVERLAY_CONTROLLERS); /* the controller applet's dialog */
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
    assert(tap(7, 3) == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  MENU") && overlay_select() == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  CONTROLLERS") && overlay_back() == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  MENU"));
    /* the save states' pages: a row per slot, the auto one first, then a slot's Save, Load and Delete, Load, Delete
       and Save over a state asked first. The list is read again each time the page opens */
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
        /* its state replaced by Save or gone with Delete, no way back: the failure colour, Save in focus written on it;
           Load, which keeps what was played since in the auto slot at most, the warning colour */
        assert(shows_as("\xFE Save", 0, LIGHT_RGBA) && shows_as("\xFE Load", 0, WARNING_RGBA) &&
               shows_as("\xFE Delete", 0, FAILURE));
        assert(overlay_select() == OVERLAY_NONE); /* Save over its state: asked, Cancel in focus */
        draw();
        assert(shows("SCARLETNX  /  SAVE OVER SLOT 2?") && shows("is replaced.") && shows_as("\xFE Save", 0, FAILURE));
        assert(overlay_select() == OVERLAY_NONE); /* Cancel: back to the slot, Save in focus */
        draw();
        assert(shows("SCARLETNX  /  SLOT 2"));
        assert(overlay_select() == OVERLAY_NONE);
        overlay_move(-1);
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
        assert(shows("SCARLETNX  /  SLOT 1") && !shows_as("\xFE Delete", 0, FAILURE)); /* nothing there: no colour */
        overlay_move(-1); /* Save: nothing replaced, nothing asked */
        assert(overlay_select() == OVERLAY_SAVE && overlay_slot() == 1);
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
    toasts_gone();
    overlay_state_saved("slot 2", 0, 1000);
    assert(overlay_toasting(1000) && !overlay_toasting(1000 + (u64)SNX_OVERLAY_TOAST_MS * 1000000u));
    overlay_toggle();
    draw();
    assert(!overlay_shown() && shows("Saved to slot 2"));
    overlay_state_saved("slot 2", 1, 1000); /* a slot that held a state: said replaced, F7 asking nothing */
    draw();
    assert(shows("Saved over slot 2"));
    overlay_state_saved("slot 2", 0, 1000);
    draw();
    /* over the game it is a blank cell, its icon, the menu's Save States' (the save icon, the launcher's too), a blank
       cell, its letters and a blank cell, a row from the corner, each backed by the layer's ground, the menu's ink, in
       its page's colour, as a line of the stats box: a band of cells side by side with no fill under them. Its letters
       are the stats box's and showcase's, a step under the menu's, which drew them a size larger than every other text
       over a running game */
    {
        static const char said[] = "Saved to slot 2";
        const u32 page = 0xFF000000u | (SNX_GUI_THEME_YORHA_PAGE & 0xFFu) << 16 | (SNX_GUI_THEME_YORHA_PAGE & 0xFF00u) |
                         (SNX_GUI_THEME_YORHA_PAGE >> 16 & 0xFFu);
        int i, t = stats_scale(s);
        assert(s > 1 && t < s); /* a picture where the two sizes differ */
        assert(list.n == (int)strlen(said) + 4 && ui_ground() == ui_rgba(ink));
        assert(toast_icon(said) == GUI_ICON_SAVE);
        for (i = 0; i < list.n; ++i) {
            SnxUiQuadView q = qv(&list.q[i]);
            int cell = i < 2 ? 1 + i : i + 2; /* the icon takes two */
            assert(q.backed && q.rgba == ui_rgba(page) && q.y == (float)(16 * t) && q.h == (float)(16 * t));
            assert(q.x == (float)(8 * t * cell) && q.w == (float)(8 * t * (i == 1 ? 2 : 1)));
            if (i == 1)
                assert(q.ink == UI_ICON && q.uw == 16 && q.vh == 16);
            else
                assert(q.ink == UI_GLYPH && q.uw == 8 && q.vh == 16 &&
                       q.glyph == (u8)(i > 2 && i < list.n - 1 ? said[i - 3] : ' '));
        }
    }
    /* the 3DS's, the Vita's and the GBA's notice (common/utilities/notice.h) is this band: its letters' scale, its
       place and its size here, and the scale on pictures and displays of every size */
    {
        static const int widths[] = {320, 640, 1280, 1920, 2424, 2560, 3840}, heights[] = {240, 360, 720, 1080, 2160};
        static const float scales[] = {1.0f, 1.25f, 1.5f, 2.0f, 2.6f};
        SnxNotice n = {0};
        int rect[4], t = stats_scale(s), sc, x, y;
        SnxUiQuadView last;
        overlay_state_toast("", 1000);
        overlay_toast(GUI_ICON_NONE, "Saved to slot 2", 1000); /* its icon aside: the 3DS's notice has none */
        draw();
        last = qv(&list.q[list.n - 1]);
        snx_notice_show(&n, "Saved to slot 2", 0);
        assert(snx_notice_scale(SW, SH, overlay_display_percent()) == t && snx_notice_rect(&n, SW, SH, t, rect));
        assert(rect[0] == (int)qv(&list.q[0]).x && rect[1] == (int)qv(&list.q[0]).y);
        assert(rect[2] == (int)(last.x + last.w) - rect[0] && rect[3] == (int)qv(&list.q[0]).h);
        for (size_t w = 0; w < sizeof widths / sizeof *widths; ++w)
            for (size_t h = 0; h < sizeof heights / sizeof *heights; ++h)
                for (size_t d = 0; d < sizeof scales / sizeof *scales; ++d) {
                    overlay_set_display_scale(scales[d]);
                    overlay_place(widths[w], heights[h], &x, &y, &sc);
                    assert(snx_notice_scale(widths[w], heights[h], snx_display_percent(scales[d])) == stats_scale(sc));
                }
        overlay_set_display_scale(1.0f);
        overlay_place(SW, SH, &x, &y, &sc);
        assert(sc == s);
        overlay_toast(GUI_ICON_NONE, "", 1000);
        overlay_state_saved("slot 2", 0, 1000);
    }
    /* the stats' box in that corner too: the band goes under it, from the box's left, half a line apart; back in its
       corner with the box in another, or off; and in the picture's top right where there is no room under the box */
    {
        static const int all[4] = {0, 0, SW, SH};
        static SnxUiQuad kept[1024];
        SnxUiList box_quads = {kept, 0, 1024};
        const IniOpt* corner = ini_find(g_display_ini, "stats_corner");
        float box[4], cw = (float)(8 * stats_scale(s)), ch = (float)(16 * stats_scale(s)); /* the band's cell */
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
        assert(list.n == (int)strlen("Saved to slot 2") + 4 && shows("Saved to slot 2"));
        assert(qv(&list.q[0]).x == box[0] && qv(&list.q[0]).y == box[1] + box[3] + ch / 2);
        h = (int)(box[1] + box[3]) + 4; /* a picture that ends four pixels under the box */
        overlay_place(SW, h, &short_x, &short_y, &short_s);
        list.n = 0;
        overlay_draw(&list, SW, h, 0);
        assert(list.n == (int)strlen("Saved to slot 2") + 4);
        assert(qv(&list.q[0]).y == (float)(16 * stats_scale(short_s)) &&
               qv(&list.q[list.n - 1]).x + qv(&list.q[list.n - 1]).w == (float)(SW - 8 * stats_scale(short_s)));
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
    toasts_gone();
    /* a notice from any thread (RenderDoc's): the notification once the next frame draws it, the last given before
       then, with its icon, and taken once */
    overlay_notice(GUI_ICON_FIRST, "RenderDoc: written in 2 s: one.rdc (2 KiB)");
    overlay_notice(overlay_key_icon(HOTKEY_CAPTURE), "RenderDoc: written in 3 s: two.rdc (2 KiB)");
    assert(!overlay_toasting(0));
    draw();
    assert(shows("RenderDoc: written in 3 s: two.rdc (2 KiB)") && !shows("one.rdc") && overlay_toasting(0));
    assert(toast_icon("RenderDoc: written in 3 s") == GUI_ICON_IMAGE);
    toasts_gone();
    assert(!overlay_toasting(0));
    draw();
    assert(!list.n);
    /* each notification after the icon the launcher and the menu give what it says. A runtime key's, its row's on F1's
       page: a screenshot's the picture (no camera among the icons), a pause's the play, the quit's warning the door */
    assert(overlay_key_icon(HOTKEY_SCREENSHOT) == GUI_ICON_IMAGE && overlay_key_icon(HOTKEY_PAUSE) == GUI_ICON_PLAY);
    assert(overlay_key_icon(HOTKEY_QUIT) == GUI_ICON_LOG_OUT && overlay_key_icon(HOTKEY_CAPTURE) == GUI_ICON_IMAGE);
    assert(overlay_key_icon(-1) == GUI_ICON_NONE);
    /* a setting changed, its group's in the launcher's Settings (Performance's gauge, Display's screen, Graphics'
       picture, Debug's bug, Overlay's pointer for On-screen controls), or its page's where that has one */
    assert(overlay_setting_icon("fps_limit") == GUI_ICON_GAUGE && gui_group_icon("Performance") == GUI_ICON_GAUGE);
    assert(overlay_setting_icon("showcase") == GUI_ICON_MONITOR && overlay_setting_icon("stats") == GUI_ICON_MONITOR);
    assert(overlay_setting_icon("docked") == GUI_ICON_MONITOR && gui_group_icon("Display") == GUI_ICON_MONITOR);
    assert(overlay_setting_icon("upscaler") == GUI_ICON_IMAGE &&
           overlay_setting_icon("anti_aliasing") == GUI_ICON_IMAGE);
    assert(overlay_setting_icon("log_max") == GUI_ICON_BUG &&
           overlay_setting_icon("controls_scale") == GUI_ICON_POINTER);
    assert(overlay_setting_icon("gamepad_rumble") == gui_group_icon("Controllers"));
    assert(overlay_setting_icon("gamepad_p1_rumble") == gui_group_icon("Controllers")); /* Player's page has none */
    {
        const IniOpt* o = NULL;
        int k, t;
        for (k = 0; k < g_ini_sections_count && !o; ++k)
            for (t = 0; t < 3 && g_ini_sections[k].table[t] && !o; ++t)
                if (g_ini_sections[k].page[t] && !strcmp(g_ini_sections[k].page[t], "Vulkan Extensions"))
                    o = g_ini_sections[k].table[t];
        assert(o && o->key && overlay_setting_icon(o->key) == GUI_ICON_GPU); /* Graphics' Vulkan Extensions */
    }
    assert(overlay_setting_icon("no_such_key") == GUI_ICON_NONE);
    overlay_setting_toast("fps_limit", "FPS limit: off", 0);
    draw();
    assert(shows("FPS limit: off") && toast_icon("FPS limit: off") == GUI_ICON_GAUGE);
    toasts_gone();
    overlay_state_toast("Slot 3: 2026-10-22 14:05  0:02:00", 0);
    draw();
    assert(toast_icon("Slot 3: ") == GUI_ICON_SAVE);
    toasts_gone();
    /* A line comes in and goes as the launcher's screens fade from black and to it, over its theme's fade_in_ms and
       fade_out_ms, on the launcher's curve and black (run_theme.h's snx_fade_done and snx_fade_shade, as
       src/gui/main.c's overlay takes them): what that black leaves of the screen, 0 at its start, 128 at the half
       (the black's 127), whole at the end; the same going. While it fades its quads are a faded span of the layer at
       that alpha, the same quads from frame to frame; whole, there is no span */
    {
        const u64 t0 = 5000000000ull, ms = 1000000ull, until = t0 + (u64)SNX_OVERLAY_TOAST_MS * ms;
        static const char said[] = "Compiling shaders";
        const SnxRunTheme theme = {.fade_in_ms = 250, .fade_out_ms = 500};
        SnxUiQuad held[64];
        SnxUiFade fades[SNX_UI_FADES];
        u32 taken[2];
        int n = (int)strlen(said) + 4;
        overlay_fades(&theme, taken);
        assert(taken[0] == 250 && taken[1] == 500);
        overlay_toast(SNX_STATS_ICON_COMPILE, said, t0);
        assert(overlay_toast_alpha(0, t0) == 0 && overlay_toast_alpha(0, t0 + 125 * ms) == 128);
        assert(overlay_toast_alpha(0, t0 + 200 * ms) ==
               255 - snx_fade_shade(1.0f - snx_fade_done(t0 / ms + 200, t0 / ms, 250)));
        assert(overlay_toast_alpha(0, t0 + 250 * ms) == 255 && overlay_toast_alpha(0, t0 + 1000 * ms) == 255);
        assert(overlay_toast_alpha(0, until - 500 * ms) == 255 && overlay_toast_alpha(0, until - 250 * ms) == 128);
        assert(overlay_toast_alpha(0, until - 100 * ms) ==
               255 - snx_fade_shade(snx_fade_done(until / ms - 100, until / ms - 500, 500)));
        assert(overlay_toast_alpha(0, until - ms) == 1 &&
               overlay_toast_alpha(0, until) == 0); /* black 254, then gone */
        assert(overlay_toast_alpha(1, t0 + 1000 * ms) == 0 && overlay_toast_alpha(-1, t0 + 1000 * ms) == 0); /* none */
        /* at 0: its quads, all of their black */
        assert(draw_fades(t0, fades) == 1 && list.n == n && fades[0].alpha == 0 && !fades[0].first &&
               fades[0].end == n);
        assert(draw_fades(t0 + 125 * ms, fades) == 1 && list.n == n && fades[0].alpha == 128 && fades[0].end == n);
        memcpy(held, list.q, sizeof *held * (size_t)n);
        assert(draw_fades(t0 + 200 * ms, fades) == 1 && list.n == n &&
               fades[0].alpha == (unsigned)overlay_toast_alpha(0, t0 + 200 * ms) &&
               !memcmp(held, list.q, sizeof *held * (size_t)n));
        /* whole: no span, the same quads */
        assert(!draw_fades(t0 + 1000 * ms, fades) && list.n == n && !memcmp(held, list.q, sizeof *held * (size_t)n));
        assert(draw_fades(until - 250 * ms, fades) == 1 && list.n == n && fades[0].alpha == 128 &&
               !memcmp(held, list.q, sizeof *held * (size_t)n));
        /* behind the menu's quads, the span is the notification's alone */
        overlay_toggle();
        assert(draw_fades(until - 250 * ms, fades) == 1 && fades[0].alpha == 128 && fades[0].first > 0 &&
               fades[0].end == list.n && fades[0].end - fades[0].first == n);
        overlay_toggle();
        /* two lines, each fading on its own: the one given later, under it, its own span and alpha; given again, a
           line keeps its fade in, its words changed */
        overlay_toast(SNX_STATS_ICON_ENCODE, "Encoding textures", t0 + 100 * ms);
        assert(draw_fades(t0 + 200 * ms, fades) == 2 && fades[0].first == 0 && fades[0].end == n &&
               fades[1].first == n && fades[1].end == list.n);
        assert(fades[0].alpha == (unsigned)overlay_toast_alpha(0, t0 + 200 * ms) &&
               fades[1].alpha == (unsigned)overlay_toast_alpha(1, t0 + 200 * ms) && fades[1].alpha < fades[0].alpha);
        assert(overlay_toast_alpha(1, t0 + 225 * ms) == 128); /* its own half */
        overlay_toast(SNX_STATS_ICON_ENCODE, "Encoding again", t0 + 600 * ms);
        assert(!draw_fades(t0 + 600 * ms, fades) && shows("Encoding again") && !shows("Encoding textures"));
        assert(overlay_toast_alpha(1, t0 + 600 * ms) == 255);
        overlay_toast(SNX_STATS_ICON_ENCODE, "", t0 + 600 * ms);
        /* YoRHa's built in, where the launcher's theme can't be read: its theme.ini's (gui_theme_colors.h) */
        overlay_fades(NULL, taken);
        assert(taken[0] == SNX_GUI_THEME_YORHA_FADE_IN_MS && taken[1] == SNX_GUI_THEME_YORHA_FADE_OUT_MS);
        assert(overlay_toast_alpha(0, t0 + SNX_GUI_THEME_YORHA_FADE_IN_MS / 2 * ms) ==
               255 - snx_fade_shade(
                         1.0f - snx_fade_done(SNX_GUI_THEME_YORHA_FADE_IN_MS / 2, 0, SNX_GUI_THEME_YORHA_FADE_IN_MS)));
        /* a fade out longer than the notification, given as the clock starts: 3 s of its 10 s left at its start */
        toasts_gone();
        overlay_fades(&(SnxRunTheme){.fade_out_ms = 10000}, taken);
        overlay_toast(SNX_STATS_ICON_COMPILE, said, 1000);
        assert(overlay_toast_alpha(0, 1000) == 77 && overlay_toast_alpha(0, 1500 * ms) == 39);
        toasts_gone();
        overlay_toast(SNX_STATS_ICON_COMPILE, said, t0);
        /* a theme with no fades: whole from its start to its end */
        overlay_fades(&(SnxRunTheme){0}, taken);
        assert(!taken[0] && !taken[1] && overlay_toast_alpha(0, t0) == 255 &&
               overlay_toast_alpha(0, until - ms) == 255);
        toasts_gone();
    }
    /* One a line, the oldest at the top, each a band of its own after its icon: a setting changed its own line, the
       next change of its key taking that line where it is; another's by its icon (a save state's slot picked, then
       saved: one line). OVERLAY_TOASTS at most: one more takes the oldest's place. Each goes when its own time is up,
       those under it up a line; empty words take their subject's line out */
    {
        const u64 ms = 1000000ull;
        const int t = stats_scale(s);
        const float ch = 16.0f * (float)t, cw = 8.0f * (float)t;
        SnxUiFade fades[SNX_UI_FADES];
        float x, y;
        assert(OVERLAY_TOASTS == 4 && OVERLAY_TOASTS <= SNX_UI_FADES);
        overlay_setting_toast("fps_limit", "FPS limit: off", 0);
        overlay_setting_toast("showcase", "Showcase: on", 100 * ms);
        assert(!draw_fades(200 * ms, fades) && list.n == (int)(strlen("FPS limit: off") + strlen("Showcase: on")) + 8);
        assert(text_xy("FPS limit: off", &x, &y) && x == 5 * cw && y == ch &&
               toast_icon("FPS limit") == GUI_ICON_GAUGE);
        assert(text_xy("Showcase: on", &x, &y) && x == 5 * cw && y == 2 * ch &&
               toast_icon("Showcase: on") == GUI_ICON_MONITOR);
        overlay_setting_toast("fps_limit", "FPS limit: the game's pace", 300 * ms); /* F4 again: its line */
        draw_fades(300 * ms, fades);
        assert(!shows("FPS limit: off") && text_xy("FPS limit: the game's pace", &x, &y) && y == ch);
        assert(text_xy("Showcase: on", &x, &y) && y == 2 * ch);
        overlay_setting_toast("stats", "Stats: on", 400 * ms); /* Display's icon too, its key its own line */
        draw_fades(400 * ms, fades);
        assert(text_xy("Stats: on", &x, &y) && y == 3 * ch && toast_icon("Stats: on") == GUI_ICON_MONITOR);
        overlay_state_toast("Slot 2: 2026-10-22 14:05  0:02:00", 500 * ms);
        overlay_state_saved("slot 2", 0, 600 * ms);
        draw_fades(600 * ms, fades);
        assert(!shows("Slot 2: ") && text_xy("Saved to slot 2", &x, &y) && y == 4 * ch);
        overlay_toast(overlay_key_icon(HOTKEY_PAUSE), "Pause: on", 700 * ms); /* a fifth: FPS limit's goes */
        draw_fades(700 * ms, fades);
        assert(!shows("FPS limit") && text_xy("Showcase: on", &x, &y) && y == ch);
        assert(text_xy("Stats: on", &x, &y) && y == 2 * ch && text_xy("Saved to slot 2", &x, &y) && y == 3 * ch);
        assert(text_xy("Pause: on", &x, &y) && y == 4 * ch && toast_icon("Pause: on") == GUI_ICON_PLAY);
        /* the showcase's time up, given at 100 ms: the rest up a line */
        draw_fades(100 * ms + (u64)SNX_OVERLAY_TOAST_MS * ms, fades);
        assert(!shows("Showcase") && text_xy("Stats: on", &x, &y) && y == ch && text_xy("Pause: on", &x, &y) &&
               y == 3 * ch);
        assert(overlay_toasting(700 * ms + (u64)SNX_OVERLAY_TOAST_MS * ms - 1) &&
               !overlay_toasting(700 * ms + (u64)SNX_OVERLAY_TOAST_MS * ms));
        overlay_setting_toast("stats", "", 3200 * ms);
        draw_fades(3200 * ms, fades);
        assert(!shows("Stats") && text_xy("Saved to slot 2", &x, &y) && y == ch);
        toasts_gone();
        assert(!overlay_toasting(0));
    }
    /* the settings off their default that change how fast the game runs (options_perf_notice): nothing before the
       game's first frame, then the game's keys after a gamepad, the launcher's after a globe and this kind of game's
       values after an info mark, a line each, for SNX_OVERLAY_SETTINGS_MS from that frame; given once, and none for no
       key. The kind's alone is a notice too */
    {
        const u64 ms = 1000000ull, t0 = 9000 * ms;
        SnxUiFade fades[SNX_UI_FADES];
        float x, y;
        overlay_settings_notice("", "", "");
        assert(!overlay_settings_waiting());
        overlay_settings_notice("", "", "emulated_cores Off");
        assert(overlay_settings_waiting());
        overlay_settings_notice("fps_limit, jit_opt", "opt, llvm_batch", "emulated_cores Off");
        assert(overlay_settings_waiting());
        draw_fades(t0, fades);
        assert(!list.n && !overlay_toasting(t0)); /* no frame of the game's yet */
        overlay_settings_start(t0);
        assert(!overlay_settings_waiting());
        draw_fades(t0 + 1000 * ms, fades);
        assert(text_xy("fps_limit, jit_opt", &x, &y) && y == 16.0f * (float)stats_scale(s) &&
               toast_icon("fps_limit, jit_opt") == GUI_ICON_GAMEPAD_2);
        assert(text_xy("opt, llvm_batch", &x, &y) && y == 32.0f * (float)stats_scale(s) &&
               toast_icon("opt, llvm_batch") == GUI_ICON_GLOBE);
        assert(text_xy("emulated_cores Off, for this kind of game", &x, &y) && y == 48.0f * (float)stats_scale(s) &&
               toast_icon("emulated_cores Off, for this kind of game") == GUI_ICON_INFO);
        assert(overlay_toasting(t0 + (u64)SNX_OVERLAY_SETTINGS_MS * ms - 1) &&
               !overlay_toasting(t0 + (u64)SNX_OVERLAY_SETTINGS_MS * ms) && SNX_OVERLAY_SETTINGS_MS == 30000);
        overlay_settings_start(t0 + 2000 * ms); /* taken already: nothing again */
        draw_fades(t0 + (u64)SNX_OVERLAY_SETTINGS_MS * ms, fades);
        assert(!list.n);
        toasts_gone();
    }
    /* its lines: as many keys a line as fit it, the game's first, then the launcher's, then this kind of game's, one
       kept for each list after while it has keys, OVERLAY_SETTINGS_LINES in all; keys past a list's last line end it
       with how many more there are, as many of its own keys left out as that takes; a key longer than a line cut there.
       The kind's last line ends with what its keys are, its keys narrowed to leave room for those words */
    {
        unsigned icon[OVERLAY_SETTINGS_LINES];
        char text[OVERLAY_SETTINGS_LINES][OVERLAY_COLS_MAX + 1];
        assert(OVERLAY_SETTINGS_LINES == 3);
        assert(overlay_settings_lines("", "", "", 72, icon, text) == 0);
        assert(overlay_settings_lines("a, b", "", "", 72, icon, text) == 1 && !strcmp(text[0], "a, b") &&
               icon[0] == GUI_ICON_GAMEPAD_2);
        assert(overlay_settings_lines("", "opt", "", 72, icon, text) == 1 && !strcmp(text[0], "opt") &&
               icon[0] == GUI_ICON_GLOBE);
        assert(overlay_settings_lines("alpha, beta, gamma", "", "", 12, icon, text) == 2 &&
               !strcmp(text[0], "alpha, beta") && !strcmp(text[1], "gamma") && icon[1] == GUI_ICON_GAMEPAD_2);
        assert(overlay_settings_lines("alpha, beta, gamma, delta, epsilon", "opt", "", 12, icon, text) == 3);
        assert(!strcmp(text[0], "alpha, beta") && !strcmp(text[1], "+3 more") && icon[1] == GUI_ICON_GAMEPAD_2);
        assert(!strcmp(text[2], "opt") && icon[2] == GUI_ICON_GLOBE);
        assert(overlay_settings_lines("alpha, beta, gamma, delta, epsilon", "", "", 16, icon, text) == 3);
        assert(!strcmp(text[0], "alpha, beta") && !strcmp(text[1], "gamma, delta") &&
               !strcmp(text[2], "epsilon")); /* three lines of its own: all of them */
        assert(overlay_settings_lines("a, b, c, d, e, f, g, h, i, j", "", "", 7, icon, text) == 3);
        assert(!strcmp(text[0], "a, b, c") && !strcmp(text[1], "d, e, f") && !strcmp(text[2], "+4 more"));
        assert(overlay_settings_lines("vk_ext_vertex_input_dynamic_state", "", "", 12, icon, text) == 1 &&
               !strcmp(text[0], "vk_ext_verte"));
        assert(overlay_settings_lines("x", "a, b, c, d, e, f, g", "", 7, icon, text) == 3 && !strcmp(text[0], "x") &&
               !strcmp(text[1], "a, b, c") && !strcmp(text[2], "+4 more") && icon[2] == GUI_ICON_GLOBE);
        /* the kind's alone: its line, after an info mark */
        assert(overlay_settings_lines("", "", "emulated_cores Off", 72, icon, text) == 1 &&
               !strcmp(text[0], "emulated_cores Off, for this kind of game") && icon[0] == GUI_ICON_INFO);
        /* all three: a line each, the game's narrowed to one, the kind's last */
        assert(overlay_settings_lines("alpha, beta, gamma, delta", "opt", "emulated_cores Off", 12, icon, text) == 3);
        assert(!strcmp(text[0], "+4 more") && icon[0] == GUI_ICON_GAMEPAD_2);
        assert(!strcmp(text[1], "opt") && icon[1] == GUI_ICON_GLOBE);
        assert(!strcmp(text[2], "emulated_cor") && icon[2] == GUI_ICON_INFO); /* narrower than its words: cut */
        assert(overlay_settings_lines("alpha, beta, gamma, delta", "opt", "a Off, b On", 72, icon, text) == 3);
        assert(!strcmp(text[0], "alpha, beta, gamma, delta") && !strcmp(text[2], "a Off, b On, for this kind of game"));
        /* the kind's keys narrowed for its words: on the lines it has, the words after the last; past its one line,
           how many more, then the words */
        assert(overlay_settings_lines("", "", "alpha On, beta Off, gamma On", 40, icon, text) == 3 &&
               !strcmp(text[0], "alpha On") && !strcmp(text[1], "beta Off") &&
               !strcmp(text[2], "gamma On, for this kind of game") && icon[2] == GUI_ICON_INFO);
        assert(overlay_settings_lines("a", "b", "alpha On, beta Off, gamma On", 40, icon, text) == 3 &&
               !strcmp(text[2], "alpha On, +2 more, for this kind of game"));
    }

    /* the caches of earlier runs found empty as the game starts: the shaders' words, else the textures'; none for
       neither, nor while the shader cache is still read (-1), which then says nothing to keep; said once a run, as
       the next frame's notification */
    assert(!overlay_cold_cache_words(0, 0) && !strcmp(overlay_cold_cache_words(1, 0), "Compiling shaders"));
    assert(!strcmp(overlay_cold_cache_words(1, 1), "Compiling shaders"));
    assert(!strcmp(overlay_cold_cache_words(0, 1), "Encoding textures"));
    assert(!overlay_cold_cache(0, 0) && !overlay_cold_cache(-1, 1) && !overlay_cold_cache(-1, 0));
    draw();
    assert(!list.n);
    assert(overlay_cold_cache(0, 1));
    draw();
    assert(shows("Encoding textures") && overlay_toasting(0) &&
           toast_icon("Encoding textures") == SNX_STATS_ICON_ENCODE);
    toasts_gone();
    assert(!overlay_cold_cache(1, 0) && !overlay_cold_cache(1, 1)); /* once */
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
