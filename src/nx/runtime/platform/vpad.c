/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/platform/vpad.h"
#include "nx/runtime/platform/controls_atlas.h"
#include "nx/runtime/platform/controls_skins.h"
#include "nx/runtime/platform/log.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { KIND_BUTTON, KIND_DPAD, KIND_STICK };
typedef struct {
    const char* name;
    int sprite, kind;
    int bit;    /* its HidNpadButton bit, -1 for none */
    float size; /* its longer side, of the clear part's shorter one, as rEden sizes it */
    float x, y; /* where it starts: the console's controllers, the left one's on the left */
} Info;
/* each control: its SNX_VPAD_ name, its .ini key, then its Info. INFO is made from this list, its order is checked
   against vpad.h's enum, and the keys of settings.txt's list controls against its keys on first use */
#define VPAD_CONTROLS(ROW)                                                                                             \
    ROW(L, "l", "L", SNX_SPRITE_L, KIND_BUTTON, 6, 0.26f, 0.09f, 0.21f)                                                \
    ROW(R, "r", "R", SNX_SPRITE_R, KIND_BUTTON, 7, 0.26f, 0.91f, 0.21f)                                                \
    ROW(ZL, "zl", "ZL", SNX_SPRITE_ZL, KIND_BUTTON, 8, 0.26f, 0.09f, 0.08f)                                            \
    ROW(ZR, "zr", "ZR", SNX_SPRITE_ZR, KIND_BUTTON, 9, 0.26f, 0.91f, 0.08f)                                            \
    ROW(MINUS, "minus", "-", SNX_SPRITE_MINUS, KIND_BUTTON, 11, 0.07f, 0.25f, 0.07f)                                   \
    ROW(PLUS, "plus", "+", SNX_SPRITE_PLUS, KIND_BUTTON, 10, 0.07f, 0.75f, 0.07f)                                      \
    ROW(CAPTURE, "capture", "Capture", SNX_SPRITE_CAPTURE, KIND_BUTTON, -1, 0.07f, 0.36f, 0.90f)                       \
    ROW(HOME, "home", "Home", SNX_SPRITE_HOME, KIND_BUTTON, -1, 0.07f, 0.64f, 0.90f)                                   \
    ROW(L3, "l3", "L3", SNX_SPRITE_L3, KIND_BUTTON, 4, 0.155f, 0.25f, 0.36f)                                           \
    ROW(R3, "r3", "R3", SNX_SPRITE_R3, KIND_BUTTON, 5, 0.155f, 0.64f, 0.62f)                                           \
    ROW(DPAD, "dpad", "D-pad", SNX_SPRITE_DPAD, KIND_DPAD, -1, 0.25f, 0.23f, 0.78f)                                    \
    ROW(STICK_L, "stick_l", "Left stick", SNX_SPRITE_STICK_RANGE, KIND_STICK, -1, 0.30f, 0.12f, 0.47f)                 \
    ROW(STICK_R, "stick_r", "Right stick", SNX_SPRITE_STICK_RANGE, KIND_STICK, -1, 0.30f, 0.77f, 0.78f)                \
    ROW(A, "a", "A", SNX_SPRITE_A, KIND_BUTTON, 0, 0.11f, 0.93f, 0.47f)                                                \
    ROW(B, "b", "B", SNX_SPRITE_B, KIND_BUTTON, 1, 0.11f, 0.87f, 0.60f)                                                \
    ROW(X, "x", "X", SNX_SPRITE_X, KIND_BUTTON, 2, 0.11f, 0.87f, 0.34f)                                                \
    ROW(Y, "y", "Y", SNX_SPRITE_Y, KIND_BUTTON, 3, 0.11f, 0.81f, 0.47f)
