/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/platform/overlay.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/fatal_screen.h"
#include "nx/runtime/platform/gamepad.h"
#include "nx/runtime/platform/prepare_screen.h"
#include "nx/runtime/platform/vpad.h"
#include "gui_theme_colors.h"
#include "gui/icons.h"
#include "common/utilities/pad_screen.h"
#include "common/utilities/users.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FIRST = 3, VISIBLE = OVERLAY_ROWS - FIRST - 1 }; /* the items' first row, and how many show at once */
enum { PAGE_MAIN, PAGE_CONTROLS, PAGE_BUTTONS, PAGE_PADS, PAGE_MAP, PAGE_STATES, PAGE_SLOT, PAGE_ASK };
enum { M_CONTROLS, M_PADS, M_STATES, M_STATS, M_EXIT, M_COUNT }; /* the first page's items */
enum { S_SAVE, S_LOAD, S_DELETE, S_BACK, S_COUNT };              /* a slot's page's, the auto slot without Save */
enum { P_PLAYER, P_SWAP, P_DEADZONE, P_RUMBLE, P_BACKGROUND, P_MAP, P_BACK, P_COUNT }; /* the gamepads' page's */
/* the controls page's items */
enum {
    C_SHOW,
    C_EDIT,
    C_SNAP,
    C_SCALE,
    C_OPACITY,
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
                                              "controls_opacity",
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
                                          "Back"};
static int shown, page, focus, top; /* top: the first item showing */
/* the save state slots listed (slot 0 the auto one), the one a slot's page is for, what its question asks (OVERLAY_LOAD
   or OVERLAY_DELETE), and whether the list is to be read again */
static OverlaySlot slots[OVERLAY_SLOTS_MAX];
static int nslots = 1, slot_at, ask, slots_stale;
static u32 slots_gen; /* counts the lists given: the picture is set again */
static char toast[OVERLAY_COLS_MAX + 1];
static u64 toast_until;
static int pad_player; /* the player the gamepads' page shows: 0 every player's, 1 to SNX_GAMEPADS */
/* the YoRHa menu's colours, from its theme.ini (gui_theme_colors.h), as R G B A bytes; the page hides the game under
   it, whose text would show through its letters */
#define SNX_OVERLAY_RGBA(rgb, a) ((u32)(a) << 24 | ((rgb) & 0xFFu) << 16 | ((rgb) & 0xFF00u) | ((rgb) >> 16 & 0xFFu))
#define INK                      SNX_OVERLAY_RGBA(SNX_GUI_THEME_YORHA_INK, 0xFFu)
#define SOFT                     SNX_OVERLAY_RGBA(SNX_GUI_THEME_YORHA_SOFT, 0xFFu)
#define LIGHT                    SNX_OVERLAY_RGBA(SNX_GUI_THEME_YORHA_PAGE, 0xFFu)
#define FAILURE                  SNX_OVERLAY_RGBA(SNX_GUI_THEME_YORHA_FAILURE, 0xFFu)

