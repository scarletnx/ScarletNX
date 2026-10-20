/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/editor.h"
#include "common/utilities/common.h"
#include "gui/i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int gui_editor_rows(const GuiEditor* editor) { return editor->count + (editor->object ? editor->action != NULL : 2); }
const GuiThemeKey* gui_editor_key_of(const GuiEditor* editor, int row) {
    int first = editor->object ? 0 : GUI_EDITOR_FOLDER + 1; /* after Save and the theme's folder */
    return row >= first && row < first + editor->count ? &editor->keys[row - first] : NULL;
}
static int is_save(const GuiEditor* editor, int row) {
    return editor->object ? editor->action && row == gui_editor_rows(editor) - 1 : row == GUI_EDITOR_SAVE_ROW;
}
static char* object(GuiEditor* editor) { return editor->object ? editor->object : (char*)&editor->theme; }

void gui_editor_start(GuiEditor* editor, const GuiTheme* theme, GuiThemeFile* file, const char* id) {
    memset(editor, 0, sizeof *editor);
    editor->keys = gui_theme_keys;
    while (gui_theme_keys[editor->count].section) ++editor->count;
    editor->theme = *theme;
    if (file) {
        editor->file = *file;
        file->others = NULL; /* the editor's now */
    } else
        snprintf(editor->file.base, sizeof editor->file.base, "%s", gui_theme_builtin(theme->id) ? theme->id : "");
    snprintf(editor->id, sizeof editor->id, "%s", id);
    editor->preview = GUI_SCREEN_MENU;
}
void gui_editor_form(GuiEditor* editor, const GuiThemeKey* keys, int count, void* object, const char* action) {
    memset(editor, 0, sizeof *editor);
    editor->action = action;
    editor->keys = keys;
    editor->count = count;
    editor->object = (char*)object;
}
void gui_editor_stop(GuiEditor* editor) { gui_theme_file_free(&editor->file); }

