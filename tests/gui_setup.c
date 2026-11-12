/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The launcher's setup (gui/main.c's setup_*) on the real renderer and its events, in a home of its own: the pages a
   first start goes through and what each writes (a portable copy, the language, the games and dumps folders, the keys,
   the first user), what it refuses, the user's page with users.ini there, the fixed uid kept for its saves, a notice
   alone after an update, --debug, the question's place as options-dialog says, and Skip */
#include <SDL3/SDL.h>
/* the keyboards main.c asks for: SDL keeps no count, and one asked for and dropped in a frame stays up on Android */
static int keyboards;
static bool keyboard_asked(SDL_Window* window) {
    ++keyboards;
    return SDL_StartTextInput(window);
}
#define SDL_StartTextInput keyboard_asked
#define SNX_GUI_NO_MAIN
#include "gui/main.c"
#define SNX_SCREEN_TEST "gui_setup.c"
#include "gui_screen.h"
#include <assert.h>

#define FIRST "0123456789abcdeffedcba9876543210"

static char *root, *bin, *home;

static char* join(const char* a, const char* b) {
    char* p = snx_path_join(a, b);
    assert(p);
    return p;
}
static void put(const char* path, const char* text) {
    char* dir = snx_dirname(path);
    assert(dir && !snx_mkdirs(dir) && !snx_write_file(path, text, strlen(text)));
    free(dir);
}
static char* text_of(const char* folder, const char* name) {
    char* path = join(folder, name);
    size_t size;
    char* text = snx_is_file(path) ? (char*)snx_read_file(path, &size) : NULL;
    free(path);
    return text;
}
static void holds(const char* folder, const char* name, const char* words) {
    char* text = text_of(folder, name);
    if (!text || !strstr(text, words)) fprintf(stderr, "%s/%s holds:\n%s\nnot: %s\n", folder, name, text, words);
    assert(text && strstr(text, words));
    free(text);
}
static void key(App* app, SDL_Keycode code) {
    SDL_Event e = {0};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = code;
    e.key.down = true;
    event(app, &e);
}
static void typed(App* app, const char* text) {
    SDL_Event e = {0};
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = text;
    event(app, &e);
}
/* the system's folder picker answering path (NULL: none), its own answer to the picker the test has none of dropped:
   with the copies folder_picked made for event() */
static void folder_answer_is(App* app, const char* path) {
    SDL_Event e = {0}, own;
    assert(app->picked_folder);
    while (SDL_PeepEvents(&own, 1, SDL_GETEVENT, app->picked_folder, app->picked_folder) == 1) {
        SDL_free(own.user.data1);
        SDL_free(own.user.data2);
    }
    e.type = app->picked_folder;
    e.user.data1 = path ? SDL_strdup(path) : NULL;
    event(app, &e);
}
static int page(const App* app) { return setup_page_of(app); }
/* a box's head colour, as pane draws it: the page's on a filled theme's band of ink */
static GuiRgb head_ink(const App* app) {
    return app->theme.panels == GUI_PANELS_FILL ? app->theme.page : app->theme.ink;
}
/* the page's choice in focus, and the one before it (NULL for none), as its question shows them */
static const char* choice_label(const App* app) {
    const char* labels[4];
    assert(setup_labels(app, labels) > app->setup_choice.focus);
    return labels[app->setup_choice.focus];
}
static const char* choice_before(const App* app) {
    const char* labels[4];
    assert(setup_labels(app, labels) > app->setup_choice.focus);
    return app->setup_choice.focus ? labels[app->setup_choice.focus - 1] : NULL;
}
/* the page's choices drawn as rows top to bottom, want's labels ("" the row between the two groups: a label, no box,
   its row's room the only one between them) each with its icon (icons.h's, as gui_screen.h writes it), each a row's step
   under the one before, the one in focus focus's: the marker, the icon and the label drawn in its row in the focus's
   look, the others' in ink */
static void rows_show(App* app, const char* const* want, const char* const* icons, int n, const char* focus) {
    GuiSetupBoxes b;
    int i, x, y;
    char label[64];
    render(app);
    setup_boxes(app, &b);
    app->choices_of = GUI_CHOICES_SETUP;
    assert(b.choices.count == n);
    for (i = 0; i < n; ++i) {
        UiRowInfo info = {0};
        GuiRect box = choice_box(app, &b.choices, i);
        bool focused = !strcmp(want[i], focus);
        assert(ui_choices_rows_get_row_info(app, (size_t)i, box.w, &info) && !strcmp(info.label, want[i]));
        assert(!want[i][0] == !!(info.attr & UI_ATTR_LABEL));
        assert(focused == (b.choices.lists[UI_CHOICES_ROWS].focus == (size_t)i));
        if (i) assert(box.y - choice_box(app, &b.choices, i - 1).y == item_h(app) + 2 * app->scale);
        if (!want[i][0]) {
            assert(!info.icon && !icons[i][0]);
            continue;
        }
        is_icon(icons[i], info.icon);
        snprintf(label, sizeof label, "%s%s %s", app->theme.marker, icons[i], want[i]);
        assert(find_text(app, label,
                         rgb(!focused               ? app->theme.ink
                             : app->theme.focus_bar ? app->theme.page
                                                    : app->theme.focus),
                         1, &x, &y));
        assert(y >= box.y && y < box.y + box.h);
    }
}
/* a click on the setup page's row i (setup_order's order) */
static void click_row(App* app, int i) {
    GuiSetupBoxes b;
    render(app);
    setup_boxes(app, &b);
    point(app, choice_box(app, &b.choices, i), true);
}
/* the Appearance's row i as drawn (ui.txt's scene Appearance): its box, past the cursor's margin, and its whole row */
static GuiRect appearance_row_box(const App* app, const GuiAppearanceBoxes* a, int i) {
    size_t first = a->lists[UI_APPEARANCE_ROWS].first;
    return ui_row_box(app, ui_appearance_row(a->rects, app->scale, UI_APPEARANCE_ROWS, (size_t)i - first));
}
static GuiRect appearance_whole_row(const App* app, const GuiAppearanceBoxes* a, int i) {
    return ui_appearance_row(a->rects, app->scale, UI_APPEARANCE_ROWS, (size_t)i - a->lists[UI_APPEARANCE_ROWS].first);
}
/* a and b one folder however each is written: the menu keeps a folder it takes resolved (snx_absolute), with
   Windows' separators where snx_path_join wrote '/' */
static bool same_path(const char* a, const char* b) {
    char *x = snx_absolute(a), *y = snx_absolute(b);
    bool same = x && y && !strcmp(x, y);
    free(x);
    free(y);
    return same;
}
/* the menu's config folder and data folder as the menu has them now */
static char* config_of(const App* app) { return snx_menu_dir(app->home_dir, SNX_USER_CONFIG); }

