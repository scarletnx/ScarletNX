/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/platform/overlay.h"
#include "nx/runtime/platform/ctrl_applet.h"
#include "nx/runtime/platform/tracy.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/fatal_screen.h"
#include "nx/runtime/platform/gamepad.h"
#include "nx/runtime/platform/hotkeys.h"
#include "nx/runtime/platform/hud.h"
#include "nx/runtime/platform/plat.h" /* the notices' lock */
#include "nx/runtime/platform/prepare_screen.h"
#include "nx/runtime/platform/stats.h"
#include "nx/runtime/platform/touch.h"
#include "nx/runtime/platform/unattended.h"
#include "nx/runtime/platform/vpad.h"
#include "nx/runtime/options.h"
#include "nx/gpu/renderer.h"
#include "nx/runtime/platform/ui_colors.h"
#include "gui/icons.h"
#include "common/utilities/pad_screen.h"
#include "common/utilities/users.h"
#include "common/utilities/common.h"
#include "common/utilities/bits.h"
#include <SDL3/SDL_keycode.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

enum { FIRST = 3, VISIBLE = OVERLAY_ROWS - FIRST - 1 }; /* the items' first row, and how many show at once */
enum {
    PAGE_MAIN,
    PAGE_CONTROLS,
    PAGE_BUTTONS,
    PAGE_PADS,
    PAGE_MAP,
    PAGE_SHORTCUTS,
    PAGE_STATES,
    PAGE_SLOT,
    PAGE_ASK,
    PAGE_KEYS,
    PAGE_SETTINGS, /* the game's .ini's sections the menu has no page of its own for (settings_entries) */
    PAGE_SECTION   /* one of them: its rows */
};
/* the first page's items: Capture frame only while RenderDoc can capture, Clean Old Settings while the game's files
   keep old keys (options.h's ini_old_settings) */
enum {
    M_CONTROLS,
    M_PADS,
    M_STATES,
    M_SHORTCUTS,
    M_TV_MODE,
    M_STATS,
    M_VOLUME,
    M_SETTINGS,
    M_CAPTURE,
    M_CLEAN,
    M_EXIT,
    M_COUNT
};
/* their icons; Clean Old Settings' is its notification's too */
static const unsigned MAIN_ICONS[M_COUNT] = {
    GUI_ICON_POINTER, GUI_ICON_GAMEPAD_2, GUI_ICON_SAVE,     GUI_ICON_KEYBOARD,
    GUI_ICON_MONITOR, GUI_ICON_ACTIVITY,  GUI_ICON_VOLUME_2, GUI_ICON_SLIDERS_HORIZONTAL,
    GUI_ICON_IMAGE,   GUI_ICON_ERASER,    GUI_ICON_LOG_OUT};
/* the Settings page's entries: the sections of the game's .ini (settings.txt's) the launcher's Settings group, those
   the menu has a page of its own for left out, in the launcher's order with Debug last, a section's own page (Vulkan
   Extensions) after it. Each a table of g_ini_sections, the game's own rows (INI_IN_GAME) left out of its page */
static const char* const SETTINGS_SECTIONS[] = {"Display", "Graphics", "Audio", "Performance", "Network", "Debug"};
enum { ENTRIES_MAX = 3 * (int)(sizeof SETTINGS_SECTIONS / sizeof *SETTINGS_SECTIONS) };
static struct {
    int section, table;
} entries[ENTRIES_MAX];
static int nentries, entry_at; /* how many; the one PAGE_SECTION shows */
static void settings_entries(void) {
    size_t k;
    int s, t;
    const IniOpt* o;
    nentries = 0;
    for (k = 0; k < sizeof SETTINGS_SECTIONS / sizeof *SETTINGS_SECTIONS; ++k)
        for (s = 0; s < g_ini_sections_count; ++s)
            for (t = 0; !strcmp(g_ini_sections[s].name, SETTINGS_SECTIONS[k]) && t < 3 && g_ini_sections[s].table[t];
                 ++t) {
                for (o = g_ini_sections[s].table[t]; o->key && (o->flags & INI_IN_GAME); ++o) continue;
                if (o->key && nentries < ENTRIES_MAX) {
                    entries[nentries].section = s;
                    entries[nentries++].table = t;
                }
            }
}
static const char* entry_name(int e) {
    const IniSection* sec = &g_ini_sections[entries[e].section];
    return sec->page[entries[e].table] ? sec->page[entries[e].table] : sec->name;
}
/* entry e's row i, NULL past its last */
static const IniOpt* entry_row(int e, int i) {
    const IniOpt* o;
    for (o = g_ini_sections[entries[e].section].table[entries[e].table]; o->key; ++o)
        if (!(o->flags & INI_IN_GAME) && !i--) return o;
    return NULL;
}
static int entry_rows(int e) {
    int n = 0;
    while (entry_row(e, n)) ++n;
    return n;
}
enum { S_SAVE, S_LOAD, S_DELETE, S_BACK, S_COUNT };              /* a slot's page's, the auto slot without Save */
enum { P_PLAYER, P_SWAP, P_DEADZONE, P_RUMBLE, P_BACKGROUND, P_MAP, P_ASSIGN, P_BACK, P_COUNT }; /* the gamepads' */
/* the controls page's items */
enum {
    C_SHOW,
    C_EDIT,
    C_SNAP,
    C_SCALE,
    C_STICK,
    C_DPAD,
    C_HAPTICS,
    C_TOUCH,
    C_BUTTONS,
    C_RESET,
    C_BACK,
    C_COUNT
};
/* the settings among them by their keys, which name them and their values (the .ini's tables, as the launcher's
   Settings show them); the others, actions, by their names */
static const char* const CONTROLS[C_COUNT] = {"controls",
                                              "Edit layout",
                                              "controls_snap",
                                              "controls_scale",
                                              "controls_rel_center",
                                              "controls_dpad_slide",
                                              "controls_haptics",
                                              "touch",
                                              SNX_VPAD_BUTTONS,
                                              "Reset layout",
                                              "Back"};
static const char* const PADS[P_COUNT] = {SNX_GAMEPAD_PLAYERS,
                                          "gamepad_swap_ab",
                                          "gamepad_deadzone",
                                          "gamepad_rumble",
                                          "gamepad_background",
                                          SNX_GAMEPAD_MAP,
                                          "Assign players",
                                          "Back"};
/* F1's page: the runtime's keys as set (hotkeys.h; backend/sdl3.c's hotkey and menu_event) and the game's buttons on
   the keyboard (its keymap), a row each with the icon the launcher's F1 box gives what it does (src/gui/main.c's
   key_icon): a binding changed there changes here. With the keys at their defaults each row fits the narrowest panel,
   OVERLAY_COLS, its what after the widest keys */
static const struct {
    unsigned icon;
    int action;      /* a runtime key's (hotkeys.h), whose row shows while it has a key; -1 for the game's buttons */
    const char* key; /* the game's buttons' keys, or what goes with the runtime key (a controller's Home); NULL none */
    const char* what;
} KEYS[] = {{GUI_ICON_APP_WINDOW, HOTKEY_MENU, "Home", "the game's menu"},
            {GUI_ICON_IMAGE, HOTKEY_SCREENSHOT, NULL, "a screenshot"},
            {GUI_ICON_GAUGE, HOTKEY_FPS_LIMIT, NULL, "FPS limit off/on"},
            {GUI_ICON_BUG, HOTKEY_THREAD_DUMP, NULL, "threads to log"},
            {GUI_ICON_MONITOR, HOTKEY_SHOWCASE, NULL, "showcase off/on"},
            {GUI_ICON_SAVE, HOTKEY_SAVE_STATE, NULL, "save to the slot"},
            {GUI_ICON_ROTATE_CCW, HOTKEY_LOAD_STATE, NULL, "load the slot"},
            {GUI_ICON_LAYERS, HOTKEY_NEXT_SLOT, NULL, "the next slot"},
            {GUI_ICON_BUG, HOTKEY_TRACY, NULL, "Tracy capture"}, /* a build with SNX_TRACY has its key */
            {GUI_ICON_PLAY, HOTKEY_PAUSE, NULL, "pause, resume"},
            {GUI_ICON_MONITOR, HOTKEY_FULLSCREEN, NULL, "fullscreen"}, /* with its second key */
            {GUI_ICON_ACTIVITY, HOTKEY_STATS, NULL, "stats off/on"},
            {GUI_ICON_IMAGE, HOTKEY_CAPTURE, NULL, "capture a frame"}, /* while RenderDoc is in the process */
            {GUI_ICON_POINTER, HOTKEY_MOUSE, NULL, "Joy-Con mouse on/off"},
            {GUI_ICON_LOG_OUT, HOTKEY_QUIT, NULL, "quit the game"},
            {GUI_ICON_MONITOR, HOTKEY_TV_MODE, NULL, "TV on/off/auto"},
            {GUI_ICON_IMAGE, HOTKEY_UPSCALER, NULL, "upscaler"},
            {GUI_ICON_IMAGE, HOTKEY_ANTI_ALIASING, NULL, "anti-aliasing"},
            {GUI_ICON_GAMEPAD, -1, "X, Space", "A"},
            {GUI_ICON_GAMEPAD, -1, "Z, C, V", "B, X, Y"},
            {GUI_ICON_GAMEPAD, -1, "Q, E", "L, R"},
            {GUI_ICON_GAMEPAD, -1, "1, 3", "ZL, ZR"},
            {GUI_ICON_GAMEPAD, -1, "Enter", "plus"},
            {GUI_ICON_GAMEPAD, -1, "Backspace", "minus"},
            {GUI_ICON_GAMEPAD_DIRECTIONAL, -1, "Arrows", "D-pad"},
            {GUI_ICON_JOYSTICK, -1, "W, A, S, D", "left stick"},
            {GUI_ICON_JOYSTICK, -1, "I, J, K, L", "right stick"}};
enum { KEYS_COUNT = sizeof KEYS / sizeof *KEYS };
/* counts the changes to what the menu and the keyboard's box show: each call below that takes input or sets their
   state adds one, and the renderer lays them out again only when it, or what they read from elsewhere, changed
   (overlay_draw) */
static _Atomic u32 g_changes;
static void changed(void) { atomic_fetch_add_explicit(&g_changes, 1, memory_order_release); }
/* RenderDoc in the process, which shows the capture key's row (overlay_keys_extra), and the debugger's name */
static int keys_capture, keys_mouse; /* keys_mouse: the mouse key's row shows (overlay_keys_mouse) */
static char keys_debugger[32];
/* KEYS' row i's keys into out (n bytes): the game's buttons', or the runtime key's as set with what goes with it,
   fullscreen's two together. 0 for a row that doesn't show: a runtime key with none, the capture key's without
   RenderDoc, the mouse key's while it is no way in and no game holds the mouse */
static int keys_text(int i, char* out, size_t n) {
    const char *key = KEYS[i].action < 0 ? "" : hotkey_text(KEYS[i].action),
               *also = KEYS[i].action == HOTKEY_FULLSCREEN ? hotkey_text(HOTKEY_FULLSCREEN_2) : KEYS[i].key;
    if (KEYS[i].action == HOTKEY_CAPTURE && !keys_capture) return 0;
    if (KEYS[i].action == HOTKEY_MOUSE && !keys_mouse) return 0;
    if (!also) also = "";
    snprintf(out, n, "%s%s%s", key, *key && *also ? ", " : "", also);
    return *out != 0;
}
/* F1's page's rows: KEYS' that show, then the debugger's while one traces the process. Row r's KEYS index, -1 for
   the debugger's */
static int keys_rows(void) {
    char text[OVERLAY_COLS_MAX + 1];
    int i, n = keys_debugger[0] != 0;
    for (i = 0; i < KEYS_COUNT; ++i) n += keys_text(i, text, sizeof text);
    return n;
}
static int keys_at(int r) {
    char text[OVERLAY_COLS_MAX + 1];
    int i;
    for (i = 0; i < KEYS_COUNT; ++i)
        if (keys_text(i, text, sizeof text) && !r--) return i;
    return -1;
}
void overlay_keys_mouse(int on) {
    if (keys_mouse == (on != 0)) return;
    keys_mouse = on != 0;
    changed();
}
void overlay_keys_extra(int capture, const char* debugger) {
    changed();
    keys_capture = capture != 0;
    snprintf(keys_debugger, sizeof keys_debugger, "%s", debugger ? debugger : "");
}
static _Atomic int shown;
static int page, focus, top; /* top: the first item showing */
/* the save state slots listed (slot 0 the auto one), the one a slot's page is for, what its question asks (OVERLAY_LOAD
   or OVERLAY_DELETE), and whether the list is to be read again */
static OverlaySlot slots[OVERLAY_SLOTS_MAX];
static int nslots = 1, slot_at, ask, slots_stale;
/* what the question asks besides the OVERLAY_ codes it gives back: the on-screen controls put back */
enum { ASK_RESET = -1, ASK_CLEAN = -2, ASK_RECORD = -3 };
/* the Save states page's rows after the slots: player 1's input recorded to a slot and one played back (unattended.h),
   then Back. The arrows choose the slot a row shows (0 none, else 1 to 9) and nothing happens before Enter takes it */
enum { T_RECORD, T_REPLAY, T_BACK, T_COUNT };
enum { TAS_SLOTS = 9 };
static int tas_pick[2], tas_frames[TAS_SLOTS + 1]; /* the rows' choices; each slot's recording in frames, as the page
                                                      opens and a row is taken: 0 none, -1 a file that is none */
static u32 slots_gen; /* counts the lists given: the picture is set again */
/* the notifications showing, a line each, the oldest first (overlay_toast) */
#define TOAST_KEY 64
typedef char snx_toasts_fade_apart[OVERLAY_TOASTS <= SNX_UI_FADES ? 1 : -1]; /* each line fades as a span of its own */
static struct {
    char text[OVERLAY_COLS_MAX + 1];
    char key[TOAST_KEY]; /* the setting it says (overlay_setting_toast), "" for another */
    unsigned icon;       /* the icon before it */
    u64 from, until;     /* when it was given, which its fade in starts from, and when its time is up */
} toasts[OVERLAY_TOASTS];
static int ntoasts;
static u32 toast_version;        /* counts the changes to the lines */
static _Atomic u64 toasts_until; /* the last line's time up, which overlay_toasting reads from any thread */
static u32 toast_fade[2];        /* overlay_fades': the fade in's and the fade out's ms */
static int pad_player; /* the player the gamepads' page shows: 0 every player's, 1 to SNX_GAMEPADS */
/* the Shortcuts page: its row in focus waits for a key, and what the last key given was refused for ("" none) */
static int listening;
static char refused[2 * OVERLAY_COLS_MAX];
/* the menu in the layer's colours (ui_colors.h); its page hides the game under it, whose text would show through
   its letters */

/* a slot's page's items: the auto slot has no Save */
static int slot_item(int i) { return slot_at ? i : i + 1; }
/* the first page's item m shown now */
static int main_shows(int m) {
    return m == M_CAPTURE ? g_renderer->capture(0) != 0 : m == M_CLEAN ? ini_old_settings() > 0 : 1;
}
/* the first page's item shown at i, those that do not show now skipped */
static int main_item(int i) {
    int m;
    for (m = 0; m < M_COUNT; ++m)
        if (main_shows(m) && !i--) return m;
    return M_COUNT;
}
static int main_count(void) {
    int m, n = 0;
    for (m = 0; m < M_COUNT; ++m) n += main_shows(m);
    return n;
}
/* the colour of item i of the page shown: Exit, which ends the game, a slot's Delete and its Save over a state have no
   way back; a slot's Load (what was played since goes, unless the auto slot keeps it) and the controls' Reset want
   care; and so the question's choice; 0 for none */
static u32 item_tone(int i) {
    int what = slot_item(i);
    if (page == PAGE_ASK)
        return i ? 0 : ask == OVERLAY_LOAD || ask == ASK_RESET || ask == ASK_CLEAN ? UI_WARNING : UI_FAILURE;
    if (page == PAGE_MAIN) return main_item(i) == M_EXIT ? UI_FAILURE : 0;
    if (page == PAGE_CONTROLS) return i == C_RESET ? UI_WARNING : 0;
    if (page != PAGE_SLOT || !slots[slot_at].when[0]) return 0;
    return what == S_DELETE || what == S_SAVE ? UI_FAILURE : what == S_LOAD ? UI_WARNING : 0;
}
/* the first page's row item m is on (it shows) */
static int main_row(int m) {
    int i = 0;
    while (main_item(i) != m && main_item(i) != M_COUNT) ++i;
    return i;
}
static int exit_row(void) { return main_row(M_EXIT); }
/* the Shortcuts page's rows: the runtime's keys this build has (hotkeys.h), then Back. The action at row i, -1 for
   Back */
