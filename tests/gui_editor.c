/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The theme editor's rows and keys (src/gui/editor.c): what each key does to the theme, typing a value, leaving over
   changes, and Save writing a theme.ini that reads back to the theme edited */
#include "common/utilities/common.h"
#include "gui/editor.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GuiEditor e;
/* the row of section/key */
static int row_of(const char* section, const char* key) {
    int r;
    for (r = 0; r < gui_editor_rows(&e); ++r) {
        const GuiThemeKey* k = gui_editor_key_of(&e, r);
        if (k && !strcmp(k->section, section) && !strcmp(k->key, key)) return r;
    }
    assert(!"no such row");
    return -1;
}
static const char* value_of(int row) {
    static char v[96];
    gui_editor_value(&e, row, v, sizeof v);
    return v;
}
static void type(const char* text) {
    while (e.text[0]) gui_editor_erase(&e);
    gui_editor_type(&e, text);
}

int main(void) {
    const GuiTheme* def = gui_theme_builtin("default");
    int keys = 0, r;
    char label[64], *dir, *path, *text;
    size_t size;
    GuiTheme back;
    while (gui_theme_keys[keys].section) ++keys;

    /* Save first, a row for the folder, one per key */
    gui_editor_start(&e, def, NULL, "mytheme");
    assert(gui_editor_rows(&e) == keys + 2 && !gui_editor_key_of(&e, GUI_EDITOR_SAVE_ROW) &&
           !gui_editor_key_of(&e, GUI_EDITOR_FOLDER));
    assert(gui_editor_key_of(&e, 2) == &gui_theme_keys[0] &&
           gui_editor_key_of(&e, keys + 1) == &gui_theme_keys[keys - 1]);
    gui_editor_label(&e, row_of("colors", "page"), label, sizeof label);
    assert(!strncmp(label, "colors", 6) && strstr(label, "page"));
    gui_editor_label(&e, row_of("colors", "ink"), label, sizeof label);
    assert(label[0] == ' ' && strstr(label, "ink"));
    gui_editor_label(&e, GUI_EDITOR_SAVE_ROW, label, sizeof label);
    assert(!strcmp(label, "Save"));
    gui_editor_label(&e, GUI_EDITOR_FOLDER, label, sizeof label);
    assert(!strcmp(label, "Folder"));

    /* a built-in is edited as a new theme that starts from it */
    gui_editor_start(&e, def, NULL, "mytheme");
    assert(!strcmp(e.id, "mytheme") && !strcmp(e.file.base, "default") && e.row == GUI_EDITOR_SAVE_ROW && !e.dirty);
    assert(!strcmp(value_of(GUI_EDITOR_SAVE_ROW), "themes/mytheme/theme.ini") &&
           !strcmp(value_of(GUI_EDITOR_FOLDER), "mytheme"));

    /* moving: the ends hold, the pages go ten */
    assert(gui_editor_key(&e, GUI_UP) == GUI_EDITOR_STAY && e.row == 0);
    gui_editor_key(&e, GUI_PAGE_DOWN);
    assert(e.row == 10);
    for (r = 0; r < 20; ++r) gui_editor_key(&e, GUI_PAGE_DOWN);
    assert(e.row == keys + 1);
    gui_editor_key(&e, GUI_PAGE_UP);
    assert(e.row == keys + 1 - 10 && !e.dirty);

    /* words go round, flags turn, numbers step and stay in their range */
    e.row = row_of("head", "style");
    gui_editor_key(&e, GUI_RIGHT);
    assert(e.theme.head == GUI_STYLE_BAR && e.dirty);
    gui_editor_key(&e, GUI_RIGHT);
    assert(e.theme.head == GUI_STYLE_PLAIN);
    gui_editor_key(&e, GUI_LEFT);
    assert(e.theme.head == GUI_STYLE_BAR);
    gui_editor_key(&e, GUI_SELECT); /* Select changes a word too, without typing */
    assert(e.theme.head == GUI_STYLE_PLAIN && !e.typing);
    e.row = row_of("head", "rule");
    gui_editor_key(&e, GUI_SELECT);
    assert(e.theme.head_rule == !def->head_rule);
    e.row = row_of("transitions", "blink_ms");
    e.theme.blink_ms = 5;
    gui_editor_key(&e, GUI_LEFT);
    assert(e.theme.blink_ms == 0);
    gui_editor_key(&e, GUI_RIGHT);
    gui_editor_key(&e, GUI_RIGHT);
    assert(e.theme.blink_ms == 20);
    e.theme.blink_ms = 9995;
    gui_editor_key(&e, GUI_RIGHT);
    assert(e.theme.blink_ms == 10000);
    e.row = row_of("splash", "chance");
    e.theme.splash_chance = 100;
    gui_editor_key(&e, GUI_RIGHT);
    assert(e.theme.splash_chance == 100);
    gui_editor_key(&e, GUI_LEFT);
    assert(e.theme.splash_chance == 99);
    /* a colour is picked: Left and Right only say so */
    e.row = row_of("colors", "page");
    back = e.theme;
    gui_editor_key(&e, GUI_RIGHT);
    assert(!memcmp(&back.page, &e.theme.page, sizeof back.page) && strstr(e.note, "#RRGGBB"));
    assert(gui_editor_colour(&e) == &e.theme.page);

    /* the picker: Select opens it on red; Left/Right step a channel by 1, the pages by 16, held in 0 to 255; Up/Down go
       between its rows, which hold at their ends; Cancel puts the colour back, and the editor's changes as they were */
    {
        GuiRgb was = {10, 250, 128};
        int dirty = e.dirty, r = row_of("colors", "page");
        e.theme.page = was;
        gui_editor_key(&e, GUI_SELECT);
        assert(e.picking && e.channel == GUI_PICK_RED && !e.typing && e.row == r);
        gui_editor_key(&e, GUI_RIGHT);
        assert(e.theme.page.r == 11 && e.dirty);
        gui_editor_key(&e, GUI_LEFT);
        gui_editor_key(&e, GUI_PAGE_DOWN);
        assert(e.theme.page.r == 0);
        gui_editor_key(&e, GUI_UP);
        assert(e.channel == GUI_PICK_RED);
        gui_editor_key(&e, GUI_DOWN);
        gui_editor_key(&e, GUI_PAGE_UP);
        assert(e.channel == GUI_PICK_GREEN && e.theme.page.g == 255 && e.row == r);
        gui_editor_key(&e, GUI_DOWN);
        gui_editor_key(&e, GUI_LEFT);
        assert(e.channel == GUI_PICK_BLUE && e.theme.page.b == 127);
        gui_editor_key(&e, GUI_DOWN);
        gui_editor_key(&e, GUI_DOWN);
        assert(e.channel == GUI_PICK_HEX);
        gui_editor_key(&e, GUI_RIGHT); /* the typed row says how */
        assert(e.theme.page.b == 127 && strstr(e.note, "#RRGGBB"));
        /* a click or a drag: a channel set, held in range, its row focused */
        gui_editor_pick(&e, GUI_PICK_GREEN, 300);
        assert(e.channel == GUI_PICK_GREEN && e.theme.page.g == 255);
        gui_editor_pick(&e, GUI_PICK_RED, -5);
        gui_editor_pick(&e, 7, 40); /* no such channel */
        assert(e.channel == GUI_PICK_RED && e.theme.page.r == 0);
        e.dirty = dirty;
        gui_editor_pick(&e, GUI_PICK_RED, 0); /* the same value changes nothing */
        assert(e.dirty == dirty);
        gui_editor_key(&e, GUI_CANCEL);
        assert(!e.picking && !memcmp(&e.theme.page, &was, sizeof was) && e.dirty == dirty && e.row == r);
        assert(gui_editor_key(&e, GUI_DOWN) == GUI_EDITOR_STAY && e.row == r + 1); /* the rows move again */
        e.row = r;
        /* Select keeps it, on the editor's row */
        gui_editor_key(&e, GUI_SELECT);
        gui_editor_key(&e, GUI_RIGHT);
        gui_editor_key(&e, GUI_SELECT);
        assert(!e.picking && e.theme.page.r == 11 && e.dirty);
        gui_editor_pick(&e, GUI_PICK_RED, 99); /* closed, it changes nothing */
        assert(e.theme.page.r == 11);
        e.theme.page = was;
    }

    /* typing, the picker's last row: it starts from the value, what the row does not take keeps it typing, Cancel
       drops it and the picker stays */
    gui_editor_key(&e, GUI_SELECT);
    for (r = 0; r < 3; ++r) gui_editor_key(&e, GUI_DOWN);
    gui_editor_key(&e, GUI_SELECT);
    assert(e.picking && e.typing && !strcmp(e.text, value_of(e.row)));
    gui_editor_key(&e, GUI_CANCEL);
    assert(e.picking && !e.typing && e.channel == GUI_PICK_HEX);
    gui_editor_key(&e, GUI_SELECT);
    gui_editor_erase(&e);
    assert(strlen(e.text) == 6);
    type("#12");
    gui_editor_key(&e, GUI_SELECT);
    assert(e.typing && strstr(e.note, "#RRGGBB"));
    assert(gui_editor_key(&e, GUI_UP) == GUI_EDITOR_STAY && e.typing); /* the keys type, they do not move */
    type("#1a2B3c");
    gui_editor_key(&e, GUI_SELECT);
    assert(!e.typing && e.picking && e.theme.page.r == 0x1A && e.theme.page.g == 0x2B && e.theme.page.b == 0x3C);
    gui_editor_key(&e, GUI_UP);
    gui_editor_key(&e, GUI_SELECT);
    assert(!e.picking && !strcmp(value_of(e.row), "#1A2B3C"));
    e.row = row_of("body", "marker");
    assert(!gui_editor_colour(&e)); /* a text holds none */
    gui_editor_key(&e, GUI_SELECT);
    assert(e.typing && !e.picking);
    gui_editor_key(&e, GUI_CANCEL);
    e.row = row_of("body", "marker");
    gui_editor_key(&e, GUI_SELECT);
    type("12345678901234567");
    gui_editor_key(&e, GUI_SELECT);
    assert(e.typing && strstr(e.note, "15 bytes"));
    type(" > ");
    gui_editor_key(&e, GUI_SELECT);
    assert(!e.typing && !strcmp(e.theme.marker, " > "));
    e.row = row_of("head", "menu");
    gui_editor_key(&e, GUI_SELECT);
    type("NIGHT");
    gui_editor_key(&e, GUI_CANCEL);
    assert(!e.typing && strcmp(e.theme.title[GUI_SCREEN_MENU], "NIGHT"));
    /* text as typed: added up to the room, a line's end refused, a UTF-8 character erased whole */
    gui_editor_key(&e, GUI_SELECT);
    type("ab\xC3\xA9");
    gui_editor_type(&e, "x\ny");
    assert(!strcmp(e.text, "ab\xC3\xA9"));
    gui_editor_erase(&e);
    assert(!strcmp(e.text, "ab"));
    for (r = 0; r < 100; ++r) gui_editor_type(&e, "z");
    assert(strlen(e.text) == sizeof e.text - 1);
    /* a character the room has not all of stays out whole: two bytes left take the a with a tilde, not U+83DC's three
       (an input method's Chinese), and of "ab" and U+83DC the room keeps "ab" */
    type("");
    for (r = 0; r < (int)sizeof e.text - 3; ++r) gui_editor_type(&e, "z");
    gui_editor_type(&e, "\xe8\x8f\x9c");
    assert(strlen(e.text) == sizeof e.text - 3);
    gui_editor_type(&e, "\xc3\xa3");
    assert(strlen(e.text) == sizeof e.text - 1 && !strcmp(e.text + sizeof e.text - 3, "\xc3\xa3"));
    type("");
    for (r = 0; r < (int)sizeof e.text - 4; ++r) gui_editor_type(&e, "z");
    gui_editor_type(&e, "ab\xe8\x8f\x9c");
    assert(strlen(e.text) == sizeof e.text - 2 && !strcmp(e.text + sizeof e.text - 4, "ab"));
    gui_editor_key(&e, GUI_CANCEL);
    /* the folder: a name every system takes */
    e.row = GUI_EDITOR_FOLDER;
    gui_editor_key(&e, GUI_SELECT);
    type(".hidden");
    gui_editor_key(&e, GUI_SELECT);
    assert(e.typing && strstr(e.note, "folder"));
    type("a/b");
    gui_editor_key(&e, GUI_SELECT);
    assert(e.typing);
    type("Night ink_2");
    gui_editor_key(&e, GUI_SELECT);
    assert(!e.typing && !strcmp(e.id, "Night ink_2"));

    /* leaving over changes asks once; a key in between asks again */
    assert(gui_editor_key(&e, GUI_CANCEL) == GUI_EDITOR_STAY && e.leaving && strstr(e.note, "ESC again"));
    gui_editor_key(&e, GUI_DOWN);
    assert(!e.leaving && gui_editor_key(&e, GUI_CANCEL) == GUI_EDITOR_STAY);
    assert(gui_editor_key(&e, GUI_CANCEL) == GUI_EDITOR_LEAVE);

    /* Save: the row says so, and the file reads back to the theme edited */
    e.row = GUI_EDITOR_SAVE_ROW;
    assert(gui_editor_key(&e, GUI_SELECT) == GUI_EDITOR_SAVE);
    dir = snx_temp_dir(NULL);
    assert(dir && !gui_editor_save(&e, dir) && !e.dirty && strstr(e.note, "Saved in"));
    path = snx_format("%s/Night ink_2/theme.ini", dir);
    text = (char*)snx_read_file(path, &size);
    assert(text && gui_theme_parse(&back, "Night ink_2", text, size));
    for (r = 2; r <= keys + 1; ++r) {
        char a[96], b[96];
        gui_theme_get(&e.theme, gui_editor_key_of(&e, r), a, sizeof a);
        gui_theme_get(&back, gui_editor_key_of(&e, r), b, sizeof b);
        assert(!strcmp(a, b));
    }
    assert(strstr(text, "base = default"));
    free(text);
    free(path);
    assert(gui_editor_key(&e, GUI_CANCEL) == GUI_EDITOR_LEAVE); /* nothing left to save: no question */
    assert(gui_editor_save(&e, NULL) && strstr(e.note, "Not saved"));
    snx_remove_tree(dir);
    free(dir);

    /* the rows shown follow the focus */
    e.top = 0;
    e.row = 30;
    gui_editor_scroll(&e, 10);
    assert(e.top == 21);
    e.row = 3;
    gui_editor_scroll(&e, 10);
    assert(e.top == 3);
    e.row = keys + 1;
    gui_editor_scroll(&e, 1000);
    assert(e.top == 0);
    gui_editor_stop(&e);
    /* a form over another object: a row per key, the first of a section named, no folder or Save; a change is kept
       as it is made, so ESC leaves at once */
    {
        static const char* const words[] = {".ini", "0", "1", NULL};
        typedef struct {
            int word;
            char text[8];
        } Values;
        Values values = {0, ""};
        const GuiThemeKey form[] = {
            {"Display", "fullscreen", GUI_KEY_WORD, offsetof(Values, word), sizeof(int), words, 0, "1 = fullscreen"},
            {"Display", "language", GUI_KEY_TEXT, offsetof(Values, text), 8, NULL, 0, "0 Japanese, 1 English"}};
        gui_editor_form(&e, form, 2, &values, NULL);
        assert(gui_editor_rows(&e) == 2 && gui_editor_key_of(&e, 0) == &form[0] && !gui_editor_key_of(&e, 2));
        gui_editor_label(&e, 0, label, sizeof label);
        assert(!strncmp(label, "Display", 7) && strstr(label, "fullscreen"));
        gui_editor_label(&e, 1, label, sizeof label);
        assert(label[0] == ' ' && strstr(label, "language"));
        gui_editor_key(&e, GUI_RIGHT);
        gui_editor_key(&e, GUI_RIGHT);
        assert(values.word == 2 && !strcmp(value_of(0), "1"));
        gui_editor_key(&e, GUI_DOWN);
        gui_editor_key(&e, GUI_SELECT);
        type("12");
        gui_editor_key(&e, GUI_SELECT);
        assert(!strcmp(values.text, "12") && e.dirty && gui_editor_key(&e, GUI_CANCEL) == GUI_EDITOR_LEAVE);
        gui_editor_key(&e, GUI_DOWN);
        assert(e.row == 1 && gui_editor_key(&e, GUI_SELECT) == GUI_EDITOR_STAY && e.typing); /* no Save to reach */
        gui_editor_stop(&e);
        /* with an action: a last row of that label, which Select asks the screen to do */
        gui_editor_form(&e, form, 2, &values, "Reset all");
        assert(gui_editor_rows(&e) == 3 && !gui_editor_key_of(&e, 2));
        gui_editor_label(&e, 2, label, sizeof label);
        assert(!strcmp(label, "Reset all") && !value_of(2)[0]);
        gui_editor_key(&e, GUI_PAGE_DOWN);
        assert(e.row == 2 && gui_editor_key(&e, GUI_SELECT) == GUI_EDITOR_SAVE && !e.typing);
        gui_editor_stop(&e);
    }
    /* action rows: the screen's to do on Select, Left or Right (step says which), their value a text of the object's,
       and a flat form's labels without the section */
    {
        struct {
            char shown[16];
        } object = {"Default"};
        const GuiThemeKey rows[] = {{"", "Theme", GUI_KEY_ACTION, 0, sizeof object.shown, NULL, 0, "the theme"},
                                    {"", "Theme editor", GUI_KEY_ACTION, 0, 0, NULL, 0, "the editor"}};
        GuiEditor e;
        char label[64], value[32];
        gui_editor_form(&e, rows, 2, &object, NULL);
        e.flat = true;
        gui_editor_label(&e, 0, label, sizeof label);
        gui_editor_value(&e, 0, value, sizeof value);
        assert(!strcmp(label, "Theme") && !strcmp(value, "Default"));
        gui_editor_value(&e, 1, value, sizeof value);
        assert(!value[0]);
        assert(gui_editor_key(&e, GUI_SELECT) == GUI_EDITOR_ACTION && e.step == 0 && !e.typing && !e.dirty);
        assert(gui_editor_key(&e, GUI_LEFT) == GUI_EDITOR_ACTION && e.step == -1);
        assert(gui_editor_key(&e, GUI_RIGHT) == GUI_EDITOR_ACTION && e.step == 1 && !strcmp(object.shown, "Default"));
        gui_editor_key(&e, GUI_DOWN);
        assert(e.row == 1 && gui_editor_key(&e, GUI_SELECT) == GUI_EDITOR_ACTION && e.step == 0);
        assert(!gui_theme_set(&object, &rows[0], "x") && !strcmp(object.shown, "Default")); /* nothing to set */
        gui_editor_stop(&e);
    }

    puts("gui_editor passed");
    return 0;
}
