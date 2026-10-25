/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What tools/uigen generates from tests/ui_test.txt, built and run: each scene's layout at two UI scales, the rows its
   lists show, its drawing (the calls main.c's ui_draw_row, ui_draw_tile and ui_draw_label would get, with the
   attributes the code and the hidden rows give), its hit test, the actions a hit, a hover and a hotkey call, how a list
   follows its focus, and a tile list's flow. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL_keycode.h>
#include "gui/ui.h"
#include "gui/icons.h"

/* the app the callbacks get: what they were called with */
struct App {
    int calls[8], hits[8];
    size_t rows[8];
    bool keys_missing;         /* Load Switch blinks in the failure colour */
    size_t active;             /* LoadList's row in the active colour, + 1; 0: none */
    int width;                 /* the width Games' get_row_info was given last */
    bool top_off, right_off;   /* Frame's Top and Right not shown */
    int top_width, info_width; /* the widths Frame's Top and Info were measured at last */
    int knob_least;            /* Frame's Knob at least this wide, at 1x; 0: no least */
};
enum { LOAD_SWITCH, LOAD_3DS, SETTINGS, GAMES, OK, BACK, DATA };
/* the row attributes are a bit each, none shared */
typedef char ui_attr_bits[(UI_ATTR_FAILURE | UI_ATTR_BLINK | UI_ATTR_MORE_UP | UI_ATTR_MORE_DOWN | UI_ATTR_ACTIVE |
                           UI_ATTR_LABEL | UI_ATTR_OPEN | UI_ATTR_TITLE) == 255
                              ? 1
                              : -1];

/* what was drawn, in order */
typedef struct {
    GuiRect r;
    char text[64];
    const void* image;
    unsigned icon, color, attr;
    int focused, label, tile;
} Drawn;
static struct {
    Drawn drawn[32];
    int ndrawn;
    char image;   /* where the last game's image is: what the drawing passes on */
    char tile[8]; /* Grid's row's label, one buffer as main.c's: it lasts until the next call */
} g_gui_ui;

void ui_draw_row(App* app, GuiRect r, const char* text, unsigned icon, const void* image, unsigned color, unsigned attr,
                 bool focused) {
    Drawn* d = &g_gui_ui.drawn[g_gui_ui.ndrawn++];
    (void)app;
    d->r = r;
    snprintf(d->text, sizeof d->text, "%s", text);
    d->icon = icon;
    d->image = image;
    d->color = color;
    d->attr = attr;
    d->focused = focused;
    d->label = d->tile = 0;
}
void ui_draw_tile(App* app, GuiRect r, const char* text, const void* image, unsigned color, unsigned attr,
                  bool focused) {
    ui_draw_row(app, r, text, 0, image, color, attr, focused);
    g_gui_ui.drawn[g_gui_ui.ndrawn - 1].tile = 1;
}
const char* ui_tr(const char* english) { return english; }
void ui_draw_label(App* app, GuiRect r, const char* text, unsigned color) {
    ui_draw_row(app, r, text, 0, NULL, color, 0, false);
    g_gui_ui.drawn[g_gui_ui.ndrawn - 1].label = 1;
}

static void called(App* app, int which, uint32_t hit, size_t row) {
    app->calls[which]++;
    app->hits[which] = (int)hit;
    app->rows[which] = row;
}
void ui_main_load_list_load_switch_action_cb(App* app, uint32_t hit) { called(app, LOAD_SWITCH, hit, 0); }
unsigned ui_main_load_list_attr_cb(App* app, size_t row) { return app->active == row + 1 ? UI_ATTR_ACTIVE : 0; }
unsigned ui_main_load_list_load_switch_attr_cb(App* app) {
    return app->keys_missing ? UI_ATTR_FAILURE | UI_ATTR_BLINK : 0;
}
void ui_main_load_list_load_3ds_game_action_cb(App* app, uint32_t hit) { called(app, LOAD_3DS, hit, 0); }
#ifdef SDL_PLATFORM_ANDROID
void ui_main_load_list_open_data_folder_action_cb(App* app, uint32_t hit) { called(app, DATA, hit, 0); }
#endif
void ui_main_load_list_settings_action_cb(App* app, uint32_t hit) { called(app, SETTINGS, hit, 0); }
/* four games: a title, the second in the failure colour with an icon, the third with no label and open (the focus's
   look, no cursor), the last with an image */