/* a first start: every page, what each writes; then nothing left */
static void test_first(App* app) {
    char *portable = join(bin, SNX_PORTABLE_DIR), *games = join(root, "games picked"), *dumps = join(root, "dumps");
    char *keys = join(root, "keys"), *config, *users, *path;
    int notices; /* the first setup's, one a page */
    assert(!snx_mkdirs(games) && !snx_mkdirs(dumps));
    path = join(keys, "prod.keys");
    put(path, "header_key = 00112233\n");
    free(path);
    path = join(keys, "title.keys");
    put(path, "0100000000000000000000000000000a = 0011aabb\n");
    free(path);
    setup_start(app);
    assert(app->setup_on && app->setup_count == 1 + SNX_SETUP_STEPS && page(app) == SETUP_NOTICE);
    render(app);
    shows_big(app, "SETUP", rgb(app->theme.ink), title_size(app));
    shows(app, "Changed in older versions", rgb(app->theme.page)); /* YoRHa's: in the box of ink at the foot */
    shows(app, "Notice", rgb(app->theme.page));                    /* the tab shown, inverted */
    shows(app, ICON_PALETTE " Appearance", rgb(app->theme.ink));
    hides(app, "1 " ICON_PALETTE " Appearance", rgb(app->theme.ink));
    { /* each tab its icon and its name, no number: the six fit a 1280 window at 2x, as before their icons */
        int i, cols = 0;
        for (i = 0; i < app->setup_count; ++i) cols += tab_cols(app, i) + 3;
        assert((cols + 1) * 16 <= 1280);
    }
    /* the notices one a page, 1/n, read to the last before the setup goes on: Next, Back past the first (the focus
       kept on it while pressed), and OK on the last only; ESC asks to exit, as the menu's */
    {
        int list[SNX_NOTICES], n = setup_notices(app, list);
        char head[32];
        snprintf(head, sizeof head, "Changed 1/%d", n);
        shows(app, head, rgb(head_ink(app)));
        assert(n > 2 && !strcmp(choice_label(app), "Next"));
        notices = n;
    }
    assert(app->setup_choice.count == 1 && !choice_before(app));
    key(app, SDLK_RETURN);
    assert(page(app) == SETUP_NOTICE && app->setup_top == 1);
    assert(!strcmp(choice_label(app), "Next") && choice_before(app) && !strcmp(choice_before(app), "Back"));
    key(app, SDLK_RETURN);
    key(app, SDLK_LEFT); /* to Back */
    assert(app->setup_top == 2 && !strcmp(choice_label(app), "Back"));
    key(app, SDLK_RETURN);
    assert(app->setup_top == 1 && !strcmp(choice_label(app), "Back"));
    key(app, SDLK_RETURN);
    assert(app->setup_top == 0 && app->setup_choice.count == 1 && !strcmp(choice_label(app), "Next"));
    {
        GuiSetupBoxes c;
        key(app, SDLK_RETURN); /* Back with the mouse, from the second */
        render(app);
        setup_boxes(app, &c);
        point(app, choice_box(app, &c.choices, 0), true);
        assert(app->setup_top == 0);
    }
    for (int i = 0; i < notices - 1; ++i) key(app, SDLK_RETURN);
    assert(page(app) == SETUP_NOTICE && app->setup_top == notices - 1);
    render(app);
    {
        char head[32];
        snprintf(head, sizeof head, "Changed %d/%d", notices, notices);
        shows(app, head, rgb(head_ink(app)));
    }
    assert(!strcmp(choice_label(app), "OK") && !strcmp(choice_before(app), "Back"));
    key(app, SDLK_ESCAPE);
    assert(app->confirm.shown && app->confirm.ask == GUI_ASK_EXIT);
    { /* the pointer over Exit, the page's choices drawn last: the question's focus moves, not the page's */
        GuiChoices c;
        int focus = app->setup_choice.focus;
        ask_place(app, &c);
        point(app, choice_box(app, &c, 1), false);
        assert(app->confirm.selected == GUI_CONFIRM_ABORT && app->setup_choice.focus == focus);
    }
    key(app, SDLK_ESCAPE);
    assert(!app->confirm.shown && app->setup_on && !app->menu.quit);
    /* Appearance: Settings > App's Language, Theme and UI scale a row each, then Next. Left/Right change the language
       and the theme at once, the screen with them, Auto again on the system's; UI scale's box and its question, the
       size kept for app.ini; a click on a row's left arrow goes back a step; nothing written until the setup is done */
    key(app, SDLK_RETURN);
    assert(page(app) == SNX_SETUP_LANGUAGE && app->setup_row == GUI_APPEARANCE_LANGUAGE && !app->language);
    assert(app->setup_choice.count == 1 && !strcmp(choice_label(app), "Next") && snx_gui_language_count == 3);
    render(app);
    shows(app, "Appearance", rgb(head_ink(app)));
    shows(app, "< English >", rgb(app->theme.page)); /* the row in focus, inverted on YoRHa's focus bar */
    { /* its value at appearance_column of the row's text, past the marker ui_draw_row draws */
        GuiSetupBoxes c;
        GuiAppearanceBoxes a;
        GuiRect row;
        int x, y;
        setup_boxes(app, &c);
        appearance_boxes(app, c.content, &a);
        row = appearance_row_box(app, &a, GUI_APPEARANCE_LANGUAGE);
        assert(find_text(app, "< English >", rgb(app->theme.page), 1, &x, &y));
        assert(x == row.x + cell_w(app) / 2 + appearance_column(app) * cell_w(app) && y > row.y);
    }
    shows(app, "< YoRHa >", rgb(app->theme.ink));
    { /* UI scale: the start's number, as App's row has it, lined up with the text of the others' values; its slider
         is its dialog's */
        GuiSetupBoxes c;
        GuiAppearanceBoxes a;
        GuiRect row;
        int x, y;
        assert(app->menu.text_size == 2 && app->auto_scale == 2); /* the test's window's */
        setup_boxes(app, &c);
        appearance_boxes(app, c.content, &a);
        row = appearance_row_box(app, &a, GUI_APPEARANCE_TEXT);
        assert(find_text(app, "2x", rgb(app->theme.ink), 1, &x, &y));
        assert(x == row.x + cell_w(app) / 2 + (appearance_column(app) + 2) * cell_w(app) && y > row.y);
        {
            UiRowInfo info = {0};
            assert(ui_appearance_rows_get_row_info(app, GUI_APPEARANCE_TEXT, row.w, &info) &&
                   !strstr(info.label, "Auto"));
        }
        assert(!app->slider_bar.w);
    }
    hides(app, "Next", rgb(app->theme.page)); /* drawn, not in focus */
    key(app, SDLK_RETURN);                    /* as Settings > App's Enter: the language a step on, the page kept */
    assert(page(app) == SNX_SETUP_LANGUAGE && app->setup_row == GUI_APPEARANCE_LANGUAGE && app->language == 2);
    key(app, SDLK_LEFT);
    assert(!app->language);
    shows_foot(app, "change", rgb(head_ink(app))); /* the foot's words, on YoRHa's band of ink */
    key(app, SDLK_RIGHT);
    assert(app->language == 2 && snx_tr_language() == 1 && app->setup_save);
    render(app);
    {
        int x, y;
        assert(find_letters(app, "< Test\303\252s >", rgb(app->theme.page), 1, &x, &y));
        assert(find_letters(app, "Apar\303\252ncia", rgb(head_ink(app)), 1, &x, &y)); /* xx's words, at once */
    }
    key(app, SDLK_RIGHT);
    assert(app->language == 3 && snx_tr_language() == 2);
    key(app, SDLK_RIGHT); /* English, the system's: Auto, as the setup found it */
    assert(!app->language && snx_tr_language() == 0);
    key(app, SDLK_LEFT);
    assert(app->language == 3);
    { /* the rows as Settings' (ui.txt's scene Appearance): 2 pixels apart and as high as the menu's, the cursor in
         the row, which a click takes whole */
        GuiSetupBoxes c;
        GuiAppearanceBoxes a;
        GuiRect row, whole;
        int s = app->scale;
        render(app);
        setup_boxes(app, &c);
        appearance_boxes(app, c.content, &a);
        row = appearance_row_box(app, &a, GUI_APPEARANCE_LANGUAGE);
        whole = appearance_whole_row(app, &a, GUI_APPEARANCE_LANGUAGE);
        assert(row.h == item_h(app) && whole.x < row.x && row.y >= a.box.y + head_h(app));
        assert(appearance_row_box(app, &a, GUI_APPEARANCE_THEME).y - row.y == item_h(app) + 2 * s);
        assert(appearance_row_box(app, &a, GUI_APPEARANCE_TEXT).y + item_h(app) <= a.box.y + a.box.h);
        mouse_px(app, row.x + cell_w(app) / 2 + appearance_column(app) * cell_w(app) + cell_w(app) / 2,
                 row.y + row.h / 2, true); /* its left arrow: back a step */
        assert(app->language == 2 && snx_tr_language() == 1 && app->setup_row == GUI_APPEARANCE_LANGUAGE);
        mouse_px(app, whole.x + 1, whole.y + whole.h / 2, true); /* in the cursor's margin: a step on */
        assert(app->language == 3 && app->setup_row == GUI_APPEARANCE_LANGUAGE);
        mouse_px(app, row.x + cell_w(app) / 2 + appearance_column(app) * cell_w(app) + cell_w(app) / 2,
                 row.y + row.h / 2, true);
        assert(app->language == 2 && snx_tr_language() == 1);
        point(app, appearance_row_box(app, &a, GUI_APPEARANCE_THEME), false); /* the pointer over a row: its focus */
        assert(app->setup_row == GUI_APPEARANCE_THEME && app->language == 2);
        point(app, choice_box(app, &c.choices, 0), false); /* over Next: the focus on it, off the rows */
        assert(app->setup_row == GUI_APPEARANCE_NEXT && page(app) == SNX_SETUP_LANGUAGE);
        point(app, appearance_row_box(app, &a, GUI_APPEARANCE_THEME), false);
        assert(app->setup_row == GUI_APPEARANCE_THEME);
        { /* UI scale clicked: its dialog */
            GuiRect text = appearance_row_box(app, &a, GUI_APPEARANCE_TEXT);
            mouse_px(app, text.x + 2 * cell_w(app), text.y + text.h / 2, true);
        }
        assert(app->sizes_shown && app->setup_row == GUI_APPEARANCE_TEXT && app->language == 2);
        key(app, SDLK_ESCAPE);
        assert(!app->sizes_shown);
        point(app, appearance_row_box(app, &a, GUI_APPEARANCE_THEME), false);
        /* a box too short for the three: the rows that fit, from the focused one's, inside it */
        app->setup_row = GUI_APPEARANCE_TEXT;
        appearance_boxes(app, gui_rect(c.content.x, c.content.y, c.content.w, head_h(app) + 4 * s + 2 * item_h(app)),
                         &a);
        assert(ui_appearance_shown(a.rects, s, UI_APPEARANCE_ROWS) == 1 && a.lists[UI_APPEARANCE_ROWS].first == 2);
        assert(appearance_whole_row(app, &a, GUI_APPEARANCE_TEXT).y + item_h(app) <= a.box.y + a.box.h);
        app->setup_row = GUI_APPEARANCE_THEME;
    }
    /* Enter on Theme: the box of themes over the page, the one shown in focus; the focus shows the theme it reaches,
       ESC goes back to the one before */
    key(app, SDLK_RETURN);
    assert(app->picks_shown && app->picks_of == GUI_PICKS_THEME &&
           !strcmp(app->theme_picks[app->picks.focus], "YoRHa"));
    render(app);
    key(app, SDLK_UP);
    assert(app->picks_shown && !strcmp(app->theme.id, "wimsy-dark"));
    key(app, SDLK_ESCAPE);
    assert(!app->picks_shown && !strcmp(app->theme.id, "yorha") && app->setup_row == GUI_APPEARANCE_THEME);
    { /* in the default's dialog, a box in the middle of the screen, the box keeps out of the Theme row it changes,
         its question and its rows in sight: at this window's size, and at a phone's held wide at 2x */
        int was_w, was_h, k;
        assert(SDL_GetWindowSize(app->window, &was_w, &was_h));
        key(app, SDLK_RETURN);
        key(app, SDLK_DOWN); /* Default, the first theme, round from YoRHa, the last */
        assert(app->picks_shown && !strcmp(app->theme.id, "default") && app->theme.dialog == GUI_DIALOG_DEFAULT);
        for (k = 0; k < 3; ++k) {
            GuiChoices c;
            GuiSetupBoxes b;
            GuiAppearanceBoxes a;
            GuiRect row;
            char question[640];
            const char* tab;
            if (k) { /* a phone held wide at 2x, and a desktop window of 1280 by 688 at 2x */
                app->menu.text_size = 2;
                assert(SDL_SetWindowSize(app->window, k == 1 ? 2340 : 1280, k == 1 ? 1080 : 688));
                SDL_SyncWindow(app->window);
            }
            render(app);
            picks_place(app, &c, question, sizeof question, &tab);
            setup_boxes(app, &b);
            appearance_boxes(app, b.content, &a);
            row = appearance_row_box(app, &a, GUI_APPEARANCE_THEME);
            assert(c.box.y >= row.y + row.h || c.box.y + c.box.h <= row.y);
            assert(c.at.h >= 3 * item_h(app) && c.at.y + c.at.h <= c.box.y + c.box.h);
            { /* the box and every row it shows inside the window, the focused one among them */
                int width, height, shown = (int)ui_choices_shown(c.rects, app->scale, UI_CHOICES_ROWS), i;
                area(app, &width, &height);
                assert(shown > 0 && c.box.y >= 0 && c.box.y + c.box.h <= height);
                for (i = 0; i < shown; ++i) {
                    GuiRect r = ui_choices_row(c.rects, app->scale, UI_CHOICES_ROWS, (size_t)i);
                    assert(r.y >= c.at.y && r.y + r.h <= c.at.y + c.at.h && r.y + r.h <= height);
                }
            }
        }
        app->menu.text_size = 0;
        assert(SDL_SetWindowSize(app->window, was_w, was_h));
        SDL_SyncWindow(app->window);
        key(app, SDLK_ESCAPE);
        assert(!app->picks_shown && !strcmp(app->theme.id, "yorha"));
    }
    key(app, SDLK_RIGHT); /* the next theme, the dialog's place with it */
    assert(!strcmp(app->theme.id, "default") && app->theme.dialog == GUI_DIALOG_DEFAULT);
    render(app);
    shows(app, "< Default >", rgb(app->theme.focus));
    {
        GuiSetupBoxes c;
        GuiAppearanceBoxes a;
        GuiRect box;
        setup_boxes(app, &c);
        appearance_boxes(app, c.content, &a);
        box = a.box;
        { /* the question in the middle of what is under the rows */
            int under = box.y + box.h + cell_h(app), bottom = c.place.list.y + c.place.list.h;
            assert(c.choices.box.y >= under);
            assert(SDL_abs(c.choices.box.y + c.choices.box.h / 2 - (under + bottom) / 2) <= cell_h(app));
        }
    }
    key(app, SDLK_LEFT);
    assert(!strcmp(app->theme.id, "yorha") && app->theme.dialog == GUI_DIALOG_YORHA);
    key(app, SDLK_DOWN);
    assert(app->setup_row == GUI_APPEARANCE_TEXT);
    render(app);
    shows_foot(app, "open", rgb(head_ink(app)));
    key(app, SDLK_RETURN); /* its dialog, over the page, as App's */
    assert(size_picking(app) && app->size_pick == 2 && page(app) == SNX_SETUP_LANGUAGE);
    key(app, SDLK_RIGHT);
    render(app);
    assert(app->size_pick == 3 && app->menu.text_size == 2);
    { /* in YoRHa too a box in the middle of the screen, as the View box is, not its foot: its title on its head, then
         the slider and the step picked, above OK and Cancel */
        GuiSizesBoxes b;
        GuiRect bar;
        int width, height, x, y;
        sizes_place(app, &b);
        bar = sizes_bar(app, &b);
        area(app, &width, &height);
        assert(app->theme.dialog == GUI_DIALOG_YORHA && app->theme.panels == GUI_PANELS_FILL);
        assert(b.c.box.x * 2 + b.c.box.w == width && b.c.box.y + b.c.box.h < height - cell_h(app));
        assert(find_text(app, "UI scale", rgb(app->theme.page), 1, &x, &y)); /* on the head's band of ink */
        assert(x == b.c.box.x + cell_w(app) && y == b.c.box.y + 2 * app->scale);
        assert(b.body.y >= b.c.box.y + head_h(app) && bar.y + bar.h <= choice_box(app, &b.c, 0).y);
        assert(find_text(app, "3x", rgb(app->theme.ink), 1, &x, &y) && x == bar.x + bar.w + cell_w(app));
    }
    key(app, SDLK_LEFT);
    key(app, SDLK_LEFT);
    key(app, SDLK_LEFT);   /* 1x, the first: it stays */
    key(app, SDLK_RETURN); /* OK on 1x */
    assert(app->sizes_shown && app->size_until && app->menu.text_size == 1 && !app->confirm.shown);
    render(app);
    shows(app, app->size_line, rgb(app->theme.warning));
    key(app, SDLK_UP); /* Keep */
    key(app, SDLK_RETURN);
    assert(!app->sizes_shown && app->menu.text_size == 1 && app->setup_on && page(app) == SNX_SETUP_LANGUAGE);
    path = join(home, ".config/scarletnx/app.ini");
    assert(!snx_is_file(path)); /* kept for the setup's end, in the folders Install picks */
    free(path);
    key(app, SDLK_RIGHT); /* its dialog, its slider a step on: 2x, used on OK and asked about, ESC bringing 1x back */
    assert(size_picking(app) && app->size_pick == 2 && app->menu.text_size == 1);
    key(app, SDLK_RETURN);
    assert(app->menu.text_size == 2 && app->size_until);
    key(app, SDLK_ESCAPE);
    assert(!app->sizes_shown && app->menu.text_size == 1 && app->setup_row == GUI_APPEARANCE_TEXT);
    key(app, SDLK_RETURN); /* its dialog again, closed by ESC */
    assert(app->sizes_shown);
    key(app, SDLK_ESCAPE);
    assert(!app->sizes_shown && app->setup_on && page(app) == SNX_SETUP_LANGUAGE);
    /* the Side panel, and Games' View and Icon size, as Settings > App and the View box have them, their words: a step
       either way, kept for the setup's end; Icon size the grid's, greyed in the list, a step there saying why (#614) */
    key(app, SDLK_DOWN);
    assert(app->setup_row == GUI_APPEARANCE_SIDE && app->side_box == GUI_SIDE_AUTO);
    render(app);
    shows(app, "< Auto >", rgb(app->theme.page)); /* in focus, inverted */
    key(app, SDLK_RIGHT);
    assert(app->side_box == GUI_SIDE_ON && app->setup_save);
    key(app, SDLK_DOWN);
    assert(app->setup_row == GUI_APPEARANCE_VIEW && !games_grid(app));
    render(app);
    shows(app, "< List >", rgb(app->theme.page));
    shows(app, "< Medium >", rgb(app->theme.soft)); /* Icon size, greyed */
    key(app, SDLK_DOWN);
    assert(app->setup_row == GUI_APPEARANCE_ICONS);
    key(app, SDLK_RIGHT);
    assert(app->options[GUI_OPTION_ICONS] == 1 && !strcmp(app->message, "Icon size is the grid's: View is List"));
    app->message[0] = 0;
    key(app, SDLK_UP);
    key(app, SDLK_RETURN); /* Grid */
    assert(games_grid(app) && app->setup_row == GUI_APPEARANCE_VIEW);
    key(app, SDLK_DOWN);
    key(app, SDLK_RIGHT);
    assert(app->options[GUI_OPTION_ICONS] == 2);
    render(app);
    shows(app, "< Large >", rgb(app->theme.page));
    key(app, SDLK_UP);
    key(app, SDLK_LEFT); /* the list again */
    assert(!games_grid(app));
    path = join(home, ".config/scarletnx/app.ini");
    assert(!snx_is_file(path)); /* still for the setup's end */
    free(path);
    key(app, SDLK_DOWN);
    key(app, SDLK_DOWN);
    assert(app->setup_row == GUI_APPEARANCE_NEXT);
    render(app);
    shows(app, "Next", rgb(app->theme.page));     /* in focus now */
    hides(app, "UI scale", rgb(app->theme.page)); /* and no row */
    key(app, SDLK_RETURN);
    assert(page(app) == SNX_SETUP_INSTALL && app->language == 2 && app->setup_save && snx_tr_language() == 1);
    app->language = 0; /* English again, for the words below, kept as the language picked */
    language_apply(app);
    app->language = 2;
    /* installed while a portable folder is there is refused; portable makes it, the menu's files in it from then. The
       box over the two says which the copy is now, and where its files are */
    assert(!app->setup_choice.focus && !snx_is_dir(portable));
    {
        char line[1100], expected[1100], shown[1024], *data = snx_user_dir(SNX_USER_DATA);
        assert(data);
        snx_home_short(data, shown, sizeof shown);
        snprintf(expected, sizeof expected, "Now: installed, its files in %s", shown);
        setup_install_now(app, line, sizeof line);
        assert(!strcmp(line, expected));
        free(data);
        render(app);
        shows(app, "Now: installed, its files in", rgb(app->theme.ink));
    }
    key(app, SDLK_DOWN);
    key(app, SDLK_RETURN);
    assert(page(app) == SNX_SETUP_FOLDERS && snx_is_dir(portable) && same_path(app->home_dir, portable));
    key(app, SDLK_ESCAPE); /* back: it is portable now, the focus on Portable */
    assert(page(app) == SNX_SETUP_INSTALL && app->setup_choice.focus == 1);
    { /* the steps' own choices have no icon: the notices' and the old files' alone do */
        UiRowInfo info = {0};
        app->choices_of = GUI_CHOICES_SETUP;
        assert(ui_choices_rows_get_row_info(app, 0, 0, &info) && !info.icon);
    }
    {
        char line[1100], expected[1100], shown[1024];
        snx_home_short(portable, shown, sizeof shown);
        snprintf(expected, sizeof expected, "Now: portable, its files in %s", shown);
        setup_install_now(app, line, sizeof line);
        assert(!strcmp(line, expected) && strstr(line, SNX_PORTABLE_DIR));
        render(app);
        shows(app, "Now: portable, its files in", rgb(app->theme.ink));
    }
    key(app, SDLK_UP);
    key(app, SDLK_RETURN);
    assert(page(app) == SNX_SETUP_INSTALL && app->setup_failed && strstr(app->message, "This copy is portable"));
    render(app);
    shows_foot(app, "This copy is portable: to install it, first move away", rgb(app->theme.failure));
    key(app, SDLK_DOWN);
    key(app, SDLK_RETURN);
    assert(page(app) == SNX_SETUP_FOLDERS && !app->setup_failed && !app->message[0]);
    /* the games' folder changed, a folder of dumps added, as Settings > App's Folders and Library would */
    key(app, SDLK_DOWN);
    key(app, SDLK_RETURN);
    folder_answer_is(app, games);
    assert(page(app) == SNX_SETUP_FOLDERS && app->place[GUI_PLACE_GAMES] && same_path(app->games_dir, games));
    config = config_of(app);
    holds(config, "app.ini", "[folders]\n");
    holds(config, "app.ini", "recompiled = ");
    key(app, SDLK_DOWN);
    key(app, SDLK_RETURN);
    folder_answer_is(app, dumps);
    assert(app->folder_count == 1 && same_path(app->folders[0], dumps));
    holds(config, "folders.txt", "dumps\n");
    render(app);
    shows(app, "Recompiled", rgb(app->theme.ink));
    key(app, SDLK_DOWN); /* round to Keep */
    key(app, SDLK_RETURN);
    /* the keys: none, Later in focus; a folder of them installed where the menu keeps them, Use These then */
    assert(page(app) == SNX_SETUP_KEYS && app->setup_choice.count == 2 && app->setup_choice.focus == 1);
    key(app, SDLK_UP);
    key(app, SDLK_RETURN);
    assert(app->setup_keys);
    folder_answer_is(app, NULL);
    assert(page(app) == SNX_SETUP_KEYS && app->setup_failed && !app->setup_keys && !app->confirm.shown);
    {
        char* keys_path = app->keys_path; /* NULL when snx_keys_dir finds no home: the variable it reads is named */
        app->keys_path = NULL;
        key(app, SDLK_RETURN);
        folder_answer_is(app, keys);
#ifdef _WIN32
        assert(app->setup_failed &&
               !strcmp(app->message, "Cannot install prod.keys: neither USERPROFILE nor HOME is set"));
#else
        assert(app->setup_failed && !strcmp(app->message, "Cannot install prod.keys: HOME is not set"));
#endif
        /* a failure, unlike none picked: in the box too, whole, until OK */
        assert(app->confirm.shown && app->confirm.ask == GUI_ASK_ERROR && !strcmp(app->error_line, app->message));
        key(app, SDLK_RETURN);
        assert(!app->confirm.shown && page(app) == SNX_SETUP_KEYS);
        app->keys_path = keys_path;
    }
    key(app, SDLK_RETURN);
    folder_answer_is(app, keys);
    assert(!app->setup_failed && strstr(app->message, "Installed prod.keys and title.keys in"));
    assert(app->setup_choice.count == 3 && !app->setup_choice.focus);
    path = snx_dirname(app->keys_path);
    holds(path, "prod.keys", "header_key");
    holds(path, "title.keys", "0100000000000000000000000000000a");
    free(path);
    key(app, SDLK_RETURN);
    /* the first user: no users.ini, so its nickname typed, a random uid; one that is no nickname refused */
    assert(page(app) == SNX_SETUP_USER && app->setup_typing && !strcmp(app->setup_name, "Player"));
    assert(app->setup_saves == 0);
    {
        char uid[33];
        snx_users_uid_text(app->setup_uid, uid);
        assert(uid[12] == '4' && strcmp(uid, FIRST));
    }
    for (int i = 0; i < 6; ++i) key(app, SDLK_BACKSPACE);
    key(app, SDLK_A); /* a letter comes as text, not as a key */
    typed(app, "A");
    typed(app, "da");
    render(app);
    shows(app, "Nickname: Ada_", rgb(app->theme.page));
    typed(app, " "); /* a space at its end, which the account service does not keep */
    key(app, SDLK_RETURN);
    assert(app->setup_on && app->setup_failed && strstr(app->message, "Ada  is not a nickname"));
    key(app, SDLK_BACKSPACE);
    typed(app, "m");
    key(app, SDLK_RETURN);
    /* done: setup.ini and users.ini in the portable copy, the language in its app.ini, the menu with its user */
    assert(!app->setup_on && !app->message[0] && app->user == 1 && !strcmp(app->user_name, "Adam"));
    users = join(portable, "users");
    holds(users, "users.ini", "nickname=Adam\n");
    {
        char seen[64]; /* every notice there is, seen */
        snprintf(seen, sizeof seen, "[setup]\nversion = 1\nnotices = %d\n", SNX_NOTICES);
        holds(config, "setup.ini", seen);
    }
    holds(config, "app.ini", "language = xx\n");
    holds(config, "app.ini", "theme = yorha\nui_scale = 1\n");
    holds(config, "app.ini", "side_box = on\n");
    holds(config, "app.ini", "games_view = list\n");
    holds(config, "app.ini", "games_icons = large\n");
    path = join(home, ".config/scarletnx/setup.ini");
    assert(!snx_is_file(path)); /* nothing in the user's folders */
    free(path);
    setup_start(app);
    assert(!app->setup_on);
    free(users);
    free(config);
    free(keys);
    free(dumps);
    free(games);
    free(portable);
    puts("PASS a first start through every page");
}

