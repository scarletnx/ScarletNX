/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A form, a screen of rows the menu opens: the theme editor, Save, a row for the folder the theme is saved in and a row
   per key of theme.ini (gui_theme_keys); or another table of keys over an object of its own, as the Settings. main.c
   draws it, the theme editor with a preview of the menu or a run in the theme as it is; this holds what it edits and
   what each key does to it, without SDL. */
#ifndef SNX_GUI_EDITOR_H
#define SNX_GUI_EDITOR_H
#include <stdbool.h>
#include <stddef.h>
#include "gui/model.h"
#include "gui/theme.h"

typedef struct {
    const GuiThemeKey* keys; /* a row each */
    int count;
    char* object;       /* what the keys' offsets are in; NULL: the theme, its rows after Save's and the folder's */
    const char* action; /* a form's last row, when not NULL: its label */
    /* the theme editor's: the theme, the extras of the file it started from (written back with it), its folder */
    GuiTheme theme;
    GuiThemeFile file;
    char id[64];
    int row, top; /* the focused row, and the first one shown */
    bool typing;  /* a value being typed for the focused row, in text */
    char text[512];      /* a path's room, as a text setting's (settings.h's GUI_SETTING_TEXT) */
    bool dirty, leaving; /* changed since it was saved; ESC pressed once over changes */
    int preview;         /* GUI_SCREEN_MENU or GUI_SCREEN_COMPILING */
    char note[1024];     /* what the last key did, when it did not do what it was asked: Folders' F5 names each of
                            its five folders not there, with its path */
    bool flat;           /* no section column: the form's rows are all of one */
    int step;            /* the last GUI_EDITOR_ACTION's key: 0 Select, -1 Left, 1 Right */
    bool picking;        /* the colour picker over the focused colour row, which Select opened */
    int channel;         /* the picker's focused row, GUI_PICK_* */
    GuiRgb was;          /* the colour as the picker opened, which Cancel puts back */
    bool was_dirty;      /* and whether there were changes then */
} GuiEditor;
/* the colour picker's rows: a channel each, then the colour typed as #RRGGBB */
enum { GUI_PICK_RED, GUI_PICK_GREEN, GUI_PICK_BLUE, GUI_PICK_HEX, GUI_PICK_ROWS };
#define GUI_EDITOR_SAVE_ROW 0 /* the theme editor's first row */
#define GUI_EDITOR_FOLDER   1
int gui_editor_rows(const GuiEditor* editor);
/* the key of a row, NULL for the folder and Save, or a form's action */
const GuiThemeKey* gui_editor_key_of(const GuiEditor* editor, int row);
/* edits theme, its file's extras taken over (file may be NULL), saved as themes/id */
void gui_editor_start(GuiEditor* editor, const GuiTheme* theme, GuiThemeFile* file, const char* id);
/* edits object through count keys: no folder, no Save, its changes kept as they are made; action, when not NULL, a last
   row that Select on returns GUI_EDITOR_SAVE for */
void gui_editor_form(GuiEditor* editor, const GuiThemeKey* keys, int count, void* object, const char* action);
void gui_editor_stop(GuiEditor* editor);
/* what a key asks of the screen: nothing more, to leave it, to save (a form: to do its action), or the focused
   GUI_KEY_ACTION row's (step says which key) */
typedef enum { GUI_EDITOR_STAY, GUI_EDITOR_LEAVE, GUI_EDITOR_SAVE, GUI_EDITOR_ACTION } GuiEditorDo;
/* Up/Down and the pages move; Left/Right change a word, a flag or a number; Select changes a word or a flag, opens the
   colour picker on a colour, starts typing the others and takes what was typed; Cancel stops typing, or leaves (asking
   once over changes). On an action row Select, Left and Right are the caller's. In the picker Up/Down go between its
   rows, Left/Right change a channel by 1 and the pages by 16, Select keeps the colour (on #RRGGBB, types it) and Cancel
   puts it back */
GuiEditorDo gui_editor_key(GuiEditor* editor, GuiKey key);
/* the focused row's colour, NULL on a row that holds none */
GuiRgb* gui_editor_colour(GuiEditor* editor);
/* the picker's channel set to value, held in 0 to 255, its row focused: what a click or a drag on its bar does */
void gui_editor_pick(GuiEditor* editor, int channel, int value);
/* the picker closed, the colour kept or put back as it opened */
void gui_editor_pick_end(GuiEditor* editor, bool keep);
/* while typing: text added, or the last character taken off */
void gui_editor_type(GuiEditor* editor, const char* text);
void gui_editor_erase(GuiEditor* editor);
/* what the focused row takes when typed, with its numbers: its range, its most bytes, a color's or a folder name's */
void gui_editor_takes(const GuiEditor* editor, char* out, size_t size);
/* a row's label and value as the screen shows them */
void gui_editor_label(const GuiEditor* editor, int row, char* out, size_t size);
void gui_editor_value(const GuiEditor* editor, int row, char* out, size_t size);
/* themes_dir/id/theme.ini written: 0, else nonzero with the note saying why */
int gui_editor_save(GuiEditor* editor, const char* themes_dir);
/* rows rows on screen: top follows the focus */
void gui_editor_scroll(GuiEditor* editor, int rows);
#endif
