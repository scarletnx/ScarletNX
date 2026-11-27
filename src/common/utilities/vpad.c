/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/vpad.h"
#include "common/utilities/game_ini.h"
#include "common/utilities/ini_lines.h"
#include "common/utilities/ui_colors.h"
#include "common/utilities/bits.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

enum { KIND_BUTTON, KIND_DPAD, KIND_STICK };
typedef struct {
    const char* name;
    int shape;         /* its look (ui.h's ui_shape): a UI_SHAPE_ kind, */
    const char* label; /* the font's letters on it, */
    float aspect;      /* and its width over its height */
    int kind;
    int bit;    /* its HidNpadButton bit, -1 for none */
    float size; /* its longer side, of the clear part's shorter one, as rEden sizes it */
    float x, y; /* where it starts: the console's controllers, the left one's on the left */
    float tx, ty; /* and in a window taller than wide, under the picture: the Vita's places, ZL and ZR over L and R */
} Info;
/* a control's look, the art's shapes: a shoulder button's box as wide as the art's (367 x 148), a stick's button's as
   tall (157 x 168) */
#define LOOK(shape, label, aspect) shape, label, aspect
#define WIDE                       (367.0f / 148)
#define TALL                       (157.0f / 168)
/* each control: its SNX_VPAD_ name, then its Info. INFO is made from this list, its order is checked against vpad.h's
   enum, and the keys of settings.txt's list controls against SNX_VPAD_IDS' on first use */
#define VPAD_CONTROLS(ROW)                                                                                             \
    ROW(L, "L", LOOK(UI_SHAPE_ROUND, "L", WIDE), KIND_BUTTON, 6, 0.26f, 0.09f, 0.21f, 0.160f, 0.670f)                  \
    ROW(R, "R", LOOK(UI_SHAPE_ROUND, "R", WIDE), KIND_BUTTON, 7, 0.26f, 0.91f, 0.21f, 0.840f, 0.670f)                  \
    ROW(ZL, "ZL", LOOK(UI_SHAPE_ROUND, "ZL", WIDE), KIND_BUTTON, 8, 0.26f, 0.09f, 0.08f, 0.160f, 0.615f)               \
    ROW(ZR, "ZR", LOOK(UI_SHAPE_ROUND, "ZR", WIDE), KIND_BUTTON, 9, 0.26f, 0.91f, 0.08f, 0.840f, 0.615f)               \
    ROW(MINUS, "-", LOOK(UI_SHAPE_CIRCLE, "-", 1), KIND_BUTTON, 11, 0.07f, 0.25f, 0.07f, 0.360f, 0.670f)               \
    ROW(PLUS, "+", LOOK(UI_SHAPE_CIRCLE, "+", 1), KIND_BUTTON, 10, 0.07f, 0.75f, 0.07f, 0.640f, 0.670f)                \
    ROW(CAPTURE, "Capture", LOOK(UI_SHAPE_ROUND, "\x07", 1), KIND_BUTTON, -1, 0.07f, 0.36f, 0.90f, 0.460f, 0.965f)     \
    ROW(HOME, "Home", LOOK(UI_SHAPE_CIRCLE, "\x7f", 1), KIND_BUTTON, -1, 0.07f, 0.64f, 0.90f, 0.558f, 0.965f)          \
    ROW(L3, "L3", LOOK(UI_SHAPE_CIRCLE, "L3", TALL), KIND_BUTTON, 4, 0.155f, 0.25f, 0.36f, 0.350f, 0.745f)             \
    ROW(R3, "R3", LOOK(UI_SHAPE_CIRCLE, "R3", TALL), KIND_BUTTON, 5, 0.155f, 0.64f, 0.62f, 0.520f, 0.870f)             \
    ROW(DPAD, "D-pad", LOOK(UI_SHAPE_CROSS, "", 1), KIND_DPAD, -1, 0.25f, 0.23f, 0.78f, 0.270f, 0.935f)                \
    ROW(STICK_L, "Left stick", LOOK(UI_SHAPE_CIRCLE, "", 1), KIND_STICK, -1, 0.30f, 0.12f, 0.47f, 0.120f, 0.809f)      \
    ROW(STICK_R, "Right stick", LOOK(UI_SHAPE_CIRCLE, "", 1), KIND_STICK, -1, 0.30f, 0.77f, 0.78f, 0.770f, 0.921f)     \
    ROW(A, "A", LOOK(UI_SHAPE_CIRCLE, "A", 1), KIND_BUTTON, 0, 0.11f, 0.93f, 0.47f, 0.910f, 0.770f)                    \
    ROW(B, "B", LOOK(UI_SHAPE_CIRCLE, "B", 1), KIND_BUTTON, 1, 0.11f, 0.87f, 0.60f, 0.800f, 0.815f)                    \
    ROW(X, "X", LOOK(UI_SHAPE_CIRCLE, "X", 1), KIND_BUTTON, 2, 0.11f, 0.87f, 0.34f, 0.800f, 0.725f)                    \
    ROW(Y, "Y", LOOK(UI_SHAPE_CIRCLE, "Y", 1), KIND_BUTTON, 3, 0.11f, 0.81f, 0.47f, 0.690f, 0.770f)
#define INFO_ROW(id, name, look, kind, bit, size, x, y, tx, ty) {name, look, kind, bit, size, x, y, tx, ty},
static const Info INFO[SNX_VPAD_COUNT] = {VPAD_CONTROLS(INFO_ROW)};
#define ORDER_INDEX(id, name, look, kind, bit, size, x, y, tx, ty) ORDER_##id,
enum { VPAD_CONTROLS(ORDER_INDEX) ORDER_COUNT };
#define ORDER_CHECK(id, name, look, kind, bit, size, x, y, tx, ty)                                                     \
    typedef char vpad_order_##id[SNX_VPAD_##id == ORDER_##id ? 1 : -1];