bool ui_main_games_get_row_info(App* app, size_t row, int width, UiRowInfo* info) {
    static const char* const names[] = {"Game 0", "Game 1", NULL, "Game 3"};
    app->width = width;
    if (row >= 4) return false;
    info->label = names[row];
    if (row == 3) info->image = &g_gui_ui.image;
    if (row == 1) info->icon = GUI_ICON_SAVE;
    info->attr = row == 0 ? UI_ATTR_TITLE : row == 1 ? UI_ATTR_FAILURE : row == 2 ? UI_ATTR_OPEN : 0;
    return true;
}
void ui_main_games_row_action_cb(App* app, size_t row, uint32_t hit) { called(app, GAMES, hit, row); }
/* Grid's thirteen rows: Head, a label, four tiles, Mid, a label, then seven tiles; T3 with an image */
bool ui_grid_tiles_get_row_info(App* app, size_t row, int width, UiRowInfo* info) {
    char* text = g_gui_ui.tile;
    app->width = width;
    if (row >= 13) return false;
    if (row == 0 || row == 5)
        snprintf(text, sizeof g_gui_ui.tile, "%s", row ? "Mid" : "Head");
    else
        snprintf(text, sizeof g_gui_ui.tile, "T%zu", row);
    info->label = text;
    if (row == 0 || row == 5) info->attr = UI_ATTR_LABEL;
    if (row == 3) info->image = &g_gui_ui.image;
    return true;
}
void ui_grid_tiles_row_action_cb(App* app, size_t row, uint32_t hit) { called(app, GAMES, hit, row); }
void ui_main_status_update_cb(App* app, char* text, size_t size) {
    (void)app;
    snprintf(text, size, "ready");
}
void ui_row_ok_action_cb(App* app, uint32_t hit) { called(app, OK, hit, 0); }
void ui_row_back_action_cb(App* app, uint32_t hit) { called(app, BACK, hit, 0); }
void ui_touch_big_a_action_cb(App* app, uint32_t hit) { called(app, OK, hit, 0); }
void ui_touch_big_b_action_cb(App* app, uint32_t hit) { called(app, OK, hit, 0); }
void ui_touch_big_c_action_cb(App* app, uint32_t hit) { called(app, OK, hit, 0); }
unsigned ui_row_back_attr_cb(App* app) {
    (void)app;
    return UI_ATTR_FAILURE;
}

/* Frame's: Top a row of 1px for each 16 of its width, Info 150px, Keys 20px times the scale, Knob's least */
bool ui_frame_top_shown_cb(App* app) { return !app->top_off; }
int ui_frame_top_measure_cb(App* app, int width, int scale) {
    (void)scale;
    app->top_width = width;
    return width / 16;
}
bool ui_frame_body_right_shown_cb(App* app) { return !app->right_off; }
int ui_frame_body_right_info_measure_cb(App* app, int width, int scale) {
    (void)scale;
    app->info_width = width;
    return 150;
}
int ui_frame_foot_keys_measure_cb(App* app, int width, int scale) {
    (void)app;
    (void)width;
    return 20 * scale;
}
int ui_frame_pair_knob_min_width_cb(App* app, int scale) { return app->knob_least * scale; }

static int same(GuiRect r, int x, int y, int w, int h) { return r.x == x && r.y == y && r.w == w && r.h == h; }
static void is(GuiRect r, int x, int y, int w, int h) {
    if (!same(r, x, y, w, h)) fprintf(stderr, "%d,%d %dx%d, not %d,%d %dx%d\n", r.x, r.y, r.w, r.h, x, y, w, h);
    assert(same(r, x, y, w, h));
}