static int shortcut(int i) {
    int a;
    for (a = 0; a < HOTKEY_COUNT; ++a)
        if (hotkey_row(a) && !i--) return a;
    return -1;
}
static int shortcuts(void) {
    int a, n = 0;
    for (a = 0; a < HOTKEY_COUNT; ++a) n += hotkey_row(a) != NULL;
    return n;
}
/* row i of the Save states page as a T_ row, -1 for a slot's */
static int tas_row(int i) { return page == PAGE_STATES && i >= nslots ? i - nslots : -1; }
/* the slots of the recordings read and the rows' choices back to what runs now */
static void tas_read(void) {
    char why[200];
    for (int s = 1; s <= TAS_SLOTS; ++s) tas_frames[s] = unattended_slot_frames(s, why, sizeof why);
    tas_pick[T_RECORD] = unattended_recording();
    tas_pick[T_REPLAY] = unattended_replaying();
}
static void tas_shown(int row, char* out, size_t n);
static int items(void) {
    return page == PAGE_MAIN        ? main_count()
           : page == PAGE_SHORTCUTS ? shortcuts() + 1
           : page == PAGE_CONTROLS  ? C_COUNT
           : page == PAGE_PADS      ? P_COUNT
           : page == PAGE_STATES    ? nslots + T_COUNT
           : page == PAGE_SLOT      ? S_COUNT - !slot_at
           : page == PAGE_ASK       ? 2
           : page == PAGE_KEYS      ? keys_rows()
           : page == PAGE_SETTINGS  ? nentries + 1
           : page == PAGE_SECTION   ? entry_rows(entry_at) + 1
                                    : SNX_VPAD_COUNT + 2;
}
static void page_open(int p) {
    if (p == PAGE_SETTINGS) settings_entries();
    page = p;
    focus = top = 0;
    listening = 0;
    refused[0] = 0;
    if (p == PAGE_STATES) {
        slots_stale = 1;
        tas_read();
    }
}
static _Atomic int display_percent = 100; /* set on the event thread, read by the renderer's */
/* A screen of fingers, as the backend says (a phone's): an item's row is a finger's high at the least, the launcher's
   48 pixels (src/gui's GUI_FINGER_ROW) at the display's scale, its letters in its middle, and a page shows as many
   items at once as the picture has room for. picture: its size as overlay_place last had it */
enum { FINGER_ROW = 48 };
static int fingers, picture[2];
void overlay_set_fingers(int on) {
    fingers = on != 0;
    changed();
}
/* the letters' scale on a picture sw by sh (overlay_place's) */
static int place_scale(int sw, int sh) {
    /* the tallest page sets the scale, so the letters keep their size from page to page: half the picture, or twice
       the display's scale as the menu's UI scale Auto (src/gui) if that is more, within three quarters of it. A
       phone's short picture held its letters at 2x by the half alone */
    return snx_menu_scale(sw, sh, atomic_load_explicit(&display_percent, memory_order_relaxed));
}
/* an item's row in pixels, its letters at scale s */
static int item_h(int s) {
    int finger = (FINGER_ROW * atomic_load_explicit(&display_percent, memory_order_relaxed) + 50) / 100;
    return fingers && finger > 16 * s ? finger : 16 * s;
}
/* the items a page shows at once: VISIBLE, or on a screen of fingers as many of their rows as the picture holds with
   the page's rows around them, one at the least */
static int visible(void) {
    int s, fit;
    if (!fingers || picture[1] <= 0) return VISIBLE;
    s = place_scale(picture[0], picture[1]);
    fit = (picture[1] - (FIRST + 1) * 16 * s) / item_h(s);
    return fit < 1 ? 1 : fit > VISIBLE ? VISIBLE : fit;
}
static void keep_in_view(void) {
    if (focus < top) top = focus;
    if (focus >= top + visible()) top = focus - visible() + 1;
}
static int all_shown(void) {
    int c;
    for (c = 0; c < SNX_VPAD_COUNT; ++c)
        if (!g_settings.control_shown[c]) return 0;
    return 1;
}
/* the setting an item is, NULL for an action */
static const IniOpt* setting(int i) {
    if (page == PAGE_SECTION) return entry_row(entry_at, i);
    if (page == PAGE_MAIN)
        return main_item(i) == M_STATS    ? ini_find(g_display_ini, "stats")
               : main_item(i) == M_VOLUME ? ini_find(g_audio_ini, "volume")
                                          : NULL;
    if (page == PAGE_CONTROLS) return ini_find(i == C_TOUCH ? g_backend_ini : g_vpad_ini, CONTROLS[i]);
    if (page == PAGE_PADS && i == P_PLAYER) return NULL;
    if (page == PAGE_PADS && pad_player && (i == P_SWAP || i == P_RUMBLE)) { /* the player's own */
        char key[32];
        snprintf(key, sizeof key, "gamepad_p%d_%s", pad_player, i == P_SWAP ? "swap_ab" : "rumble");
        return ini_find(g_gamepad_players_ini, key);
    }
    if (page == PAGE_PADS) return ini_find(g_gamepad_ini, PADS[i]);
    return page == PAGE_BUTTONS && i > 0 && i <= SNX_VPAD_COUNT ? &g_vpad_shown_ini[i - 1] : NULL;
}
/* the key of item i that the game's picks can hold: a setting's, and TV mode's (docked); NULL for an action */
static const char* pick_key(int i) {
    const IniOpt* o = setting(i);
    if (o) return o->key;
    return page == PAGE_MAIN && main_item(i) == M_TV_MODE ? "docked" : NULL;
}
/* item i set for this game alone, in its picks: its value marked *, Y or Delete back to Global */
static int picked(int i) {
    const char* k = pick_key(i);
    return k && ini_picked(k);
}
/* a page with an item the game's picks hold, which its second line of help says how to read */
static int page_picked(void) {
    int i, n = page == PAGE_KEYS ? 0 : items();
    for (i = 0; i < n; ++i)
        if (picked(i)) return 1;
    return 0;
}
/* a number, stepped by the arrows around it, not a word; a slider's is its own (INI_SLIDER) */
static int arrows(const IniOpt* o) {
    /* a text's words are stepped as words, one with none or of boxes only read */
    if (!o || o->type == INI_TEXT || (o->flags & INI_BOXES)) return 0;
    return o->type == INI_FLOAT || (!(o->flags & INI_SLIDER) && (!o->words || strchr(o->words, '#')));
}
static const IniOpt* slider(int i) {
    const IniOpt* o = setting(i);
    return o && (o->flags & INI_SLIDER) ? o : NULL;
}
/* a slider's value now, its knob's place in thousandths of the way from its lowest to its highest */
static int slider_place(const IniOpt* o) {
    int v = atomic_load_explicit((_Atomic const int*)o->var, memory_order_relaxed);
    return o->hi > o->lo ? (int)(((double)v - o->lo) * 1000.0 / (o->hi - o->lo) + 0.5) : 0;
}
/* the columns its value takes at its widest, with a pick's " *", which the slider ends before whatever it says */
static int slider_cols(const IniOpt* o) {
    char word[OVERLAY_COLS];
    int v, step = o->step > 0 ? o->step : 1, most = 0;
    for (v = (int)o->lo; v <= (int)o->hi; v += step) {
        ini_word(o, v, word, sizeof word);
        if ((int)strlen(word) > most) most = (int)strlen(word);
    }
    return most + 2;
}
/* the slider of item i set to the step nearest to at, thousandths of its way: stored, as a step is (act) */
static int slide_to(int i, int at) {
    const IniOpt* o = slider(i);
    int step, steps, k;
    if (!o) return OVERLAY_NONE;
    step = o->step > 0 ? o->step : 1;
    steps = (int)((o->hi - o->lo) / step);
    k = (snx_clamp_int(at, 0, 1000) * steps + 500) / 1000;
    atomic_store_explicit((_Atomic int*)o->var, (int)o->lo + k * step, memory_order_relaxed);
    vpad_store_opt(o);
    return OVERLAY_NONE;
}
/* a text's value as the menu shows it: its word (words value=word between '|'), else the text */
static void text_word(const IniOpt* o, const char* v, char* out, size_t n) {
    const char* w = o->words;
    size_t len = strlen(v);
    while (w && *w) {
        const char* end = w + strcspn(w, "|");
        const char* eq = memchr(w, '=', (size_t)(end - w));
        if (eq && (size_t)(eq - w) == len && !strncmp(w, v, len)) {
            snprintf(out, n, "%.*s", (int)(end - eq - 1), eq + 1);
            return;
        }
        w = *end ? end + 1 : end;
    }
    snprintf(out, n, "%s", v);
}
/* a text's value step words from v, round, into out: 1, or 0 for a text with no words, which the menu only reads */
static int text_step(const IniOpt* o, const char* v, int step, char* out, size_t n) {
    const char* at[64];
    const char* w = o->words;
    int count = 0, i, now = -1;
    while (w && *w && count < 64) {
        at[count] = w;
        if (!strncmp(w, v, strlen(v)) && w[strlen(v)] == '=') now = count;
        ++count;
        w += strcspn(w, "|");
        if (*w) ++w;
    }
    if (!count) return 0;
    i = now < 0 ? 0 : ((now + step) % count + count) % count;
    snprintf(out, n, "%.*s", (int)strcspn(at[i], "="), at[i]);
    return 1;
}
enum { PICK_TEXT = 256 }; /* a value the Settings page reads or steps */
/* a section row's value: the next start's where it is read at the start and that differs (ini_next_start), else the
   value in effect, as text */
static void row_now(const IniOpt* o, char* out, size_t n) {
    if ((o->flags & INI_STARTUP) && ini_next_start(o->key, out, n)) return;
    if (o->type == INI_INT)
        snprintf(out, n, "%d", atomic_load_explicit((_Atomic const int*)o->var, memory_order_relaxed));
    else if (o->type == INI_FLOAT)
        snprintf(out, n, "%g", (double)*(const float*)o->var);
    else
        snprintf(out, n, "%s", (const char*)o->var);
}
/* a section row stepped (a select steps up): the next word or number from row_now, a float by a quarter or, for one
   within 2 of its range, a twentieth; saved to the game's picks and applied by the next tick (ini_set_apply), at the
   next start for one read then. A text without words is only read */
static int section_step(const IniOpt* o, int step) {
    char now[PICK_TEXT], to[PICK_TEXT];
    if (o->flags & INI_BOXES) return OVERLAY_NONE; /* a set of items, whose boxes the launcher's Settings have */
    row_now(o, now, sizeof now);
    if (o->type == INI_INT)
        snprintf(to, sizeof to, "%d", ini_step(o, atoi(now), step));
    else if (o->type == INI_FLOAT)
        snprintf(to, sizeof to, "%g", ini_float_step(strtod(now, NULL), o->lo, o->hi, step));
    else if (!text_step(o, now, step, to, sizeof to))
        return OVERLAY_NONE;
    ini_set_apply(o->key, to);
    return OVERLAY_NONE;
}
/* an item's text, and its value at the right, * after a value the game's picks hold */
static void label(int i, char* text, size_t size, char* value, size_t vsize) {
    const IniOpt* o = setting(i);
    const char* own = picked(i) ? " *" : "";
    *value = 0;
    if (o && page == PAGE_SECTION) { /* any type; one read when the game starts says so */
        char now[PICK_TEXT], word[OVERLAY_COLS_MAX];
        row_now(o, now, sizeof now);
        if (o->flags & INI_BOXES)
            snprintf(word, sizeof word, "%s", now);
        else if (o->type == INI_INT)
            ini_word(o, atoi(now), word, sizeof word);
        else if (o->type == INI_TEXT)
            text_word(o, now, word, sizeof word);
        else
            snprintf(word, sizeof word, "%s", now);
        snprintf(text, size, (o->flags & INI_STARTUP) ? "%s (next start)" : "%s", o->name ? o->name : o->key);
        snprintf(value, vsize, arrows(o) ? "< %s%s >" : "%s%s", word, own);
        return;
    }
    if (o) {
        char word[OVERLAY_COLS];
        snprintf(text, size, "%s", o->name);
        ini_word(o, atomic_load_explicit((_Atomic const int*)o->var, memory_order_relaxed), word, sizeof word);
        snprintf(value, vsize, arrows(o) ? "< %s%s >" : "%s%s", word, own);
        return;
    }
    if (page == PAGE_SETTINGS || page == PAGE_SECTION) { /* an entry, which opens its rows; then Back */
        int last = page == PAGE_SETTINGS ? nentries : entry_rows(entry_at);
        snprintf(text, size, "%s", i < last && page == PAGE_SETTINGS ? entry_name(i) : "Back");
        if (i < last && page == PAGE_SETTINGS) snprintf(value, vsize, ">");
        return;
    }
    if (page == PAGE_MAIN) {
        static const char* const main_items[M_COUNT] = {SNX_VPAD_PAGE, SNX_GAMEPAD_PAGE, "Save states",
                                                        "Shortcuts",   "TV mode",        "Stats",
                                                        "Volume",      "Settings",       "Capture frame",
                                                        "Clean Old Settings", "Exit"};
        char word[OVERLAY_COLS];
        snprintf(text, size, "%s", main_items[main_item(i)]);
        if (main_item(i) == M_TV_MODE) { /* the choice in effect, in docked's words */
            ini_word(ini_find(g_display_ini, "docked"), options_tv_mode_choice(), word, sizeof word);
            snprintf(value, vsize, "%s%s", word, own);
        } else if (main_item(i) < M_CAPTURE)
            snprintf(value, vsize, ">");
        return;
    }
    if (page == PAGE_BUTTONS) {
        snprintf(text, size, "%s", i == 0 ? "All" : "Back");
        if (i == 0) snprintf(value, vsize, "%s", all_shown() ? "Shown" : "Some hidden");
        return;
    }
    if (page == PAGE_STATES) {
        if (tas_row(i) >= 0) {
            static const char* const tas_items[T_COUNT] = {"Record input", "Play input", "Back"};
            snprintf(text, size, "%s", tas_items[tas_row(i)]);
            if (tas_row(i) < T_BACK) { /* a choice the arrows step, as a value's row shows it */
                char shown[OVERLAY_COLS_MAX + 1];
                tas_shown(tas_row(i), shown, sizeof shown);
                snprintf(value, vsize, "< %s >", shown);
            }
            return;
        }
        if (i)
            snprintf(text, size, "Slot %d", i);
        else
            snprintf(text, size, "Auto");
        snprintf(value, vsize, "%s", slots[i].when[0] ? slots[i].when : "Empty");
        return;
    }
    if (page == PAGE_SLOT) {
        static const char* const slot_items[S_COUNT] = {"Save", "Load", "Delete", "Back"};
        snprintf(text, size, "%s", slot_items[slot_item(i)]);
        return;
    }
    if (page == PAGE_ASK) {
        snprintf(text, size, "%s",
                 i                     ? "Cancel"
                 : ask == OVERLAY_LOAD ? "Load"
                 : ask == OVERLAY_SAVE ? "Save"
                 : ask == OVERLAY_EXIT ? "Exit"
                 : ask == ASK_RESET    ? "Reset"
                 : ask == ASK_CLEAN    ? "Clean"
                 : ask == ASK_RECORD   ? "Record"
                                       : "Delete");
        return;
    }
    if (page == PAGE_SHORTCUTS) { /* the key, or the wait for one */
        int a = shortcut(i);
        snprintf(text, size, "%s", a < 0 ? "Back" : hotkey_row(a)->name);
        if (a >= 0)
            snprintf(value, vsize, "%s",
                     listening && i == focus ? "Press a key"
                     : *hotkey_text(a)       ? hotkey_text(a)
                                             : "None");
        return;
    }
    if (page == PAGE_PADS && i == P_PLAYER) { /* as Eden's tabs: every player's, or one's own */
        snprintf(text, size, "%s", SNX_GAMEPAD_PLAYERS);
        if (pad_player)
            snprintf(value, vsize, "< %d >", pad_player);
        else
            snprintf(value, vsize, "< All >");
        return;
    }
    snprintf(text, size, "%s", page == PAGE_PADS ? PADS[i] : CONTROLS[i]);
    if ((page == PAGE_CONTROLS && i == C_BUTTONS) || (page == PAGE_PADS && i == P_MAP)) snprintf(value, vsize, ">");
}
/* ---- the button map page: CONTROLS, pad_screen.c's layout over the picture ---- */
static SnxPadScreen map;
static SnxPadDrawing map_drawing; /* as last drawn: what a tap lands on */
static int map_swallow;           /* the press the page took: no menu key */
/* the pad the page is for: the player's, or with every player's the first one; its name, its labels and what it
   holds (with every player's, what any holds) */