/* users.ini there: kept, or overwritten (the old one kept), out of the nickname with ESC; the fixed uid for its saves */
static void test_users(App* app) {
    char *config = config_of(app), *before, *now, *saves;
    {
        char* file = join(config, "setup.ini");
        remove(file);
        free(file);
    }
    setup_start(app);
    app->setup_page = app->setup_count - 1;
    setup_enter(app);
    assert(page(app) == SNX_SETUP_USER && !app->setup_typing && app->setup_users == 1 && !app->setup_choice.focus);
    render(app);
    shows(app, "users.ini has 1 user", rgb(app->theme.page));
    shows(app, "Adam", rgb(app->theme.ink));
    key(app, SDLK_DOWN);
    key(app, SDLK_RETURN);
    assert(app->setup_typing);
    key(app, SDLK_ESCAPE);
    assert(!app->setup_typing && app->setup_on && app->setup_choice.count == 2 && app->setup_choice.focus == 1);
    key(app, SDLK_UP);
    before = text_of(app->users_dir, "users.ini");
    key(app, SDLK_RETURN);
    now = text_of(app->users_dir, "users.ini");
    assert(!app->setup_on && before && now && !strcmp(before, now));
    free(now);
    /* overwritten, games with saves of the fixed uid: it keeps it */
    saves = join(app->games_dir, "0100A/0100A_data/save/user." FIRST);
    assert(!snx_mkdirs(saves));
    {
        char* file = join(config, "setup.ini");
        remove(file);
        free(file);
    }
    setup_start(app);
    app->setup_page = app->setup_count - 1;
    setup_enter(app);
    assert(app->setup_saves == 1);
    key(app, SDLK_DOWN);
    key(app, SDLK_RETURN);
    assert(app->setup_typing && app->setup_saves == 1);
    render(app);
    shows(app, "of the saves of 1 game", rgb(app->theme.ink));
    key(app, SDLK_RETURN);
    assert(!app->setup_on);
    holds(app->users_dir, "users.ini", "uid=" FIRST "\nnickname=Player\n");
    holds(app->users_dir, "users.ini.old", "nickname=Adam\n");
    free(saves);
    free(before);
    free(config);
    puts("PASS users.ini kept or overwritten, the fixed uid kept for its saves");
}