/* a slot's page's items: the auto slot has no Save */
static int slot_item(int i) { return slot_at ? i : i + 1; }
static int items(void) {
    return page == PAGE_MAIN       ? M_COUNT
           : page == PAGE_CONTROLS ? C_COUNT
           : page == PAGE_PADS     ? P_COUNT
           : page == PAGE_STATES   ? nslots + 1
           : page == PAGE_SLOT     ? S_COUNT - !slot_at
           : page == PAGE_ASK      ? 2
                                   : SNX_VPAD_COUNT + 2;
}
static void page_open(int p) {
    page = p;
    focus = top = 0;
    if (p == PAGE_STATES) slots_stale = 1;
}
static void keep_in_view(void) {
    if (focus < top) top = focus;
    if (focus >= top + VISIBLE) top = focus - VISIBLE + 1;
}
static int all_shown(void) {
    int c;
    for (c = 0; c < SNX_VPAD_COUNT; ++c)
        if (!g_settings.control_shown[c]) return 0;
    return 1;
}
static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
/* the setting an item is, NULL for an action */
static const IniOpt* setting(int i) {
    if (page == PAGE_MAIN) return i == M_STATS ? ini_find(g_display_ini, "stats") : NULL;
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
/* a number, stepped by the arrows around it, not a word */
static int arrows(const IniOpt* o) { return o && (!o->words || strchr(o->words, '#')); }
/* an item's text, and its value at the right */
static void label(int i, char* text, size_t size, char* value, size_t vsize) {
    const IniOpt* o = setting(i);
    *value = 0;
    if (o) {
        char word[OVERLAY_COLS];
        snprintf(text, size, "%s", o->name);
        ini_word(o, *(const int*)o->var, word, sizeof word);
        snprintf(value, vsize, arrows(o) ? "< %s >" : "%s", word);
        return;
    }
    if (page == PAGE_MAIN) {
        static const char* const main_items[M_COUNT] = {SNX_VPAD_PAGE, SNX_GAMEPAD_PAGE, "Save states", "Stats",
                                                        "Exit"};
        snprintf(text, size, "%s", main_items[i]);
        if (i != M_EXIT) snprintf(value, vsize, ">");
        return;
    }
    if (page == PAGE_BUTTONS) {
        snprintf(text, size, "%s", i == 0 ? "All" : "Back");
        if (i == 0) snprintf(value, vsize, "%s", all_shown() ? "Shown" : "Some hidden");
        return;
    }
    if (page == PAGE_STATES) {
        if (i == nslots) {
            snprintf(text, size, "Back");
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
        snprintf(text, size, "%s", i ? "Cancel" : ask == OVERLAY_LOAD ? "Load" : "Delete");
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
/* ---- the button map page: NieR's CONTROLS, pad_screen.c's layout over the picture ---- */
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
void overlay_press(int control) {
    if (!overlay_listening()) return;
    map_swallow = 1;
    map_did(snx_pad_screen_press(&map, control));
}
/* a round part as strips: the font has no circles */
static void disc(SnxUiList* l, float x, float y, float w, float h, u32 rgba) {
    const int n = 6;
    float r = w < h ? w / 2 : h / 2, cx = x + w / 2, cy = y + h / 2;
    int k;
    for (k = 0; k < n; ++k) {
        float y0 = cy - r + 2 * r * (float)k / n, mid = y0 + r / n - cy, half = sqrtf(r * r - mid * mid);
        ui_fill(l, cx - half, y0, 2 * half, 2 * r / n, rgba);
    }
}
static void user_draw(SnxUiList* l, float left, float right, float y, int s);
static void map_draw(SnxUiList* l, int sw, int sh) {
    const u32 box = SNX_OVERLAY_RGBA(SNX_GUI_THEME_YORHA_SOFT, 0x40u); /* a panel: the page a little darker */
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
    ui_fill(l, x, y, w, h, LIGHT); /* the whole picture */
    ui_fill(l, x, y, w, ch, INK);
    {
        char title[48];
        if (pad_player) /* the player's own */
            snprintf(title, sizeof title, "SCARLETNX  /  PLAYER %d  /  BUTTON MAP", pad_player);
        else
            snprintf(title, sizeof title, "SCARLETNX  /  BUTTON MAP");
        ui_text(l, x + cw, y, s, title, LIGHT);
        user_draw(l, x + cw * (float)(strlen(title) + 2), x + w - cw, y, s);
    }
    snx_pad_screen_lay(&map, x, y + ch, w, h - ch, cw, ch, &map_drawing);
    for (i = 0; i < map_drawing.n; ++i) {
        const SnxPadShape* p = &map_drawing.shape[i];
        u32 fill = p->style == SNX_PAD_STYLE_FOCUS ? INK : p->style == SNX_PAD_STYLE_LIT ? FAILURE : box;
        if (p->kind == SNX_PAD_SHAPE_TEXT)
            ui_text(l, p->x, p->y, s, p->text,
                    p->style == SNX_PAD_STYLE_FOCUS || p->style == SNX_PAD_STYLE_LIT ? LIGHT
                    : p->style == SNX_PAD_STYLE_SOFT                                 ? SOFT
                    : p->style == SNX_PAD_STYLE_NOTE                                 ? FAILURE
                                                                                     : INK);
        else if (p->kind == SNX_PAD_SHAPE_DISC)
            disc(l, p->x, p->y, p->w, p->h, p->style == SNX_PAD_STYLE_PLAIN ? SOFT : fill);
        else
            ui_fill(l, p->x, p->y, p->w, p->h,
                    p->kind == SNX_PAD_SHAPE_BAR && p->style == SNX_PAD_STYLE_PLAIN ? SOFT : fill);
    }
}
/* the item i done, or its value stepped (step -1 or 1; 0 for a select, which steps up) */
static int act(int i, int step) {
    const IniOpt* o = setting(i);
    if (o) {
        int* v = (int*)o->var;
        int to = ini_step(o, *v, step ? step : 1);
        if (page == PAGE_CONTROLS && i == C_TOUCH)
            backend_touch_set(to);
        else if (page == PAGE_BUTTONS) {
            *v = to;
            vpad_store_shown(i - 1);
        } else if (page == PAGE_MAIN) { /* stats: the renderer's thread reads it */
            __atomic_store_n(v, to, __ATOMIC_RELAXED);
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
        if (i == M_EXIT) return OVERLAY_EXIT;
        page_open(i == M_PADS ? PAGE_PADS : i == M_STATES ? PAGE_STATES : PAGE_CONTROLS);
        return OVERLAY_NONE;
    }
    if (step) return OVERLAY_NONE;
    if (page == PAGE_STATES && i < nslots) {
        slot_at = i;
        page = PAGE_SLOT;
        focus = top = 0;
        return OVERLAY_NONE;
    }
    if (page == PAGE_SLOT && slot_item(i) != S_BACK) {
        int what = slot_item(i);
        if (what == S_SAVE) return OVERLAY_SAVE;
        if (!slots[slot_at].when[0] || (what == S_LOAD && slots[slot_at].why[0])) return OVERLAY_NONE;
        ask = what == S_LOAD ? OVERLAY_LOAD : OVERLAY_DELETE;
        page = PAGE_ASK;
        focus = 1; /* Cancel: a stray press does nothing */
        top = 0;
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
    else if (page == PAGE_CONTROLS && i == C_RESET)
        vpad_reset_layout();
    else if (page == PAGE_PADS && i == P_MAP)
        map_open();
    else /* Back */
        overlay_back();
    return OVERLAY_NONE;
}

int overlay_shown(void) { return __atomic_load_n(&shown, __ATOMIC_ACQUIRE); }
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
    if (page == PAGE_STATES && focus > nslots) focus = nslots;
    slots_stale = 0;
    ++slots_gen;
    keep_in_view();
}
void overlay_toast(const char* text, u64 now_ns) {
    snprintf(toast, sizeof toast, "%s", text);
    toast_until = now_ns + (u64)SNX_OVERLAY_TOAST_MS * 1000000u;
}
int overlay_toasting(u64 now_ns) { return toast[0] && now_ns < toast_until; }
void overlay_toggle(void) {
    if (!shown) page_open(PAGE_MAIN);
    __atomic_store_n(&shown, !shown, __ATOMIC_RELEASE);
}
/* the panel's rows: the page's items under the head, a blank row under them */
static int rows(void) { return FIRST + (items() < VISIBLE ? items() : VISIBLE) + 1; }
static int display_percent = 100; /* set on the event thread, read by the renderer's */
void overlay_set_display_scale(float scale) {
    __atomic_store_n(&display_percent, scale > 0.0f ? (int)(scale * 100.0f) : 100, __ATOMIC_RELAXED);
}
int overlay_display_percent(void) { return __atomic_load_n(&display_percent, __ATOMIC_RELAXED); }
void overlay_place(int sw, int sh, int* x, int* y, int* scale) {
    /* the tallest page sets the scale, so the letters keep their size from page to page: half the picture, or twice
       the display's scale as the menu's UI scale Auto (src/gui) if that is more, within three quarters of it. A
       phone's short picture held its letters at 2x by the half alone */
    int w = OVERLAY_COLS * 8, h = OVERLAY_ROWS * 16;
    int s = sw / 2 / w < sh / 2 / h ? sw / 2 / w : sh / 2 / h;
    int dense = 2 * __atomic_load_n(&display_percent, __ATOMIC_RELAXED) / 100;
    if (dense > s) s = dense;
    if (s > sw * 3 / 4 / w) s = sw * 3 / 4 / w;
    if (s > sh * 3 / 4 / h) s = sh * 3 / 4 / h;
    *scale = s < 1 ? 1 : s;
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
} DialogPara;
typedef struct {
    const char* title;
    int user; /* the user playing at the head's right, by their nickname: the menu's pages */
    DialogPara para[DIALOG_PARAS];
    int paras;
    const char* label[VISIBLE];   /* the items showing, from top on */
    unsigned icon[VISIBLE];       /* beside them, gui/icons.h's; GUI_ICON_NONE for none */
    const char* value[VISIBLE];   /* at their right; NULL for none */
    int items, top, shown, focus; /* all the items, the first showing, how many show, the one in focus */
    int aside;                    /* room at the box's right for a picture (the save states' thumbnail), if it fits */
} Dialog;
typedef struct {
    SnxOverlayRect box, link, item[VISIBLE];
    SnxOverlayRect aside; /* the picture's room: empty when there was none */
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
    if (*name) ui_text(l, x + cw, y, s, name, LIGHT);
    ui_fill(l, x, y, box, edge, LIGHT);
    ui_fill(l, x, y + ch - edge, box, edge, LIGHT);
    ui_fill(l, x, y, edge, ch, LIGHT);
    ui_fill(l, x + box - edge, y, edge, ch, LIGHT);
}
/* the rows paragraphs wrap into: each thread that lays a dialog out has its own */
typedef char DialogRows[DIALOG_PARAS][DIALOG_LINES][FATAL_ROW];
/* d on a picture sw x sh, into l (NULL: only where it all goes, in out): the head, each paragraph after a blank row,
   a blank row, the items and a blank row; overlay_cols wide, in the middle */
static void dialog_draw(SnxUiList* l, int sw, int sh, const Dialog* d, DialogRows rows_of, DialogDrawn* out) {
    char line[OVERLAY_COLS_MAX + 4];
    int px, py, s, cols, n[DIALOG_PARAS], k, i, total, room, row, last, aside = 0;
    float cw, ch, x, y, w;
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
        n[k] = d->para[k].text ? fatal_wrap(d->para[k].text, cols - 2, rows_of[k], DIALOG_LINES) : 0;
        if (n[k] > DIALOG_LINES) n[k] = DIALOG_LINES;
    }
    room = sh / (int)ch;
    for (;;) {
        int pick = -1;
        total = 1 + 1 + d->shown + 1;
        for (k = 0, last = -1; k < d->paras; ++k) {
            if (!n[k]) continue;
            total += n[k] + (last >= 0 && !(d->para[k].on && last == k - 1));
            last = k;
            if (d->para[k].cut && (pick < 0 || d->para[k].cut < d->para[pick].cut)) pick = k;
        }
        if (total <= room || pick < 0) break;
        --n[pick];
    }
    x = (float)((sw - cols * 8 * s - aside) / 2);
    y = (float)((sh - total * (int)ch) / 2);
    if (aside) {
        out->aside.x = x + w + (float)(8 * s);
        out->aside.y = y;
        out->aside.w = (float)(SNX_UI_IMAGE_W / 2 * s);
        out->aside.h = (float)(SNX_UI_IMAGE_H / 2 * s);
    }
    out->box.x = x;
    out->box.y = y;
    out->box.w = w;
    out->box.h = ch * (float)total;
    out->cols = cols;
    out->scale = s;
    if (l) {
        ui_fill(l, x, y, w, ch * (float)total, LIGHT);
        ui_fill(l, x, y, w, ch, INK);
        ui_text(l, x + cw, y, s, d->title, LIGHT);
        if (d->user) user_draw(l, x + cw * (float)(strlen(d->title) + 2), x + w - cw, y, s);
    }
    row = 1;
    for (k = 0, last = -1; k < d->paras; ++k) {
        if (!n[k]) continue;
        if (last >= 0 && !(d->para[k].on && last == k - 1)) ++row;
        last = k;
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
        at->x = x;
        at->y = y + ch * (float)(total - 1 - d->shown + i);
        at->w = w;
        at->h = ch;
        if (!l) continue;
        if (on) ui_fill(l, x, at->y, w, ch, INK);
        if (d->icon[i]) { /* the marker, the icon's two cells and one, then the label */
            ui_text(l, x + cw, at->y, s, "\xFE", on ? LIGHT : INK);
            ui_icon(l, x + 3 * cw, at->y, s, d->icon[i], on ? LIGHT : INK);
            ui_text(l, x + 6 * cw, at->y, s, d->label[i], on ? LIGHT : INK);
        } else {
            snprintf(line, sizeof line, "\xFE %s", d->label[i]);
            ui_text(l, x + cw, at->y, s, line, on ? LIGHT : INK);
        }
        if (d->value[i])
            ui_text(l, x + cw * (float)(cols - 1 - (int)strlen(d->value[i])), at->y, s, d->value[i], on ? LIGHT : INK);
    }
    /* more above or under: the font's arrows at the edge */
    if (l && d->shown && d->top) ui_text(l, x + cw * (float)(cols - 1), out->item[0].y, s, "\x18", SOFT);
    if (l && d->shown && d->top + d->shown < d->items)
        ui_text(l, x + cw * (float)(cols - 1), out->item[d->shown - 1].y, s, "\x19", SOFT);
}
int overlay_select(void) { return page == PAGE_MAP ? map_key(SNX_PAD_KEY_SELECT) : act(focus, 0); }
void overlay_move(int rows) {
    int n = items();
    if (page == PAGE_MAP) {
        map_key(rows < 0 ? SNX_PAD_KEY_UP : SNX_PAD_KEY_DOWN);
        return;
    }
    focus = ((focus + rows) % n + n) % n;
    keep_in_view();
}
void overlay_scroll(int rows) {
    if (page == PAGE_MAP) {
        overlay_move(rows);
        return;
    }
    focus = clamp(focus + rows, 0, items() - 1);
    keep_in_view();
}
void overlay_side(int step) {
    if (page == PAGE_MAP)
        map_key(step < 0 ? SNX_PAD_KEY_LEFT : SNX_PAD_KEY_RIGHT);
    else if (page != PAGE_MAIN)
        act(focus, step);
}
int overlay_pad(u64 button) {
    switch (button) {
        case 1ull << 0: return overlay_select(); /* A */
        case 1ull << 1: return overlay_back();   /* B */
        case 1ull << 12:                         /* the D-pad's left and the left stick's */
        case 1ull << 16: overlay_side(-1); break;
        case 1ull << 13: /* up */
        case 1ull << 17: overlay_move(-1); break;
        case 1ull << 14: /* right */
        case 1ull << 18: overlay_side(1); break;
        case 1ull << 15: /* down */
        case 1ull << 19: overlay_move(1); break;
        default: break;
    }
    return OVERLAY_NONE;
}
int overlay_controls(u64 buttons, const i32 sticks[4], u64 ms) {
    static u64 seen, next;       /* the buttons of the last call; when a held direction moves again */
    const u64 dirs = 0xFF000ull; /* the D-pad's and the left stick's: bits 12 to 19 */
    u64 now = buttons | (sticks[0] < -16384 ? 1ull << 16 : 0) | (sticks[1] > 16384 ? 1ull << 17 : 0) |
              (sticks[0] > 16384 ? 1ull << 18 : 0) | (sticks[1] < -16384 ? 1ull << 19 : 0),
        pressed = shown ? now & ~seen : 0;
    int bit, did = OVERLAY_NONE;
    if (map_swallow) { /* the press the button map page took */
        map_swallow = 0;
        seen = now;
        return OVERLAY_NONE;
    }
    if ((pressed & dirs) || !shown) /* a direction held as the menu opens waits as a fresh press would */
        next = ms + SNX_OVERLAY_REPEAT_DELAY;
    else if (shown && (now & dirs) && ms >= next) { /* held: the lowest direction again */
        pressed |= now & dirs & (~(now & dirs) + 1);
        next = ms + SNX_OVERLAY_REPEAT_MS;
    }
    seen = now;
    for (bit = 0; bit < 20 && did == OVERLAY_NONE; ++bit)
        if (pressed >> bit & 1) did = overlay_pad(1ull << bit);
    return did;
}
int overlay_back(void) {
    int from = page;
    if (page == PAGE_MAIN) return OVERLAY_CLOSE;
    if (page == PAGE_MAP) return map_key(SNX_PAD_KEY_BACK);
    if (page == PAGE_BUTTONS) {
        page_open(PAGE_CONTROLS);
        focus = C_BUTTONS;
    } else if (page == PAGE_ASK) {
        page = PAGE_SLOT;
        focus = slot_at ? ask == OVERLAY_LOAD ? S_LOAD : S_DELETE : ask == OVERLAY_LOAD ? 0 : 1;
    } else if (page == PAGE_SLOT) {
        page = PAGE_STATES;
        focus = slot_at;
    } else {
        page_open(PAGE_MAIN);
        focus = from == PAGE_PADS ? M_PADS : from == PAGE_STATES ? M_STATES : M_CONTROLS;
    }
    keep_in_view();
    return OVERLAY_NONE;
}
static const char* pads_hint(char* out, size_t size);
/* the page showing as a dialog: its title, its hint, the items in view, their texts in text and value. pads holds the
   title and the hint made for it */
static void menu_dialog(Dialog* d, char (*text)[OVERLAY_COLS_MAX + 1], char (*value)[OVERLAY_COLS_MAX + 1], char* pads,
                        size_t size) {
    static const char* const titles[] = {
        "SCARLETNX  /  MENU",       "SCARLETNX  /  CONTROLS", "SCARLETNX  /  BUTTONS", "SCARLETNX  /  CONTROLLERS", "",
        "SCARLETNX  /  SAVE STATES"};
    static const char* const hints[] = {
        "Back or Home: back to the game.",        "Back: the menu.", "Tap one to show or hide it.", "", "",
        "The game waits while this page is open."};
    const OverlaySlot* sl = &slots[slot_at];
    char* title = pads + size / 2;
    int r, n = items();
    memset(d, 0, sizeof *d);
    d->title = page < PAGE_SLOT ? titles[page] : title;
    d->user = 1;
    d->para[0].text = page == PAGE_PADS ? pads_hint(pads, size / 2) : page < PAGE_SLOT ? hints[page] : pads;
    d->para[0].rgba = SOFT;
    d->paras = 1;
    if (page >= PAGE_SLOT) {
        char name[16];
        if (slot_at)
            snprintf(name, sizeof name, "SLOT %d", slot_at);
        else
            snprintf(name, sizeof name, "AUTO SLOT");
        if (page == PAGE_SLOT)
            snprintf(title, size / 2, "SCARLETNX  /  %s", name);
        else
            snprintf(title, size / 2, "SCARLETNX  /  %s %s?", ask == OVERLAY_LOAD ? "LOAD" : "DELETE", name);
        if (page == PAGE_ASK && ask == OVERLAY_LOAD)
            snprintf(pads, size / 2, "The game goes back to %s.%s", sl->when,
                     g_settings.save_state_backup ? " What was played since is kept in the auto slot." : "");
        else if (page == PAGE_ASK)
            snprintf(pads, size / 2, "The state saved %s is deleted.", sl->when);
        else
            snprintf(pads, size / 2, "%s", sl->when[0] ? sl->when : "Empty.");
        if (page == PAGE_SLOT && sl->why[0]) { /* why it can't load, under its date */
            d->para[1].text = sl->why;
            d->para[1].rgba = FAILURE;
            d->para[1].on = 1;
            d->paras = 2;
        }
    }
    d->aside = page >= PAGE_STATES ? 1 : 0;
    d->items = n;
    d->top = top;
    d->focus = focus;
    d->shown = n - top < VISIBLE ? n - top : VISIBLE;
    for (r = 0; r < d->shown; ++r) {
        static const unsigned main_icons[M_COUNT] = {GUI_ICON_POINTER, GUI_ICON_GAMEPAD_2, GUI_ICON_SAVE,
                                                     GUI_ICON_ACTIVITY, GUI_ICON_LOG_OUT};
        label(top + r, text[r], sizeof *text, value[r], sizeof *value);
        d->label[r] = text[r];
        d->icon[r] = page == PAGE_MAIN ? main_icons[top + r] : GUI_ICON_NONE;
        d->value[r] = value[r];
    }
}
int overlay_tap(float x, float y, int sw, int sh) {
    static DialogRows rows_of; /* the event thread's */
    char text[VISIBLE][OVERLAY_COLS_MAX + 1], value[VISIBLE][OVERLAY_COLS_MAX + 1], pads[2 * OVERLAY_COLS_MAX + 64];
    Dialog d;
    DialogDrawn at;
    int r, col;
    if (page == PAGE_MAP) return map_did(snx_pad_screen_tap(&map, snx_pad_screen_hit(&map_drawing, x, y)));
    menu_dialog(&d, text, value, pads, sizeof pads);
    dialog_draw(NULL, sw, sh, &d, rows_of, &at);
    if (!overlay_error_hit(&at.box, x, y)) return OVERLAY_CLOSE;
    for (r = 0; r < d.shown && !overlay_error_hit(&at.item[r], x, y); ++r) {}
    if (r == d.shown) return OVERLAY_NONE;
    focus = d.top + r;
    col = (int)((x - at.box.x) / (8.0f * (float)at.scale));
    /* a value's row: a tap on its last five columns (the right arrow's) steps it up, a tap anywhere left of them
       down */
    if (arrows(setting(focus))) return act(focus, col < at.cols - 5 ? -1 : 1);
    return act(focus, 0);
}
static u64 tap_id;   /* what went down off the on-screen controls, */
static int tap_down; /* whose lift taps the menu */
void overlay_pointer_down(u64 id, float x, float y, const int safe[4]) {
    /* the on-screen controls take no finger under the loading screen, where they aren't drawn (vk/ui.c) */
    if (!prepare_screen_shown() && vpad_down(id, x, y, safe)) return;
    tap_id = id;
    tap_down = 1;
}
void overlay_pointer_move(u64 id, float x, float y, const int safe[4]) { vpad_move(id, x, y, safe); }
int overlay_pointer_up(u64 id, float x, float y, int sw, int sh) {
    if (vpad_up(id) || !tap_down || id != tap_id) return OVERLAY_NONE;
    tap_down = 0;
    return overlay_tap(x, y, sw, sh);
}
void overlay_pointer_cancel(u64 id) {
    if (id == tap_id) tap_down = 0;
}
void overlay_pointers_reset(void) { tap_down = 0; }
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
/* the notification: a band of one row in the picture's top left corner */
static void toast_draw(SnxUiList* l, int sw, int sh) {
    int px, py, s;
    float cw, ch, w;
    overlay_place(sw, sh, &px, &py, &s);
    cw = 8.0f * (float)s;
    ch = 16.0f * (float)s;
    w = cw * (float)(strlen(toast) + 2);
    ui_fill(l, cw, ch, w, ch, INK);
    ui_text(l, 2 * cw, ch, s, toast, LIGHT);
}
/* the slot whose thumbnail shows beside the save states' pages: the one in focus on the list, or the page's */
static void aside_draw(SnxUiList* l, const SnxOverlayRect* r) {
    static u32 gen;
    static int at = -1;
    int slot = page == PAGE_STATES ? (focus < nslots ? focus : -1) : slot_at;
    float e = r->w / (float)(SNX_UI_IMAGE_W / 2); /* the scale: a pixel of it */
    if (slot != at || gen != slots_gen) {
        ui_image_set(slot >= 0 ? slots[slot].thumb : NULL);
        at = slot;
        gen = slots_gen;
    }
    ui_fill(l, r->x - e, r->y - e, r->w + 2 * e, r->h + 2 * e, INK);
    if (!ui_image(l, r->x, r->y, r->w, r->h)) {
        ui_fill(l, r->x, r->y, r->w, r->h, LIGHT);
        ui_text(l, r->x + (r->w - 8.0f * 8 * e) / 2, r->y + (r->h - 16 * e) / 2, (int)e,
                slot >= 0 && slots[slot].when[0] ? "No image" : "Empty", SOFT);
    }
}
void overlay_draw(SnxUiList* l, int sw, int sh, u64 now_ns) {
    static DialogRows rows_of; /* the renderer's */
    char text[VISIBLE][OVERLAY_COLS_MAX + 1], value[VISIBLE][OVERLAY_COLS_MAX + 1], pads[2 * OVERLAY_COLS_MAX + 64];
    Dialog d;
    DialogDrawn at;
    if (!shown) {
        if (overlay_toasting(now_ns)) toast_draw(l, sw, sh);
        return;
    }
    if (page == PAGE_MAP) {
        map_draw(l, sw, sh);
        return;
    }
    menu_dialog(&d, text, value, pads, sizeof pads);
    dialog_draw(l, sw, sh, &d, rows_of, &at);
    if (at.aside.w > 0) aside_draw(l, &at.aside);
    if (overlay_toasting(now_ns)) toast_draw(l, sw, sh);
}
int overlay_error_hit(const SnxOverlayRect* r, float x, float y) {
    return r && r->w > 0 && r->h > 0 && x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}
void overlay_error_draw(SnxUiList* l, int sw, int sh, SnxOverlayError* e) {
    static DialogRows rows_of; /* the thread that shows the panel's */
    static const char* const labels[SNX_OVERLAY_ERROR_ITEMS] = {"Share Log", "Copy path", "Close"};
    /* on a short picture the log's label gives up rows first, then the hint, the log and the error; never the game's
       name or what Share Log or Copy path did. The log goes right under its label */
    const DialogPara para[] = {{e->name, SOFT, 0, 0, 0}, {e->msg, FAILURE, 0, 0, 4}, {fatal_hint(e->msg), INK, 0, 0, 2},
                               {e->how, SOFT, 0, 0, 1},  {e->log, INK, 1, 1, 3},     {e->said, SOFT, 0, 0, 0}};
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