static void map_pad(void) {
    int i = pad_player ? gamepad_of_player(pad_player - 1) : 0;
    const char* name = i >= 0 ? gamepad_name(i) : NULL;
    snprintf(map.pad, sizeof map.pad, "%s", name ? name : "");
    if (i < 0 || !gamepad_layout(i, map.labels, &map.symmetric)) {
        memset(map.labels, 0, sizeof map.labels);
        map.symmetric = 0;
    }
    map.held = i < 0 ? 0 : gamepad_held(pad_player ? i : -1);
}
static void map_open(void) {
    SnxGamepadConfig* g = gamepad_config();
    const SnxPlayerConfig* p = pad_player ? gamepad_player(pad_player - 1) : NULL;
    snx_pad_screen_start(&map);
    map.own = p && p->has_map ? p->own : g->own;
    map.swap_ab = p && g_settings.player_swap_a_b[pad_player - 1] >= 0 ? g_settings.player_swap_a_b[pad_player - 1]
                                                                       : g_settings.swap_a_b;
    map_pad();
    page = PAGE_MAP;
    focus = top = 0;
}
/* what the page did, kept in the game's .ini as the menu's other settings */
static int map_did(int did) {
    SnxGamepadConfig* g = gamepad_config();
    if (did == SNX_PAD_DID_MAP && pad_player) { /* the player's own map from now on */
        SnxPlayerConfig* p = gamepad_player(pad_player - 1);
        p->own = map.own;
        p->has_map = 1;
        gamepad_store_player_map(pad_player - 1);
    } else if (did == SNX_PAD_DID_MAP) {
        g->own = map.own;
        gamepad_store_map();
    } else if (did == SNX_PAD_DID_LEAVE) {
        page = PAGE_PADS;
        focus = P_MAP;
        top = 0;
    }
    return OVERLAY_NONE;
}
static int map_key(int key) { return map_did(snx_pad_screen_key(&map, key)); }
int overlay_listening(void) { return overlay_shown() && page == PAGE_MAP && map.listening; }
int overlay_shortcut_listening(void) { return overlay_shown() && page == PAGE_SHORTCUTS && listening; }
/* the row in focus set to key with mods, saved; refused, and said, where another row has that key */
static void shortcut_give(unsigned key, unsigned mods) {
    int a = shortcut(focus), owner;
    char name[64];
    if (a < 0) return;
    refused[0] = 0;
    if (!hotkey_assign(a, key, mods, &owner)) {
        vpad_store_opt(hotkey_row(a));
        return;
    }
    hotkey_name(key, mods, name, sizeof name);
    snprintf(refused, sizeof refused, "%s already does %s: %s keeps %s.", name, hotkey_row(owner)->name,
             hotkey_row(a)->name, *hotkey_text(a) ? hotkey_text(a) : "no key");
}
void overlay_shortcut_key(unsigned key, unsigned mods) {
    if (!overlay_shortcut_listening() || hotkey_modifier(key)) return; /* a modifier waits for its key */
    changed();
    listening = 0;
    if (key != SDLK_ESCAPE && key != SDLK_AC_BACK) shortcut_give(key, mods);
}
int overlay_delete(void) {
    const char* k = page == PAGE_KEYS || page == PAGE_MAP ? NULL : pick_key(focus);
    if (page == PAGE_SHORTCUTS) return overlay_backspace();
    if (!k || !ini_picked(k)) return OVERLAY_NONE;
    changed();
    ini_unpick(k);
    return OVERLAY_NONE;
}
int overlay_backspace(void) {
    unsigned key, mods;
    char why[160];
    int a = page == PAGE_SHORTCUTS ? shortcut(focus) : -1;
    if (a < 0) return overlay_back();
    changed();
    listening = 0;
    if (!hotkey_parse(hotkey_row(a)->def, &key, &mods, why, sizeof why)) shortcut_give(key, mods);
    return OVERLAY_NONE;
}
void overlay_press(int control) {
    if (!overlay_listening()) return;
    changed();
    map_swallow = 1;
    map_did(snx_pad_screen_press(&map, control));
}
static void user_draw(SnxUiList* l, float left, float right, float y, int s);
static void map_draw(SnxUiList* l, int sw, int sh) {
    const u32 box = UI_PANEL;
    int px, py, s, i;
    float cw, ch, x, y, w, h;
    overlay_place(sw, sh, &px, &py, &s);
    cw = 8.0f * (float)s;
    ch = 16.0f * (float)s;
    x = cw;
    y = ch;
    w = (float)sw - 2 * cw;
    h = (float)sh - 2 * ch;
    map_pad();
    ui_fill(l, x, y, w, h, UI_PAGE); /* the whole picture */
    ui_fill(l, x, y, w, ch, UI_INK);
    {
        char title[48];
        if (pad_player) /* the player's own */
            snprintf(title, sizeof title, "SCARLETNX  /  PLAYER %d  /  BUTTON MAP", pad_player);
        else
            snprintf(title, sizeof title, "SCARLETNX  /  BUTTON MAP");
        ui_text(l, x + cw, y, s, title, UI_PAGE);
        user_draw(l, x + cw * (float)(strlen(title) + 2), x + w - cw, y, s);
    }
    snx_pad_screen_lay(&map, x, y + ch, w, h - ch, cw, ch, &map_drawing);
    for (i = 0; i < map_drawing.n; ++i) {
        const SnxPadShape* p = &map_drawing.shape[i];
        u32 fill = p->style == SNX_PAD_STYLE_FOCUS ? UI_INK : p->style == SNX_PAD_STYLE_LIT ? UI_FAILURE : box;
        if (p->kind == SNX_PAD_SHAPE_TEXT)
            ui_text(l, p->x, p->y, s, p->text,
                    p->style == SNX_PAD_STYLE_FOCUS || p->style == SNX_PAD_STYLE_LIT ? UI_PAGE
                    : p->style == SNX_PAD_STYLE_SOFT                                 ? UI_SOFT
                    : p->style == SNX_PAD_STYLE_NOTE                                 ? UI_FAILURE
                                                                                     : UI_INK);
        else if (p->kind == SNX_PAD_SHAPE_DISC) /* a round part: a circle the fragment shader draws, filled */
            ui_shape(l, p->x, p->y, p->w, p->h, UI_SHAPE_CIRCLE, NULL, UI_SHAPE_FILLED, 1,
                     p->style == SNX_PAD_STYLE_PLAIN ? UI_SOFT : fill);
        else
            ui_fill(l, p->x, p->y, p->w, p->h,
                    p->kind == SNX_PAD_SHAPE_BAR && p->style == SNX_PAD_STYLE_PLAIN ? UI_SOFT : fill);
    }
}
/* what a T_ row says: what runs, and what its choice would do */
static void tas_shown(int row, char* out, size_t n) {
    int now = row == T_RECORD ? unattended_recording() : unattended_replaying(), pick = tas_pick[row];
    if (!now && !pick)
        snprintf(out, n, "Off");
    else if (pick == now)
        snprintf(out, n, row == T_RECORD ? "Slot %d, recording" : "Slot %d, playing", pick);
    else if (!pick)
        snprintf(out, n, "Off: Enter ends slot %d", now);
    else
        snprintf(out, n, row == T_RECORD && tas_frames[pick] != 0 ? "Slot %d, over its recording" : "Slot %d", pick);
}
/* a T_ row's choice a slot on (step), round 0 and 1 to TAS_SLOTS; a play's goes past the slots with no recording */
static void tas_step(int row, int step) {
    int at = tas_pick[row], n = TAS_SLOTS + 1;
    do at = (at + (step < 0 ? n - 1 : 1)) % n;
    while (row == T_REPLAY && at && tas_frames[at] <= 0 && at != tas_pick[row]);
    tas_pick[row] = at;
    changed();
}
/* a T_ row's slot set (0 ends it), what came of it said */
static void tas_apply(int row, int slot) {
    char why[200] = "", said[OVERLAY_COLS_MAX + 1];
    int ok = row == T_RECORD ? unattended_record_from_menu(slot, why, sizeof why)
                             : unattended_replay_from_menu(slot, why, sizeof why);
    if (!ok)
        snprintf(said, sizeof said, "%s", why);
    else if (slot)
        snprintf(said, sizeof said, row == T_RECORD ? "Recording to slot %d" : "Playing slot %d", slot);
    else
        snprintf(said, sizeof said, "%s", row == T_RECORD ? "Recording ended" : "Playing ended");
    overlay_notice(MAIN_ICONS[M_STATES], said);
    tas_read();
    changed();
}
/* Enter on a T_ row: the slot it shows taken, or what runs ended. A recording over another is asked first */
static void tas_take(int row) {
    int now = row == T_RECORD ? unattended_recording() : unattended_replaying();
    int to = tas_pick[row] == now ? 0 : tas_pick[row];
    if (!to && !now) return;
    if (row == T_RECORD && to && tas_frames[to] != 0) {
        ask = ASK_RECORD;
        page = PAGE_ASK;
        focus = 1; /* Cancel */
        top = 0;
        return;
    }
    tas_apply(row, to);
}
static void tas_ask_done(void) {
    page = PAGE_STATES;
    focus = nslots + T_RECORD;
    keep_in_view();
}
/* F1's row i taken: a runtime key's opens the Shortcuts page on its row, waiting for its new key; the game's buttons'
   and the debugger's take nothing, nor a step */
static int keys_act(int i, int step) {
    int k = keys_at(i), r;
    if (step || k < 0 || KEYS[k].action < 0) return OVERLAY_NONE;
    for (r = 0; shortcut(r) >= 0 && shortcut(r) != KEYS[k].action; ++r) {}
    if (shortcut(r) < 0) return OVERLAY_NONE;
    page_open(PAGE_SHORTCUTS);
    focus = r;
    keep_in_view();
    listening = 1;
    return OVERLAY_NONE;
}
/* the item i done, or its value stepped (step -1 or 1; 0 for a select, which steps up) */
static int act(int i, int step) {
    const IniOpt* o = setting(i);
    if (page == PAGE_KEYS) return keys_act(i, step);
    if (page == PAGE_MAIN && main_item(i) == M_TV_MODE) {
        /* a select or a tap steps on, as the key does; Left and Right either way */
        options_tv_mode_step(step < 0 ? -1 : 1, NULL, 0);
        return OVERLAY_NONE;
    }
    if (o && page == PAGE_SECTION) return section_step(o, step ? step : 1);
    if (o) {
        int* v = (int*)o->var;
        int to = ini_step(o, *v, step ? step : 1);
        if (page == PAGE_CONTROLS && i == C_TOUCH)
            backend_touch_set(to);
        else if (page == PAGE_BUTTONS) {
            *v = to;
            vpad_store_shown(i - 1);
        } else if (page == PAGE_MAIN) { /* stats: the renderer's thread reads it */
            atomic_store_explicit((_Atomic int*)v, to, memory_order_relaxed);
            vpad_store_opt(o);
        } else {
            *v = to;
            if (page == PAGE_PADS)
                gamepad_store(v);
            else
                vpad_store(v);
        }
        return OVERLAY_NONE;
    }
    if (page == PAGE_PADS && i == P_PLAYER) { /* round All and 1 to 8, a select stepping up */
        pad_player = (pad_player + (step < 0 ? SNX_GAMEPADS : 1)) % (SNX_GAMEPADS + 1);
        return OVERLAY_NONE;
    }
    if (page == PAGE_MAIN) {
        if (step) return OVERLAY_NONE;
        if (main_item(i) == M_EXIT) { /* asked first: the game ends */
            ask = OVERLAY_EXIT;
            page = PAGE_ASK;
            focus = 1; /* Cancel */
            top = 0;
            return OVERLAY_NONE;
        }
        if (main_item(i) == M_CAPTURE) return OVERLAY_CAPTURE;
        if (main_item(i) == M_SETTINGS) {
            page_open(PAGE_SETTINGS);
            return OVERLAY_NONE;
        }
        if (main_item(i) == M_CLEAN) { /* asked first: the old keys go */
            ask = ASK_CLEAN;
            page = PAGE_ASK;
            focus = 1; /* Cancel */
            top = 0;
            return OVERLAY_NONE;
        }
        page_open(i == M_PADS        ? PAGE_PADS
                  : i == M_STATES    ? PAGE_STATES
                  : i == M_SHORTCUTS ? PAGE_SHORTCUTS
                                     : PAGE_CONTROLS);
        return OVERLAY_NONE;
    }
    if (tas_row(i) >= 0 && tas_row(i) < T_BACK) {
        if (step)
            tas_step(tas_row(i), step);
        else
            tas_take(tas_row(i));
        return OVERLAY_NONE;
    }
    if (step) return OVERLAY_NONE;
    if (page == PAGE_SETTINGS && i < nentries) { /* an entry: its rows */
        entry_at = i;
        page_open(PAGE_SECTION);
        return OVERLAY_NONE;
    }
    if (page == PAGE_SHORTCUTS && shortcut(i) >= 0) { /* the next key pressed is its key */
        listening = 1;
        refused[0] = 0;
        return OVERLAY_NONE;
    }
    if (page == PAGE_STATES && i < nslots) {
        slot_at = i;
        page = PAGE_SLOT;
        focus = top = 0;
        return OVERLAY_NONE;
    }
    if (page == PAGE_SLOT && slot_item(i) != S_BACK) {
        int what = slot_item(i);
        if (what == S_SAVE && !slots[slot_at].when[0]) return OVERLAY_SAVE; /* an empty slot: nothing is replaced */
        if (!slots[slot_at].when[0] || (what == S_LOAD && slots[slot_at].why[0])) return OVERLAY_NONE;
        ask = what == S_LOAD ? OVERLAY_LOAD : what == S_SAVE ? OVERLAY_SAVE : OVERLAY_DELETE;
        page = PAGE_ASK;
        focus = 1; /* Cancel: a stray press does nothing */
        top = 0;
        return OVERLAY_NONE;
    }
    if (page == PAGE_ASK && i == 0 && ask == OVERLAY_EXIT) {
        page_open(PAGE_MAIN);
        return OVERLAY_EXIT;
    }
    if (page == PAGE_ASK && i == 0 && ask == ASK_CLEAN) { /* at the next tick; the menu's first page again */
        char said[64];
        snprintf(said, sizeof said, "Clean Old Settings: %d taken out", ini_old_settings());
        ini_clean_old();
        overlay_notice(MAIN_ICONS[M_CLEAN], said);
        page_open(PAGE_MAIN);
        return OVERLAY_NONE;
    }
    if (page == PAGE_ASK && i == 0 && ask == ASK_RECORD) {
        tas_ask_done();
        tas_apply(T_RECORD, tas_pick[T_RECORD]);
        return OVERLAY_NONE;
    }
    if (page == PAGE_ASK && i == 0 && ask == ASK_RESET) {
        vpad_reset_layout();
        page_open(PAGE_CONTROLS);
        focus = C_RESET;
        return OVERLAY_NONE;
    }
    if (page == PAGE_ASK && i == 0) {
        int did = ask;
        page = PAGE_SLOT;
        focus = top = 0;
        return did;
    }
    if (page == PAGE_BUTTONS && i == 0) {
        int c, all = all_shown();
        for (c = 0; c < SNX_VPAD_COUNT; ++c) {
            g_settings.control_shown[c] = !all;
            vpad_store_shown(c);
        }
    } else if (page == PAGE_CONTROLS && i == C_EDIT) {
        /* not over the loading screen, which hides the controls: they would be moved unseen */
        if (!prepare_screen_shown()) return OVERLAY_EDIT;
    } else if (page == PAGE_CONTROLS && i == C_BUTTONS)
        page_open(PAGE_BUTTONS);
    else if (page == PAGE_CONTROLS && i == C_RESET) { /* asked first: the controls' places go */
        ask = ASK_RESET;
        page = PAGE_ASK;
        focus = 1; /* Cancel */
        top = 0;
    } else if (page == PAGE_PADS && i == P_ASSIGN)
        return OVERLAY_CONTROLLERS; /* the controller applet's dialog, the game paused */
    else if (page == PAGE_PADS && i == P_MAP)
        map_open();
    else /* Back */
        overlay_back();
    return OVERLAY_NONE;
}