#define INFO_ROW(id, key, name, sprite, kind, bit, size, x, y) {name, sprite, kind, bit, size, x, y},
static const Info INFO[SNX_VPAD_COUNT] = {VPAD_CONTROLS(INFO_ROW)};
#define ORDER_INDEX(id, key, name, sprite, kind, bit, size, x, y) ORDER_##id,
enum { VPAD_CONTROLS(ORDER_INDEX) ORDER_COUNT };
#define ORDER_CHECK(id, key, name, sprite, kind, bit, size, x, y)                                                      \
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
#define FIRST_PLACE 8   /* g_vpad_ini's first place row (settings.txt) */

/* the layout the settings' places read into (their values in g_settings) */
static SnxVpadConfig g_vpad;
static int g_placed; /* g_vpad.at has its defaults: set on first use, a static initializer cannot read INFO */
static void (*g_store)(const char* key, const char* value);
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
static SnxSkinControl g_skin[SNX_VPAD_COUNT]; /* a skin's controls, pieces 0 for the built-in look */

static void default_place(int c, SnxVpadPlace* p) {
    const SnxSkinControl* k = vpad_skin(c);
    p->x = k ? k->x : INFO[c].x;
    p->y = k ? k->y : INFO[c].y;
    p->size = DEF_SIZE;
}
#define KEY_ROW(id, key, name, sprite, kind, bit, size, x, y) "controls_" key,
static const char* const KEYS[SNX_VPAD_COUNT] = {VPAD_CONTROLS(KEY_ROW)};
static void placed(void) {
    if (!g_placed) {
        int c;
        g_placed = 1;
        for (c = 0; c < SNX_VPAD_COUNT; ++c) {
            default_place(c, &g_vpad.at[c]);
            if (strcmp(g_vpad_ini[FIRST_PLACE + c].key, KEYS[c]))
                fatal("settings.txt's list controls has %s where VPAD_CONTROLS has %s", g_vpad_ini[FIRST_PLACE + c].key,
                      KEYS[c]);
        }
    }
}
static int g_skin_changed; /* controls_skin read: the main thread applies it */
void vpad_skin_choose(const IniOpt* o, const char* v) {
    (void)o;
    (void)v;
    __atomic_store_n(&g_skin_changed, 1, __ATOMIC_RELEASE);
}
int vpad_skin_changed(void) { return __atomic_exchange_n(&g_skin_changed, 0, __ATOMIC_ACQ_REL); }
int vpad_skin_shipped(int i, char* out, size_t n) {
    size_t k;
    int seen = 0;
    for (k = 0; k < snx_controls_skins_count; ++k) /* the table is by skin, a skin's files together */
        if ((!k || strcmp(snx_controls_skins[k].skin, snx_controls_skins[k - 1].skin)) && seen++ == i) {
            snprintf(out, n, "%s", snx_controls_skins[k].skin);
            return 1;
        }
    return 0;
}
const SnxSkinControl* vpad_skin(int control) {
    return control >= 0 && control < SNX_VPAD_COUNT && g_skin[control].pieces ? &g_skin[control] : NULL;
}
void vpad_skin_set(const SnxSkinControl at[SNX_VPAD_COUNT]) {
    int c;
    vpad_release(); /* a finger on a control whose box moves */
    if (at)
        memcpy(g_skin, at, sizeof g_skin);
    else
        memset(g_skin, 0, sizeof g_skin);
    if (!g_placed) return; /* placed() puts the defaults in on first use */
    for (c = 0; c < SNX_VPAD_COUNT; ++c)
        if (!g_settings.control_place[c][0]) default_place(c, &g_vpad.at[c]);
}
const char* vpad_name(int control) { return control >= 0 && control < SNX_VPAD_COUNT ? INFO[control].name : "?"; }
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
    SnxVpadConfig d;
    int c;
    vpad_defaults(&d);
    for (c = 0; c < SNX_VPAD_COUNT; ++c) {
        g_vpad.at[c] = d.at[c];
        g_settings.control_shown[c] = 1;
        vpad_store_control(c);
        vpad_store_shown(c);
    }
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
/* the settings' table (settings.txt): its settings in the game's menu's order, then a control's place, in the order of
   SNX_VPAD_ (VPAD_CONTROLS, which settings.txt's list controls follows). A place deleted from the file while the game
   runs stays where it is until the game starts again: it has no default the reload could put back */