/* after an update: the notice it brings, alone; --debug: nothing */
static void test_notice(App* app) {
    char *config = config_of(app), *file = join(config, "setup.ini"), before[64], after[64];
    /* the newest notice unseen, all the others seen */
    snprintf(before, sizeof before, "[setup]\nversion = 1\nnotices = %d\n", SNX_NOTICES - 1);
    snprintf(after, sizeof after, "notices = %d\n", SNX_NOTICES);
    put(file, before);
    setup_start(app);
    assert(app->setup_on && app->setup_count == 1 && page(app) == SETUP_NOTICE);
    render(app);
    shows(app, "Changed since the last version", rgb(app->theme.page));
    shows(app, "Changed 1/1", rgb(head_ink(app)));
    assert(!strcmp(choice_label(app), "OK"));
    shows(app, snx_notices[SNX_NOTICES - 1].what, rgb(app->theme.ink)); /* in English: xx has no word of it */
    hides(app, "Game settings", rgb(app->theme.ink));
    hides(app, "1 Appearance", rgb(app->theme.ink));
    key(app, SDLK_RETURN);
    assert(!app->setup_on);
    holds(config, "setup.ini", after);
    remove(file);
    snx_setup_skip = 1;
    setup_start(app);
    assert(!app->setup_on && !snx_is_file(file));
    snx_setup_skip = 0;
    free(file);
    free(config);
    puts("PASS a notice alone after an update, and --debug");
}