/* a folder of themes/ on every system: letters, digits, space, - _ and ., not starting with . */
static int folder_name(const char* s) {
    const char* c;
    if (!*s || *s == '.' || strlen(s) >= sizeof(((GuiEditor*)0)->id)) return 0;
    for (c = s; *c; ++c)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == ' ' ||
              *c == '-' || *c == '_' || *c == '.'))
            return 0;
    return 1;
}
/* a word, a flag or a number moved by step: a word goes round, a number stays in 0 to its max */
static void change(GuiEditor* editor, const GuiThemeKey* key, int step) {
    char* field = object(editor) + key->at;
    if (key->type == GUI_KEY_FLAG)
        *(int*)field = !*(int*)field;
    else if (key->type == GUI_KEY_WORD) {
        int n = 0;
        while (key->words[n]) ++n;
        *(int*)field = (*(int*)field + n + step) % n;
    } else if (key->type == GUI_KEY_NUMBER) {
        long v = (long)*(uint32_t*)field + step * (key->max >= 1000 ? 10 : 1); /* ms by tens */
        *(uint32_t*)field = (uint32_t)(v<0 ? 0 : v>(long) key->max ? (long)key->max : v);
    } else {
        snprintf(editor->note, sizeof editor->note, "%s",
                 key->type == GUI_KEY_COLOUR ? snx_tr("Select to pick it, or type it as #RRGGBB")
                                             : snx_tr("Select to type it: its text"));
        return;
    }
    editor->dirty = true;
}
GuiRgb* gui_editor_colour(GuiEditor* editor) {
    const GuiThemeKey* key = gui_editor_key_of(editor, editor->row);
    return key && key->type == GUI_KEY_COLOUR ? (GuiRgb*)(object(editor) + key->at) : NULL;
}
static uint8_t* channel_of(GuiRgb* c, int channel) {
    return channel == GUI_PICK_RED ? &c->r : channel == GUI_PICK_GREEN ? &c->g : &c->b;
}
void gui_editor_pick(GuiEditor* editor, int channel, int value) {
    GuiRgb* c = gui_editor_colour(editor);
    uint8_t* at;
    if (!editor->picking || !c || channel < GUI_PICK_RED || channel > GUI_PICK_BLUE) return;
    at = channel_of(c, channel);
    editor->channel = channel;
    value = value < 0 ? 0 : value > 255 ? 255 : value;
    if (*at == value) return;
    *at = (uint8_t)value;
    editor->dirty = true;
}
void gui_editor_pick_end(GuiEditor* editor, bool keep) {
    GuiRgb* c = gui_editor_colour(editor);
    if (!editor->picking) return;
    if (!keep && c) {
        *c = editor->was;
        editor->dirty = editor->was_dirty;
    }
    editor->picking = editor->typing = false;
}
static GuiEditorDo pick_key(GuiEditor* editor, GuiKey key) {
    int step = key == GUI_RIGHT ? 1 : key == GUI_LEFT ? -1 : key == GUI_PAGE_UP ? 16 : key == GUI_PAGE_DOWN ? -16 : 0;
    if (key == GUI_UP)
        editor->channel = editor->channel ? editor->channel - 1 : 0;
    else if (key == GUI_DOWN)
        editor->channel = editor->channel < GUI_PICK_HEX ? editor->channel + 1 : GUI_PICK_HEX;
    else if (step && editor->channel < GUI_PICK_HEX)
        gui_editor_pick(editor, editor->channel, *channel_of(gui_editor_colour(editor), editor->channel) + step);
    else if (step)
        snprintf(editor->note, sizeof editor->note, "%s", snx_tr("Select to type it: #RRGGBB"));
    else if (key == GUI_SELECT && editor->channel == GUI_PICK_HEX) {
        gui_editor_value(editor, editor->row, editor->text, sizeof editor->text);
        editor->typing = true;
    } else if (key == GUI_SELECT || key == GUI_CANCEL)
        gui_editor_pick_end(editor, key == GUI_SELECT);
    return GUI_EDITOR_STAY;
}
/* what was typed into the focused row: kept, or the note says what the row takes */
static void take(GuiEditor* editor) {
    const GuiThemeKey* key = gui_editor_key_of(editor, editor->row);
    if (!key) {
        if (!folder_name(editor->text)) {
            snprintf(editor->note, sizeof editor->note, "%s",
                     snx_tr("A folder name: letters, digits, spaces, - _ and ., not starting with ."));
            return;
        }
        snprintf(editor->id, sizeof editor->id, "%s", editor->text);
    } else if (!gui_theme_set(object(editor), key, editor->text)) {
        if (key->type == GUI_KEY_TEXT)
            snprintf(editor->note, sizeof editor->note, snx_tr("At most %u bytes"), (unsigned)key->size - 1);
        else
            snprintf(editor->note, sizeof editor->note, "%s",
                     key->type == GUI_KEY_COLOUR ? snx_tr("A colour: #RRGGBB, as #1A2B3C")
                                                 : snx_tr("A whole number, 0 to 10000"));
        return;
    }
    editor->typing = false;
    editor->dirty = true;
}
GuiEditorDo gui_editor_key(GuiEditor* editor, GuiKey key) {
    const GuiThemeKey* k = gui_editor_key_of(editor, editor->row);
    int rows = gui_editor_rows(editor), leaving = editor->leaving;
    editor->leaving = false;
    editor->note[0] = 0;
    if (editor->typing) {
        if (key == GUI_SELECT)
            take(editor);
        else if (key == GUI_CANCEL)
            editor->typing = false;
        return GUI_EDITOR_STAY;
    }
    if (editor->picking) return pick_key(editor, key);
    switch (key) {
        case GUI_UP: editor->row = editor->row ? editor->row - 1 : 0; break;
        case GUI_DOWN: editor->row = editor->row + 1 < rows ? editor->row + 1 : rows - 1; break;
        case GUI_PAGE_UP: editor->row = editor->row > 10 ? editor->row - 10 : 0; break;
        case GUI_PAGE_DOWN: editor->row = editor->row + 10 < rows ? editor->row + 10 : rows - 1; break;
        case GUI_LEFT:
        case GUI_RIGHT:
            if (k && k->type == GUI_KEY_ACTION) {
                editor->step = key == GUI_RIGHT ? 1 : -1;
                return GUI_EDITOR_ACTION;
            }
            if (k) change(editor, k, key == GUI_RIGHT ? 1 : -1);
            break;
        case GUI_SELECT:
            if (is_save(editor, editor->row)) return GUI_EDITOR_SAVE;
            if (k && k->type == GUI_KEY_ACTION) {
                editor->step = 0;
                return GUI_EDITOR_ACTION;
            }
            if (k && (k->type == GUI_KEY_FLAG || k->type == GUI_KEY_WORD)) {
                change(editor, k, 1);
                break;
            }
            if (k && k->type == GUI_KEY_COLOUR) { /* the picker, on its first channel */
                editor->picking = true;
                editor->channel = GUI_PICK_RED;
                editor->was = *gui_editor_colour(editor);
                editor->was_dirty = editor->dirty;
                break;
            }
            gui_editor_value(editor, editor->row, editor->text, sizeof editor->text);
            editor->typing = true;
            break;
        case GUI_CANCEL:
            if (!editor->dirty || leaving || editor->object) return GUI_EDITOR_LEAVE;
            editor->leaving = true;
            snprintf(editor->note, sizeof editor->note, "%s",
                     snx_tr("Not saved: ESC again leaves without it, Save keeps it"));
            break;
    }
    return GUI_EDITOR_STAY;
}
void gui_editor_type(GuiEditor* editor, const char* text) {
    size_t n = strlen(editor->text), add = strlen(text);
    if (!editor->typing || strchr(text, '\n')) return;
    if (n + add >= sizeof editor->text) add = sizeof editor->text - 1 - n;
    while (add && ((unsigned char)text[add] & 0xC0) == 0x80) --add; /* a UTF-8 character whole, or none of it */
    memcpy(editor->text + n, text, add);
    editor->text[n + add] = 0;
}
void gui_editor_erase(GuiEditor* editor) {
    size_t n = strlen(editor->text);
    while (n && ((unsigned char)editor->text[n - 1] & 0xC0) == 0x80) --n; /* a UTF-8 character's continuation */
    if (n) --n;
    editor->text[n] = 0;
}