/* padding 4px, gap 10px, a column: the label's row, two lists sharing what is left, the status a fifth of the inside */
static void test_layout(void) {
    GuiRect r[UI_MAIN_ITEMS], o[UI_ROW_ITEMS];
    ui_main_layout(gui_rect(10, 20, 400, 300), 1, r);
    is(r[UI_MAIN_PICK_ONE], 14, 24, 392, 17);
    is(r[UI_MAIN_LOAD_LIST], 14, 51, 392, 93);
    is(r[UI_MAIN_GAMES], 14, 154, 392, 93);
    is(r[UI_MAIN_STATUS], 14, 257, 392, 58);
    is(r[UI_MAIN_LOAD_LIST_LOAD_SWITCH], 0, 0, 0, 0);    /* a row: its list's */
    assert(ui_main_shown(r, 1, UI_MAIN_LOAD_LIST) == 3); /* five fit, max_rows 3 */
    assert(ui_main_shown(r, 1, UI_MAIN_GAMES) == 5);
    assert(ui_main_shown(r, 1, UI_MAIN_STATUS) == 0);
    /* at 2x the pixels double, the fractions stay: fewer rows fit */
    ui_main_layout(gui_rect(10, 20, 400, 300), 2, r);
    is(r[UI_MAIN_PICK_ONE], 18, 28, 384, 34);
    is(r[UI_MAIN_LOAD_LIST], 18, 82, 384, 66);
    assert(ui_main_shown(r, 2, UI_MAIN_LOAD_LIST) == 1);
    /* a row, centred both ways: OK 60 by 20, Back a quarter wide and the whole height */
    ui_row_layout(gui_rect(0, 0, 400, 100), 1, o);
    is(o[UI_ROW_OK], 120, 40, 60, 20);
    is(o[UI_ROW_BACK], 180, 0, 100, 100);
    /* a platform's own row metrics: a phone's rows 30px high (Android's and iOS's), the desktop's at most 2 */
    {
        GuiRect t[UI_TOUCH_ITEMS];
        ui_touch_layout(gui_rect(0, 0, 100, 100), 1, t);
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS) /* a phone's: Android's own metrics on iOS too */
        is(ui_touch_row(t, 1, UI_TOUCH_BIG, 1), 0, 32, 100, 30);
        assert(ui_touch_shown(t, 1, UI_TOUCH_BIG) == 3); /* 100 / 32: 3 */
#else
        is(ui_touch_row(t, 1, UI_TOUCH_BIG, 1), 0, 19, 100, 17);
        assert(ui_touch_shown(t, 1, UI_TOUCH_BIG) == 2); /* 5 fit, max_rows 2 */
#endif
    }
    puts("PASS the layouts at 1x and 2x, the rows that fit, a platform's own metrics");
}

