/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The launcher's setup (gui/main.c's setup_*) on the real renderer and its events, in a home of its own: the pages a
   first start goes through and what each writes (a portable copy, the language, the games and dumps folders, the keys,
   the first user), what it refuses, the user's page with users.ini there, the fixed uid kept for its saves, a notice
   alone after an update, --debug, the question's place as options-dialog says, and Skip */
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
    {
        char line[64];
        snprintf(line, sizeof line, "  Auto (%dx)  ...", app->auto_scale);
        shows(app, line, rgb(app->theme.ink));
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
        point(app, appearance_row_box(app, &a, GUI_APPEARANCE_TEXT), true); /* UI scale clicked: its box */
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
    key(app, SDLK_RETURN); /* its box, over the page */
    assert(app->sizes_shown && app->sizes.focus == 3 && page(app) == SNX_SETUP_LANGUAGE);
    render(app);
    shows(app, "0.5 of Auto", rgb(app->theme.ink));
    key(app, SDLK_DOWN);
    key(app, SDLK_RETURN);
    assert(!app->sizes_shown && app->menu.text_size == 1 && app->confirm.shown && app->confirm.ask == GUI_ASK_TEXT);
    render(app);
    key(app, SDLK_RIGHT); /* Keep */
    key(app, SDLK_RETURN);
    assert(!app->confirm.shown && app->menu.text_size == 1 && app->setup_on && page(app) == SNX_SETUP_LANGUAGE);
    path = join(home, ".config/scarletnx/app.ini");
    assert(!snx_is_file(path)); /* kept for the setup's end, in the folders Install picks */
    free(path);
    key(app, SDLK_LEFT); /* UI scale's box again, closed by ESC */
    assert(app->sizes_shown);
    key(app, SDLK_ESCAPE);
    assert(!app->sizes_shown && app->setup_on && page(app) == SNX_SETUP_LANGUAGE);
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
    key(app, SDLK_RETURN);
    assert(app->setup_on && app->setup_failed && strstr(app->message, "Ada is not a nickname"));
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
        assert(b.place.keys.h == 2 * 16 * fs + 4 * fs && b.place.tips.h == b.place.keys.h);
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
    shows(app, "\xfe Skip", rgb(app->theme.ink));
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
    shows(app, "\xfe Skip", rgb(app->theme.page)); /* in focus, on YoRHa's band */
    key(app, SDLK_RETURN);
    assert(!app->setup_on && !app->setup_failed && !app->confirm.shown && !app->message[0]);
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
        key(app, SDLK_RETURN);
        assert(app->setup_on && page(app) == SNX_SETUP_USER && app->setup_failed && app->setup_typing);
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
    test_translated(&app);
    test_places(&app);
    test_skip(&app);
    cleanup(&app);
    assert(!snx_remove_tree(root));
    free(home);
    free(bin);
    free(root);
    puts("GUI setup: every page, what each writes, the notices and the question's place passed");
    return 0;
}