int overlay_shown(void) { return atomic_load_explicit(&shown, memory_order_acquire); }
int overlay_states_shown(void) {
    return overlay_shown() && (page == PAGE_STATES || page == PAGE_SLOT || page == PAGE_ASK);
}
int overlay_states_stale(void) { return slots_stale; }
int overlay_slot(void) { return slot_at; }
void overlay_slots_set(const OverlaySlot* given, int n) {
    int i;
    for (i = 0; i < OVERLAY_SLOTS_MAX; ++i) free(slots[i].thumb);
    memset(slots, 0, sizeof slots);
    nslots = n < 1 ? 1 : n > OVERLAY_SLOTS_MAX ? OVERLAY_SLOTS_MAX : n;
    memcpy(slots, given, sizeof *slots * (size_t)nslots);
    for (i = nslots; i < n; ++i) free(given[i].thumb);
    if (slot_at >= nslots) slot_at = 0;
    if (page == PAGE_STATES && focus > nslots + T_BACK) focus = nslots + T_BACK;
    slots_stale = 0;
    ++slots_gen;
    keep_in_view();
    changed();
}
void overlay_state_saved(const char* slot, int over, u64 now_ns) {
    char text[OVERLAY_COLS_MAX + 1];
    snprintf(text, sizeof text, "Saved %s %s", over ? "over" : "to", slot);
    overlay_state_toast(text, now_ns);
}
void overlay_state_toast(const char* text, u64 now_ns) { overlay_toast(MAIN_ICONS[M_STATES], text, now_ns); }
/* the lines changed: counted, and the last one's time up told */
static void toasts_changed(void) {
    u64 until = 0;
    int i;
    for (i = 0; i < ntoasts; ++i)
        if (toasts[i].until > until) until = toasts[i].until;
    atomic_store_explicit(&toasts_until, until, memory_order_relaxed);
    ++toast_version;
}
/* the lines whose time is up at now_ns out, the others up in their places */
static void toasts_drop(u64 now_ns) {
    int i, n = 0;
    for (i = 0; i < ntoasts; ++i)
        if (now_ns < toasts[i].until) toasts[n++] = toasts[i];
    if (n == ntoasts) return;
    ntoasts = n;
    toasts_changed();
}
/* line i out, those under it up */
static void toast_out(int i) {
    memmove(&toasts[i], &toasts[i + 1], sizeof *toasts * (size_t)(ntoasts - i - 1));
    --ntoasts;
    toasts_changed();
}
/* text after icon on the line of its subject (key, a setting's; "" for another, whose icon is its subject), or a new
   one under the others */
static void toast_give(unsigned icon, const char* key, const char* text, u64 now_ns, u32 ms) {
    int i;
    toasts_drop(now_ns);
    for (i = 0; i < ntoasts; ++i)
        if (key[0] ? !strcmp(toasts[i].key, key) : !toasts[i].key[0] && toasts[i].icon == icon) break;
    if (!text[0]) {
        if (i < ntoasts) toast_out(i);
        return;
    }
    if (i == ntoasts) { /* a new line under the others: past the most, the oldest goes */
        if (ntoasts == OVERLAY_TOASTS) toast_out(0);
        i = ntoasts++;
        toasts[i].from = now_ns;
        snprintf(toasts[i].key, sizeof toasts[i].key, "%s", key);
    } else if (toasts[i].from > now_ns) /* given again before it was first: from now */
        toasts[i].from = now_ns;
    /* a line already in keeps its fade in: its words change, it does not come in again */
    snprintf(toasts[i].text, sizeof toasts[i].text, "%s", text);
    toasts[i].icon = icon;
    toasts[i].until = now_ns + (u64)ms * 1000000u;
    toasts_changed();
}
void overlay_toast(unsigned icon, const char* text, u64 now_ns) {
    toast_give(icon, "", text, now_ns, SNX_OVERLAY_TOAST_MS);
}
int overlay_toasting(u64 now_ns) { return now_ns < atomic_load_explicit(&toasts_until, memory_order_relaxed); }
unsigned overlay_setting_icon(const char* key) {
    int s, t;
    for (s = 0; s < g_ini_sections_count; ++s)
        for (t = 0; t < 3 && g_ini_sections[s].table[t]; ++t)
            if (ini_find(g_ini_sections[s].table[t], key)) {
                const char* in = g_ini_sections[s].page[t];
                unsigned icon = in ? gui_page_icon(in) : GUI_ICON_NONE;
                return icon != GUI_ICON_NONE ? icon : gui_group_icon(gui_group_name(g_ini_sections[s].name));
            }
    return GUI_ICON_NONE;
}
void overlay_setting_toast(const char* key, const char* text, u64 now_ns) {
    toast_give(overlay_setting_icon(key), key, text, now_ns, SNX_OVERLAY_TOAST_MS);
}
unsigned overlay_key_icon(int action) {
    size_t i;
    for (i = 0; i < sizeof KEYS / sizeof *KEYS; ++i)
        if (action >= 0 && KEYS[i].action == action) return KEYS[i].icon; /* -1: the game's buttons' rows */
    return GUI_ICON_NONE;
}
void overlay_fades(const SnxRunTheme* theme, u32 ms[2]) {
    toast_fade[0] = theme ? theme->fade_in_ms : SNX_GUI_THEME_YORHA_FADE_IN_MS;
    toast_fade[1] = theme ? theme->fade_out_ms : SNX_GUI_THEME_YORHA_FADE_OUT_MS;
    ms[0] = toast_fade[0];
    ms[1] = toast_fade[1];
}
int overlay_toast_alpha(int line, u64 now_ns) {
    const u64 now = now_ns / 1000000u;
    u64 from, until;
    u8 in, out;
    if (line < 0 || line >= ntoasts || now_ns >= toasts[line].until) return 0;
    from = toasts[line].from / 1000000u;
    until = toasts[line].until / 1000000u;
    /* the launcher's black coming in from it, and going out to it: that fade begins fade_out_ms before the end,
       counted on from there, a fade longer than the toast too */
    in = snx_fade_shade(1.0f - snx_fade_done(now, from, toast_fade[0]));
    out = toast_fade[1] ? snx_fade_shade(snx_fade_done(now + toast_fade[1], until, toast_fade[1])) : 0;
    return 255 - (in > out ? in : out);
}
/* overlay_notice's line until the next draw takes it ("" none), under a lock of its own: its callers may hold ui_lock
   or not */
static PMutex notice_lock = PMUTEX_INIT;
static char notice[OVERLAY_COLS_MAX + 1];
static unsigned notice_icon;
static _Atomic int notice_given; /* a line waits: the draw takes the lock only then */
void overlay_notice(unsigned icon, const char* text) {
    pm_lock(&notice_lock);
    snprintf(notice, sizeof notice, "%s", text);
    notice_icon = icon;
    atomic_store_explicit(&notice_given, 1, memory_order_release);
    pm_unlock(&notice_lock);
}
const char* overlay_cold_cache_words(int shaders, int textures) {
    return shaders ? "Compiling shaders" : textures ? "Encoding textures" : NULL;
}
int overlay_cold_cache(int shaders, int textures) {
    static _Atomic int said;
    const char* words = shaders < 0 ? NULL : overlay_cold_cache_words(shaders, textures);
    if (!words || atomic_exchange_explicit(&said, 1, memory_order_relaxed)) return 0;
    overlay_notice(shaders ? SNX_STATS_ICON_COMPILE : SNX_STATS_ICON_ENCODE, words);
    return 1;
}
void overlay_toggle(void) {
    changed();
    if (!atomic_load_explicit(&shown, memory_order_relaxed)) page_open(PAGE_MAIN);
    atomic_store_explicit(&shown, !atomic_load_explicit(&shown, memory_order_relaxed), memory_order_release);
    vpad_menu(atomic_load_explicit(&shown, memory_order_relaxed)); /* the on-screen controls hide under it */
}
/* the panel's rows: the page's items under the head, a blank row under them */
static int rows(void) { return FIRST + (items() < visible() ? items() : visible()) + 1; }
void overlay_set_display_scale(float scale) {
    atomic_store_explicit(&display_percent, snx_display_percent(scale), memory_order_relaxed);
    changed();
}
int overlay_display_percent(void) { return atomic_load_explicit(&display_percent, memory_order_relaxed); }
void overlay_place(int sw, int sh, int* x, int* y, int* scale) {
    picture[0] = sw;
    picture[1] = sh;
    *scale = place_scale(sw, sh);
    *x = (sw - overlay_cols(sw, *scale) * 8 * *scale) / 2;
    *y = (sh - rows() * 16 * *scale) / 2;
}
int overlay_cols(int sw, int scale) {
    int cols = sw * 3 / 4 / (8 * (scale < 1 ? 1 : scale));
    return cols < OVERLAY_COLS ? OVERLAY_COLS : cols > OVERLAY_COLS_MAX ? OVERLAY_COLS_MAX : cols;
}
/* ---- a dialog over the game: the menu's pages and a stopped game's panel, laid out and drawn as one ---- */
enum { DIALOG_PARAS = 6, DIALOG_LINES = 12 };
typedef struct {
    const char* text; /* UTF-8, wrapped to the box; NULL for none */
    u32 rgba;
    int link; /* underlined: its rows are a target (DialogDrawn's link) */
    int on;   /* right under the paragraph before it, no blank row between */
    int cut;  /* on a short picture the lowest gives up its rows first; 0 never does */
    /* a line typed into: cut as it is, its last DIALOG_FIELD_ROWS rows, on a panel (DialogDrawn's field) */
    int field;
} DialogPara;
enum { DIALOG_FIELD_ROWS = 4 };
typedef struct {
    const char* title;
    int user; /* the user playing at the head's right, by their nickname: the menu's pages */
    DialogPara para[DIALOG_PARAS];
    int paras;
    const char* label[VISIBLE];   /* the items showing, from top on */
    u32 tone[VISIBLE]; /* 0, or the colour of one with no way back (UI_FAILURE) or to take with care (UI_WARNING) */
    unsigned icon[VISIBLE];       /* beside them, gui/icons.h's; GUI_ICON_NONE for none */
    const char* value[VISIBLE];   /* at their right; NULL for none */
    const char* key[VISIBLE];     /* the keys F1's page names before the label, on a band; NULL for none */
    int keys;                     /* their column's width: the label after it and two more */
    /* a slider's: the columns its value takes at its widest, 0 for no slider, and its knob in thousandths of its way */
    int slider_cols[VISIBLE], knob[VISIBLE];
    int items, top, shown, focus; /* all the items, the first showing, how many show, the one in focus */
    int aside;                    /* room at the box's right for a picture (the save states' thumbnail), if it fits */
    int dim[VISIBLE];             /* an item that does nothing, its label in the soft colour */
    /* the foot, a band under the items of the page's keys, "Key: what." each (empty: none), and on the row over it
       where the focus is among the items ("14 / 27"; empty: nothing) */
    char foot[OVERLAY_COLS_MAX + 1], count[16];
} Dialog;
/* what a key of the foot does, tapped */
enum { FOOT_SELECT = 1, FOOT_BACK, FOOT_DEFAULT, FOOT_CHIPS = 6 };
typedef struct {
    SnxOverlayRect chip[FOOT_CHIPS]; /* the foot's keys, as drawn, and what each does: FOOT_ */
    int chip_do[FOOT_CHIPS], chips;
    SnxOverlayRect box, link, item[VISIBLE];
    SnxOverlayRect bar[VISIBLE]; /* each item's slider, its knob's travel and a knob's width: empty for none */
    SnxOverlayRect aside;        /* the picture's room: empty when there was none */
    SnxOverlayRect field; /* the field paragraph's panel, empty without one */
    int cols, scale;
} DialogDrawn;
/* the user playing on the head's band, against right, past left: a box framed as the launcher's are, in the band's
   colours swapped, holding their nickname and their picture at its right, two of the box's pixels in. Without room
   for the nickname, the picture alone; without room for that, nothing. None when the theme shows no picture */
static void user_draw(SnxUiList* l, float left, float right, float y, int s) {
    const char* name = g_user_count ? snx_users_selected()->nickname : "";
    float cw = 8.0f * (float)s, ch = 16.0f * (float)s, edge = (float)s, side = ch - 4 * edge;
    float box = cw + cw * (float)strlen(name) + cw + side + 2 * edge, x = right - box;
    if (!g_user_count || !SNX_GUI_THEME_YORHA_PICTURE) return;
    if (x < left) { /* the picture alone */
        box = side + 4 * edge;
        x = right - box;
        name = "";
    }
    if (x < left) return;
    ui_picture(l, right - 2 * edge - side, y + 2 * edge, side);
    if (*name) ui_text(l, x + cw, y, s, name, UI_PAGE);
    ui_fill(l, x, y, box, edge, UI_PAGE);
    ui_fill(l, x, y + ch - edge, box, edge, UI_PAGE);
    ui_fill(l, x, y, edge, ch, UI_PAGE);
    ui_fill(l, x + box - edge, y, edge, ch, UI_PAGE);
}
/* the rows paragraphs wrap into: each thread that lays a dialog out has its own */
typedef char DialogRows[DIALOG_PARAS][DIALOG_LINES][FATAL_ROW];
/* the keyboard's box's answer */
enum { KB_WAITING, KB_GIVEN, KB_CANCELLED };
/* The keyboard's box: what the game asked, the line as typed (kb.text) and its UTF-16 units, whether it shows (the
   renderer and the backend read it), Enter pressed short of the least (said until the line changes), and the answer
   until it is taken. The event thread's rows, for its taps and the line's place. What the hints name (overlay_input_):
   a controller's buttons when pad, with the names of those sending A and B, else the keyboard's keys */
static struct {
    OverlayKeyboard kb;
    u32 units;
    _Atomic int kb_shown;
    int refused, answer;
    DialogRows event_rows;
    struct {
        int pad;
        char ok[OVERLAY_INPUT_NAME], back[OVERLAY_INPUT_NAME];
    } input;
    /* a save state's load (overlay_loading): its slot's words, when the box came, and whether it shows (LOADING), or
       waits for the game's frame (LOADED); the rows of its box */
    struct {
        char slot[OVERLAY_COLS_MAX + 1];
        u64 since_ns;
        _Atomic int state; /* read unlocked by the present, which takes ui_lock only while there is one */
        DialogRows rows;
    } loading;
    /* the settings' notice (overlay_settings_notice) until the game's first frame takes it, under notice_lock */
    struct {
        char game[1024], global[1024], kind[256];
        _Atomic int waiting; /* given, not yet taken: the present reads it unlocked */
    } settings;
} g_overlay
#ifdef __ANDROID__
    = {.input = {1, "A", "B"}}