VPAD_CONTROLS(ORDER_CHECK)
typedef char vpad_order_count[ORDER_COUNT == SNX_VPAD_COUNT ? 1 : -1];
/* the D-pad's directions (HidNpadButton bits 12 to 15) */
#define DPAD_LEFT  (1u << 12)
#define DPAD_UP    (1u << 13)
#define DPAD_RIGHT (1u << 14)
#define DPAD_DOWN  (1u << 15)
#define FINGERS    10
#define STICK_KNOB 1.66f /* the ring is this many knobs wide (rEden) */
#define DEADZONE   0.5f  /* of the D-pad's half: rEden's */

#define DEF_SIZE    100 /* a control's, in percent */
#define FIRST_PLACE 6   /* g_vpad_ini's first place row (settings.txt) */

/* the layout the settings' places read into (their values in g_settings) */
static SnxVpadConfig g_vpad;
static int g_placed; /* g_vpad.at has its defaults: set on first use, a static initializer cannot read INFO */
static void (*g_store)(const char* key, const char* value);
static void (*g_say)(const char* line);
typedef struct {
    u64 id;
    int control;  /* -1: a finger of edit mode on no control */
    float cx, cy; /* a stick's middle while it is held; in edit mode, the finger's offset from the control's */
    int moved;
} Finger;
static Finger g_fingers[FINGERS];
static int g_nfingers;
static u32 g_held;           /* a bit per control held down */
static u32 g_dpad;           /* DPAD_ bits */
static u64 g_tapped;         /* the buttons pressed since vpad_state last read them */
static float g_axis[2][2];   /* each stick, -1 to 1 right and down */
static float g_middle[2][2]; /* each stick's middle while held, 0 to 1 across the clear part as places are */
static unsigned g_actions;
static int g_edit, g_selected = -1;
/* counts the changes to what the controls draw (vpad_draw keeps their quads while it is the same) */
static u32 g_version;
void vpad_changed(void) { ++g_version; }

/* the console's own pad (vpad_console), NULL for the Switch's, which has every control as INFO has it */
static const SnxVpadConsole* g_console;
static int g_tall;  /* its window is taller than wide, as a draw or a finger last had it */
static u32 g_moved; /* a bit per control whose place is the player's: the window's turn leaves it */
/* control c as the console has it, NULL for one it has none of; only with a console's own pad */
static const SnxVpadControl* own(int c) {
    int i;
    for (i = 0; i < g_console->count; ++i)
        if (g_console->pad[i].control == c) return &g_console->pad[i];
    return NULL;
}
static int has(int c) { return !g_console || own(c); }
int vpad_has(int control) { return control >= 0 && control < SNX_VPAD_COUNT && has(control); }
static const char* name_of(int c) { return g_console ? own(c)->name : INFO[c].name; }
static const char* label_of(int c) { return g_console ? own(c)->label : INFO[c].label; }
/* where c starts: the Switch's place, or the console's own where it has one (wide.x 0 for none); in a window taller
   than wide the console's own place for it, or the Switch's for that window */
static void default_place(int c, SnxVpadPlace* p) {
    const SnxVpadControl* o = g_console ? own(c) : NULL;
    if (g_tall) {
        p->x = o ? o->tall.x : INFO[c].tx;
        p->y = o ? o->tall.y : INFO[c].ty;
    } else {
        p->x = o && o->wide.x > 0 ? o->wide.x : INFO[c].x;
        p->y = o && o->wide.x > 0 ? o->wide.y : INFO[c].y;
    }
    p->size = DEF_SIZE;
}
/* a console's window turned: the controls the player never moved go where they start in the other */
static void turned(const int safe[4]) {
    int c, tall = safe[3] > safe[2];
    if (tall == g_tall) return;
    g_tall = tall;
    ++g_version;
    for (c = 0; c < SNX_VPAD_COUNT; ++c)
        if (!(g_moved >> c & 1)) default_place(c, &g_vpad.at[c]);
}
#define KEY_ROW(name, id) "controls_" id,
static const char* const KEYS[SNX_VPAD_COUNT] = {SNX_VPAD_IDS(KEY_ROW)};
static void placed(void) {
    if (!g_placed) {
        int c;
        g_placed = 1;
        for (c = 0; c < SNX_VPAD_COUNT; ++c) {
            default_place(c, &g_vpad.at[c]);
            if (strcmp(g_vpad_ini[FIRST_PLACE + c].key, KEYS[c])) {
                snx_error("settings.txt's list controls has %s where SNX_VPAD_IDS has %s",
                          g_vpad_ini[FIRST_PLACE + c].key, KEYS[c]);
                abort();
            }
        }
    }
}
void vpad_console(const SnxVpadConsole* console) {
    ++g_version;
    g_console = console;
    g_placed = g_tall = 0;
    g_moved = 0;
    placed();
}
const char* vpad_console_name(void) { return g_console ? g_console->name : "Switch"; }
const char* vpad_name(int control) {
    return control >= 0 && control < SNX_VPAD_COUNT && has(control) ? name_of(control) : "?";
}
SnxVpadConfig* vpad_config(void) {
    placed();
    return &g_vpad;
}
void vpad_defaults(SnxVpadConfig* c) {
    int i;
    placed();
    for (i = 0; i < SNX_VPAD_COUNT; ++i) default_place(i, &c->at[i]);
}
void vpad_reset_layout(void) {
    ++g_version;
    SnxVpadConfig d;
    int c;
    vpad_defaults(&d);
    for (c = 0; c < SNX_VPAD_COUNT; ++c) {
        if (!has(c)) continue;
        g_vpad.at[c] = d.at[c];
        g_settings.control_shown[c] = 1;
        vpad_store_control(c);
        vpad_store_shown(c);
    }
    g_moved = 0;
}