void gui_editor_label(const GuiEditor* editor, int row, char* out, size_t size) {
    const GuiThemeKey *k = gui_editor_key_of(editor, row), *before = gui_editor_key_of(editor, row - 1);
    int first = k && (!before || strcmp(before->section, k->section));
    if (!k)
        snprintf(out, size, "%s",
                 editor->object             ? snx_tr(editor->action)
                 : row == GUI_EDITOR_FOLDER ? snx_tr("Folder")
                                            : snx_tr("Save"));
    else if (editor->flat) /* a form's rows are the menu's words; the theme's, the file's keys */
        snprintf(out, size, "%s", editor->object ? snx_tr(k->key) : k->key);
    else
        snprintf(out, size, "%-11s %s", first ? k->section : "", k->key);
}
void gui_editor_value(const GuiEditor* editor, int row, char* out, size_t size) {
    const GuiThemeKey* k = gui_editor_key_of(editor, row);
    if (k)
        gui_theme_get(editor->object ? editor->object : (const char*)&editor->theme, k, out, size);
    else if (editor->object)
        snprintf(out, size, "%s", "");
    else if (row == GUI_EDITOR_FOLDER)
        snprintf(out, size, "%s", editor->id);
    else
        snprintf(out, size, "themes/%s/theme.ini", editor->id);
}

int gui_editor_save(GuiEditor* editor, const char* themes_dir) {
    char* folder = themes_dir ? snx_path_join(themes_dir, editor->id) : NULL;
    char* path = folder ? snx_path_join(folder, "theme.ini") : NULL;
    char* text = gui_theme_write(&editor->theme, &editor->file);
    int failed = !path || !text || snx_mkdirs(folder) || snx_write_file(path, text, strlen(text));
    if (failed)
        snprintf(editor->note, sizeof editor->note, snx_tr("Not saved: %s could not be written"),
                 path ? path : snx_tr("the themes folder"));
    else {
        snprintf(editor->note, sizeof editor->note, snx_tr("Saved in %s"), path);
        editor->dirty = false;
    }
    free(text);
    free(path);
    free(folder);
    return failed;
}

void gui_editor_scroll(GuiEditor* editor, int rows) {
    if (rows < 1) rows = 1;
    if (editor->row < editor->top) editor->top = editor->row;
    if (editor->row >= editor->top + rows) editor->top = editor->row - rows + 1;
    if (editor->top > gui_editor_rows(editor) - rows) editor->top = gui_editor_rows(editor) - rows;
    if (editor->top < 0) editor->top = 0;
}