typedef char vpad_ini_places[SETTINGS_CONTROLS_COUNT == SNX_VPAD_COUNT ? 1 : -1];
void vpad_on_set(const IniOpt* o, const char* v) {
    (void)o;
    (void)v;
    if (!g_settings.show_controls) vpad_release();
}
void vpad_place_set(const IniOpt* o, const char* v) { /* x y size, and shown in an older version's */
    int c = (int)(o - g_vpad_ini) - FIRST_PLACE, i;
    double f[4];
    char* end;
    for (i = 0; i < 4; ++i, v = end) {
        f[i] = strtod(v, &end);
        if (end == v && i == 3) break;
        if (end == v || f[i] != f[i]) return; /* not three numbers, or one NaN: the place stays */
    }
    g_vpad.at[c].x = (float)clampd(f[0], 0, 1);
    g_vpad.at[c].y = (float)clampd(f[1], 0, 1);
    g_vpad.at[c].size = (int)clampd(f[2], SNX_VPAD_SIZE_MIN, SNX_VPAD_SIZE_MAX);
    if (i == 4) g_settings.control_shown[c] = f[3] != 0;
}
int vpad_ini(const char* key, const char* value) {
    const IniOpt* o = ini_find(g_vpad_ini, key);
    if (!o) o = ini_find(g_vpad_shown_ini, key);
    if (!o) return 0;
    placed();
    ini_read(o, value);
    return 1;
}
void vpad_set_store(void (*store)(const char* key, const char* value)) { g_store = store; }
void vpad_store(const int* setting) {
    const IniOpt* o;
    for (o = g_vpad_ini; o < g_vpad_ini + FIRST_PLACE; ++o)
        if (o->var == setting) {
            vpad_store_opt(o);
            return;
        }
}
void vpad_store_opt(const IniOpt* o) {
    char v[64];
    ini_format(o, v, sizeof v);
    if (g_store) g_store(o->key, v);
}
void vpad_store_control(int control) {
    char v[64];
    const SnxVpadPlace* p;
    if (control < 0 || control >= SNX_VPAD_COUNT) return;
    p = &g_vpad.at[control];
    snprintf(v, sizeof v, "%.3f %.3f %d", p->x, p->y, p->size);
    if (g_store) g_store(g_vpad_ini[FIRST_PLACE + control].key, v);
}
void vpad_store_shown(int control) {
    if (control >= 0 && control < SNX_VPAD_COUNT && g_store)
        g_store(g_vpad_shown_ini[control].key, g_settings.control_shown[control] ? "1" : "0");
}