/* the settings' table (settings.txt): its settings in the game's menu's order, then a control's place, in the order of
   SNX_VPAD_ (SNX_VPAD_IDS, which settings.txt's list controls follows). A place deleted from the file while the game
   runs stays where it is until the game starts again: it has no default the reload could put back */
typedef char vpad_ini_places[SETTINGS_CONTROLS_COUNT == SNX_VPAD_COUNT ? 1 : -1];
/* controls=2 (Auto): shown while the device has a touch screen and no gamepad (vpad_auto), atomic: the backend's thread
   writes it, the drawing reads it */
static _Atomic int g_auto_shown;
static _Atomic int g_menu_open; /* atomic: the game's menu over the picture, which hides the controls (vpad_menu) */
static int on(void) {
    int c = atomic_load_explicit((_Atomic int*)&g_settings.show_controls, memory_order_relaxed);
    if (atomic_load_explicit(&g_menu_open, memory_order_acquire)) return 0;
    return c == 1 || (c == 2 && atomic_load_explicit(&g_auto_shown, memory_order_relaxed));
}
void vpad_menu(int open) {
    ++g_version;
    atomic_store_explicit(&g_menu_open, open != 0, memory_order_release);
    if (open) vpad_release();
}
int vpad_auto_shown(int touch, int pads) { return touch && !pads; }
int vpad_auto_said(int touch, int pads, char* out, size_t n) {
    if (vpad_auto_shown(touch, pads))
        return snprintf(out, n, "controls=2 (Auto): shown, the device has a touch screen and no gamepad");
    if (!touch) return snprintf(out, n, "controls=2 (Auto): hidden, the device has no touch screen");
    return snprintf(out, n, "controls=2 (Auto): hidden, %d gamepad%s connected", pads, pads == 1 ? " is" : "s are");
}
void vpad_auto(int touch, int pads) {
    static int said = -1;
    int shown = vpad_auto_shown(touch, pads);
    char line[128];
    atomic_store_explicit(&g_auto_shown, shown, memory_order_relaxed);
    if (atomic_load_explicit((_Atomic int*)&g_settings.show_controls, memory_order_relaxed) != 2) return;
    if (!shown) vpad_release();
    if (said == shown) return; /* said once a change */
    said = shown;
    vpad_auto_said(touch, pads, line, sizeof line);
    if (g_say) g_say(line);
}
void vpad_on_set(const IniOpt* o, const char* v) {
    ++g_version;
    (void)o;
    (void)v;
    if (!on()) vpad_release();
}
void vpad_place_set(const IniOpt* o, const char* v) {
    ++g_version; /* x y size, and shown in an older version's */
    int c = (int)(o - g_vpad_ini) - FIRST_PLACE, i;
    double f[4];
    char* end;
    for (i = 0; i < 4; ++i, v = end) {
        f[i] = strtod(v, &end);
        if (end == v && i == 3) break;
        if (end == v || f[i] != f[i]) return; /* not three numbers, or one NaN: the place stays */
    }
    g_vpad.at[c].x = (float)snx_clamp_double(f[0], 0, 1);
    g_vpad.at[c].y = (float)snx_clamp_double(f[1], 0, 1);
    g_vpad.at[c].size = (int)snx_clamp_double(f[2], SNX_VPAD_SIZE_MIN, SNX_VPAD_SIZE_MAX);
    g_moved |= 1u << c;
    if (i == 4) g_settings.control_shown[c] = f[3] != 0;
}
int vpad_ini(const char* key, const char* value) {
    ++g_version;
    const IniOpt* o = ini_find(g_vpad_ini, key);
    if (!o) o = ini_find(g_vpad_shown_ini, key);
    if (!o) return 0;
    placed();
    ini_read(o, value);
    return 1;
}
void vpad_set_store(void (*store)(const char* key, const char* value)) { g_store = store; }
void vpad_set_say(void (*say)(const char* line)) { g_say = say; }
static void (*g_touch)(void);
void vpad_set_touch(void (*changed)(void)) { g_touch = changed; }
void vpad_touch_set(const IniOpt* o, const char* v) {
    (void)o;
    (void)v;
    if (g_touch) g_touch();
}
void vpad_store(const int* setting) {
    ++g_version;
    const IniOpt* o;
    for (o = g_vpad_ini; o < g_vpad_ini + FIRST_PLACE; ++o)
        if (o->var == setting) {
            vpad_store_opt(o);
            return;
        }
}
void vpad_store_opt(const IniOpt* o) {
    ++g_version;
    char v[64];
    ini_format(o, v, sizeof v);
    if (g_store) g_store(o->key, v);
}
void vpad_store_control(int control) {
    ++g_version;
    char v[64];
    const SnxVpadPlace* p;
    if (control < 0 || control >= SNX_VPAD_COUNT) return;
    p = &g_vpad.at[control];
    g_moved |= 1u << control;
    snprintf(v, sizeof v, "%.3f %.3f %d", p->x, p->y, p->size);
    if (g_store) g_store(g_vpad_ini[FIRST_PLACE + control].key, v);
}
void vpad_store_shown(int control) {
    ++g_version;
    if (control >= 0 && control < SNX_VPAD_COUNT && g_store)
        g_store(g_vpad_shown_ini[control].key, g_settings.control_shown[control] ? "1" : "0");
}