#endif
;
enum { LOADING_NONE, LOADING_SHOWN, LOADING_LOADED };
void overlay_settings_notice(const char* game, const char* global, const char* kind) {
    pm_lock(&notice_lock);
    snprintf(g_overlay.settings.game, sizeof g_overlay.settings.game, "%s", game);
    snprintf(g_overlay.settings.global, sizeof g_overlay.settings.global, "%s", global);
    snprintf(g_overlay.settings.kind, sizeof g_overlay.settings.kind, "%s", kind);
    atomic_store_explicit(&g_overlay.settings.waiting, game[0] || global[0] || kind[0], memory_order_release);
    pm_unlock(&notice_lock);
}
int overlay_settings_waiting(void) { return atomic_load_explicit(&g_overlay.settings.waiting, memory_order_relaxed); }
/* the key that starts at list (", " between) and the next one's start into *next */
static size_t key_at(const char* list, const char** next) {
    size_t len = strcspn(list, ",");
    *next = list + len;
    while (**next == ',' || **next == ' ') ++*next;
    return len;
}
/* keys of list from at as line's text, ", " between, cols letters at most (one key at the least, cut there): how many */
static int line_keys(const char* at, int keys, int cols, char* line) {
    size_t used = 0;
    int n = 0;
    line[0] = 0;
    while (*at && (keys < 0 || n < keys)) {
        const char* next;
        size_t len = key_at(at, &next), need = used + (used ? 2 : 0) + len;
        if (used && need > (size_t)cols) break;
        snprintf(line + used, (size_t)cols + 1 - used, "%s%.*s", used ? ", " : "", (int)len, at);
        used = need < (size_t)cols ? need : (size_t)cols;
        ++n;
        at = next;
    }
    return n;
}
/* how many keys list holds, ", " between */
static int keys_in(const char* list) {
    int n = 0;
    for (const char* next; *list; list = next, ++n) key_at(list, &next);
    return n;
}
int overlay_settings_lines(const char* game, const char* global, const char* kind, int cols,
                           unsigned icon[OVERLAY_SETTINGS_LINES],
                           char text[OVERLAY_SETTINGS_LINES][OVERLAY_COLS_MAX + 1]) {
    const char* lists[3] = {game, global, kind};
    const unsigned icons[3] = {GUI_ICON_GAMEPAD_2, GUI_ICON_GLOBE, GUI_ICON_INFO};
    /* what the kind's keys are, after them on its last line: no file nor command line gave them */
    const char* const after[3] = {"", "", ", for this kind of game"};
    int n = 0;
    if (cols > OVERLAY_COLS_MAX) cols = OVERLAY_COLS_MAX;
    for (int k = 0; k < 3; ++k) {
        const char *at = lists[k], *last = at;
        int later = 0, took = 0, first = n;
        for (int j = k + 1; j < 3; ++j) later += lists[j][0] != 0; /* a line kept for each list after it */
        int room = OVERLAY_SETTINGS_LINES - n - later, width = cols - (int)strlen(after[k]);
        if (width < 1) width = cols; /* a line too narrow for the words after the keys: its keys, the words cut */
        for (; *at && room > 0; --room, ++n) {
            last = at;
            took = line_keys(at, -1, width, text[n]);
            icon[n] = icons[k];
            for (int i = 0; i < took; ++i) key_at(at, &at);
        }
        if (*at) { /* keys left: the last line says how many, as many of its own keys left out as that takes */
            int left = keys_in(at);
            for (int keep = took; keep >= 0; --keep) {
                char more[24];
                snprintf(more, sizeof more, "+%d more", left + took - keep);
                line_keys(last, keep, width, text[n - 1]);
                size_t used = strlen(text[n - 1]);
                if (keep && used + 2 + strlen(more) > (size_t)width) continue;
                snprintf(text[n - 1] + used, (size_t)width + 1 - used, "%s%s", used ? ", " : "", more);
                break;
            }
        }
        if (n > first && after[k][0]) {
            size_t used = strlen(text[n - 1]);
            if (used < (size_t)cols) snprintf(text[n - 1] + used, (size_t)cols + 1 - used, "%s", after[k]);
        }
    }
    return n;
}
void overlay_settings_start(u64 now_ns) {
    unsigned icon[OVERLAY_SETTINGS_LINES];
    char text[OVERLAY_SETTINGS_LINES][OVERLAY_COLS_MAX + 1];
    int n = 0;
    pm_lock(&notice_lock);
    if (atomic_load_explicit(&g_overlay.settings.waiting, memory_order_acquire))
        n = overlay_settings_lines(g_overlay.settings.game, g_overlay.settings.global, g_overlay.settings.kind,
                                   OVERLAY_COLS_MAX, icon, text);
    atomic_store_explicit(&g_overlay.settings.waiting, 0, memory_order_relaxed);
    pm_unlock(&notice_lock);
    for (int i = 0; i < n; ++i) {
        char subject[16];
        snprintf(subject, sizeof subject, "@settings %d", i); /* a line each, apart from any setting's own */
        toast_give(icon[i], subject, text[i], now_ns, SNX_OVERLAY_SETTINGS_MS);
    }
}
void overlay_loading(const char* slot, u64 shown_ns) {
    snprintf(g_overlay.loading.slot, sizeof g_overlay.loading.slot, "%s", slot ? slot : "");
    g_overlay.loading.since_ns = shown_ns;
    atomic_store_explicit(&g_overlay.loading.state, LOADING_SHOWN, memory_order_relaxed);
}
void overlay_loading_done(void) {
    if (overlay_loading_shown()) atomic_store_explicit(&g_overlay.loading.state, LOADING_LOADED, memory_order_relaxed);
}
void overlay_loading_end(void) { atomic_store_explicit(&g_overlay.loading.state, LOADING_NONE, memory_order_relaxed); }
int overlay_loading_shown(void) {
    return atomic_load_explicit(&g_overlay.loading.state, memory_order_relaxed) != LOADING_NONE;
}
void overlay_game_drew(void) {
    if (atomic_load_explicit(&g_overlay.loading.state, memory_order_relaxed) == LOADING_LOADED) overlay_loading_end();
}
void overlay_input_keys(void) {
    if (!g_overlay.input.pad) return;
    g_overlay.input.pad = 0;
    changed();
}
void overlay_input_pad(const char* ok, const char* back) {
    char was_ok[OVERLAY_INPUT_NAME], was_back[OVERLAY_INPUT_NAME];
    int was = g_overlay.input.pad;
    memcpy(was_ok, g_overlay.input.ok, sizeof was_ok);
    memcpy(was_back, g_overlay.input.back, sizeof was_back);
    g_overlay.input.pad = 1;
    snprintf(g_overlay.input.ok, sizeof g_overlay.input.ok, "%s", ok ? ok : "A");
    snprintf(g_overlay.input.back, sizeof g_overlay.input.back, "%s", back ? back : "B");
    if (!was || strcmp(was_ok, g_overlay.input.ok) || strcmp(was_back, g_overlay.input.back)) changed();
}
/* item i's slider, at its row's letters ty on a box from x cols wide (cells cw x ch), in its letters' colour fg as
   the launcher's (src/gui's slider_row_draw): a thin line, thicker up to the knob, and the knob, a cell wide; ending a
   column before its value at its widest, OVERLAY_SLIDER_COLS long where the label leaves room, a column after it.
   Where it lies into bar, the knob's travel with the knob's width (empty without room for a cell of travel); drawn
   into l when not NULL */
static void slider_draw(SnxUiList* l, const Dialog* d, int i, float x, float ty, float cw, float ch, int cols, u32 fg,
                        SnxOverlayRect* bar) {
    int start = d->key[i] ? 6 + d->keys + 2 : d->icon[i] ? 6 : 3; /* the label's first column */
    float right = x + cw * (float)(cols - 2 - d->slider_cols[i]), left = right - cw * (float)OVERLAY_SLIDER_COLS;
    float lowest = x + cw * (float)(start + (int)strlen(d->label[i]) + 1), mid = ty + ch / 2, px = ch / 16, knob;
    if (left < lowest) left = lowest;
    memset(bar, 0, sizeof *bar);
    if (right - left < 2 * cw) return;
    bar->x = left;
    bar->y = ty;
    bar->w = right - left;
    bar->h = ch;
    if (!l) return;
    knob = left + (bar->w - cw) * (float)d->knob[i] / 1000.0f;
    /* the line from a knob's middle at one end to the knob's middle at the other */
    if (knob < right - cw) ui_fill(l, knob + cw / 2, mid - px, right - cw - knob, px, fg);
    if (knob > left) ui_fill(l, left + cw / 2, mid - 2 * px, knob - left, 3 * px, fg);
    ui_fill(l, knob, ty + 2 * px, cw, ch - 4 * px, fg);
}
/* the foot of d at y, cols cells wide from x, into l (NULL: only where its keys go, in out): a band of ink as the head's,
   each sentence of d->foot on it, a "Key: what." its key on the page's colour as F1's rows have theirs and what it does
   after it, another as it is; the sentences that do not fit left out. The keys that select, go back and put a default
   back are taps there */
static void foot_draw(SnxUiList* l, const Dialog* d, DialogDrawn* out, float x, float y, int cols, int s) {
    const char *at = d->foot,
               *ok = g_overlay.input.pad ? g_overlay.input.ok
                     : fingers           ? "Tap"
                                         : "Enter",
               *back = g_overlay.input.pad ? g_overlay.input.back
                       : fingers           ? "Back"
                                           : "Esc";
    float cw = 8.0f * (float)s, ch = 16.0f * (float)s, cx = x + cw;
    if (l) ui_fill(l, x, y, cw * (float)cols, ch, UI_INK);
    while (*at) {
        const char* stop = strstr(at, ". ");
        size_t full = stop ? (size_t)(stop - at) + 1 : strlen(at); /* the sentence, its stop with it */
        const char* colon = (const char*)memchr(at, ':', full);
        /* a key's sentence drawn without its stop, as the launcher's foot is; another whole */
        size_t len = colon && at[full - 1] == '.' ? full - 1 : full, key = colon ? (size_t)(colon - at) : 0,
               skip = colon ? key + 2 : 0;
        char word[OVERLAY_COLS_MAX + 1];
        if (cx + cw * (float)(len - (colon ? 1 : 0)) > x + cw * (float)(cols - 1)) break; /* no room for it */
        if (colon) {
            snprintf(word, sizeof word, "%.*s", (int)key, at);
            if (l) {
                ui_fill(l, cx - (float)s, y, cw * (float)key + 2.0f * (float)s, ch, UI_PAGE);
                ui_text(l, cx, y, s, word, UI_INK);
            }
            int does = !strcmp(word, ok)                                  ? FOOT_SELECT
                       : !strcmp(word, back)                              ? FOOT_BACK
                       : !strcmp(word, "Backspace") || !strcmp(word, "Y") ? FOOT_DEFAULT
                                                                          : 0;
            if (does && out->chips < FOOT_CHIPS) {
                SnxOverlayRect* r = &out->chip[out->chips];
                r->x = cx - (float)s;
                r->y = y;
                r->w = cw * (float)(len - 1);
                r->h = ch;
                out->chip_do[out->chips++] = does;
            }
        }
        snprintf(word, sizeof word, "%.*s", (int)(len - skip), at + skip);
        if (l) ui_text(l, cx + (colon ? cw * (float)(key + 1) : 0), y, s, word, UI_PAGE);
        cx += cw * (float)(len - (colon ? 1 : 0) + 2);
        if (!stop) break;
        at = stop + 2;
    }
}
/* d on a picture sw x sh, into l (NULL: only where it all goes, in out): the head, each paragraph after a blank row,
   a blank row, the items and a blank row, and the foot when it has one; overlay_cols wide, in the middle */
static void dialog_draw(SnxUiList* l, int sw, int sh, const Dialog* d, DialogRows rows_of, DialogDrawn* out) {
    char line[OVERLAY_COLS_MAX + 4];
    int px, py, s, cols, n[DIALOG_PARAS], k, i, total, row, last, aside = 0, taller, foot = 0;
    float cw, ch, x, y, w, rh, into;
    memset(out, 0, sizeof *out);
    overlay_place(sw, sh, &px, &py, &s);
    cols = overlay_cols(sw, s);
    cw = 8.0f * (float)s;
    ch = 16.0f * (float)s;
    if (d->aside) { /* the picture at half its size a scale step, a column apart: the box narrower to fit it */
        aside = SNX_UI_IMAGE_W / 2 * s + 8 * s;
        if ((sw - aside) / (8 * s) < OVERLAY_COLS)
            aside = 0;
        else if (cols > (sw - aside) * 7 / 8 / (8 * s))
            cols = (sw - aside) * 7 / 8 / (8 * s) < OVERLAY_COLS ? OVERLAY_COLS : (sw - aside) * 7 / 8 / (8 * s);
    }
    w = cw * (float)cols;
    for (k = 0; k < d->paras; ++k) {
        if (!d->para[k].text)
            n[k] = 0;
        else if (d->para[k].field) /* where the player types: its end */
            n[k] = fatal_cut(d->para[k].text, cols - 2, rows_of[k], DIALOG_FIELD_ROWS);
        else
            n[k] = fatal_wrap(d->para[k].text, cols - 2, rows_of[k], DIALOG_LINES);
        if (n[k] > DIALOG_LINES) n[k] = DIALOG_LINES;
    }
    /* an item's row, a finger's on a screen of fingers, its letters into pixels down it; taller: what the items'
       rows add to the box's rows of letters */
    rh = (float)item_h(s);
    into = (rh - ch) / 2;
    taller = d->shown * (int)(rh - ch);
    for (;;) {
        int pick = -1;
        /* the foot on a row of its own under a blank one, or in the blank row's place under a paragraph */
        for (k = 0, foot = d->foot[0] != 0; k < d->paras; ++k) foot &= !n[k];
        total = 1 + 1 + d->shown + 1 + foot;
        for (k = 0, last = -1; k < d->paras; ++k) {
            if (!n[k]) continue;
            total += n[k] + (last >= 0 && !(d->para[k].on && last == k - 1));
            last = k;
            if (d->para[k].cut && (pick < 0 || d->para[k].cut < d->para[pick].cut)) pick = k;
        }
        if (total * (int)ch + taller <= sh || pick < 0) break;
        --n[pick];
    }
    x = (float)((sw - cols * 8 * s - aside) / 2);
    y = (float)((sh - total * (int)ch - taller) / 2);
    if (aside) {
        out->aside.x = x + w + (float)(8 * s);
        out->aside.y = y;
        out->aside.w = (float)(SNX_UI_IMAGE_W / 2 * s);
        out->aside.h = (float)(SNX_UI_IMAGE_H / 2 * s);
    }
    out->box.x = x;
    out->box.y = y;
    out->box.w = w;
    out->box.h = ch * (float)total + (float)taller;
    out->cols = cols;
    out->scale = s;
    if (l) {
        ui_fill(l, x, y, w, ch * (float)total + (float)taller, UI_PAGE);
        ui_fill(l, x, y, w, ch, UI_INK);
        ui_text(l, x + cw, y, s, d->title, UI_PAGE);
        if (d->user) user_draw(l, x + cw * (float)(strlen(d->title) + 2), x + w - cw, y, s);
    }
    row = 1;
    for (k = 0, last = -1; k < d->paras; ++k) {
        if (!n[k]) continue;
        if (last >= 0 && !(d->para[k].on && last == k - 1)) ++row;
        last = k;
        if (d->para[k].field) { /* half a column wider than its letters at each side */
            out->field.x = x + cw / 2;
            out->field.y = y + ch * (float)row;
            out->field.w = w - cw;
            out->field.h = ch * (float)n[k];
            if (l) ui_fill(l, out->field.x, out->field.y, out->field.w, out->field.h, UI_PANEL);
        }
        for (i = 0; i < n[k]; ++i, ++row) {
            float width, at = y + ch * (float)row;
            fatal_cp437(rows_of[k][i], line, sizeof line);
            width = (float)strlen(line) * cw;
            if (l) ui_text(l, x + cw, at, s, line, d->para[k].rgba);
            if (!d->para[k].link || width <= 0) continue;
            if (l) ui_fill(l, x + cw, at + ch - (float)s, width, (float)s, d->para[k].rgba);
            if (!out->link.h) {
                out->link.x = x + cw;
                out->link.y = at;
            }
            if (width > out->link.w) out->link.w = width;
            out->link.h += ch;
        }
    }
    for (i = 0; i < d->shown; ++i) {
        SnxOverlayRect* at = &out->item[i];
        int on = d->top + i == d->focus;
        u32 fg;
        float ty; /* its letters' row, in the middle of its own */
        at->x = x;
        at->y = y + ch * (float)(total - 1 - foot - d->shown) + rh * (float)i;
        at->w = w;
        at->h = rh;
        ty = at->y + into;
        if (!l) {
            if (d->slider_cols[i]) slider_draw(NULL, d, i, x, ty, cw, ch, cols, 0, &out->bar[i]);
            continue;
        }
        fg = on ? UI_PAGE : d->tone[i] ? d->tone[i] : d->dim[i] ? UI_SOFT : UI_INK;
        if (on)
            ui_fill(l, x, at->y, w, rh, d->tone[i] ? d->tone[i] : UI_INK);
        else if (d->tone[i]) { /* framed in its colour, as the launcher's rows */
            ui_fill(l, x, at->y, w, (float)s, d->tone[i]);
            ui_fill(l, x, at->y + rh - (float)s, w, (float)s, d->tone[i]);
            ui_fill(l, x, at->y, (float)s, rh, d->tone[i]);
            ui_fill(l, x + w - (float)s, at->y, (float)s, rh, d->tone[i]);
        }
        if (d->key[i]) { /* the marker, the icon, the keys on a band as the launcher's F1 box draws them, the label */
            ui_text(l, x + cw, ty, s, "\xFE", on ? UI_PAGE : UI_INK);
            ui_icon(l, x + 3 * cw, ty, s, d->icon[i], on ? UI_PAGE : UI_INK);
            ui_fill(l, x + 6 * cw - (float)s, ty, cw * (float)strlen(d->key[i]) + 2.0f * (float)s, ch,
                    on ? UI_PAGE : UI_INK);
            ui_text(l, x + 6 * cw, ty, s, d->key[i], on ? UI_INK : UI_PAGE);
            ui_text(l, x + cw * (float)(6 + d->keys + 2), ty, s, d->label[i], fg);
        } else if (d->icon[i]) { /* the marker, the icon's two cells and one, then the label */
            ui_text(l, x + cw, ty, s, "\xFE", fg);
            ui_icon(l, x + 3 * cw, ty, s, d->icon[i], fg);
            ui_text(l, x + 6 * cw, ty, s, d->label[i], fg);
        } else {
            snprintf(line, sizeof line, "\xFE %s", d->label[i]);
            ui_text(l, x + cw, ty, s, line, fg);
        }
        if (d->value[i]) ui_text(l, x + cw * (float)(cols - 1 - (int)strlen(d->value[i])), ty, s, d->value[i], fg);
        if (d->slider_cols[i]) slider_draw(l, d, i, x, ty, cw, ch, cols, fg, &out->bar[i]);
    }
    /* more above or under: the font's arrows at the edge */
    if (l && d->shown && d->top) ui_text(l, x + cw * (float)(cols - 1), out->item[0].y + into, s, "\x18", UI_SOFT);
    if (l && d->shown && d->top + d->shown < d->items)
        ui_text(l, x + cw * (float)(cols - 1), out->item[d->shown - 1].y + into, s, "\x19", UI_SOFT);
    if (d->foot[0]) {
        if (l && d->count[0] && foot) /* where the focus is, at the right of the blank row under the items */
            ui_text(l, x + cw * (float)(cols - 1 - (int)strlen(d->count)), y + ch * (float)(total - 2) + (float)taller,
                    s, d->count, UI_SOFT);
        foot_draw(l, d, out, x, y + ch * (float)(total - 1) + (float)taller, cols, s);
    }
}