/* a control's rectangle (x, y, w, h) on a clear part of safe: its sprite's shape, or its skin's box */
static void box(int c, const int safe[4], float r[4]) {
    const unsigned short* s = snx_controls_sprites[INFO[c].sprite];
    const SnxSkinControl* k = vpad_skin(c);
    float shorter = (float)(safe[2] < safe[3] ? safe[2] : safe[3]), big = (float)(s[2] > s[3] ? s[2] : s[3]);
    float scale = shorter * (float)(g_settings.controls_scale + 50) / 100 * (float)g_vpad.at[c].size / 100;
    float side = INFO[c].size * scale;
    r[2] = k ? k->w * scale : side * (float)s[2] / big;
    r[3] = k ? k->h * scale : side * (float)s[3] / big;
    r[0] = (float)safe[0] + g_vpad.at[c].x * (float)safe[2] - r[2] / 2;
    r[1] = (float)safe[1] + g_vpad.at[c].y * (float)safe[3] - r[3] / 2;
}
static int inside(const float r[4], float x, float y) {
    return x >= r[0] && y >= r[1] && x < r[0] + r[2] && y < r[1] + r[3];
}
/* x, y on control c's hit area: its box, or its skin's (an ellipse or a rectangle, reaching past the box) */
static int over(int c, const float r[4], float x, float y) {
    const SnxSkinControl* k = vpad_skin(c);
    float dx, dy, rx, ry;
    if (!k) return inside(r, x, y);
    dx = x - r[0] - r[2] / 2;
    dy = y - r[1] - r[3] / 2;
    rx = r[2] / 2 * k->reach[dx < 0 ? 0 : 1];
    ry = r[3] / 2 * k->reach[dy < 0 ? 2 : 3];
    if (rx <= 0 || ry <= 0) return 0;
    dx /= rx;
    dy /= ry;
    return k->round ? dx * dx + dy * dy <= 1 : fabsf(dx) <= 1 && fabsf(dy) <= 1;
}
static int hit(float x, float y, const int safe[4]) {
    int c;
    float r[4];
    for (c = SNX_VPAD_COUNT - 1; c >= 0; --c) {
        box(c, safe, r);
        if (g_settings.control_shown[c] && over(c, r, x, y)) return c;
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
    box(c, safe, r);
    ax = (x - r[0] - r[2] / 2) / (r[2] / 2);
    ay = (y - r[1] - r[3] / 2) / (r[3] / 2);
    g_dpad = (ax < -DEADZONE ? DPAD_LEFT : 0) | (ax > DEADZONE ? DPAD_RIGHT : 0) | (ay < -DEADZONE ? DPAD_UP : 0) |
             (ay > DEADZONE ? DPAD_DOWN : 0);
    if (g_dpad != was) buzz();
}
static void stick_at(int c, float x, float y, const int safe[4]) {
    const SnxSkinControl* k = vpad_skin(c);
    float r[4], ax, ay, len, reach = k ? k->saturate : 1; /* all the way at this share of half the ring */
    int s = stick_of(c);
    box(c, safe, r);
    ax = (x - (float)safe[0] - g_middle[s][0] * (float)safe[2]) / (r[2] / 2 * reach);
    ay = (y - (float)safe[1] - g_middle[s][1] * (float)safe[3]) / (r[3] / 2 * reach);
    len = sqrtf(ax * ax + ay * ay);
    if (len > 1) {
        ax /= len;
        ay /= len;
    }
    g_axis[s][0] = ax;
    g_axis[s][1] = ay;
}

/* edit mode's bar on top: less, the selected control's size, more, done; in cells of 8x16 at a whole scale */
enum { BAR_LESS, BAR_SIZE, BAR_MORE, BAR_DONE, BAR_COUNT };
static const int BAR_COLS[BAR_COUNT] = {5, 18, 5, 8};
static int bar_scale(const int safe[4]) {
    int s = (safe[2] < safe[3] ? safe[2] : safe[3]) / 360;
    return s < 1 ? 1 : s;
}
static void bar(const int safe[4], float r[BAR_COUNT][4]) {
    int i, s = bar_scale(safe), total = 0;
    float x;
    for (i = 0; i < BAR_COUNT; ++i) total += BAR_COLS[i] + (i ? 2 : 0);
    x = (float)safe[0] + ((float)safe[2] - (float)(total * 8 * s)) / 2;
    for (i = 0; i < BAR_COUNT; ++i) {
        r[i][0] = x;
        r[i][1] = (float)(safe[1] + 8 * s);
        r[i][2] = (float)(BAR_COLS[i] * 8 * s);
        r[i][3] = (float)(16 * s);
        x += (float)((BAR_COLS[i] + 2) * 8 * s);
    }
}
static float grid(const int safe[4]) { return (float)(safe[2] < safe[3] ? safe[2] : safe[3]) / 32; }
static void resize(int step) {
    if (g_selected < 0) return;
    g_vpad.at[g_selected].size = clampi(g_vpad.at[g_selected].size + step, SNX_VPAD_SIZE_MIN, SNX_VPAD_SIZE_MAX);
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
        box(f->control, safe, r);
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
    g_vpad.at[f->control].x = clampf((cx - (float)safe[0]) / (float)safe[2], 0, 1);
    g_vpad.at[f->control].y = clampf((cy - (float)safe[1]) / (float)safe[3], 0, 1);
    f->moved = 1;
}

int vpad_down(u64 id, float x, float y, const int safe[4]) {
    Finger* f;
    int c;
    placed();
    if ((!g_settings.show_controls && !g_edit) || finger(id) || g_nfingers == FINGERS) return 0;
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
        g_middle[s][0] = g_settings.relative_stick_center ? (x - (float)safe[0]) / (float)safe[2] : g_vpad.at[c].x;
        g_middle[s][1] = g_settings.relative_stick_center ? (y - (float)safe[1]) / (float)safe[3] : g_vpad.at[c].y;
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
    Finger* f = finger(id);
    if (!f) return 0;
    lift(f);
    return 1;
}
void vpad_release(void) {
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
    vpad_release();
    g_edit = on;
    g_selected = -1;
}
int vpad_editing(void) { return g_edit; }
int vpad_shown(void) { return g_settings.show_controls || g_edit; }

/* the D-pad, and pressed (dpad, DPAD_ bits) its pressed look over it: one direction is the up sprite turned, two the
   up-left one. The pressed drawables are a dark cross, which alone sank into a dark game, its middle first */
static void dpad_draw(SnxUiList* l, const float r[4], u32 alpha, u32 dpad) {
    static const u32 one[4] = {DPAD_UP, DPAD_RIGHT, DPAD_DOWN, DPAD_LEFT};
    static const u32 two[4] = {DPAD_UP | DPAD_LEFT, DPAD_UP | DPAD_RIGHT, DPAD_DOWN | DPAD_RIGHT,
                               DPAD_DOWN | DPAD_LEFT};
    int t;
    ui_sprite(l, SNX_SPRITE_DPAD, r[0], r[1], r[2], r[3], alpha, 0);
    for (t = 0; t < 4; ++t) {
        if (dpad == one[t]) {
            ui_sprite(l, SNX_SPRITE_DPAD_UP, r[0], r[1], r[2], r[3], alpha, t);
            return;
        }
        if (dpad == two[t]) {
            ui_sprite(l, SNX_SPRITE_DPAD_UP_LEFT, r[0], r[1], r[2], r[3], alpha, t);
            return;
        }
    }
}
/* a stick's ring on r, its knob ax, ay (-1 to 1 right and down) from the middle, the pressed one when pressed */
static void knob_draw(SnxUiList* l, const float r[4], float ax, float ay, int pressed, u32 alpha) {
    float kw = r[2] / STICK_KNOB, kh = r[3] / STICK_KNOB;
    float mx = r[0] + r[2] / 2 + ax * r[2] / 2, my = r[1] + r[3] / 2 + ay * r[3] / 2;
    ui_sprite(l, SNX_SPRITE_STICK_RANGE, r[0], r[1], r[2], r[3], alpha, 0);
    ui_sprite(l, pressed ? SNX_SPRITE_STICK_PRESSED : SNX_SPRITE_STICK, mx - kw / 2, my - kh / 2, kw, kh, alpha, 0);
}
/* a skin's control: each piece on the box, its pressed look where lit has its bits (SNX_SKIN_LIT_HELD for held, the
   D-pad's directions), the knob moved by ax, ay (-1 to 1 right and down) */
static void skin_draw(SnxUiList* l, const SnxSkinControl* k, const float r[4], u32 lit, float ax, float ay, u32 alpha) {
    int i;
    for (i = 0; i < k->pieces; ++i) {
        const SnxSkinPiece* p = &k->piece[i];
        int on = (p->lit & lit) != 0, one = p->pu == p->u && p->pv == p->v;
        float x = r[0] + p->x * r[2], y = r[1] + p->y * r[3], a = (float)alpha;
        if (i == k->knob) {
            x += ax * r[2] / 2 * k->saturate;
            y += ay * r[3] / 2 * k->saturate;
        }
        if (on && one) a = a * k->alpha_mod > 255 ? 255 : a * k->alpha_mod; /* RetroArch's pressed look */
        ui_texels(l, x, y, p->w * r[2], p->h * r[3], on ? p->pu : p->u, on ? p->pv : p->v, p->uw, p->vh,
                  0x00FFFFFFu | (u32)a << 24);
    }
}
/* a built-in control's alpha at percent (0 to 100) of controls_alpha: SNX_VPAD_BUILTIN_CURVE's, 153 at 50 */
static u32 builtin_alpha(int percent) { return (u32)(pow(percent / 100.0, SNX_VPAD_BUILTIN_CURVE) * 255 + 0.5); }
/* alpha a skin's control's, builtin the built-in look's */
static void stick_draw(SnxUiList* l, int c, float r[4], const int safe[4], u32 alpha, u32 builtin) {
    const SnxSkinControl* k = vpad_skin(c);
    int s = stick_of(c), held = g_held >> c & 1;
    if (held && !g_edit) { /* the ring where the finger landed, the knob where it is */
        r[0] = (float)safe[0] + g_middle[s][0] * (float)safe[2] - r[2] / 2;
        r[1] = (float)safe[1] + g_middle[s][1] * (float)safe[3] - r[3] / 2;
        if (k)
            skin_draw(l, k, r, SNX_SKIN_LIT_HELD, g_axis[s][0], g_axis[s][1], alpha);
        else
            knob_draw(l, r, g_axis[s][0], g_axis[s][1], 1, builtin);
    } else if (k)
        skin_draw(l, k, r, 0, 0, 0, alpha);
    else
        knob_draw(l, r, 0, 0, 0, builtin);
}
static void outline(SnxUiList* l, const float r[4], float t, u32 rgba) {
    ui_fill(l, r[0] - t, r[1] - t, r[2] + 2 * t, t, rgba);
    ui_fill(l, r[0] - t, r[1] + r[3], r[2] + 2 * t, t, rgba);
    ui_fill(l, r[0] - t, r[1], t, r[3], rgba);
    ui_fill(l, r[0] + r[2], r[1], t, r[3], rgba);
}
static void edit_draw(SnxUiList* l, const int safe[4]) {
    const u32 ink = 0xF03A4145u, page = 0xFFBED6DAu, line = 0x40FFFFFFu;
    float b[BAR_COUNT][4], r[4], g = grid(safe), v;
    int s = bar_scale(safe);
    char label[32];
    if (g_settings.snap_to_grid && g > 0) {
        for (v = (float)safe[0]; v <= (float)(safe[0] + safe[2]); v += g)
            ui_fill(l, v, (float)safe[1], 1, (float)safe[3], line);
        for (v = (float)safe[1]; v <= (float)(safe[1] + safe[3]); v += g)
            ui_fill(l, (float)safe[0], v, (float)safe[2], 1, line);
    }
    if (g_selected >= 0) {
        box(g_selected, safe, r);
        outline(l, r, (float)(2 * s), 0xFF00C8FFu);
    }
    bar(safe, b);
    ui_fill(l, b[0][0] - (float)(8 * s), b[0][1] - (float)(4 * s),
            b[BAR_DONE][0] + b[BAR_DONE][2] - b[0][0] + (float)(16 * s), (float)(24 * s), ink);
    ui_text(l, b[BAR_LESS][0], b[BAR_LESS][1], s, "[ - ]", page);
    if (g_selected >= 0)
        snprintf(label, sizeof label, "%s %d%%", INFO[g_selected].name, g_vpad.at[g_selected].size);
    else
        snprintf(label, sizeof label, "Tap a control");
    ui_text(l, b[BAR_SIZE][0] + (b[BAR_SIZE][2] - (float)(strlen(label) * 8 * (size_t)s)) / 2, b[BAR_SIZE][1], s, label,
            page);
    ui_text(l, b[BAR_MORE][0], b[BAR_MORE][1], s, "[ + ]", page);
    ui_text(l, b[BAR_DONE][0], b[BAR_DONE][1], s, "[ Done ]", page);
}
void vpad_draw(SnxUiList* l, const int safe[4]) {
    int c;
    float r[4];
    u32 alpha, builtin;
    int percent;
    placed();
    if (!g_settings.show_controls && !g_edit) return;
    /* in edit mode even a clear pad shows where its controls are */
    percent = clampi(g_edit && g_settings.controls_alpha < 50 ? 50 : g_settings.controls_alpha, 0, 100);
    alpha = (u32)percent * 255 / 100;
    builtin = builtin_alpha(percent);
    if (g_edit) edit_draw(l, safe);
    for (c = 0; c < SNX_VPAD_COUNT; ++c) {
        const SnxSkinControl* k = vpad_skin(c);
        u32 held = g_held >> c & 1 ? SNX_SKIN_LIT_HELD : 0;
        if (!g_settings.control_shown[c]) continue;
        box(c, safe, r);
        if (k && INFO[c].kind != KIND_STICK)
            skin_draw(l, k, r, INFO[c].kind == KIND_DPAD ? g_dpad | held : held, 0, 0, alpha);
        else if (INFO[c].kind == KIND_DPAD)
            dpad_draw(l, r, builtin, g_dpad);
        else if (INFO[c].kind == KIND_STICK)
            stick_draw(l, c, r, safe, alpha, builtin);
        else
            ui_sprite(l, INFO[c].sprite + (int)(g_held >> c & 1), r[0], r[1], r[2], r[3], builtin, 0);
    }
}

/* the showcase: player 1's controller as the game gets it, in a corner */
static u64 g_show_buttons;
static float g_show_axis[2][2]; /* each stick, -1 to 1 right and down */
static int g_corner = 3;        /* showcase_corner's, an index of CORNERS */
static const char* const CORNERS[4] = {"tl", "tr", "bl", "br"};
/* each control's middle across and down the showcase's box (5:3) and its longer side, of the box's height; none for
   Capture and Home, which never reach the game, nor for L3 and R3, which press their stick's knob */
static const float SHOWCASE[SNX_VPAD_COUNT][3] = {
    [SNX_VPAD_ZL] = {0.13f, 0.11f, 0.22f},      [SNX_VPAD_ZR] = {0.87f, 0.11f, 0.22f},
    [SNX_VPAD_L] = {0.13f, 0.30f, 0.22f},       [SNX_VPAD_R] = {0.87f, 0.30f, 0.22f},
    [SNX_VPAD_MINUS] = {0.40f, 0.30f, 0.10f},   [SNX_VPAD_PLUS] = {0.60f, 0.30f, 0.10f},
    [SNX_VPAD_STICK_L] = {0.22f, 0.60f, 0.34f}, [SNX_VPAD_DPAD] = {0.40f, 0.84f, 0.28f},
    [SNX_VPAD_STICK_R] = {0.62f, 0.84f, 0.30f}, [SNX_VPAD_X] = {0.80f, 0.44f, 0.13f},
    [SNX_VPAD_Y] = {0.72f, 0.58f, 0.13f},       [SNX_VPAD_A] = {0.88f, 0.58f, 0.13f},
    [SNX_VPAD_B] = {0.80f, 0.72f, 0.13f}};
#define SHOWCASE_BOX 0.30f /* the box's height, of safe's shorter side; its gap to the corner a fifteenth of that */

int vpad_corner(const char* word) {
    int i;
    for (i = 0; i < 4; ++i)
        if (!strcmp(word, CORNERS[i])) return i;
    return -1;
}
const char* vpad_corner_word(int corner) { return CORNERS[corner & 3]; }
void vpad_showcase_corner_set(const IniOpt* o, const char* v) {
    int i = vpad_corner(v);
    if (i >= 0) {
        __atomic_store_n(&g_corner, i, __ATOMIC_RELAXED); /* the renderer's thread reads it */
        return;
    }
    i = __atomic_load_n(&g_corner, __ATOMIC_RELAXED);
    LOG("settings: showcase_corner=%s is not tl, tr, bl or br: it keeps %s\n", v, CORNERS[i]);
    snprintf((char*)o->var, o->size, "%s", CORNERS[i]);
}
void vpad_showcase_input(u64 buttons, const i32 sticks[4]) {
    int s;
    g_show_buttons = buttons;
    for (s = 0; s < 2; ++s) {
        float ax = (float)sticks[s * 2] / 32767, ay = -(float)sticks[s * 2 + 1] / 32767, len = sqrtf(ax * ax + ay * ay);
        if (len > 1) { /* a square of keys reaches the corners: the knob stays in its ring */
            ax /= len;
            ay /= len;
        }
        g_show_axis[s][0] = ax;
        g_show_axis[s][1] = ay;
    }
}
int vpad_showcase_fps(u32 presents, u64 now_ns) {
    static u64 since_ns;
    static u32 since;
    static int fps = -1;
    if (!since_ns || now_ns - since_ns >= 500000000ull) {
        if (since_ns)
            fps = (int)(((u64)(presents - since) * 1000000000ull + (now_ns - since_ns) / 2) / (now_ns - since_ns));
        since_ns = now_ns;
        since = presents;
    }
    return fps;
}
int vpad_showcase_box(const int safe[4], float box[4]) {
    float shorter = (float)(safe[2] < safe[3] ? safe[2] : safe[3]), h = SHOWCASE_BOX * shorter, w = h * 5 / 3;
    int corner = __atomic_load_n(&g_corner, __ATOMIC_RELAXED);
    if (!__atomic_load_n(&g_settings.showcase, __ATOMIC_RELAXED)) return -1;
    box[0] = corner & 1 ? (float)(safe[0] + safe[2]) - h / 15 - w : (float)safe[0] + h / 15;
    box[1] = corner & 2 ? (float)(safe[1] + safe[3]) - h / 15 - h : (float)safe[1] + h / 15;
    box[2] = w;
    box[3] = h;
    return corner;
}
void vpad_showcase_draw(SnxUiList* l, const int safe[4], int fps) {
    const u32 alpha = 138; /* 230 on rEden's drawables, 0.6 opaque at most, which the built-in art makes opaque */
    float box[4], x0, y0, w, h, r[4];
    int c, scale;
    char text[24];
    if (vpad_showcase_box(safe, box) < 0) return;
    x0 = box[0];
    y0 = box[1];
    w = box[2];
    h = box[3];
    ui_fill(l, x0, y0, w, h, 0x80000000u);
    for (c = 0; c < SNX_VPAD_COUNT; ++c) {
        const unsigned short* sp = snx_controls_sprites[INFO[c].sprite];
        float big = (float)(sp[2] > sp[3] ? sp[2] : sp[3]), side = SHOWCASE[c][2] * h;
        if (side <= 0) continue;
        r[2] = side * (float)sp[2] / big;
        r[3] = side * (float)sp[3] / big;
        r[0] = x0 + SHOWCASE[c][0] * w - r[2] / 2;
        r[1] = y0 + SHOWCASE[c][1] * h - r[3] / 2;
        if (INFO[c].kind == KIND_DPAD)
            dpad_draw(l, r, alpha, (u32)g_show_buttons & (DPAD_LEFT | DPAD_UP | DPAD_RIGHT | DPAD_DOWN));
        else if (INFO[c].kind == KIND_STICK) {
            int s = stick_of(c);
            knob_draw(l, r, g_show_axis[s][0], g_show_axis[s][1],
                      (int)(g_show_buttons >> INFO[s ? SNX_VPAD_R3 : SNX_VPAD_L3].bit & 1), alpha);
        } else
            ui_sprite(l, INFO[c].sprite + (int)(g_show_buttons >> INFO[c].bit & 1), r[0], r[1], r[2], r[3], alpha, 0);
    }
    if (fps < 0) return;
    scale = (int)(h * 0.11f / 16 + 0.5f);
    if (scale < 1) scale = 1;
    snprintf(text, sizeof text, "%d FPS", fps);
    ui_text(l, x0 + (w - (float)(strlen(text) * 8 * (size_t)scale)) / 2, y0 + h / 25, scale, text, 0xFFFFFFFFu);
}