/* ---- the settings as every console but the Switch reads them (its options.c has its own) ---- */
static const IniOpt* (*g_also)(const char* key);
void vpad_settings_also(const IniOpt* (*find)(const char* key)) { g_also = find; }
static const IniOpt* setting(const char* key) {
    const IniOpt* o = ini_find(g_vpad_ini, key);
    if (!o) o = ini_find(g_vpad_shown_ini, key);
    if (!o) o = ini_find(g_backend_ini, key);
    return o || !g_also ? o : g_also(key);
}
/* one of them read, or said to do nothing where the console has none of what it sets */
static void setting_read(const IniOpt* o, const char* value) {
    char line[192];
    int c, n = 0;
    line[0] = 0;
    if (o >= g_vpad_ini + FIRST_PLACE && o < g_vpad_ini + FIRST_PLACE + SNX_VPAD_COUNT)
        c = (int)(o - g_vpad_ini) - FIRST_PLACE;
    else if (o >= g_vpad_shown_ini && o < g_vpad_shown_ini + SNX_VPAD_COUNT)
        c = (int)(o - g_vpad_shown_ini);
    else
        c = -1;
    if (c >= 0 && !has(c))
        n = snprintf(line, sizeof line, "settings: %s=%s does nothing: the %s has no such control", o->key, value,
                     g_console->name);
    else if (o->var == &g_settings.touch_screen && !g_console->touch)
        n = snprintf(line, sizeof line,
                     "settings: %s=%s does nothing: the %s has no touch screen, and it keeps 0 (the fingers press the "
                     "on-screen controls all the same)",
                     o->key, value, g_console->name);
    else if (o->var == &g_settings.relative_stick_center && !has(SNX_VPAD_STICK_L) && !has(SNX_VPAD_STICK_R))
        n = snprintf(line, sizeof line, "settings: %s=%s does nothing: the %s has no stick", o->key, value,
                     g_console->name);
    if (n > 0 && g_say)
        g_say(line);
    else if (!n)
        ini_read(o, value);
}
void vpad_settings_file(const char* path) {
    size_t size;
    char *text = (char*)snx_read_file(path, &size), *key, *value, why[256];
    SnxIni ini;
    if (!text) {
        if (snx_path_exists(path))
            snx_error("%s cannot be read (%s): the on-screen controls' settings it holds are not in effect", path,
                      snx_os_error(why, sizeof why));
        return;
    }
    snx_ini_start(&ini, text);
    while (snx_ini_next(&ini, &key, &value))
        if (setting(key)) setting_read(setting(key), value);
    free(text);
}
/* the launcher's word that the command line's settings are its picks for every game: 0 or 1, -1 for another argument
   and -2 for a value that is neither */
static int launcher_picks(const char* arg) {
    size_t n = sizeof "--" SNX_LAUNCHER_PICKS "=" - 1;
    if (strncmp(arg, "--" SNX_LAUNCHER_PICKS "=", n)) return -1;
    return !strcmp(arg + n, "0") ? 0 : !strcmp(arg + n, "1") ? 1 : -2;
}
int vpad_arg(const char* arg, char* error, size_t n) {
    char key[64], why[192];
    const char* value;
    const IniOpt* o;
    int picks = launcher_picks(arg);
    if (picks == -2) {
        snprintf(error, n, "%s is not 0 or 1", arg);
        return -1;
    }
    if (picks >= 0) return 1;
    value = settings_arg(arg, key, sizeof key);
    o = value ? setting(key) : NULL;
    if (!o) return 0;
    if (ini_check(o, value, why, sizeof why)) {
        snprintf(error, n, "%s", why);
        return -1;
    }
    return 1;
}
static void settings_args(int argc, char** argv) {
    char key[64];
    int i;
    for (i = 1; i < argc; ++i) {
        const char* value = settings_arg(argv[i], key, sizeof key);
        if (value && setting(key)) setting_read(setting(key), value);
    }
}
void vpad_settings(const char* folder, const char* name, int argc, char** argv) {
    char *ini = snx_format("%s/%s.ini", folder, name), *settings = snx_game_ini(folder, name);
    char* picks = snx_game_picks(settings);
    int i, launcher = 0;
    for (i = 1; i < argc; ++i)
        if (launcher_picks(argv[i]) >= 0) launcher = launcher_picks(argv[i]);
    if (!g_console->touch) g_settings.touch_screen = 0;
    vpad_settings_file(ini);
    if (launcher) settings_args(argc, argv);
    vpad_settings_file(picks);
    if (!launcher) settings_args(argc, argv);
    free(picks);
    free(settings);
    free(ini);
}