/* ---- the software keyboard's box ---- */
/* the bytes of the UTF-8 character at s: a lead byte's continuations, three at most; any other byte alone */
static size_t kb_step(const char* s) {
    size_t n = 1;
    if ((unsigned char)s[0] >= 0xC0)
        while (n < 4 && s[n] && ((unsigned char)s[n] & 0xC0) == 0x80) ++n;
    return n;
}
/* text added to the line: its characters while the line's most has room for each (two units for one past U+FFFF,
   which UTF-16 makes a pair of), controls left out */
static void kb_add(const char* text) {
    size_t at = strlen(g_overlay.kb.text);
    while (*text) {
        size_t n = kb_step(text);
        u32 units = n == 4 ? 2 : 1;
        if ((unsigned char)*text >= 0x20 && *text != 0x7f) {
            if (g_overlay.units + units > g_overlay.kb.max || at + n >= sizeof g_overlay.kb.text) break;
            memcpy(g_overlay.kb.text + at, text, n);
            at += n;
            g_overlay.units += units;
        }
        text += n;
    }
    g_overlay.kb.text[at] = 0;
}
void overlay_keyboard_show(const OverlayKeyboard* k) {
    g_overlay.kb = *k;
    if (!g_overlay.kb.max || g_overlay.kb.max > OVERLAY_KB_UNITS) g_overlay.kb.max = OVERLAY_KB_UNITS;
    if (g_overlay.kb.min > g_overlay.kb.max) g_overlay.kb.min = g_overlay.kb.max;
    g_overlay.kb.text[0] = 0;
    g_overlay.units = 0;
    kb_add(k->text);
    g_overlay.refused = 0;
    g_overlay.answer = KB_WAITING;
    atomic_store_explicit(&g_overlay.kb_shown, 1, memory_order_release);
    changed();
}
void overlay_keyboard_hide(u32 ticket) {
    if (g_overlay.kb.ticket == ticket) atomic_store_explicit(&g_overlay.kb_shown, 0, memory_order_release);
    changed();
}
int overlay_keyboard_shown(void) { return atomic_load_explicit(&g_overlay.kb_shown, memory_order_acquire); }
void overlay_keyboard_type(const char* text) {
    if (!atomic_load_explicit(&g_overlay.kb_shown, memory_order_relaxed)) return;
    kb_add(text);
    g_overlay.refused = 0;
    changed();
}
static int kb_answer(int answer) {
    g_overlay.answer = answer;
    atomic_store_explicit(&g_overlay.kb_shown, 0, memory_order_release);
    return OVERLAY_KEYBOARD;
}
int overlay_keyboard_key(int key) {
    size_t end, at;
    if (!atomic_load_explicit(&g_overlay.kb_shown, memory_order_relaxed)) return OVERLAY_NONE;
    changed();
    if (key == OVERLAY_KEY_CANCEL) return kb_answer(KB_CANCELLED);
    if (key == OVERLAY_KEY_ENTER) {
        if (g_overlay.units >= g_overlay.kb.min) return kb_answer(KB_GIVEN);
        g_overlay.refused = 1;
        return OVERLAY_NONE;
    }
    end = strlen(g_overlay.kb.text); /* Backspace: the last character, from the byte that leads it */
    if (end) {
        for (at = end - 1; at && ((unsigned char)g_overlay.kb.text[at] & 0xC0) == 0x80; --at) {}
        g_overlay.units -= end - at == 4 ? 2 : 1;
        g_overlay.kb.text[at] = 0;
    }
    g_overlay.refused = 0;
    return OVERLAY_NONE;
}
int overlay_keyboard_take(u32* ticket, char* out, size_t size) {
    int answer = g_overlay.answer;
    if (answer == KB_WAITING) return -1;
    g_overlay.answer = KB_WAITING;
    *ticket = g_overlay.kb.ticket;
    if (answer == KB_CANCELLED) return 1;
    snprintf(out, size, "%s", g_overlay.kb.text);
    return 0;
}
/* the texts the box's dialog points at */
typedef struct {
    char line[OVERLAY_KB_TEXT + 3]; /* the line and its cursor */
    char count[96], ok[sizeof g_overlay.kb.ok], hint[sizeof g_overlay.kb.ok + 2 * OVERLAY_INPUT_NAME + 16];
} KbTexts;
/* the box as a dialog: the game's header, sub and guide texts, the line with its cursor at the end (each character an
   asterisk for a password), how many characters it holds against the game's limits, what the keys do, and the game's
   OK, in focus as Enter and A give it, and Cancel */
static void kb_dialog(Dialog* d, KbTexts* t) {
    const OverlayKeyboard* k = &g_overlay.kb;
    const char* ok = k->ok[0] ? k->ok : "OK";
    size_t n = 0;
    const char* c;
    memset(d, 0, sizeof *d);
    d->title = "SCARLETNX  /  KEYBOARD";
    d->user = 1;
    if (k->password)
        for (c = k->text; *c && n < OVERLAY_KB_UNITS; c += kb_step(c)) t->line[n++] = '*';
    else
        for (; k->text[n]; ++n) t->line[n] = k->text[n];
    memcpy(t->line + n, "\xE2\x96\x8C", 4); /* the cursor, the font's left half block */
    if (g_overlay.refused) {
        u32 more = k->min - g_overlay.units; /* Enter was short of the least */
        snprintf(t->count, sizeof t->count, "%u more character%s needed, %u at least.", (unsigned)more,
                 more == 1 ? "" : "s", (unsigned)k->min);
    } else if (k->min)
        snprintf(t->count, sizeof t->count, "%u of %u characters, %u at least.", (unsigned)g_overlay.units,
                 (unsigned)k->max, (unsigned)k->min);
    else
        snprintf(t->count, sizeof t->count, "%u of %u characters.", (unsigned)g_overlay.units, (unsigned)k->max);
    if (g_overlay.input.pad)
        snprintf(t->hint, sizeof t->hint, "%s: %s. %s: Cancel.", g_overlay.input.ok, ok, g_overlay.input.back);
    else
        snprintf(t->hint, sizeof t->hint, "Enter: %s. Esc: Cancel.", ok);
    fatal_cp437(ok, t->ok, sizeof t->ok); /* the items are drawn in the font's bytes */
    {
        /* on a short picture the hint gives up its rows first, then the guide, the sub and the header text; never the
           line or its count */
        const DialogPara para[DIALOG_PARAS] = {{k->header, UI_INK, 0, 0, 4, 0},
                                               {k->sub, UI_SOFT, 0, 1, 3, 0},
                                               {k->guide, UI_SOFT, 0, 0, 2, 0},
                                               {t->line, UI_INK, 0, 0, 0, 1},
                                               {t->count, g_overlay.refused ? UI_FAILURE : UI_SOFT, 0, 1, 0, 0},
                                               {t->hint, UI_SOFT, 0, 0, 1, 0}};
        memcpy(d->para, para, sizeof para);
    }
    d->paras = DIALOG_PARAS;
    d->label[0] = t->ok;
    d->label[1] = "Cancel";
    d->items = d->shown = 2;
}
/* a tap on the box: OK and Cancel do as Enter and Esc, the line asks for the system's keyboard again */
static int kb_tap(float x, float y, int sw, int sh) {
    KbTexts t;
    Dialog d;
    DialogDrawn at;
    kb_dialog(&d, &t);
    dialog_draw(NULL, sw, sh, &d, g_overlay.event_rows, &at);
    if (overlay_error_hit(&at.field, x, y)) return OVERLAY_TYPE;
    if (overlay_error_hit(&at.item[0], x, y)) return overlay_keyboard_key(OVERLAY_KEY_ENTER);
    if (overlay_error_hit(&at.item[1], x, y)) return overlay_keyboard_key(OVERLAY_KEY_CANCEL);
    return OVERLAY_NONE;
}
/* a button pressed on the box (one HidNpadButton bit): A gives the line, B cancels */
static int kb_pad(u64 button) {
    if (button == 1ull << 0) return overlay_keyboard_key(OVERLAY_KEY_ENTER);
    if (button == 1ull << 1) return overlay_keyboard_key(OVERLAY_KEY_CANCEL);
    return OVERLAY_NONE;
}
int overlay_keyboard_typing(int sw, int sh, OverlayTyping* t) {
    KbTexts texts;
    Dialog d;
    DialogDrawn at;
    if (!atomic_load_explicit(&g_overlay.kb_shown, memory_order_relaxed)) return 0;
    kb_dialog(&d, &texts);
    dialog_draw(NULL, sw, sh, &d, g_overlay.event_rows, &at);
    t->ticket = g_overlay.kb.ticket;
    t->password = g_overlay.kb.password;
    t->numbers = g_overlay.kb.numbers;
    t->line = at.field;
    return 1;
}

int overlay_select(void) {
    changed();
    return page == PAGE_MAP ? map_key(SNX_PAD_KEY_SELECT) : act(focus, 0);
}
void overlay_move(int rows) {
    int n = items();
    changed();
    listening = 0;
    if (page == PAGE_MAP) {
        map_key(rows < 0 ? SNX_PAD_KEY_UP : SNX_PAD_KEY_DOWN);
        return;
    }
    focus = ((focus + rows) % n + n) % n;
    keep_in_view();
}
void overlay_scroll(int rows) {
    changed();
    listening = 0;
    if (page == PAGE_MAP) {
        overlay_move(rows);
        return;
    }
    focus = snx_clamp_int(focus + rows, 0, items() - 1);
    keep_in_view();
}
/* the first page's settings take no side step but Volume's and the TV mode row's, that one only on a press */
static int side(int step, int held) {
    changed();
    listening = 0;
    if (page == PAGE_KEYS) return OVERLAY_NONE; /* its rows have no value to step */
    if (page == PAGE_MAP)
        map_key(step < 0 ? SNX_PAD_KEY_LEFT : SNX_PAD_KEY_RIGHT);
    else if (page != PAGE_MAIN || main_item(focus) == M_VOLUME || (main_item(focus) == M_TV_MODE && !held))
        act(focus, step);
    return OVERLAY_NONE;
}
int overlay_side(int step) { return side(step, 0); }
int overlay_side_held(int step) { return side(step, 1); }
int overlay_keys(void) {
    changed();
    if (page == PAGE_KEYS) return OVERLAY_CLOSE;
    page_open(PAGE_KEYS);
    return OVERLAY_NONE;
}
int overlay_other(void) { return page == PAGE_KEYS ? OVERLAY_CLOSE : OVERLAY_NONE; }
int overlay_pad(u64 button) {
    switch (button) {
        case 1ull << 0: return overlay_select(); /* A */
        case 1ull << 1: return overlay_back();   /* B */
        case 1ull << 3: return overlay_delete(); /* Y */
        case 1ull << 12:                         /* the D-pad's left and the left stick's */
        case 1ull << 16: return overlay_side(-1);
        case 1ull << 13: /* up */
        case 1ull << 17: overlay_move(-1); break;
        case 1ull << 14: /* right */
        case 1ull << 18: return overlay_side(1);
        case 1ull << 15: /* down */
        case 1ull << 19: overlay_move(1); break;
        default: return overlay_other();
    }
    return OVERLAY_NONE;
}
int overlay_controls(u64 buttons, const i32 sticks[4], u64 ms) {
    static u64 seen, next;       /* the buttons of the last call; when a held direction moves again */
    const u64 dirs = 0xFF000ull; /* the D-pad's and the left stick's: bits 12 to 19 */
    const u64 sides = 1ull << 12 | 1ull << 14 | 1ull << 16 | 1ull << 18; /* their lefts and rights */
    u64 now = buttons | (sticks[0] < -16384 ? 1ull << 16 : 0) | (sticks[1] > 16384 ? 1ull << 17 : 0) |
              (sticks[0] > 16384 ? 1ull << 18 : 0) | (sticks[1] < -16384 ? 1ull << 19 : 0),
        pressed = atomic_load_explicit(&shown, memory_order_relaxed) ||
                          atomic_load_explicit(&g_overlay.kb_shown, memory_order_relaxed)
                      ? now & ~seen
                      : 0;
    int bit, did = OVERLAY_NONE;
    if (map_swallow) { /* the press the button map page took */
        map_swallow = 0;
        seen = now;
        return OVERLAY_NONE;
    }
    u64 held = 0;                   /* the lowest direction held, again */
    if ((pressed & dirs) ||
        !atomic_load_explicit(
            &shown, memory_order_relaxed)) /* a direction held as the menu opens waits as a fresh press would */
        next = ms + SNX_OVERLAY_REPEAT_DELAY;
    else if (atomic_load_explicit(&shown, memory_order_relaxed) && (now & dirs) && ms >= next) {
        held = now & dirs & (~(now & dirs) + 1);
        next = ms + SNX_OVERLAY_REPEAT_MS;
    }
    seen = now;
    for (bit = 0; bit < 20 && did == OVERLAY_NONE; ++bit)
        if (pressed >> bit & 1)
            did = atomic_load_explicit(&shown, memory_order_relaxed) ? overlay_pad(1ull << bit) : kb_pad(1ull << bit);
    if (did == OVERLAY_NONE && held)
        did = held & sides ? overlay_side_held(held & (1ull << 12 | 1ull << 16) ? -1 : 1) : overlay_pad(held);
    return did;
}
int overlay_back(void) {
    int from = page;
    changed();
    if (listening) { /* the wait for a key, cancelled */
        listening = 0;
        return OVERLAY_NONE;
    }
    if (page == PAGE_MAIN || page == PAGE_KEYS) return OVERLAY_CLOSE;
    if (page == PAGE_MAP) return map_key(SNX_PAD_KEY_BACK);
    if (page == PAGE_BUTTONS) {
        page_open(PAGE_CONTROLS);
        focus = C_BUTTONS;
    } else if (page == PAGE_ASK && ask == OVERLAY_EXIT) {
        page_open(PAGE_MAIN);
        focus = exit_row();
    } else if (page == PAGE_ASK && ask == ASK_RECORD) {
        tas_ask_done();
        tas_read(); /* the choice back to what runs */
    } else if (page == PAGE_ASK && ask == ASK_CLEAN) {
        page_open(PAGE_MAIN);
        focus = main_row(M_CLEAN);
        keep_in_view();
    } else if (page == PAGE_SECTION) { /* the Settings page, on its entry */
        page_open(PAGE_SETTINGS);
        focus = entry_at;
    } else if (page == PAGE_SETTINGS) {
        page_open(PAGE_MAIN);
        focus = main_row(M_SETTINGS);
    } else if (page == PAGE_ASK && ask == ASK_RESET) {
        page_open(PAGE_CONTROLS);
        focus = C_RESET;
    } else if (page == PAGE_ASK) {
        page = PAGE_SLOT;
        focus = slot_at               ? ask == OVERLAY_LOAD   ? S_LOAD
                                        : ask == OVERLAY_SAVE ? S_SAVE
                                                              : S_DELETE
                : ask == OVERLAY_LOAD ? 0
                                      : 1;
    } else if (page == PAGE_SLOT) {
        page = PAGE_STATES;
        focus = slot_at;
    } else {
        page_open(PAGE_MAIN);
        focus = from == PAGE_PADS        ? M_PADS
                : from == PAGE_STATES    ? M_STATES
                : from == PAGE_SHORTCUTS ? M_SHORTCUTS
                                         : M_CONTROLS;
    }
    keep_in_view();
    return OVERLAY_NONE;
}
static const char* pads_hint(char* out, size_t size);
/* the keys of the page showing, its foot's "Key: what." sentences, in out: the keyboard's, a controller's (its own
   names of A and B, overlay_input_pad) or a finger's; none while a key is waited for */