static void test_draw(void) {
    GuiRect r[UI_MAIN_ITEMS];
    UiList lists[UI_MAIN_ITEMS] = {{0}};
    App app = {0};
    const Drawn* d = g_gui_ui.drawn;
    lists[UI_MAIN_LOAD_LIST].focus = 1;
    lists[UI_MAIN_GAMES].focus = 3;
    ui_main_layout(gui_rect(10, 20, 400, 300), 1, r);
    g_gui_ui.ndrawn = 0;
    ui_main_draw(&app, r, 1, lists);
    /* the label, three of LoadList's four rows (max_rows), Games's four (the fifth is past its last), the status */
    assert(g_gui_ui.ndrawn == 1 + 3 + 4 + 1);
    assert(d[0].label && !strcmp(d[0].text, "Pick one") && d[0].color == UI_COLOR_SOFT);
    is(d[0].r, 14, 24, 392, 17);
    assert(!strcmp(d[1].text, "Load Switch") && !d[1].focused && !d[1].attr); /* its attr_cb: keys in use */
    is(d[1].r, 14, 51, 200, 17);                                              /* max_row_width */
    assert(!strcmp(d[2].text, "Load 3DS game") && d[2].focused);
    is(d[2].r, 14, 70, 200, 17);
    is(ui_main_row(r, 1, UI_MAIN_LOAD_LIST, 1), 14, 70, 200, 17);       /* as drawn */
    is(ui_main_row(r, 1, UI_MAIN_GAMES, 3), 14, 154 + 3 * 19, 300, 17); /* max_row_width */
    is(ui_main_row(r, 1, UI_MAIN_STATUS, 0), 0, 0, 0, 0);               /* not a list */
#ifdef SDL_PLATFORM_ANDROID
    assert(!strcmp(d[3].text, "Open Data Folder"));
#else
    assert(!strcmp(d[3].text, "Settings") && d[3].color == UI_COLOR_FOCUS);
#endif
    assert(d[2].attr == 0 && d[3].attr == UI_ATTR_MORE_DOWN); /* the last row shown, rows below it */
    assert(!strcmp(d[4].text, "Game 0") && d[4].attr == UI_ATTR_TITLE);
    assert(!strcmp(d[5].text, "Game 1") && d[5].attr == UI_ATTR_FAILURE);
    assert(!strcmp(d[6].text, "(null)") && d[6].attr == UI_ATTR_OPEN && !d[6].focused);
    assert(!strcmp(d[7].text, "Game 3") && d[7].focused);
    assert(d[7].image == &g_gui_ui.image && !d[4].image && !d[5].image && !d[6].image); /* a row's own image */
    assert(!d[0].image && !d[3].image); /* a static list's rows have none */
    /* an icon: a static row's from ui.txt, a list without items' from its get_row_info, none for the others */
    assert(d[2].icon == GUI_ICON_BOX && !d[1].icon && !d[3].icon && !d[0].icon);
    assert(d[5].icon == GUI_ICON_SAVE && !d[4].icon && !d[6].icon && !d[7].icon);
    is(d[7].r, 14, 154 + 3 * 19, 300, 17);
    assert(app.width == 300); /* each row asked with its own width, max_row_width */
    assert(d[8].label && !strcmp(d[8].text, "ready"));
    is(d[8].r, 14, 257, 392, 58);
    /* scrolled: the rows from the first, an arrow up on it; the arrow down only while rows are left below */
    lists[UI_MAIN_LOAD_LIST].first = 1;
    lists[UI_MAIN_GAMES].first = 1;
    g_gui_ui.ndrawn = 0;
    ui_main_draw(&app, r, 1, lists);
    assert(!strcmp(d[1].text, "Load 3DS game") && d[1].focused && d[1].attr == UI_ATTR_MORE_UP);
#ifdef SDL_PLATFORM_ANDROID
    assert(!strcmp(d[3].text, "Settings") && d[3].attr == UI_ATTR_MORE_DOWN);
#else
    assert(!strcmp(d[3].text, "Decor") && d[3].attr == 0);
#endif
    assert(!strcmp(d[4].text, "Game 1") && d[4].attr == (UI_ATTR_FAILURE | UI_ATTR_MORE_UP));
    assert(!strcmp(d[6].text, "Game 3") && d[6].attr == 0);
    /* the code's attributes: Load Switch with no keys; a scene's button */
    app.keys_missing = true;
    lists[UI_MAIN_LOAD_LIST].first = 0;
    g_gui_ui.ndrawn = 0;
    ui_main_draw(&app, r, 1, lists);
    assert(!strcmp(d[1].text, "Load Switch") && d[1].attr == (UI_ATTR_FAILURE | UI_ATTR_BLINK));
    app.active = 2; /* the list's: any row's, by its index, with the row's own */
    g_gui_ui.ndrawn = 0;
    ui_main_draw(&app, r, 1, lists);
    assert(d[1].attr == (UI_ATTR_FAILURE | UI_ATTR_BLINK) && d[2].attr == UI_ATTR_ACTIVE);
    app.active = 1;
    g_gui_ui.ndrawn = 0;
    ui_main_draw(&app, r, 1, lists);
    assert(d[1].attr == (UI_ATTR_FAILURE | UI_ATTR_BLINK | UI_ATTR_ACTIVE) && d[2].attr == 0);
    {
        GuiRect o[UI_ROW_ITEMS];
        ui_row_layout(gui_rect(0, 0, 400, 100), 1, o);
        g_gui_ui.ndrawn = 0;
        ui_row_draw(&app, o, 1, NULL);
        assert(g_gui_ui.ndrawn == 2 && !strcmp(d[1].text, "Back") && d[1].attr == UI_ATTR_FAILURE && !d[0].attr);
    }
    /* at 2x Games shows one row: the arrow down, from get_row_info's next row */
    lists[UI_MAIN_GAMES].first = 0;
    ui_main_layout(gui_rect(10, 20, 400, 300), 2, r);
    assert(ui_main_shown(r, 2, UI_MAIN_GAMES) == 1);
    g_gui_ui.ndrawn = 0;
    ui_main_draw(&app, r, 2, lists);
    assert(!strcmp(d[2].text, "Game 0") && d[2].attr == (UI_ATTR_TITLE | UI_ATTR_MORE_DOWN) && app.width == 384 &&
           d[2].r.w == 384);
    lists[UI_MAIN_GAMES].first = 3; /* its last row: none below */
    g_gui_ui.ndrawn = 0;
    ui_main_draw(&app, r, 2, lists);
    assert(!strcmp(d[2].text, "Game 3") && d[2].attr == UI_ATTR_MORE_UP);
    puts("PASS the drawing: labels, rows from items and from get_row_info, focus, colours, scroll, attributes");
}