/* the last notice asks Clean Old Settings while it has something to take: Clean Now in focus cleans there, the box
   saying what it did and OK going on; Not Now goes on and cleans none, nor does Skip; with nothing to take, OK */
static void test_clean_notice(App* app) {
    char *config = config_of(app), *file = join(config, "setup.ini"), *game = join(app->games_dir, "0100000000ABC000");
    char *ini = join(game, "0100000000ABC000.ini"), before[64], *text;
    const char* labels[GUI_SETUP_CHOICES];
    static const char deprecated[] = "[Display]\nfullscreen=1\n\n[Deprecated]\n; renderer: Vulkan is the only "
                                     "renderer\nrenderer=opengl\n";
    snprintf(before, sizeof before, "[setup]\nversion = 1\nnotices = %d\n", SNX_NOTICES - 2);
    put(file, before);
    put(ini, deprecated);
    setup_start(app);
    assert(app->setup_on && page(app) == SETUP_NOTICE && !strcmp(choice_label(app), "Next"));
    assert(app->clean_inis_found < 0); /* looked for as the last comes, not before */
    key(app, SDLK_RETURN);
    assert(app->setup_top == 1 && app->clean_inis_found == 1 && app->setup_choice.count == 3);
    assert(setup_labels(app, labels) == 4 && !strcmp(labels[0], "Back") && !strcmp(labels[1], "Clean Now") &&
           !strcmp(labels[2], "Not Now") && !strcmp(labels[3], "Skip"));
    assert(!strcmp(choice_label(app), "Clean Now"));
    { /* two groups: Back, Not Now and Skip, which keep the files as they are, a row apart from Clean Now, in focus;
         the keys go round them past that row, which takes no click */
        static const char* const rows[] = {"Back", "Not Now", "Skip", "", "Clean Now"};
        static const char* const icons[] = {ICON_ARROW_LEFT, ICON_X, ICON_SKIP_FORWARD, "", ICON_ERASER};
        rows_show(app, rows, icons, 5, "Clean Now");
        key(app, SDLK_UP);
        assert(!strcmp(choice_label(app), "Skip"));
        key(app, SDLK_DOWN);
        assert(!strcmp(choice_label(app), "Clean Now"));
        key(app, SDLK_DOWN);
        assert(!strcmp(choice_label(app), "Back"));
        key(app, SDLK_UP);
        click_row(app, 3);
        assert(app->setup_top == 1 && !app->setup_cleaned && !strcmp(choice_label(app), "Clean Now"));
        rows_show(app, rows, icons, 5, "Clean Now");
    }
    render(app);
    shows(app, "Clean Old Settings? [Deprecated] in 1 .ini", rgb(app->theme.ink));
    key(app, SDLK_RETURN); /* Clean Now: done there, said in the box, OK in focus; nothing over the screen */
    text = text_of(game, "0100000000ABC000.ini");
    assert(text && !strcmp(text, "[Display]\nfullscreen=1\n"));
    free(text);
    assert(app->setup_on && app->setup_cleaned && !app->cleaned_shown && app->cleaned.count == 1);
    assert(app->setup_choice.count == 2 && !strcmp(choice_label(app), "OK") && !strcmp(choice_before(app), "Back"));
    render(app);
    shows(app, "Cleaned: 1 done, 0 not", rgb(app->theme.ink));
    shows(app, "[Deprecated] removed", rgb(app->theme.soft));
    hides(app, "Clean Old Settings?", rgb(app->theme.ink));
    key(app, SDLK_RETURN);
    assert(!app->setup_on);
    gui_cleaned_free(&app->cleaned);
    /* Not Now: on, the file as it was; and Skip, which cleans none */
    for (int skip = 0; skip < 2; ++skip) {
        snprintf(before, sizeof before, "[setup]\nversion = 1\nnotices = %d\n", SNX_NOTICES - 1);
        put(file, before);
        put(ini, deprecated);
        setup_start(app);
        assert(app->setup_on && !app->setup_top && !strcmp(choice_label(app), "Clean Now") && !choice_before(app));
        key(app, SDLK_DOWN);
        if (skip) key(app, SDLK_DOWN);
        assert(!strcmp(choice_label(app), skip ? "Skip" : "Not Now"));
        key(app, SDLK_RETURN);
        assert(!app->setup_on && !app->setup_cleaned && !app->cleaned.count);
        text = text_of(game, "0100000000ABC000.ini");
        assert(text && !strcmp(text, deprecated));
        free(text);
    }
    /* nothing to take: OK, as the other notices */
    put(file, before);
    put(ini, "[Display]\nfullscreen=1\n");
    setup_start(app);
    assert(app->setup_on && !app->clean_inis_found && !strcmp(choice_label(app), "OK"));
    key(app, SDLK_RETURN);
    assert(!app->setup_on);
    assert(!snx_remove_tree(game));
    free(ini);
    free(game);
    free(file);
    free(config);
    puts("PASS the last notice's Clean Now, Not Now, and Skip cleaning none");
}

/* what an older version left: a setup page each folder (a game's save/ for each of its kinds), the last, after the
   notices and the steps, alone when nothing else is due; its size, where it goes and what Move All and Delete All
   would do; on the last Move All, Delete All (asked, pressed twice) or Not Now, in focus; a save and keys never
   deleted; F5 and the 5 s look again; the old files' pages alone write no setup.ini */