/* a control's rectangle (x, y, w, h) on a clear part of safe: its look's shape, its longer side size */
float vpad_side(const int safe[4]) { return (float)(safe[2] < safe[3] ? safe[2] : safe[3]); }
void vpad_box(int c, const int safe[4], float r[4]) {
    float shorter = vpad_side(safe), aspect = INFO[c].aspect;
    float scale = shorter * (float)(g_settings.controls_scale + 50) / 100 * (float)g_vpad.at[c].size / 100;
    float side = INFO[c].size * scale;
    r[2] = aspect >= 1 ? side : side * aspect;
    r[3] = aspect >= 1 ? side / aspect : side;
    r[0] = (float)safe[0] + g_vpad.at[c].x * (float)safe[2] - r[2] / 2;
    r[1] = (float)safe[1] + g_vpad.at[c].y * (float)safe[3] - r[3] / 2;
    if (!g_console) return;
    r[0] = snx_clamp_float(r[0], (float)safe[0], (float)safe[0] + (float)safe[2] - r[2]);
    r[1] = snx_clamp_float(r[1], (float)safe[1], (float)safe[1] + (float)safe[3] - r[3]);
}
static int inside(const float r[4], float x, float y) {
    return x >= r[0] && y >= r[1] && x < r[0] + r[2] && y < r[1] + r[3];
}
static int hit(float x, float y, const int safe[4]) {
    int c;
    float r[4];
    for (c = SNX_VPAD_COUNT - 1; c >= 0; --c) {
        vpad_box(c, safe, r);
        if (has(c) && g_settings.control_shown[c] && inside(r, x, y)) return c;
    }
    return -1;
}
static Finger* finger(u64 id) {
    int i;
    for (i = 0; i < g_nfingers; ++i)
        if (g_fingers[i].id == id) return &g_fingers[i];
    return NULL;
}
static int stick_of(int c) { return c == SNX_VPAD_STICK_R; }
static void buzz(void) {
    if (g_settings.haptics) g_actions |= SNX_VPAD_DO_BUZZ;
}
static void dpad_at(int c, float x, float y, const int safe[4]) {
    float r[4], ax, ay;
    u32 was = g_dpad;
    vpad_box(c, safe, r);
    ax = (x - r[0] - r[2] / 2) / (r[2] / 2);
    ay = (y - r[1] - r[3] / 2) / (r[3] / 2);
    g_dpad = (ax < -DEADZONE ? DPAD_LEFT : 0) | (ax > DEADZONE ? DPAD_RIGHT : 0) | (ay < -DEADZONE ? DPAD_UP : 0) |
             (ay > DEADZONE ? DPAD_DOWN : 0);
    if (g_dpad != was) buzz();
}
static void stick_at(int c, float x, float y, const int safe[4]) {
    float r[4], ax, ay, len;
    int s = stick_of(c);
    vpad_box(c, safe, r);
    ax = (x - (float)safe[0] - g_middle[s][0] * (float)safe[2]) / (r[2] / 2);
    ay = (y - (float)safe[1] - g_middle[s][1] * (float)safe[3]) / (r[3] / 2);
    len = sqrtf(ax * ax + ay * ay);
    if (len > 1) {
        ax /= len;
        ay /= len;
    }
    g_axis[s][0] = ax;
    g_axis[s][1] = ay;
}

/* edit mode's bar on top: less, the selected control's size, more, done; in cells of 8x16 at a whole scale. Its ground
   keeps a fortieth of the window's shorter side from every control shown, as the option buttons keep from the rest:
   one in its way (the Switch's minus and plus in a wide window) takes it under that control */
enum { BAR_LESS, BAR_SIZE, BAR_MORE, BAR_DONE, BAR_COUNT };
enum { BAR_COLS_SIZE = 18 }; /* the middle part's cells: the longest name, a space and "200%" fit */
static const int BAR_COLS[BAR_COUNT] = {5, BAR_COLS_SIZE, 5, 8};
static int g_scale = 1; /* the menu's letters' scale as vpad_draw last had it: the bar's, drawn and pressed */
/* the bar's row at letters' scale s over safe: the highest whose ground keeps gap from every control there and stays
   in the picture (the top, or right under one of them), -1 for none. x: the bar's left */
static float bar_row(const int safe[4], int s, float x, int total, float gap) {
    float left = x - (float)(8 * s), right = x + (float)((total + 1) * 8 * s); /* the ground's, as vpad_draw fills it */
    float top = (float)(safe[1] + 8 * s), best = -1, b[4];
    for (int k = -1; k < SNX_VPAD_COUNT; ++k) {
        float at = top;
        if (k >= 0) {
            if (!has(k) || !g_settings.control_shown[k]) continue;
            vpad_box(k, safe, b);
            at = ceilf(b[1] + b[3] + gap) + (float)(4 * s);
        }
        if (at < top || (best >= 0 && at >= best) || at + (float)(20 * s) > (float)(safe[1] + safe[3])) continue;
        int clear = 1;
        for (int c = 0; c < SNX_VPAD_COUNT && clear; ++c) {
            if (!has(c) || !g_settings.control_shown[c]) continue;
            vpad_box(c, safe, b);
            clear = !(b[0] - gap < right && b[0] + b[2] + gap > left && b[1] - gap < at + (float)(20 * s) &&
                      b[1] + b[3] + gap > at - (float)(4 * s));
        }
        if (clear) best = at;
    }
    return best;
}
/* the bar's parts over safe into r, in the menu's letters (g_scale) or a step smaller while no row keeps it clear of the
   controls, down to 1, the top at the menu's where none does: its letters' scale */