static void test_hit(void) {
    GuiRect r[UI_MAIN_ITEMS];
    UiList lists[UI_MAIN_ITEMS] = {{0}};
    App app = {0};
    int item;
    size_t row;
    ui_main_layout(gui_rect(10, 20, 400, 300), 1, r);
    /* a row of LoadList: its button, and the bits of the event */
    assert(ui_main_hit(&app, r, 1, lists, 20, 75, UI_HIT_CLICK, &item, &row) == (UI_HIT_HOVER | UI_HIT_CLICK));
    assert(item == UI_MAIN_LOAD_LIST_LOAD_3DS_GAME && row == 1);
    ui_main_act(&app, item, row, UI_HIT_CLICK);
    assert(app.calls[LOAD_3DS] == 1 && app.hits[LOAD_3DS] == UI_HIT_CLICK);
    /* past max_row_width: no row there, in a list with items or without */
    assert(!ui_main_hit(&app, r, 1, lists, 300, 60, 0, &item, &row) && item == -1);
    assert(!ui_main_hit(&app, r, 1, lists, 350, 157, 0, &item, &row) && item == -1);
    /* a row of Games, scrolled one down: its row from the list's first */
    lists[UI_MAIN_GAMES].first = 1;
    assert(ui_main_hit(&app, r, 1, lists, 20, 154 + 19 + 3, UI_HIT_RELEASE, &item, &row) ==
           (UI_HIT_HOVER | UI_HIT_RELEASE));
    assert(item == UI_MAIN_GAMES && row == 2);
    ui_main_act(&app, item, row, UI_HIT_RELEASE);
    assert(app.calls[GAMES] == 1 && app.rows[GAMES] == 2 && app.hits[GAMES] == UI_HIT_RELEASE);
    /* a row past get_row_info's last is not there; each asked with its width */
    app.width = 0;
    assert(!ui_main_hit(&app, r, 1, lists, 20, 154 + 19 * 3 + 3, 0, &item, &row));
    assert(app.width == 300);
    /* the label is hit, and acts on nothing */
    assert(ui_main_hit(&app, r, 1, lists, 20, 260, 0, &item, &row) == UI_HIT_HOVER && item == UI_MAIN_STATUS);
    ui_main_act(&app, item, row, UI_HIT_CLICK);
    assert(app.calls[LOAD_3DS] == 1 && app.calls[GAMES] == 1);
    /* a hover takes the same path: the callback gets UI_HIT_HOVER alone */
    assert(ui_main_hit(&app, r, 1, lists, 20, 75, 0, &item, &row) == UI_HIT_HOVER);
    ui_main_act(&app, item, row, UI_HIT_HOVER);
    assert(app.calls[LOAD_3DS] == 2 && app.hits[LOAD_3DS] == UI_HIT_HOVER);
    puts("PASS the hit test's items, rows and bits, and the actions a hit and a hover call");
}

static void test_follow_and_keys(void) {
    GuiRect r[UI_MAIN_ITEMS];
    UiList list = {0};
    App app = {0};
    ui_main_layout(gui_rect(10, 20, 400, 300), 1, r);
    /* LoadList keeps its focus in the middle (center, the default), as far as its rows go */
    list.first = 0;
    list.focus = 3;
    ui_main_follow(r, 1, UI_MAIN_LOAD_LIST, 4, &list);
    assert(list.first == 1);
    list.focus = 1;
    ui_main_follow(r, 1, UI_MAIN_LOAD_LIST, 4, &list);
    assert(list.first == 0);
    /* a focus the pointer put there: on a row that shows, nothing moves (the middle would be first 0); on one that
       does not, as little as it takes */
    list.first = 1;
    list.pointed = true;
    ui_main_follow(r, 1, UI_MAIN_LOAD_LIST, 4, &list);
    assert(list.first == 1);
    list.first = 0;
    list.focus = 3;
    ui_main_follow(r, 1, UI_MAIN_LOAD_LIST, 4, &list);
    assert(list.first == 1);
    list.pointed = false;
    /* Games (edge) moves as little as it takes */
    list.first = 0;
    list.focus = 7;
    ui_main_follow(r, 1, UI_MAIN_GAMES, 10, &list);
    assert(list.first == 3);
    list.focus = 2;
    ui_main_follow(r, 1, UI_MAIN_GAMES, 10, &list);
    assert(list.first == 2);
    list.focus = 4;
    ui_main_follow(r, 1, UI_MAIN_GAMES, 10, &list);
    assert(list.first == 2);
    /* the hotkeys, each scene its own */
    assert(ui_main_key(&app, SDLK_F4, UI_MOD_ALT) && app.calls[LOAD_3DS] == 1 && app.hits[LOAD_3DS] == UI_HIT_CLICK);
    assert(!ui_main_key(&app, SDLK_F4, 0) && !ui_main_key(&app, 'o', 0) && app.calls[LOAD_3DS] == 1);
    assert(ui_row_key(&app, 'o', 0) && app.calls[OK] == 1);
    /* the stub of a decorative row does nothing */
    ui_main_act(&app, UI_MAIN_LOAD_LIST_DECOR, 0, UI_HIT_CLICK);
    puts("PASS how lists follow their focus, and the hotkeys");
}

/* a frame at 1x 400 by 300: padding 8, its column 4 apart; Top 384 / 16 high, Bar a row and 4px, Foot as tall as
   Keys, Pair a row, Body the rest. Body's row 8 apart: Side 80, Left and Right sharing 288 by 2 and 3, Left held at
   its 160 and Right given the 128 left. Right's column: Info's 150 held at half of 188, About the rest. Keys a
   quarter of 384 against the right; Note and Knob halves of 376 */