static void page_foot(char* out, size_t size) {
    const int pad = g_overlay.input.pad, finger = fingers && !pad;
    const char *ok = pad      ? g_overlay.input.ok
                     : finger ? "Tap"
                              : "Enter",
               *back = pad      ? g_overlay.input.back
                       : finger ? "Back"
                                : "Esc",
               *move = pad ? "D-pad" : "Up/Down", *sides = pad ? "D-pad" : "Left/Right", *def = pad ? "Y" : "Backspace";
    /* the narrowest panel's 38 columns take the first two, the key that goes back always among them */
    out[0] = 0;
    if (page == PAGE_MAP) return;
    if (page == PAGE_SHORTCUTS && listening && shortcut(focus) >= 0) { /* the wait, which the key back cancels */
        snprintf(out, size, "%s: cancel. Press the key for %s.", back, hotkey_row(shortcut(focus))->name);
        return;
    }
    if (page == PAGE_KEYS)
        snprintf(out, size, "%s: change. %s: game. %s: move.", ok, back, finger ? "Drag" : move);
    else if (page == PAGE_MAIN)
        snprintf(out, size, "%s: select. %s: game.", ok, back);
    else if (page == PAGE_SHORTCUTS && !finger)
        snprintf(out, size, "%s: new key. %s: back. %s: default.", ok, back, def);
    else if (page == PAGE_SHORTCUTS)
        snprintf(out, size, "%s: new key. %s: back.", ok, back);
    else if (page == PAGE_BUTTONS)
        snprintf(out, size, "%s: show or hide. %s: back.", ok, back);
    else if (page == PAGE_STATES && tas_row(focus) >= 0 && tas_row(focus) < T_BACK)
        snprintf(out, size, "%s: take. %s: back. %s: slot.", ok, back, sides);
    else
        snprintf(out, size, "%s: select. %s: back.", ok, back);
}
/* the page showing as a dialog: its title, its hint, the items in view, their texts in text and value. pads holds the
   title and the hint made for it */
static void menu_dialog(Dialog* d, char (*text)[OVERLAY_COLS_MAX + 1], char (*value)[OVERLAY_COLS_MAX + 1], char* pads,
                        size_t size) {
    static const char* const titles[] = {
        "SCARLETNX  /  MENU",      "SCARLETNX  /  CONTROLS",   "SCARLETNX  /  BUTTONS", "SCARLETNX  /  CONTROLLERS", "",
        "SCARLETNX  /  SHORTCUTS", "SCARLETNX  /  SAVE STATES"};
    /* what a page says over its rows; its keys are in its foot */
    static const char* const hints[] = {NULL, NULL, NULL, "", "", NULL, "The game waits while this page is open."};
    const OverlaySlot* sl = &slots[slot_at];
    char* title = pads + size / 2;
    int r, n = items();
    memset(d, 0, sizeof *d);
    page_foot(d->foot, sizeof d->foot);
    if (page == PAGE_KEYS) { /* F1's: a row a key, in focus as the other pages' are, where the focus is said */
        d->title = "SCARLETNX  /  KEYS";
        d->user = 1;
        d->items = n;
        d->top = top;
        d->focus = focus;
        d->shown = n - top < visible() ? n - top : visible();
        snprintf(d->count, sizeof d->count, "%d / %d", focus + 1, n);
        for (r = 0; r < KEYS_COUNT; ++r) /* the widest keys of every row that shows, the ones scrolled away too */
            if (keys_text(r, *text, sizeof *text))
                d->keys = (int)strlen(*text) > d->keys ? (int)strlen(*text) : d->keys;
        for (r = 0; r < d->shown; ++r) {
            int k = keys_at(top + r);
            if (k < 0) { /* the debugger's, last */
                d->key[r] = "Debugger";
                d->icon[r] = GUI_ICON_BUG;
                d->label[r] = keys_debugger;
                d->dim[r] = 1;
                continue;
            }
            keys_text(k, text[r], sizeof *text);
            d->key[r] = text[r];
            d->icon[r] = KEYS[k].icon;
            d->label[r] = KEYS[k].what;
            d->dim[r] = KEYS[k].action < 0; /* the game's buttons: no key of the runtime's to change */
        }
        return;
    }
    d->title = page < PAGE_SLOT ? titles[page] : title;
    d->user = 1;
    d->para[0].text = page == PAGE_PADS ? pads_hint(pads, size / 2) : page < PAGE_SLOT ? hints[page] : pads;
    d->para[0].rgba = UI_SOFT;
    d->paras = 1;
    if (page_picked()) { /* with fingers, a tap on the line takes the focused row's pick out (overlay_tap) */
        d->para[1].text = fingers ? "*: set for this game alone. Tap here: the row in focus back to Global."
                                  : "*: set for this game alone. Y or Delete: back to Global.";
        d->para[1].rgba = UI_SOFT;
        d->para[1].link = fingers;
        d->paras = 2;
    }
    if (page == PAGE_SHORTCUTS && refused[0]) { /* why the last key given isn't the row's */
        d->para[1].text = refused;
        d->para[1].rgba = UI_FAILURE;
        d->para[1].on = 1;
        d->paras = 2;
    }
    if (page == PAGE_ASK && (ask == OVERLAY_EXIT || ask == ASK_RESET)) {
        snprintf(title, size / 2, "SCARLETNX  /  %s", ask == OVERLAY_EXIT ? "EXIT THE GAME?" : "RESET THE CONTROLS?");
        snprintf(pads, size / 2, "%s",
                 ask == OVERLAY_EXIT ? "The game ends: what was played since it last saved is lost."
                                     : "The on-screen controls go back where they start.");
    } else if (page == PAGE_SETTINGS || page == PAGE_SECTION) {
        char* c;
        if (page == PAGE_SETTINGS)
            snprintf(title, size / 2, "SCARLETNX  /  SETTINGS");
        else
            snprintf(title, size / 2, "SCARLETNX  /  SETTINGS  /  %s", entry_name(entry_at));
        for (c = title; *c; ++c) *c = (char)toupper((unsigned char)*c);
        snprintf(pads, size / 2, "%s",
                 page == PAGE_SETTINGS ? "This game's own, over the launcher's Settings for every game."
                                       : "(next start): read when the game starts, taken at its next start.");
    } else if (page == PAGE_ASK && ask == ASK_CLEAN) {
        int n = ini_old_settings();
        snprintf(title, size / 2, "SCARLETNX  /  CLEAN OLD SETTINGS?");
        snprintf(pads, size / 2, "%d key%s this version does not use go from the game's .ini and its own picks.", n,
                 n == 1 ? "" : "s");
    } else if (page == PAGE_ASK && ask == ASK_RECORD) {
        snprintf(title, size / 2, "SCARLETNX  /  RECORD OVER SLOT %d?", tas_pick[T_RECORD]);
        if (tas_frames[tas_pick[T_RECORD]] > 0)
            snprintf(pads, size / 2, "Its recording, %d frames, is replaced.", tas_frames[tas_pick[T_RECORD]]);
        else
            snprintf(pads, size / 2, "Its file, which is no recording, is replaced.");
    } else if (page >= PAGE_SLOT) {
        char name[16];
        if (slot_at)
            snprintf(name, sizeof name, "SLOT %d", slot_at);
        else
            snprintf(name, sizeof name, "AUTO SLOT");
        if (page == PAGE_SLOT)
            snprintf(title, size / 2, "SCARLETNX  /  %s", name);
        else
            snprintf(title, size / 2, "SCARLETNX  /  %s %s?",
                     ask == OVERLAY_LOAD   ? "LOAD"
                     : ask == OVERLAY_SAVE ? "SAVE OVER"
                                           : "DELETE",
                     name);
        if (page == PAGE_ASK && ask == OVERLAY_LOAD)
            snprintf(pads, size / 2, "The game goes back to %s.%s", sl->when,
                     g_settings.save_state_backup ? " What was played since is kept in the auto slot." : "");
        else if (page == PAGE_ASK)
            snprintf(pads, size / 2,
                     ask == OVERLAY_SAVE ? "The state saved %s is replaced." : "The state saved %s is deleted.",
                     sl->when);
        else
            snprintf(pads, size / 2, "%s", sl->when[0] ? sl->when : "Empty.");
        if (page == PAGE_SLOT && sl->why[0]) { /* why it can't load, under its date */
            d->para[1].text = sl->why;
            d->para[1].rgba = UI_FAILURE;
            d->para[1].on = 1;
            d->paras = 2;
        }
    }
    d->aside = page >= PAGE_STATES && page <= PAGE_ASK ? 1 : 0; /* the save states' picture */
    d->items = n;
    d->top = top;
    d->focus = focus;
    d->shown = n - top < visible() ? n - top : visible();
    for (r = 0; r < d->shown; ++r) {
        const IniOpt* o = slider(top + r);
        label(top + r, text[r], sizeof *text, value[r], sizeof *value);
        d->label[r] = text[r];
        d->icon[r] = page == PAGE_MAIN ? MAIN_ICONS[main_item(top + r)] : GUI_ICON_NONE;
        d->tone[r] = item_tone(top + r);
        d->value[r] = value[r];
        if (o) {
            d->slider_cols[r] = slider_cols(o);
            d->knob[r] = slider_place(o);
        }
    }
}
/* pixel x on bar, a slider drawn at scale: the knob's place it puts the knob's middle at, in thousandths of its way */
static int bar_place(const SnxOverlayRect* bar, float x, int scale) {
    float cw = 8.0f * (float)scale, travel = bar->w - cw;
    return travel > 0 ? snx_clamp_int((int)((x - bar->x - cw / 2) * 1000.0f / travel + 0.5f), 0, 1000) : 0;
}
int overlay_tap(float x, float y, int sw, int sh) {
    char text[VISIBLE][OVERLAY_COLS_MAX + 1], value[VISIBLE][OVERLAY_COLS_MAX + 1], pads[2 * OVERLAY_COLS_MAX + 64];
    Dialog d;
    DialogDrawn at;
    int r, col;
    changed();
    if (!atomic_load_explicit(&shown, memory_order_relaxed) &&
        atomic_load_explicit(&g_overlay.kb_shown, memory_order_relaxed))
        return kb_tap(x, y, sw, sh);
    if (listening) { /* a tap, no key: the wait cancelled */
        listening = 0;
        return OVERLAY_NONE;
    }
    if (page == PAGE_MAP) return map_did(snx_pad_screen_tap(&map, snx_pad_screen_hit(&map_drawing, x, y)));
    menu_dialog(&d, text, value, pads, sizeof pads);
    dialog_draw(NULL, sw, sh, &d, g_overlay.event_rows, &at);
    if (!overlay_error_hit(&at.box, x, y)) return OVERLAY_CLOSE;
    for (r = 0; r < at.chips; ++r) /* a key of the foot: what it does */
        if (overlay_error_hit(&at.chip[r], x, y))
            return at.chip_do[r] == FOOT_SELECT ? act(focus, 0)
                   : at.chip_do[r] == FOOT_BACK ? overlay_back()
                                                : overlay_backspace();
    if (at.link.w > 0 && overlay_error_hit(&at.link, x, y)) return overlay_delete(); /* the picks' line */
    for (r = 0; r < d.shown && !overlay_error_hit(&at.item[r], x, y); ++r) {}
    if (r == d.shown) return OVERLAY_NONE;
    focus = d.top + r;
    col = (int)((x - at.box.x) / (8.0f * (float)at.scale));
    /* a slider's row: a tap on the slider takes the step under it, one left of it steps it down and one right of it
       (its value) up */
    if (at.bar[r].w > 0 && x >= at.bar[r].x && x < at.bar[r].x + at.bar[r].w)
        return slide_to(focus, bar_place(&at.bar[r], x, at.scale));
    if (at.bar[r].w > 0) return act(focus, x < at.bar[r].x ? -1 : 1);
    /* a value's row: a tap on its last five columns (the right arrow's) steps it up, a tap anywhere left of them
       down */
    if (arrows(setting(focus))) return act(focus, col < at.cols - 5 ? -1 : 1);
    /* Record input and Play input: a tap on the last five columns chooses the next slot, one anywhere else takes it */
    if (tas_row(focus) >= 0 && tas_row(focus) < T_BACK && col >= at.cols - 5) return act(focus, 1);
    return act(focus, 0);
}
/* whether x, y lies on the open menu's panel on the picture overlay_place last had: a page, not the button map's or
   F1's, which a tap anywhere answers */
static int on_panel(float x, float y) {
    char text[VISIBLE][OVERLAY_COLS_MAX + 1], value[VISIBLE][OVERLAY_COLS_MAX + 1], pads[2 * OVERLAY_COLS_MAX + 64];
    Dialog d;
    DialogDrawn at;
    if (!atomic_load_explicit(&shown, memory_order_relaxed) || page == PAGE_MAP || picture[0] <= 0 || picture[1] <= 0)
        return 0;
    menu_dialog(&d, text, value, pads, sizeof pads);
    dialog_draw(NULL, picture[0], picture[1], &d, g_overlay.event_rows, &at);
    return overlay_error_hit(&at.box, x, y);
}
static u64 tap_id;      /* what went down on the panel or off the on-screen controls, */
static int tap_down;    /* whose lift taps the menu where it went down, */
static float tap_at[2]; /* there, */
static int tap_dragged; /* unless it went further than a finger's slip: then it scrolls the page, */
static int tap_rows;    /* by this many rows so far */
/* or it went down on a slider: the item's, -1 for none, which follows it until it lifts, on its bar as drawn then at
   the scale then */
static int tap_slider = -1, tap_scale;
static SnxOverlayRect tap_bar;
/* the item whose slider lies under x, y on the picture overlay_place last had, with its bar and the scale; -1 none */
static int slider_under(float x, float y, SnxOverlayRect* bar, int* scale) {
    char text[VISIBLE][OVERLAY_COLS_MAX + 1], value[VISIBLE][OVERLAY_COLS_MAX + 1], pads[2 * OVERLAY_COLS_MAX + 64];
    Dialog d;
    DialogDrawn at;
    int r;
    if (!atomic_load_explicit(&shown, memory_order_relaxed) || page == PAGE_MAP || picture[0] <= 0 || picture[1] <= 0)
        return -1;
    menu_dialog(&d, text, value, pads, sizeof pads);
    dialog_draw(NULL, picture[0], picture[1], &d, g_overlay.event_rows, &at);
    for (r = 0; r < d.shown; ++r)
        if (at.bar[r].w > 0 && overlay_error_hit(&at.item[r], x, y) && x >= at.bar[r].x &&
            x < at.bar[r].x + at.bar[r].w) {
            *bar = at.bar[r];
            *scale = at.scale;
            return d.top + r;
        }
    return -1;
}
void overlay_pointer_down(u64 id, float x, float y, const int safe[4]) {
    /* on the panel a finger is the menu's, whatever control lies under it; off it a control's, but under the loading
       screen, where they aren't drawn (vk/ui.c) */
    if (!on_panel(x, y) && !prepare_screen_shown() && vpad_down(id, x, y, safe)) return;
    tap_id = id;
    tap_down = 1;
    tap_at[0] = x;
    tap_at[1] = y;
    tap_dragged = tap_rows = 0;
    /* on a slider: the step under it at once, its focus, and the drag after it */
    tap_slider = slider_under(x, y, &tap_bar, &tap_scale);
    if (tap_slider < 0) return;
    changed();
    listening = 0;
    focus = tap_slider;
    slide_to(focus, bar_place(&tap_bar, x, tap_scale));
}
/* a drag's rows: the page goes by under the finger, the item in focus kept on it */
static void page_scroll(int rows) {
    int n = items();
    changed();
    listening = 0;
    top = snx_clamp_int(top + rows, 0, n > visible() ? n - visible() : 0);
    focus = snx_clamp_int(focus, top, top + visible() - 1);
}
/* the mouse over the open menu, no button held: the row under it takes the focus, as the launcher's do */
static void hover(float x, float y) {
    char text[VISIBLE][OVERLAY_COLS_MAX + 1], value[VISIBLE][OVERLAY_COLS_MAX + 1], pads[2 * OVERLAY_COLS_MAX + 64];
    Dialog d;
    DialogDrawn at;
    if (!on_panel(x, y) || listening) return;
    menu_dialog(&d, text, value, pads, sizeof pads);
    dialog_draw(NULL, picture[0], picture[1], &d, g_overlay.event_rows, &at);
    for (int r = 0; r < d.shown; ++r)
        if (overlay_error_hit(&at.item[r], x, y) && focus != d.top + r) {
            focus = d.top + r;
            changed();
        }
}
void overlay_pointer_move(u64 id, float x, float y, const int safe[4]) {
    /* a finger's slip: 8 pixels at the display's scale, as a tap a phone takes */
    float slip = 8.0f * (float)atomic_load_explicit(&display_percent, memory_order_relaxed) / 100.0f, dx, dy;
    int row, rows;
    if (!tap_down || id != tap_id) {
        if (id == SNX_TOUCH_MOUSE && !tap_down) hover(x, y);
        vpad_move(id, x, y, safe);
        return;
    }
    if (tap_slider >= 0) { /* the knob follows it across, and past either end stays there */
        changed();
        slide_to(tap_slider, bar_place(&tap_bar, x, tap_scale));
        return;
    }
    dx = x - tap_at[0];
    dy = y - tap_at[1];
    if (!tap_dragged && dx * dx + dy * dy < slip * slip) return;
    tap_dragged = 1;
    if (!atomic_load_explicit(&shown, memory_order_relaxed) || page == PAGE_MAP || picture[1] <= 0) return;
    row = item_h(place_scale(picture[0], picture[1]));
    rows = (int)(-dy / (float)row); /* up a row, the page a row on */
    if (rows != tap_rows) page_scroll(rows - tap_rows);
    tap_rows = rows;
}
int overlay_pointer_up(u64 id, float x, float y, int sw, int sh) {
    int slid = tap_slider >= 0;
    if (vpad_up(id) || !tap_down || id != tap_id) return OVERLAY_NONE;
    tap_down = 0;
    tap_slider = -1;
    (void)x;
    (void)y;
    return tap_dragged || slid ? OVERLAY_NONE : overlay_tap(tap_at[0], tap_at[1], sw, sh);
}
void overlay_pointer_cancel(u64 id) {
    if (id != tap_id) return;
    tap_down = 0;
    tap_slider = -1;
}
void overlay_pointers_reset(void) {
    tap_down = 0;
    tap_slider = -1;
}
/* the gamepads' page's second row: the first one connected by name, and how many more */
static const char* pads_hint(char* out, size_t size) {
    int more = 0;
    if (!gamepad_name(0)) return "None connected.";
    while (gamepad_name(more + 1)) ++more;
    if (more)
        snprintf(out, size, "%.27s and %d more", gamepad_name(0), more);
    else
        snprintf(out, size, "%s", gamepad_name(0));
    return out;
}
/* the notifications: a band of one row each from the picture's top left corner down, the oldest first, which is its
   icon, a blank cell and its letters, as the stats box's lines, backed (ui.h's UI_BACKED) by the layer's ink, between a
   blank cell at either end: nothing is drawn under them. Their letters are the stats box's and showcase's
   (stats_scale), the other text over a running game, not the menu's that stops it. Each line's first quad into span,
   and its end after the last */