static void test_old_files(App* app) {
    char *config = config_of(app), *file = join(config, "setup.ini"), *user = snx_user_data_dir();
    char *gui = join(user, "gui"), *settings = join(gui, "settings.ini"), *work = join(app->data_dir, "work");
    char *object = join(work, "a.o"), *games = join(app->data_dir, "games"), *old_game = join(games, "x");
    char *game = join(app->games_dir, "0100000000ABC000"), *saves = join(game, "0100000000ABC000_data/save");
    char *device = join(saves, "device/d.bin"), *moved = join(app->data_dir, "save/device/0100000000ABC000/d.bin");
    char head[32], *text;
    const char* labels[GUI_SETUP_CHOICES];
    static const char mine[] = "; kept as it is\n[setup]\nversion = 1\nnotices = 99\n";
    put(file, mine);
    left_behind(app);
    assert(!app->lefts);
    put(settings, "[menu]\n");
    put(object, "o");
    put(old_game, "x");
    put(device, "save");
    {
        char* account = join(saves, "user." FIRST "/u.bin");
        put(account, "save");
        free(account);
    }
    left_behind(app);
    while (!__atomic_load_n(&app->left_space->done, __ATOMIC_ACQUIRE)) SDL_Delay(1);
    assert(app->lefts == 5 && left_groups(app) == 4);
    setup_start(app);
    assert(app->setup_on && app->setup_count == 1 && page(app) == SETUP_LEFT && !setup_steps(app));
    {
        char tab[64];
        tab_label(app, 0, tab, sizeof tab);
        assert(!strcmp(tab, ICON_FOLDER_INPUT " Old Files"));
    }
    /* the first: gui/, Next alone, no Skip */
    assert(setup_labels(app, labels) == 1 && !strcmp(labels[0], "Next"));
    render(app);
    shows(app, "Left by older versions", rgb(app->theme.page));
    shows(app, "Old Files 1/4", rgb(head_ink(app)));
    shows(app, "size        1 KB", rgb(app->theme.ink));
    shows(app, "Move All    1 entry moves", rgb(app->theme.ink));
    shows(app, "Delete All  Deletes it", rgb(app->theme.ink));
    /* PgDn a page on, PgUp back; End the last, Home the first */
    key(app, SDLK_PAGEDOWN);
    assert(app->setup_top == 1 && setup_labels(app, labels) == 2 && !strcmp(choice_label(app), "Next"));
    { /* none acts: one group, each with its icon */
        static const char* const rows[] = {"Back", "Next"};
        static const char* const icons[] = {ICON_ARROW_LEFT, ICON_CHECK};
        rows_show(app, rows, icons, 2, "Next");
    }
    key(app, SDLK_PAGEUP);
    key(app, SDLK_PAGEUP); /* none before the first */
    assert(!app->setup_top);
    key(app, SDLK_RETURN);
    key(app, SDLK_RETURN);
    assert(app->setup_top == 2); /* the game's save/, its two kinds one page */
    render(app);
    shows(app, "Old Files 3/4", rgb(head_ink(app)));
    shows(app, "Move All    2 entries move", rgb(app->theme.ink));
    shows(app, "Delete All  Stays: a save,", rgb(app->theme.ink));
    {
        SDL_Event end = {0};
        end.type = SDL_EVENT_KEY_DOWN;
        end.key.key = SDLK_END;
        end.key.down = true;
        event(app, &end);
        assert(app->setup_top == 3);
        end.key.key = SDLK_HOME;
        event(app, &end);
        assert(!app->setup_top);
        end.key.key = SDLK_END;
        event(app, &end);
    }
    /* the last: Back, Move All, Delete All and Not Now, in focus */
    assert(setup_labels(app, labels) == 4 && !strcmp(labels[0], "Back") && !strcmp(labels[1], "Move All") &&
           !strcmp(labels[2], "Delete All") && !strcmp(choice_label(app), "Not Now"));
    snprintf(head, sizeof head, "Old Files %d/%d", 4, 4);
    render(app);
    shows(app, head, rgb(head_ink(app)));
    { /* two groups: Back and Not Now, in focus, which keep them as they are, a row apart from Move All and Delete All;
         Down from Not Now passes that row, which takes no click */
        static const char* const rows[] = {"Back", "Not Now", "", "Move All", "Delete All"};
        static const char* const icons[] = {ICON_ARROW_LEFT, ICON_X, "", ICON_FOLDER_INPUT, ICON_TRASH};
        rows_show(app, rows, icons, 5, "Not Now");
        key(app, SDLK_DOWN);
        assert(!strcmp(choice_label(app), "Move All"));
        key(app, SDLK_UP);
        click_row(app, 2);
        assert(app->setup_top == 3 && !app->confirm.shown && !app->left_did && !strcmp(choice_label(app), "Not Now"));
    }
    /* Delete All asks, Delete pressed twice: what it deletes, a save staying */
    key(app, SDLK_DOWN);
    key(app, SDLK_DOWN);
    key(app, SDLK_RETURN);
    assert(app->confirm.shown && app->confirm.ask == GUI_ASK_DELETE_OLD && app->confirm.twice &&
           !strcmp(app->ask_line, "Delete All? 3 deleted, 2 stay: a save and the console's keys are only moved"));
    key(app, SDLK_ESCAPE); /* Cancel: nothing deleted */
    assert(!app->confirm.shown && snx_is_file(settings) && !app->left_did);
    key(app, SDLK_RETURN);
    key(app, SDLK_DOWN);
    key(app, SDLK_RETURN);
    assert(app->confirm.shown && app->confirm.armed && snx_is_file(settings)); /* armed, not yet */
    render(app);
    shows(app, "Delete: press again", rgb(app->theme.page));
    key(app, SDLK_RETURN);
    assert(!app->confirm.shown && app->left_did == 2 && !snx_path_exists(gui) && !snx_path_exists(work) &&
           !snx_path_exists(games) && snx_is_file(device));
    assert(app->moved.count == 4 && app->moved.done == 3 && app->report_delete && !app->cleaned_shown);
    assert(setup_labels(app, labels) == 1 && !strcmp(labels[0], "OK"));
    {
        static const char* const rows[] = {"OK"};
        static const char* const icons[] = {ICON_CHECK};
        rows_show(app, rows, icons, 1, "OK");
    }
    shows(app, "Delete All: 3 done, 1 not", rgb(head_ink(app)));
    key(app, SDLK_RETURN);
    assert(!app->setup_on && app->lefts == 2);
    text = text_of(config, "setup.ini");
    assert(text && !strcmp(text, mine)); /* nothing else due: setup.ini as it was */
    free(text);
    /* the next start: the save alone; Delete All deletes none, said, nothing asked; Move All moves it */
    left_behind(app);
    setup_start(app);
    assert(app->setup_on && page(app) == SETUP_LEFT && setup_labels(app, labels) == 3 &&
           !strcmp(choice_label(app), "Not Now"));
    {
        static const char* const rows[] = {"Not Now", "", "Move All", "Delete All"};
        static const char* const icons[] = {ICON_X, "", ICON_FOLDER_INPUT, ICON_TRASH};
        rows_show(app, rows, icons, 4, "Not Now");
    }
    key(app, SDLK_UP); /* round to the last, Delete All */
    key(app, SDLK_RETURN);
    assert(!app->confirm.shown && app->setup_failed && app->setup_on &&
           !strcmp(app->message, "Delete All deletes none of these: a save and the console's keys are only moved"));
    key(app, SDLK_UP);
    key(app, SDLK_RETURN);
    assert(app->left_did == 1 && snx_is_file(moved) && !snx_path_exists(device) && app->moved.count == 1);
    render(app);
    shows(app, "Move All: 1 done, 0 not", rgb(head_ink(app)));
    key(app, SDLK_RETURN);
    /* the save in the data folder's save/ now, its files in the folder itself as the versions before kept one: listed
       again, on the saves' page, and the next Move All puts them in its 0/ (left_save) */
    assert(!app->setup_on && app->lefts == 2 && strstr(app->left, "save")); /* the device and the account save */
    left_behind(app);
    setup_start(app);
    assert(app->setup_on && page(app) == SETUP_LEFT);
    key(app, SDLK_END);
    for (int n = 0; n < 8 && strcmp(choice_label(app), "Move All"); ++n) key(app, SDLK_DOWN);
    key(app, SDLK_RETURN);
    assert(app->left_did == 1 && !snx_path_exists(moved));
    key(app, SDLK_RETURN);
    assert(!app->setup_on && !app->lefts);
    left_behind(app);
    setup_start(app);
    assert(!app->setup_on); /* nothing left, nothing due */
    /* Not Now keeps them; F5 and the 5 s look find them gone, and the setup goes on */
    for (int how = 0; how < 3; ++how) {
        put(settings, "[menu]\n");
        left_behind(app);
        setup_start(app);
        assert(app->setup_on && page(app) == SETUP_LEFT && !strcmp(choice_label(app), "Not Now"));
        if (!how) {
            key(app, SDLK_RETURN);
            assert(!app->setup_on && snx_is_file(settings) && app->lefts == 1);
            continue;
        }
        assert(!snx_remove_tree(gui));
        if (how == 1) {
            SDL_Event f5 = {0};
            f5.type = SDL_EVENT_KEY_DOWN;
            f5.key.key = SDLK_F5;
            f5.key.down = true;
            event(app, &f5);
        } else {
            app->left_at = 0;
            pump(app);
        }
        assert(!app->setup_on && !app->lefts);
    }
    /* after the notices: Skip goes through them with Not Now */
    put(settings, "[menu]\n");
    {
        char seen[64];
        snprintf(seen, sizeof seen, "[setup]\nversion = 1\nnotices = %d\n", SNX_NOTICES - 1);
        put(file, seen);
    }
    left_behind(app);
    setup_start(app);
    assert(app->setup_on && app->setup_count == 2 && app->setup_pages[1] == SETUP_LEFT);
    key(app, SDLK_DOWN); /* from OK to Skip */
    assert(!strcmp(choice_label(app), "Skip"));
    key(app, SDLK_RETURN);
    assert(!app->setup_on && snx_is_file(settings) && !app->left_did);
    assert(!snx_remove_tree(gui) && !snx_remove_tree(game));
    text = join(app->data_dir, "save"); /* where Move All put the saves */
    assert(!snx_remove_tree(text));
    free(text);
    left_behind(app);
    assert(!app->lefts);
    free(moved);
    free(device);
    free(saves);
    free(game);
    free(old_game);
    free(games);
    free(object);
    free(work);
    free(settings);
    free(gui);
    free(user);
    free(file);
    free(config);
    puts("PASS the old files a page each, Move All, Delete All asked twice keeping saves, Not Now");
}

/* the games' own saves, however many games, one Old Files page: how many games and a line each, rolling under the
   page's lines when they do not fit, how many more said; Delete All deletes none of them, Move All moves them all, its
   page then a line a game */