static void test_frame(void) {
    App app = {0};
    GuiRect r[UI_FRAME_ITEMS];
    ui_frame_layout(&app, gui_rect(0, 0, 400, 300), 1, r);
    is(r[UI_FRAME_TOP], 8, 8, 384, 24);
    is(r[UI_FRAME_BAR], 8, 36, 384, 20);
    is(r[UI_FRAME_BODY], 8, 60, 384, 188);
    is(r[UI_FRAME_BODY_LEFT], 8, 60, 160, 188);
    is(r[UI_FRAME_BODY_RIGHT], 176, 60, 128, 188);
    is(r[UI_FRAME_BODY_RIGHT_ABOUT], 176, 60, 128, 90);
    is(r[UI_FRAME_BODY_RIGHT_INFO], 176, 154, 128, 94);
    is(r[UI_FRAME_BODY_SIDE], 312, 60, 80, 188);
    is(r[UI_FRAME_FOOT], 8, 252, 384, 20);
    is(r[UI_FRAME_FOOT_KEYS], 296, 252, 96, 20);
    is(r[UI_FRAME_PAIR_NOTE], 8, 276, 188, 16);
    is(r[UI_FRAME_PAIR_KNOB], 204, 276, 188, 16);
    assert(app.top_width == 384 && app.info_width == 128);
    /* Knob's least over its half: Note what is left; past what there is, Knob all of it and Note none, its gap kept */
    app.knob_least = 300;
    ui_frame_layout(&app, gui_rect(0, 0, 400, 300), 1, r);
    is(r[UI_FRAME_PAIR_NOTE], 8, 276, 76, 16);
    is(r[UI_FRAME_PAIR_KNOB], 92, 276, 300, 16);
    app.knob_least = 500;
    ui_frame_layout(&app, gui_rect(0, 0, 400, 300), 1, r);
    is(r[UI_FRAME_PAIR_NOTE], 8, 276, 0, 16);
    is(r[UI_FRAME_PAIR_KNOB], 16, 276, 376, 16);
    app.knob_least = 0;
    /* off: Top takes nothing, nor its gap, and Body grows by both; Right and its slots empty, Left the rest */
    app.top_off = app.right_off = true;
    ui_frame_layout(&app, gui_rect(0, 0, 400, 300), 1, r);
    assert(!r[UI_FRAME_TOP].w && !r[UI_FRAME_TOP].h && !r[UI_FRAME_BODY_RIGHT].w && !r[UI_FRAME_BODY_RIGHT_INFO].h);
    is(r[UI_FRAME_BAR], 8, 8, 384, 20);
    is(r[UI_FRAME_BODY], 8, 32, 384, 216);
    is(r[UI_FRAME_BODY_LEFT], 8, 32, 296, 216);
    is(r[UI_FRAME_BODY_SIDE], 312, 32, 80, 216);
    app.top_off = app.right_off = false;
    /* at 2x the pixels double: Left over its least now, Info under its half */
    ui_frame_layout(&app, gui_rect(0, 0, 800, 600), 2, r);
    is(r[UI_FRAME_TOP], 16, 16, 768, 48);
    is(r[UI_FRAME_BAR], 16, 72, 768, 40);
    is(r[UI_FRAME_BODY_LEFT], 16, 120, 320, 376);
    is(r[UI_FRAME_BODY_RIGHT], 352, 120, 256, 376);
    is(r[UI_FRAME_BODY_RIGHT_ABOUT], 352, 120, 256, 218);
    is(r[UI_FRAME_BODY_RIGHT_INFO], 352, 346, 256, 150);
    is(r[UI_FRAME_BODY_SIDE], 624, 120, 160, 376);
    is(r[UI_FRAME_FOOT_KEYS], 592, 504, 192, 40);
    is(r[UI_FRAME_PAIR_KNOB], 408, 552, 376, 32);
    /* under 40 columns inside, 320px at 1x: Body a column, Left two fifths of its 187 and Right the rest, Side off */
    assert(!UI_FRAME_NARROW(336, 1) && UI_FRAME_NARROW(335, 1) && !UI_FRAME_NARROW(672, 2) && UI_FRAME_NARROW(671, 2));
    ui_frame_layout(&app, gui_rect(0, 0, 300, 300), 1, r);
    is(r[UI_FRAME_TOP], 8, 8, 284, 17);
    is(r[UI_FRAME_BODY], 8, 53, 284, 195);
    is(r[UI_FRAME_BODY_LEFT], 8, 53, 284, 74);
    is(r[UI_FRAME_BODY_RIGHT], 8, 135, 284, 113);
    is(r[UI_FRAME_BODY_RIGHT_ABOUT], 8, 135, 284, 53);
    is(r[UI_FRAME_BODY_RIGHT_INFO], 8, 192, 284, 56);
    assert(!r[UI_FRAME_BODY_SIDE].w && !r[UI_FRAME_BODY_SIDE].h);
    is(r[UI_FRAME_FOOT_KEYS], 221, 252, 71, 20);
    is(r[UI_FRAME_PAIR_KNOB], 154, 276, 138, 16);
    assert(app.top_width == 284 && app.info_width == 284);
    puts("PASS a frame: lengths, shares and their bounds, measures, slots off, 2x, narrow");
}