static float toast_width(int i, float cw) {
    return cw * (float)(strlen(toasts[i].text) + 2 + (toasts[i].icon != GUI_ICON_NONE ? 3 : 0));
}
/* whether the boxes a and b (x, y, w, h each) have a point in common */
static int boxes_meet(const float a[4], const float b[4]) {
    return a[0] < b[0] + b[2] && a[0] + a[2] > b[0] && a[1] < b[1] + b[3] && a[1] + a[3] > b[1];
}
static void toast_draw(SnxUiList* l, int sw, int sh, const float* stats, const float* debug,
                       int span[OVERLAY_TOASTS + 1]) {
    int px, py, s, i, right = 0;
    float cw, ch, x, y, w = 0, h;
    SNX_ZONE_V(z_toast, "plat overlay toast lay", (u64)ntoasts);
    overlay_place(sw, sh, &px, &py, &s);
    s = stats_scale(s);
    cw = 8.0f * (float)s;
    ch = 16.0f * (float)s;
    x = cw;
    y = ch;
    for (i = 0; i < ntoasts; ++i)
        if (toast_width(i, cw) > w) w = toast_width(i, cw);
    h = ch * (float)ntoasts;
    /* the stats' box (x, y, w, h; NULL for none) in that corner too: the bands go under it, from its left, half a
       line apart, where they would lie on it; in the picture's top right corner, each to its edge, when there is no
       room under it and under the Debug list, where that is */
    if (stats && boxes_meet((const float[4]){x, y, w, h}, stats)) {
        x = stats[0];
        y = stats[1] + stats[3] + ch / 2;
    }
    /* the Debug list's (debug) too, which is under the stats' box when both are shown */
    if (debug && boxes_meet((const float[4]){x, y, w, h}, debug)) y = debug[1] + debug[3] + ch / 2;
    if (y + h > (float)sh) {
        right = 1;
        y = ch;
    }
    for (i = 0; i < ntoasts; ++i) {
        float at = right ? (float)sw - cw - toast_width(i, cw) : x, row = y + (float)i * ch;
        span[i] = l->n;
        at += ui_text_backed(l, at, row, s, " ", UI_PAGE);
        if (toasts[i].icon != GUI_ICON_NONE) {
            at += ui_icon_backed(l, at, row, s, toasts[i].icon, UI_PAGE);
            at += ui_text_backed(l, at, row, s, " ", UI_PAGE);
        }
        at += ui_text_backed(l, at, row, s, toasts[i].text, UI_PAGE);
        ui_text_backed(l, at, row, s, " ", UI_PAGE);
    }
    span[ntoasts] = l->n;
}
/* the slot whose thumbnail shows beside the save states' pages: the one in focus on the list, or the page's. Given
   again when the layer's picture is no longer the one it gave (the loading screen's cover took it meanwhile) */
static void aside_draw(SnxUiList* l, const SnxOverlayRect* r) {
    static u32 gen, given;
    static int at = -1;
    int slot = page == PAGE_STATES ? (focus < nslots ? focus : -1) : slot_at;
    float e = r->w / (float)(SNX_UI_IMAGE_W / 2); /* the scale: a pixel of it */
    u32 version;
    ui_image_get(&version);
    if (slot != at || gen != slots_gen || version != given) {
        ui_image_set(slot >= 0 ? slots[slot].thumb : NULL);
        ui_image_get(&given);
        at = slot;
        gen = slots_gen;
    }
    ui_fill(l, r->x - e, r->y - e, r->w + 2 * e, r->h + 2 * e, UI_INK);
    if (!ui_image(l, r->x, r->y, r->w, r->h)) {
        ui_fill(l, r->x, r->y, r->w, r->h, UI_PAGE);
        ui_text(l, r->x + (r->w - 8.0f * 8 * e) / 2, r->y + (r->h - 16 * e) / 2, (int)e,
                slot >= 0 && slots[slot].when[0] ? "No image" : "Empty", UI_SOFT);
    }
}
/* The menu's page or the keyboard's box as laid out last, kept from frame to frame as the controls are (vpad.c):
   laid out again only when the overlay's count of its changes differs (g_changes), or the picture's size, or what
   they read from elsewhere: the keys' and the gamepads' counts, the TV mode, whether a frame can be captured, the
   users, and the settings the page's rows show, whose variables are watched (g_menu.watch) */
enum { MENU_NONE, MENU_PAGE, MENU_KB };
enum { MENU_QUADS = 4096, MENU_WATCH = SNX_VPAD_COUNT + VISIBLE + 2 };
typedef struct {
    int sw, sh, what, tv, capture, users;
    u32 changes, hotkeys, pads;
} MenuKey;
static struct {
    int laid;
    MenuKey key;
    const int* watch[MENU_WATCH];
    int was[MENU_WATCH], watched;
    SnxUiQuad q[MENU_QUADS];
    int n;
    u32 version;
} g_menu;
static struct {
    SnxUiQuad q[OVERLAY_TOASTS * (OVERLAY_COLS_MAX + 4)]; /* a line's letters, its blanks and its icon */
    int n, sw, sh, percent, boxed;
    int span[OVERLAY_TOASTS + 1]; /* each line's first quad, and the end of them (toast_draw) */
    float stats[4];               /* the stats' box it was placed for, when boxed */
    float debug[4];               /* the Debug list's, when listed */
    int listed;
    u32 given, version;
} g_toast;
static void watch(const void* var) {
    if (g_menu.watched == MENU_WATCH) return;
    g_menu.watch[g_menu.watched] = (const int*)var;
    g_menu.was[g_menu.watched++] = *(const int*)var;
}
static int watched_same(void) {
    int i;
    for (i = 0; i < g_menu.watched; ++i)
        if (*g_menu.watch[i] != g_menu.was[i]) return 0;
    return 1;
}
static void menu_lay(int sw, int sh, const MenuKey* key) {
    static DialogRows rows_of; /* the renderer's */
    SNX_ZONE_TV(z_menu, "plat overlay menu lay", (u64)key->what, "dialog %d", (int)key->what);
    char text[VISIBLE][OVERLAY_COLS_MAX + 1], value[VISIBLE][OVERLAY_COLS_MAX + 1], pads[2 * OVERLAY_COLS_MAX + 64];
    SnxUiList l = {g_menu.q, 0, MENU_QUADS};
    Dialog d;
    DialogDrawn at;
    int r;
    g_menu.watched = 0;
    if (key->what == MENU_KB) {
        KbTexts t;
        kb_dialog(&d, &t);
        dialog_draw(&l, sw, sh, &d, rows_of, &at);
    } else if (key->what == MENU_PAGE) {
        menu_dialog(&d, text, value, pads, sizeof pads);
        dialog_draw(&l, sw, sh, &d, rows_of, &at);
        if (at.aside.w > 0) aside_draw(&l, &at.aside);
        if (page != PAGE_KEYS)
            for (r = 0; r < d.shown; ++r) {
                const IniOpt* o = setting(d.top + r);
                if (o) watch(o->var);
            }
        if (page == PAGE_BUTTONS)
            for (r = 0; r < SNX_VPAD_COUNT; ++r) watch(&g_settings.control_shown[r]); /* All's */
        if (page == PAGE_ASK) watch(&g_settings.save_state_backup);
        watch(ini_picks_changes()); /* a pick the launcher set or took out meanwhile: its * */
    }
    g_menu.n = l.n;
    g_menu.key = *key;
    g_menu.laid = 1;
    ++g_menu.version;
}
/* a save state's load: the shade over the whole picture, its alpha the fade's (the first faded span, which the
   notifications' come after), then the box and its ring, on the row of its words */
static void loading_draw(SnxUiList* l, int sw, int sh, u64 now_ns) {
    const u64 in = now_ns > g_overlay.loading.since_ns ? (now_ns - g_overlay.loading.since_ns) / 1000000u : 0;
    const unsigned alpha = in >= OVERLAY_LOADING_FADE_MS ? 128 : (unsigned)(128 * in / OVERLAY_LOADING_FADE_MS);
    const DialogPara para[2] = {{"Loading the save state", UI_INK, 0, 0, 0, 0},
                                {g_overlay.loading.slot, UI_SOFT, 0, 1, 0, 0}};
    const int first = l->n;
    Dialog d;
    DialogDrawn at;
    ui_fill(l, 0, 0, (float)sw, (float)sh, 0xFF000000u);
    ui_fade_set(first, l->n, alpha);
    memset(&d, 0, sizeof d);
    d.title = "SCARLETNX  /  SAVE STATES";
    memcpy(d.para, para, sizeof para);
    d.paras = 2;
    dialog_draw(l, sw, sh, &d, g_overlay.loading.rows, &at);
    {
        const float cw = 8.0f * (float)at.scale, ch = 16.0f * (float)at.scale;
        ui_loading_ring(l, at.box.x + at.box.w - cw * 3, at.box.y + ch * 2, at.scale, now_ns / 1000000u, 0,
                        UI_RGB(SNX_GUI_THEME_YORHA_DIM), UI_SOFT, UI_INK);
    }
}
void overlay_draw(SnxUiList* l, int sw, int sh, u64 now_ns) {
    MenuKey key;
    int before = l->n; /* the list's quads before this overlay's, so the plot counts only the overlay's own */
    SNX_ZONE(z_draw, "plat overlay draw");
    if (atomic_load_explicit(&notice_given, memory_order_acquire)) {
        pm_lock(&notice_lock);
        if (notice[0]) overlay_toast(notice_icon, notice, now_ns);
        notice[0] = 0;
        atomic_store_explicit(&notice_given, 0, memory_order_relaxed);
        pm_unlock(&notice_lock);
    }
    if (atomic_load_explicit(&shown, memory_order_relaxed) &&
        page == PAGE_MAP) { /* the button map's page shows what the pads hold as they hold it */
        map_draw(l, sw, sh);
        g_menu.laid = 0;
        SNX_ZONE_VALUE(z_draw, 3); /* button map */
        SNX_PLOT("plat overlay quads", (double)(l->n - before));
        return;
    }
    memset(&key, 0, sizeof key);
    key.sw = sw;
    key.sh = sh;
    key.what = atomic_load_explicit(&shown, memory_order_relaxed)                ? MENU_PAGE
               : atomic_load_explicit(&g_overlay.kb_shown, memory_order_relaxed) ? MENU_KB
                                                                                 : MENU_NONE;
    SNX_ZONE_VALUE(z_draw, key.what); /* 0: toasts only, 1: menu, 2: keyboard, 3: map */
    key.changes = atomic_load_explicit(&g_changes, memory_order_acquire);
    if (key.what != MENU_NONE) {
        key.hotkeys = hotkeys_version();
        key.pads = gamepad_names_version();
        key.tv = options_tv_mode_choice();
        key.capture = g_renderer->capture(0);
        key.users = g_user_count;
        if (!g_menu.laid || memcmp(&key, &g_menu.key, sizeof key) || !watched_same()) menu_lay(sw, sh, &key);
        ui_append_kept(l, &(SnxUiList){g_menu.q, g_menu.n, g_menu.n}, g_menu.version, &g_menu, NULL);
    }
    if (overlay_loading_shown()) loading_draw(l, sw, sh, now_ns);
    if (!atomic_load_explicit(&shown, memory_order_relaxed) &&
        ctrl_dialog_shown()) { /* the controller applet's, under the menu */
        int px, py, scale;
        overlay_place(sw, sh, &px, &py, &scale);
        ctrl_dialog_draw(l, sw, sh, scale);
    }
    toasts_drop(now_ns);
    if (ntoasts) {
        int percent = overlay_display_percent(), first = l->n, i;
        float stats[4] = {0}, debug[4] = {0};
        int boxed = stats_box_drawn(stats), listed = hud_box_drawn(debug); /* both kept clear of */
        if (g_toast.given != toast_version || g_toast.sw != sw || g_toast.sh != sh || g_toast.percent != percent ||
            g_toast.boxed != boxed || g_toast.listed != listed || memcmp(g_toast.stats, stats, sizeof stats) ||
            memcmp(g_toast.debug, debug, sizeof debug) || !g_toast.version) {
            SnxUiList t = {g_toast.q, 0, (int)(sizeof g_toast.q / sizeof *g_toast.q)};
            toast_draw(&t, sw, sh, boxed ? stats : NULL, listed ? debug : NULL, g_toast.span);
            g_toast.boxed = boxed;
            g_toast.listed = listed;
            memcpy(g_toast.stats, stats, sizeof stats);
            memcpy(g_toast.debug, debug, sizeof debug);
            g_toast.n = t.n;
            g_toast.given = toast_version;
            g_toast.sw = sw;
            g_toast.sh = sh;
            g_toast.percent = percent;
            ++g_toast.version;
        }
        ui_append_kept(l, &(SnxUiList){g_toast.q, g_toast.n, g_toast.n}, g_toast.version, &g_toast, NULL);
        for (i = 0; i < ntoasts; ++i) /* as one fades: its quads the same, their alpha moving; whole, no span */
            ui_fade_set(first + g_toast.span[i], first + g_toast.span[i + 1], (unsigned)overlay_toast_alpha(i, now_ns));
    }
    SNX_PLOT("plat overlay quads", (double)(l->n - before));
}
int overlay_error_hit(const SnxOverlayRect* r, float x, float y) {
    return r && r->w > 0 && r->h > 0 && x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}
void overlay_error_draw(SnxUiList* l, int sw, int sh, SnxOverlayError* e) {
    static DialogRows rows_of; /* the thread that shows the panel's */
    static const char* const labels[SNX_OVERLAY_ERROR_ITEMS] = {"Share Log", "Copy path", "Close"};
    /* on a short picture the log's label gives up rows first, then the hint, the log and the error; never the game's
       name or what Share Log or Copy path did. The log goes right under its label */
    const DialogPara para[] = {
        {e->name, UI_SOFT, 0, 0, 0, 0}, {e->msg, UI_FAILURE, 0, 0, 4, 0}, {fatal_hint(e->msg), UI_INK, 0, 0, 2, 0},
        {e->how, UI_SOFT, 0, 0, 1, 0},  {e->log, UI_INK, 1, 1, 3, 0},     {e->said, UI_SOFT, 0, 0, 0, 0}};
    Dialog d;
    DialogDrawn at;
    int i;
    memset(&d, 0, sizeof d);
    d.title = "SCARLETNX  /  STOPPED";
    memcpy(d.para, para, sizeof para);
    d.paras = (int)(sizeof para / sizeof *para);
    d.items = d.shown = SNX_OVERLAY_ERROR_ITEMS;
    d.focus = e->focus;
    for (i = 0; i < SNX_OVERLAY_ERROR_ITEMS; ++i) d.label[i] = labels[i];
    dialog_draw(l, sw, sh, &d, rows_of, &at);
    e->link = at.link;
    for (i = 0; i < SNX_OVERLAY_ERROR_ITEMS; ++i) e->items[i] = at.item[i];
}