static int bar(const int safe[4], float r[BAR_COUNT][4]) {
    int i, s, total = 0;
    float gap = 0.025f * vpad_side(safe), x = 0, y = -1;
    for (i = 0; i < BAR_COUNT; ++i) total += BAR_COLS[i] + (i ? 2 : 0);
    for (s = g_scale; s >= 1 && y < 0; --s) {
        x = (float)safe[0] + ((float)safe[2] - (float)(total * 8 * s)) / 2;
        y = bar_row(safe, s, x, total, gap);
    }
    if (y < 0) { /* none: the menu's letters at the top */
        s = g_scale;
        x = (float)safe[0] + ((float)safe[2] - (float)(total * 8 * s)) / 2;
        y = (float)(safe[1] + 8 * s);
    } else
        ++s;
    for (i = 0; i < BAR_COUNT; ++i) {
        r[i][0] = x;
        r[i][1] = y;
        r[i][2] = (float)(BAR_COLS[i] * 8 * s);
        r[i][3] = (float)(16 * s);
        x += (float)((BAR_COLS[i] + 2) * 8 * s);
    }
    return s;
}
int vpad_edit_bar(const int safe[4], float box[4]) {
    float b[BAR_COUNT][4];
    int s = bar(safe, b);
    box[0] = b[0][0] - (float)(8 * s), box[1] = b[0][1] - (float)(4 * s);
    box[2] = b[BAR_DONE][0] + b[BAR_DONE][2] - b[0][0] + (float)(16 * s), box[3] = (float)(24 * s);
    return g_edit;
}
static float grid(const int safe[4]) { return (float)(safe[2] < safe[3] ? safe[2] : safe[3]) / 32; }
static void resize(int step) {
    if (g_selected < 0) return;
    g_vpad.at[g_selected].size = snx_clamp_int(g_vpad.at[g_selected].size + step, SNX_VPAD_SIZE_MIN, SNX_VPAD_SIZE_MAX);
    vpad_store_control(g_selected);
}
static int edit_down(Finger* f, float x, float y, const int safe[4]) {
    float b[BAR_COUNT][4], r[4];
    bar(safe, b);
    f->control = -1;
    if (inside(b[BAR_DONE], x, y)) {
        vpad_edit(0);
        return 1;
    }
    if (inside(b[BAR_LESS], x, y) || inside(b[BAR_MORE], x, y)) {
        resize(inside(b[BAR_LESS], x, y) ? -10 : 10);
        return 1;
    }
    f->control = hit(x, y, safe);
    g_selected = f->control;
    if (f->control >= 0) {
        vpad_box(f->control, safe, r);
        f->cx = r[0] + r[2] / 2 - x;
        f->cy = r[1] + r[3] / 2 - y;
    }
    return 1;
}
static void edit_move(Finger* f, float x, float y, const int safe[4]) {
    float cx = x + f->cx, cy = y + f->cy, g = grid(safe);
    if (f->control < 0 || safe[2] <= 0 || safe[3] <= 0) return;
    if (g_settings.snap_to_grid && g > 0) {
        cx = (float)safe[0] + floorf((cx - (float)safe[0]) / g + 0.5f) * g;
        cy = (float)safe[1] + floorf((cy - (float)safe[1]) / g + 0.5f) * g;
    }
    g_vpad.at[f->control].x = snx_clamp_float((cx - (float)safe[0]) / (float)safe[2], 0, 1);
    g_vpad.at[f->control].y = snx_clamp_float((cy - (float)safe[1]) / (float)safe[3], 0, 1);
    f->moved = 1;
}

int vpad_down(u64 id, float x, float y, const int safe[4]) {
    ++g_version;
    Finger* f;
    int c;
    placed();
    turned(safe);
    if ((!on() && !g_edit) || finger(id) || g_nfingers == FINGERS) return 0;
    c = g_edit ? -1 : hit(x, y, safe);
    if (!g_edit && c < 0) return 0;
    f = &g_fingers[g_nfingers++];
    memset(f, 0, sizeof *f);
    f->id = id;
    f->control = c;
    if (g_edit) return edit_down(f, x, y, safe);
    if (INFO[c].kind == KIND_DPAD) {
        dpad_at(c, x, y, safe);
        if (!g_dpad) buzz(); /* its middle: held, no direction yet */
    } else if (INFO[c].kind == KIND_STICK) {
        int s = stick_of(c);
        float r[4], mx = g_vpad.at[c].x, my = g_vpad.at[c].y;
        vpad_box(c, safe, r);
        if (g_console) { /* where it is drawn, whole in the window */
            mx = (r[0] + r[2] / 2 - (float)safe[0]) / (float)safe[2];
            my = (r[1] + r[3] / 2 - (float)safe[1]) / (float)safe[3];
        }
        g_middle[s][0] = g_settings.relative_stick_center ? (x - (float)safe[0]) / (float)safe[2] : mx;
        g_middle[s][1] = g_settings.relative_stick_center ? (y - (float)safe[1]) / (float)safe[3] : my;
        stick_at(c, x, y, safe);
        buzz();
    } else {
        if (c == SNX_VPAD_HOME) g_actions |= SNX_VPAD_DO_MENU;
        if (c == SNX_VPAD_CAPTURE) g_actions |= SNX_VPAD_DO_SHOT;
        buzz();
    }
    g_held |= 1u << c;
    /* a tap shorter than the game's read still reaches it once, as a quick press of a pad's button does */
    g_tapped |= INFO[c].kind == KIND_DPAD ? (u64)g_dpad : INFO[c].bit >= 0 ? 1ull << INFO[c].bit : 0;
    return 1;
}
void vpad_move(u64 id, float x, float y, const int safe[4]) {
    ++g_version;
    Finger* f = finger(id);
    if (!f) return;
    if (g_edit)
        edit_move(f, x, y, safe);
    else if (INFO[f->control].kind == KIND_DPAD && g_settings.d_pad_slide) {
        dpad_at(f->control, x, y, safe);
        g_tapped |= g_dpad;
    } else if (INFO[f->control].kind == KIND_STICK)
        stick_at(f->control, x, y, safe);
}
static void lift(Finger* f) {
    int c = f->control, moved = f->moved, i;
    *f = g_fingers[--g_nfingers];
    if (g_edit) { /* a control dragged in edit mode keeps its place however the drag ends: lifted, Done, the menu */
        if (c >= 0 && moved) vpad_store_control(c);
        return;
    }
    if (c < 0) return;
    for (i = 0; i < g_nfingers; ++i)
        if (g_fingers[i].control == c) return; /* another finger still holds it */
    g_held &= ~(1u << c);
    if (INFO[c].kind == KIND_DPAD) g_dpad = 0;
    if (INFO[c].kind == KIND_STICK) g_axis[stick_of(c)][0] = g_axis[stick_of(c)][1] = 0;
    buzz();
}
int vpad_up(u64 id) {
    ++g_version;
    Finger* f = finger(id);
    if (!f) return 0;
    lift(f);
    return 1;
}
void vpad_release(void) {
    ++g_version;
    while (g_nfingers) lift(&g_fingers[0]);
    g_held = g_dpad = 0;
    g_tapped = 0;
    memset(g_axis, 0, sizeof g_axis);
}
void vpad_state(u64* buttons, i32 sticks[4]) {
    int c, s;
    *buttons = g_dpad | g_tapped;
    g_tapped = 0;
    for (c = 0; c < SNX_VPAD_COUNT; ++c)
        if ((g_held >> c & 1) && INFO[c].bit >= 0) *buttons |= 1ull << INFO[c].bit;
    for (s = 0; s < 2; ++s) {
        sticks[s * 2] = (i32)(g_axis[s][0] * 32767);
        sticks[s * 2 + 1] = (i32)(-g_axis[s][1] * 32767);
    }
}
unsigned vpad_actions(void) {
    unsigned a = g_actions;
    g_actions = 0;
    return a;
}
void vpad_edit(int on) {
    ++g_version;
    vpad_release();
    g_edit = on;
    g_selected = -1;
}
int vpad_editing(void) { return g_edit; }
int vpad_shown(void) { return on() || g_edit; }