static void test_saves_one_page(App* app) {
    char *config = config_of(app), *file = join(config, "setup.ini"), path[SNX_PATH_SIZE];
    const char* labels[GUI_SETUP_CHOICES];
    bool more = false;
    int i, window_w, window_h;
    put(file, "[setup]\nversion = 1\nnotices = 99\n");
    assert(SDL_GetWindowSize(app->window, &window_w, &window_h));
    assert(SDL_SetWindowSize(app->window, 800, 1)); /* the least a window takes: its rows too few for 14 */
    SDL_SyncWindow(app->window);
    for (i = 0; i < 14; ++i) {
        assert(!snx_path_format(path, sizeof path, "%s/0100000000AB%X000/0100000000AB%X000_data/save/device/d.bin",
                                app->games_dir, i, i));
        put(path, "save");
    }
    left_behind(app);
    while (!__atomic_load_n(&app->left_space->done, __ATOMIC_ACQUIRE)) SDL_Delay(1);
    assert(app->lefts == 14 && left_groups(app) == 1);
    setup_start(app);
    assert(app->setup_on && page(app) == SETUP_LEFT && setup_labels(app, labels) == 3 &&
           !strcmp(choice_label(app), "Not Now"));
    render(app);
    shows(app, "Old Files 1/1", rgb(head_ink(app)));
    shows(app, "The games' own saves: 14 games", rgb(app->theme.ink));
    shows(app, "Move All    14 entries move", rgb(app->theme.ink));
    shows(app, "Delete All  Stays: a save,", rgb(app->theme.ink));
    shows(app, "0100000000AB0000  1 KB", rgb(app->theme.ink)); /* the first game, a line */
    for (i = 1; i < 14; ++i) {                                 /* the rest under it, said; they roll into view */
        char said[64];
        int x, y;
        snprintf(said, sizeof said, "and %d more, which Share Log lists", i);
        more = more || find_text(app, said, rgb(app->theme.soft), 1, &x, &y);
    }
    assert(more);
    shows(app, "\x1f", rgb(app->theme.soft));
    key(app, SDLK_UP); /* Delete All: none of them, said, nothing asked */
    key(app, SDLK_RETURN);
    assert(!app->confirm.shown && app->setup_failed &&
           !strcmp(app->message, "Delete All deletes none of these: a save and the console's keys are only moved"));
    key(app, SDLK_UP); /* Move All */
    key(app, SDLK_RETURN);
    assert(app->left_did == 1 && app->moved.count == 14 && app->moved.done == 14);
    render(app); /* a line a game, rolling: none cut */
    shows(app, "Move All: 14 done, 0 not", rgb(head_ink(app)));
    shows(app, "\x1f", rgb(app->theme.soft));
    for (i = 0; i < 14; ++i) {
        assert(!snx_path_format(path, sizeof path, "%s/save/device/0100000000AB%X000/d.bin", app->data_dir, i));
        assert(snx_is_file(path));
    }
    key(app, SDLK_RETURN);
    assert(!app->setup_on && app->lefts == 14); /* in the data folder's save/ now, for the next Move All (left_save) */
    for (i = 0; i < 14; ++i) {
        assert(!snx_path_format(path, sizeof path, "%s/0100000000AB%X000", app->games_dir, i));
        assert(!snx_remove_tree(path));
    }
    assert(!snx_path_format(path, sizeof path, "%s/save", app->data_dir) && !snx_remove_tree(path));
    left_behind(app);
    assert(SDL_SetWindowSize(app->window, window_w, window_h));
    SDL_SyncWindow(app->window);
    layout(app);
    free(file);
    free(config);
    puts("PASS the games' own saves one page, rolling, moved and never deleted");
}

/* in another language: the notices in its words (setup.c marks them), and the rows' labels padded to the columns their
   letters take, not their bytes: xx's "at\303\251" (3 columns in 4 bytes) and "agora" lined up */
static void test_translated(App* app) {
    static const char* const said[] = {"before", "now"};
    char *config = config_of(app), *file = join(config, "setup.ini"), pad[32];
    int x, y, before_x, now_x, row = cell_h(app);
    remove(file);
    app->language = 2;
    language_apply(app);
    assert(setup_cols(said, 2, 2) == 7 && !strcmp(setup_pad(pad, sizeof pad, "before", 7), "at\303\251    "));
    assert(!strcmp(setup_pad(pad, sizeof pad, "now", 7), "agora  ") &&
           !strcmp(setup_pad(pad, sizeof pad, "", 3), "   "));
    setup_start(app);
    assert(app->setup_on && page(app) == SETUP_NOTICE && !app->setup_top);
    render(app);
    shows(app, "Ajustes do jogo", rgb(app->theme.ink));
    assert(find_letters(app, "at\303\251    removed", rgb(app->theme.ink), 1, &before_x, &y));
    assert(find_letters(app, "agora  under", rgb(app->theme.ink), 1, &now_x, &x) && x == y + row);
    assert(before_x == now_x);
    app->language = 0;
    language_apply(app);
    setup_finish(app);
    free(file);
    free(config);
    puts("PASS the setup in another language");
}

/* the question at the foot with its choices above it at its right (yorha), or in the middle of the boxes (default),
   neither darkening the page */
static void test_places(App* app) {
    GuiSetupBoxes b;
    int width, height;
    char* file;
    file = config_of(app);
    {
        char* setup = join(file, "setup.ini");
        remove(setup);
        free(setup);
    }
    setup_start(app);
    app->setup_page = 2; /* Install: the Appearance's question goes under its rows (test_first) */
    setup_enter(app);
    render(app);
    setup_boxes(app, &b);
    area(app, &width, &height);
    assert(b.choices.box.y + b.choices.box.h == b.place.list.y + b.place.list.h && b.choices.box.w == b.place.list.w);
    assert(b.choices.at.y + b.choices.at.h <= b.choices.tab.y && b.choices.at.x > width / 2);
    assert(b.content.y + b.content.h <= b.choices.at.y && b.content.h > 0);
    assert(!strcmp(b.tab, "ScarletNX  2/5"));
    shows(app, "ScarletNX  2/5", rgb(app->theme.ink));
    { /* where the choices leave the page's boxes too few rows above them (a phone's, a finger's row each: here the
         largest letters, a phone held wide's 2340 pixels), the boxes go beside them, down to the question: a notice
         there to its "now" line, which the rows above the choices cut */
        int h, above = 0, list[SNX_NOTICES];
        char now[64], pad[32];
        const SnxNotice* notice;
        app->setup_page = 0;
        setup_enter(app);
        assert(page(app) == SETUP_NOTICE);
        app->menu.text_size = GUI_TEXT_MAX;
        for (h = 1080; h > 200; h -= 10) {
            assert(SDL_SetWindowSize(app->window, 2340, h));
            SDL_SyncWindow(app->window);
            render(app);
            setup_boxes(app, &b);
            above = b.choices.at.y - cell_h(app) / 2 - b.place.list.y;
            if (above < GUI_SETUP_CONTENT_ROWS * cell_h(app)) break;
        }
        assert(h > 200 && app->scale == GUI_TEXT_MAX);
        assert(b.content.x + b.content.w < b.choices.at.x && b.content.h > above &&
               b.content.y + b.content.h == b.choices.tab.y - cell_h(app) / 2);
        assert(setup_notices(app, list) > 0);
        notice = &snx_notices[list[0]];
        snprintf(now, sizeof now, "%s%.3s",
                 setup_pad(pad, sizeof pad, "now", setup_cols((const char* const[]){"before", "now"}, 2, 2)),
                 notice->now);
        shows(app, now, rgb(app->theme.ink));
        app->menu.text_size = 0;
        assert(SDL_SetWindowSize(app->window, width, height));
        SDL_SyncWindow(app->window);
        app->setup_page = 2;
        setup_enter(app);
        render(app);
        setup_boxes(app, &b);
    }
    app->theme = *gui_theme_builtin("default");
    render(app);
    setup_boxes(app, &b);
    assert(b.content.h == b.place.list.h && !b.choices.tab.h);
    assert(SDL_abs(b.choices.box.x + b.choices.box.w / 2 - (b.place.list.x + b.place.list.w / 2)) <= cell_w(app));
    assert(SDL_abs(b.choices.box.y + b.choices.box.h / 2 - (b.place.list.y + b.place.list.h / 2)) <= cell_h(app));
    shows(app, "Install Type", rgb(app->theme.warning));
    shows(app, "2/5", rgb(app->theme.soft));
    app->theme = *gui_theme_builtin("yorha");
    /* the foot: the tips beside the keys, halves; where half is under the keys' widest sentence, which a row cannot
       break (the narrowest window, 800 wide, at 4x: the foot's letters 3x), the keys that wide and the tips what is
       left */
    render(app);
    setup_boxes(app, &b);
    assert(b.place.tips.h && b.place.tips.y == b.place.keys.y && SDL_abs(b.place.tips.w - b.place.keys.w) <= 1);
    { /* a row of the foot's letters for the keys, two with [tips] wrap: the tips' rows */
        int fs = foot_scale(app);
        assert(b.place.keys.h == 16 * fs + 4 * fs);
        app->theme.tips_wrap = true;
        render(app);
        setup_boxes(app, &b);
        /* two, four pixels between them for the keys' buttons (hints_h) */
        assert(b.place.keys.h == 2 * 16 * fs + 4 * fs + 4 * fs && b.place.tips.h == b.place.keys.h);
        app->theme.tips_wrap = false;
    }
    assert(SDL_SetWindowSize(app->window, 800, 1024));
    SDL_SyncWindow(app->window);
    app->menu.text_size = GUI_TEXT_MAX;
    render(app);
    setup_boxes(app, &b);
    assert(app->scale == 4);
    {
        int least = frame_keys_least(app);
        assert(least > (b.place.tips.w + b.place.keys.w) / 2 && b.place.keys.w == least && b.place.tips.w > 0 &&
               b.place.tips.w + cell_w(app) + least == b.place.keys.x + b.place.keys.w - b.place.tips.x);
    }
    app->menu.text_size = 0;
    assert(SDL_SetWindowSize(app->window, width, height));
    SDL_SyncWindow(app->window);
    setup_finish(app);
    free(file);
    puts("PASS the question where options-dialog puts it, the foot's keys their widest sentence wide");
}

/* Skip, on the first page alone, after its choices: every page left taken as its defaults have it (the notices read
   to the last, Next, the install kept, Keep, Later, the nickname Player), setup.ini written as done; nothing else
   written. From the Appearance as the first page too, by its keys and the pointer; a page whose default fails stays */