/* Grid at 1x in 150 by 200, padding 4: three tiles a line (142px: 40 each, 4 apart), Head's line 10px high, every
   line 4px under the one before; nine rows at most */
static void test_tiles(void) {
    GuiRect r[UI_GRID_ITEMS];
    UiList lists[UI_GRID_ITEMS] = {{0}};
    App app = {0};
    const Drawn* d = g_gui_ui.drawn;
    UiFlow f;
    GuiRect at;
    int item;
    size_t row;
    ui_grid_layout(gui_rect(0, 0, 150, 200), 1, r);
    is(r[UI_GRID_TILES], 4, 4, 142, 192);
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS) /* a phone's 60px tiles: two a line */
    ui_grid_flow(r, 1, UI_GRID_TILES, &f);
    assert(f.cols == 2 && f.tile_w == 60);
#else
    /* the flow: a label's line, tiles, a new line when one is full and before a label */
    ui_grid_flow(r, 1, UI_GRID_TILES, &f);
    assert(f.cols == 3 && f.tile_w == 40 && f.tile_h == 30 && f.row_h == 10 && f.gap == 4 && f.max == 9);
    assert(ui_flow_next(&f, true, &at) && f.line == 0);
    is(at, 4, 4, 142, 10);
    assert(ui_flow_next(&f, false, &at) && f.line == 1);
    is(at, 4, 18, 40, 30);
    assert(ui_flow_next(&f, false, &at));
    is(at, 48, 18, 40, 30);
    assert(ui_flow_next(&f, false, &at));
    is(at, 92, 18, 40, 30);
    assert(ui_flow_next(&f, false, &at) && f.line == 2); /* the line full: the next */
    is(at, 4, 52, 40, 30);
    assert(ui_flow_next(&f, true, &at) && f.line == 3); /* a label after one tile: a line of its own */
    is(at, 4, 86, 142, 10);
    assert(ui_flow_next(&f, false, &at) && f.line == 4);
    is(at, 4, 100, 40, 30);
    /* past the bottom: not placed, nothing changed */
    f.area.h = 130;
    assert(ui_flow_next(&f, false, &at) && !ui_flow_next(&f, true, &at) && f.line == 4 && f.placed == 8);
    /* every row a tile: shown, row, follow */
    assert(ui_grid_shown(r, 1, UI_GRID_TILES) == 9); /* 3 by 5 fit, max_rows 9 */
    is(ui_grid_row(r, 1, UI_GRID_TILES, 4), 48, 38, 40, 30);
    lists[UI_GRID_TILES].focus = 12;
    ui_grid_follow(r, 1, UI_GRID_TILES, 13, &lists[UI_GRID_TILES]);
    assert(lists[UI_GRID_TILES].first == 6); /* the last 3 lines of 5, at a line's first */
    lists[UI_GRID_TILES].focus = 7;
    lists[UI_GRID_TILES].pointed = true;
    ui_grid_follow(r, 1, UI_GRID_TILES, 13, &lists[UI_GRID_TILES]);
    assert(lists[UI_GRID_TILES].first == 6); /* the pointer's on a line that shows: nothing moves (the middle: 3) */
    lists[UI_GRID_TILES].pointed = false;
    lists[UI_GRID_TILES].focus = 4;
    ui_grid_follow(r, 1, UI_GRID_TILES, 13, &lists[UI_GRID_TILES]);
    assert(lists[UI_GRID_TILES].first == 0);
    /* drawn as placed: the labels as rows, the others tiles, an image its row's; nine rows, the last's arrow down */
    lists[UI_GRID_TILES].focus = 2;
    g_gui_ui.ndrawn = 0;
    ui_grid_draw(&app, r, 1, lists);
    assert(g_gui_ui.ndrawn == 9 && app.width == 40);
    assert(!strcmp(d[0].text, "Head") && d[0].attr == UI_ATTR_LABEL && !d[0].tile);
    is(d[0].r, 4, 4, 142, 10);
    assert(!strcmp(d[2].text, "T2") && d[2].tile && d[2].focused && !d[2].attr);
    is(d[2].r, 48, 18, 40, 30);
    assert(!strcmp(d[3].text, "T3") && d[3].image == &g_gui_ui.image && !d[2].image);
    assert(!strcmp(d[5].text, "Mid") && !d[5].tile);
    is(d[5].r, 4, 86, 142, 10);
    assert(!strcmp(d[8].text, "T8") && d[8].attr == UI_ATTR_MORE_DOWN);
    is(d[8].r, 92, 100, 40, 30);
    /* from row 6: an arrow up on the first, none down on the last row */
    lists[UI_GRID_TILES].first = 6;
    g_gui_ui.ndrawn = 0;
    ui_grid_draw(&app, r, 1, lists);
    assert(g_gui_ui.ndrawn == 7 && !strcmp(d[0].text, "T6") && d[0].attr == UI_ATTR_MORE_UP);
    is(d[0].r, 4, 4, 40, 30);
    assert(!strcmp(d[6].text, "T12") && !d[6].attr);
    is(d[6].r, 4, 72, 40, 30);
    /* at 2x one tile a line: three rows fit, the third with the arrow down */
    lists[UI_GRID_TILES].first = 0;
    ui_grid_layout(gui_rect(0, 0, 150, 200), 2, r);
    g_gui_ui.ndrawn = 0;
    ui_grid_draw(&app, r, 2, lists);
    assert(g_gui_ui.ndrawn == 3 && d[2].attr == UI_ATTR_MORE_DOWN);
    is(d[2].r, 8, 104, 80, 60);
    /* hit where drawn: a tile, a label; nothing between tiles or past a line's last */
    ui_grid_layout(gui_rect(0, 0, 150, 200), 1, r);
    assert(ui_grid_hit(&app, r, 1, lists, 50, 20, UI_HIT_CLICK, &item, &row) == (UI_HIT_HOVER | UI_HIT_CLICK));
    assert(item == UI_GRID_TILES && row == 2);
    ui_grid_act(&app, item, row, UI_HIT_CLICK);
    assert(app.calls[GAMES] == 1 && app.rows[GAMES] == 2);
    assert(ui_grid_hit(&app, r, 1, lists, 100, 8, 0, &item, &row) == UI_HIT_HOVER && item == UI_GRID_TILES && !row);
    assert(!ui_grid_hit(&app, r, 1, lists, 45, 20, 0, &item, &row) && item == -1);
    assert(!ui_grid_hit(&app, r, 1, lists, 140, 20, 0, &item, &row) && item == -1);
    lists[UI_GRID_TILES].first = 6;
    assert(ui_grid_hit(&app, r, 1, lists, 50, 40, 0, &item, &row) == UI_HIT_HOVER && row == 10);
    /* the code's own tiles (UiList's tile_w and tile_h): placed, drawn, hit and followed at them, as many a line as
       then fit; 0 ui.txt's again */
    lists[UI_GRID_TILES].first = 0;
    lists[UI_GRID_TILES].tile_w = 20;
    lists[UI_GRID_TILES].tile_h = 10;
    ui_grid_flow(r, 1, UI_GRID_TILES, &f);
    ui_flow_sized(&f, &lists[UI_GRID_TILES], 1);
    assert(f.tile_w == 20 && f.tile_h == 10 && f.cols == (f.area.w + f.gap) / (20 + f.gap) && f.cols > 3);
    g_gui_ui.ndrawn = 0;
    ui_grid_draw(&app, r, 1, lists);
    assert(d[1].tile && !strcmp(d[1].text, "T1"));
    is(d[1].r, 4, 18, 20, 10);
    is(d[2].r, 28, 18, 20, 10);
    assert(ui_grid_hit(&app, r, 1, lists, 30, 20, 0, &item, &row) == UI_HIT_HOVER && row == 2);
    lists[UI_GRID_TILES].focus = 12;
    ui_grid_follow(r, 1, UI_GRID_TILES, 13, &lists[UI_GRID_TILES]);
    assert(lists[UI_GRID_TILES].first % (size_t)f.cols == 0 && lists[UI_GRID_TILES].first + (size_t)f.cols > 12);
    lists[UI_GRID_TILES].tile_w = lists[UI_GRID_TILES].tile_h = 0;
#endif
    puts("PASS a tile list: its flow, tiles and labels drawn and hit where placed, shown, row and follow");
}

int main(void) {
    test_layout();
    test_tiles();
    test_frame();
    test_draw();
    test_hit();
    test_follow_and_keys();
    return 0;
}