/* The built-in look: each control a shape (ui.h's ui_shape), white, its edge a 32nd of its shorter side (a pixel at
   least) and its label on it; pressed, the shape filled with its label cut out */
static int edge_of(const float r[4]) {
    int e = (int)((r[2] < r[3] ? r[2] : r[3]) / 32);
    return e < 1 ? 1 : e;
}
static void button_draw(SnxUiList* l, int c, const float r[4], int held) {
    ui_shape(l, r[0], r[1], r[2], r[3], INFO[c].shape, label_of(c), held ? UI_SHAPE_FILLED : 0, edge_of(r), UI_WHITE);
}
/* the D-pad on r: its cross's edge, and in each arm its arrow; the arms pressed (dpad, DPAD_ bits) filled, their arrows
   cut out. The arms are a third of the shorter side wide, as the cross's (ui.frag) */
static void dpad_draw(SnxUiList* l, const float r[4], u32 dpad) {
    static const u32 dir[4] = {DPAD_UP, DPAD_RIGHT, DPAD_DOWN, DPAD_LEFT};
    /* the font's triangles: up, right, down, left */
    static const char* const arrow[4] = {"\x1e", "\x10", "\x1f", "\x11"};
    float a = (r[2] < r[3] ? r[2] : r[3]) / 3, cx = r[0] + r[2] / 2, cy = r[1] + r[3] / 2;
    float across = (r[2] - a) / 2, down = (r[3] - a) / 2;
    const float arm[4][4] = {{cx - a / 2, r[1], a, down},
                             {cx + a / 2, cy - a / 2, across, a},
                             {cx - a / 2, cy + a / 2, a, down},
                             {r[0], cy - a / 2, across, a}};
    int t, e = edge_of(r);
    ui_shape(l, r[0], r[1], r[2], r[3], UI_SHAPE_CROSS, NULL, 0, e, UI_WHITE);
    for (t = 0; t < 4; ++t)
        ui_shape(l, arm[t][0], arm[t][1], arm[t][2], arm[t][3], UI_SHAPE_BOX, arrow[t],
                 dpad & dir[t] ? UI_SHAPE_FILLED : UI_SHAPE_BARE, e, UI_WHITE);
}
/* a stick's ring on r, its knob ax, ay (-1 to 1 right and down) from the middle, filled when pressed */
static void knob_draw(SnxUiList* l, const float r[4], float ax, float ay, int pressed) {
    float kw = r[2] / STICK_KNOB, kh = r[3] / STICK_KNOB;
    float mx = r[0] + r[2] / 2 + ax * r[2] / 2, my = r[1] + r[3] / 2 + ay * r[3] / 2;
    int e = edge_of(r);
    ui_shape(l, r[0], r[1], r[2], r[3], UI_SHAPE_CIRCLE, NULL, 0, e, UI_WHITE);
    ui_shape(l, mx - kw / 2, my - kh / 2, kw, kh, UI_SHAPE_CIRCLE, NULL, pressed ? UI_SHAPE_FILLED : 0, e, UI_WHITE);
}
float vpad_aspect(int control) { return INFO[control].aspect; }
void vpad_control_draw(SnxUiList* l, int control, const float r[4], u64 buttons, const float axis[2]) {
    if (INFO[control].kind == KIND_DPAD) /* the D-pad shows its directions; it has no bit of its own */
        dpad_draw(l, r, (u32)buttons & (DPAD_LEFT | DPAD_UP | DPAD_RIGHT | DPAD_DOWN));
    else if (INFO[control].kind == KIND_STICK)
        knob_draw(l, r, axis[0], axis[1],
                  (int)(buttons >> INFO[stick_of(control) ? SNX_VPAD_R3 : SNX_VPAD_L3].bit & 1));
    else
        button_draw(l, control, r, (int)(buttons >> INFO[control].bit & 1));
}
static void stick_draw(SnxUiList* l, int c, float r[4], const int safe[4]) {
    int s = stick_of(c), held = g_held >> c & 1;
    if (held && !g_edit) { /* the ring where the finger landed, the knob where it is */
        r[0] = (float)safe[0] + g_middle[s][0] * (float)safe[2] - r[2] / 2;
        r[1] = (float)safe[1] + g_middle[s][1] * (float)safe[3] - r[3] / 2;
        knob_draw(l, r, g_axis[s][0], g_axis[s][1], 1);
    } else
        knob_draw(l, r, 0, 0, 0);
}
static void outline(SnxUiList* l, const float r[4], float t, u32 rgba) {
    ui_fill(l, r[0] - t, r[1] - t, r[2] + 2 * t, t, rgba);
    ui_fill(l, r[0] - t, r[1] + r[3], r[2] + 2 * t, t, rgba);
    ui_fill(l, r[0] - t, r[1], t, r[3], rgba);
    ui_fill(l, r[0] + r[2], r[1], t, r[3], rgba);
}
static void edit_draw(SnxUiList* l, const int safe[4]) {
    float b[BAR_COUNT][4], r[4], g = grid(safe), v;
    int s = g_scale, first, n = 0, k;
    char said[BAR_COLS_SIZE + 24], label[BAR_COLS_SIZE + 1]; /* said: a name cut to the field, a space, a size, % */
    const char* name = g_selected >= 0 ? name_of(g_selected) : "Tap a control";
    if (g_settings.snap_to_grid && g > 0) {
        for (v = (float)safe[0]; v <= (float)(safe[0] + safe[2]); v += g)
            ui_fill(l, v, (float)safe[1], 1, (float)safe[3], UI_WHITE);
        for (v = (float)safe[1]; v <= (float)(safe[1] + safe[3]); v += g)
            ui_fill(l, (float)safe[0], v, (float)safe[2], 1, UI_WHITE);
    }
    if (g_selected >= 0) {
        vpad_box(g_selected, safe, r);
        outline(l, r, (float)(2 * s), UI_PICKED);
    }
    s = bar(safe, b); /* the bar's letters from here */
    vpad_edit_bar(safe, r);
    ui_fill(l, r[0], r[1], r[2], r[3], UI_INK);
    ui_text(l, b[BAR_LESS][0], b[BAR_LESS][1], s, "[ - ]", UI_PAGE);
    /* the picked control's name and size, or what to do: a field of cells (ui_cells) over the bar's middle part, which
       takes the same quads at the same places whatever it says, so that a size stepped or another control picked
       changes its letters and moves nothing after it. In its middle by whole cells, written without printf */
    while (name[n] && n < BAR_COLS_SIZE) said[n] = name[n], ++n;
    if (g_selected >= 0) {
        said[n++] = ' ';
        n = (int)(ui_put_u(said + n, (u64)g_vpad.at[g_selected].size) - said);
        said[n++] = '%';
    }
    if (n > BAR_COLS_SIZE) n = BAR_COLS_SIZE;
    k = (BAR_COLS_SIZE - n) / 2;
    memset(label, ' ', (size_t)k);
    memcpy(label + k, said, (size_t)n);
    label[k + n] = 0;
    first = ui_cells(l, b[BAR_SIZE][0], b[BAR_SIZE][1], s, BAR_COLS_SIZE, UI_PAGE);
    if (first >= 0) ui_cells_set(&l->q[first], BAR_COLS_SIZE, label);
    ui_text(l, b[BAR_MORE][0], b[BAR_MORE][1], s, "[ + ]", UI_PAGE);
    ui_text(l, b[BAR_DONE][0], b[BAR_DONE][1], s, "[ Done ]", UI_PAGE);
}
/* The controls' quads, kept from frame to frame: made again only when what they show changes (counted by each call that
   changes it, g_version) or the picture's safe area does, and appended to the renderer's list where they are still from
   the frame before (ui_append_kept) */