static void test_skip(App* app) {
    char *config = config_of(app), *file = join(config, "setup.ini"), *users = join(app->users_dir, "users.ini");
    char *app_ini = text_of(config, "app.ini"), *games = snx_strdup(app->games_dir), seen[64], *now;
    const char* labels[GUI_SETUP_CHOICES];
    int n, i, list[SNX_NOTICES];
    snprintf(seen, sizeof seen, "[setup]\nversion = 1\nnotices = %d\n", SNX_NOTICES);
    remove(file);
    remove(users);
    setup_start(app);
    assert(app->setup_on && !app->setup_page && page(app) == SETUP_NOTICE && app->setup_choice.count == 1);
    n = setup_labels(app, labels);
    assert(n == 2 && !strcmp(labels[0], "Next") && !strcmp(labels[1], "Skip"));
    render(app);
    shows(app, "\xfe " ICON_SKIP_FORWARD " Skip", rgb(app->theme.ink)); /* a notice's choice: with its icon */
    /* past the first page: none */
    for (i = setup_notices(app, list); i > 0; --i) key(app, SDLK_RETURN);
    assert(page(app) == SNX_SETUP_LANGUAGE && app->setup_page == 1 && setup_labels(app, labels) == 1);
    render(app);
    hides(app, "Skip", rgb(app->theme.ink));
    key(app, SDLK_UP); /* the Appearance's choices from its rows' top: Next alone */
    assert(app->setup_row == GUI_APPEARANCE_NEXT && !app->setup_choice.focus);
    key(app, SDLK_RETURN);
    assert(page(app) == SNX_SETUP_INSTALL && setup_labels(app, labels) == 2 && strcmp(labels[1], "Skip"));
    key(app, SDLK_ESCAPE);
    key(app, SDLK_ESCAPE);
    assert(page(app) == SETUP_NOTICE && !app->setup_page && !app->setup_top);
    key(app, SDLK_UP); /* round from Next to Skip */
    assert(!strcmp(choice_label(app), "Skip"));
    render(app);
    shows(app, "\xfe " ICON_SKIP_FORWARD " Skip", rgb(app->theme.page)); /* in focus, on YoRHa's band */
    keyboards = 0;
    key(app, SDLK_RETURN);
    pump(app);
    assert(!app->setup_on && !app->setup_failed && !app->confirm.shown && !app->message[0]);
    assert(!keyboards && !SDL_TextInputActive(app->window)); /* the nickname typed and taken: no keyboard asked for */
    holds(config, "setup.ini", seen);
    holds(app->users_dir, "users.ini", "nickname=Player\n"); /* the nickname's page took its Player */
    now = text_of(config, "app.ini");
    assert(app_ini && now && !strcmp(app_ini, now) && same_path(app->games_dir, games)); /* nothing else */
    free(now);
    setup_start(app);
    assert(!app->setup_on);
    /* the Appearance first: Next, then Skip, under its rows; Up from the rows' top to Skip, Down round to Language;
       the pointer on Skip takes it, users.ini kept */
    put(file, "[setup]\nnotices = 99\n");
    now = text_of(app->users_dir, "users.ini");
    setup_start(app);
    assert(app->setup_on && !app->setup_page && page(app) == SNX_SETUP_LANGUAGE && setup_labels(app, labels) == 2);
    key(app, SDLK_UP);
    assert(app->setup_row == GUI_APPEARANCE_NEXT && !strcmp(choice_label(app), "Skip") && setup_focus(app) == 1);
    key(app, SDLK_UP);
    assert(app->setup_row == GUI_APPEARANCE_NEXT && !strcmp(choice_label(app), "Next"));
    key(app, SDLK_RIGHT);
    assert(!strcmp(choice_label(app), "Skip"));
    key(app, SDLK_DOWN);
    assert(app->setup_row == GUI_APPEARANCE_LANGUAGE && setup_focus(app) < 0);
    {
        GuiSetupBoxes b;
        render(app);
        setup_boxes(app, &b);
        point(app, choice_box(app, &b.choices, 1), true);
    }
    assert(!app->setup_on && !app->setup_failed);
    holds(config, "setup.ini", "version = 1\nnotices = 99\n"); /* a newer file's count kept */
    {
        char* kept = text_of(app->users_dir, "users.ini");
        assert(kept && now && !strcmp(kept, now));
        free(kept);
    }
    free(now);
    /* a default that fails: users.ini cannot be written, the user's page stays, saying why. Skip from the Appearance's
       Enter */
    {
        char *was = app->users_dir, *blocker = join(root, "a file");
        put(blocker, "x");
        app->users_dir = join(blocker, "users");
        put(file, "[setup]\nnotices = 99\n");
        setup_start(app);
        key(app, SDLK_UP);
        assert(page(app) == SNX_SETUP_LANGUAGE && !strcmp(choice_label(app), "Skip"));
        keyboards = 0;
        key(app, SDLK_RETURN);
        assert(app->setup_on && page(app) == SNX_SETUP_USER && app->setup_failed && app->setup_typing && !keyboards);
        pump(app); /* the page stays on its nickname, whose keyboard comes up with the next frame */
        assert(keyboards == 1 && SDL_TextInputActive(app->window));
        assert(strstr(app->message, "No user written: ") && app->confirm.shown && app->confirm.ask == GUI_ASK_ERROR);
        key(app, SDLK_RETURN);
        free(app->users_dir);
        app->users_dir = was;
        setup_finish(app);
        remove(blocker);
        free(blocker);
    }
    free(games);
    free(app_ini);
    free(users);
    free(file);
    free(config);
    puts("PASS Skip on the first page, every page's defaults taken, and where a default fails");
}

/* a start whose app.ini has no UI scale (an older version's, or a hand's): the number a start takes in its window,
   written there at once when the setup has nothing to ask, with nothing to say */
static void test_start_scale(App* app) {
    size_t size = 0;
    char *was = (char*)snx_read_file(app->app_file, &size), *now, line[64];
    assert(was && strstr(was, "ui_scale = "));
    put(app->app_file, "[app]\ntheme = yorha\n");
    app->menu.text_size = 0;
    layout(app); /* as a start measures its window */
    assert(app->menu.text_size == app->auto_scale && app->auto_scale == 2);
    app->message[0] = 0;
    app_load(app);
    now = (char*)snx_read_file(app->app_file, &size);
    assert(app->menu.text_size == 2 && app->scale_new && !app->message[0] && now && !strstr(now, "ui_scale"));
    free(now);
    setup_start(app); /* setup.ini done: no page to ask */
    assert(!app->setup_on && !app->scale_new && !app->message[0]);
    snprintf(line, sizeof line, "theme = yorha\nui_scale = %d\n", app->auto_scale);
    now = (char*)snx_read_file(app->app_file, &size);
    assert(now && strstr(now, line));
    free(now);
    put(app->app_file, was);
    free(was);
    puts("PASS a start without a UI scale in app.ini writes the one it takes");
}

/* name set (NULL: unset) where getenv reads it as well as SDL: on Windows the C runtime keeps a copy of its own, which
   SDL_setenv_unsafe leaves naming the real user's folders */
static void set_env(const char* name, const char* value) {
#ifdef _WIN32
    assert(!_putenv_s(name, value ? value : ""));
#endif
    assert(!(value ? SDL_setenv_unsafe(name, value, 1) : SDL_unsetenv_unsafe(name)));
}

int main(void) {
    App app;
    root = snx_temp_dir(NULL);
    assert(root);
    /* a program as deep as one in a Windows user's Downloads: what the foot says has to come before the path, which
       it cuts */
    bin = join(root, "Downloads/ScarletNX-windows-x86_64-0123456789abcdef/a copy kept here/bin");
    home = join(root, "home");
    assert(!snx_mkdirs(bin) && !snx_mkdirs(home));
    /* the user's folders in that home: XDG's under HOME, Windows' in APPDATA and the keys in USERPROFILE */
    set_env("HOME", home);
    set_env("APPDATA", home);
    set_env("USERPROFILE", home);
    set_env("XDG_DATA_HOME", NULL);
    set_env("XDG_CONFIG_HOME", NULL);
    set_env("XDG_STATE_HOME", NULL);
    set_env("XDG_CACHE_HOME", NULL);
    set_env("APPIMAGE", NULL);
    /* the keys the setup installs go there, never into the real user's .switch */
    assert(getenv("USERPROFILE") && !strcmp(getenv("USERPROFILE"), home));
    {
        char* self = join(bin, "scarletnx");
        assert(initialize(&app, self));
        free(self);
    }
    assert(!strcmp(app.theme.id, "yorha") && app.theme.dialog == GUI_DIALOG_YORHA);
    app.animations_off = true; /* each page read as it settles, not as it fades in */
    app.clock = 1; /* a text too long for its row read from its start, whatever time the test takes to get there */
    open_files(&app);
    test_first(&app);
    test_users(&app);
    test_notice(&app);
    test_clean_notice(&app);
    test_old_files(&app);
    test_saves_one_page(&app);
    test_translated(&app);
    test_places(&app);
    test_skip(&app);
    test_start_scale(&app);
    cleanup(&app);
    assert(!snx_remove_tree(root));
    free(home);
    free(bin);
    free(root);
    puts("GUI setup: every page, what each writes, the notices and the question's place passed");
    return 0;
}