#define KEPT_QUADS 2048 /* edit mode's grid of lines and its bar, and the controls' shapes */
static struct {
    int laid, safe[4];
    u32 drawn; /* g_version they show */
    SnxUiQuad q[KEPT_QUADS];
    int n;
    u32 version;
} g_drawn;
static void controls_draw(SnxUiList* l, const int safe[4]);
int vpad_draw(SnxUiList* l, const int safe[4], int scale) {
    int how = VPAD_DRAW_KEPT;
    placed();
    turned(safe);
    if (scale < 1) scale = 1;
    if (scale != g_scale) { /* the bar's letters another size: laid out again */
        g_scale = scale;
        ++g_version;
    }
    if (!on() && !g_edit) return VPAD_DRAW_HIDDEN;
    if (!g_drawn.laid || g_drawn.drawn != g_version || memcmp(g_drawn.safe, safe, sizeof g_drawn.safe)) {
        SnxUiList kept = {g_drawn.q, 0, KEPT_QUADS};
        controls_draw(&kept, safe);
        g_drawn.n = kept.n;
        memcpy(g_drawn.safe, safe, sizeof g_drawn.safe);
        g_drawn.drawn = g_version;
        g_drawn.laid = 1;
        ++g_drawn.version;
        how = VPAD_DRAW_LAID;
    }
    ui_append_kept(l, &(SnxUiList){g_drawn.q, g_drawn.n, KEPT_QUADS}, g_drawn.version, &g_drawn, NULL);
    return how;
}
static void controls_draw(SnxUiList* l, const int safe[4]) {
    int c;
    float r[4];
    if (g_edit) edit_draw(l, safe);
    for (c = 0; c < SNX_VPAD_COUNT; ++c) {
        if (!has(c) || !g_settings.control_shown[c]) continue;
        vpad_box(c, safe, r);
        if (INFO[c].kind == KIND_DPAD)
            dpad_draw(l, r, g_dpad);
        else if (INFO[c].kind == KIND_STICK)
            stick_draw(l, c, r, safe);
        else
            button_draw(l, c, r, (int)(g_held >> c & 1));
    }
}
