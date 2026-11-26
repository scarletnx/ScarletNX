/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/editor.h"
#include "common/utilities/config_file.h"
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
        file->others = file->top = NULL; /* the editor's now */
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
    } else if (key->type == GUI_KEY_TEXT && key->numeric) {
        if (key->whole)
            snprintf(editor->note, sizeof editor->note, snx_tr("Select to type it: a whole number from %.0f to %.0f"),
                     key->lo, key->hi);
        else
            snprintf(editor->note, sizeof editor->note,
                     snx_tr("Select to type it: a number from %g to %g, three digits and two decimals at most"),
                     key->lo, key->hi);
        return;
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
/* nothing, a dot, or a nought: a scale emptied or given 0 is the setting's default, 1 */
static int zero_text(const char* s) {
    char* end;
    double d;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s || (s[0] == '.' && !s[1])) return 1;
    d = strtod(s, &end);
    while (*end == ' ' || *end == '\t') end++;
    return !*end && d == 0;
}
/* what was typed, which the focused row does not take, said with what it takes */
static void refused(GuiEditor* editor, const GuiThemeKey* key) {
    const char* t = editor->text;
    if (!key)
        snprintf(editor->note, sizeof editor->note,
                 snx_tr("%s is not a folder name: letters, digits, spaces, - _ and ., not starting with ."), t);
    else if (key->type == GUI_KEY_COLOUR)
        snprintf(editor->note, sizeof editor->note, snx_tr("%s is not a color #RRGGBB, as #1A2B3C"), t);
    else if (key->type == GUI_KEY_NUMBER)
        snprintf(editor->note, sizeof editor->note, snx_tr("%s is not a whole number from 0 to %lu"), t,
                 (unsigned long)key->max);
    else if (key->numeric && key->whole)
        snprintf(editor->note, sizeof editor->note, snx_tr("%s is not a whole number from %.0f to %.0f"), t, key->lo,
                 key->hi);
    else if (key->numeric)
        snprintf(editor->note, sizeof editor->note, snx_tr("%s is not a number from %g to %g, two decimals at most"), t,
                 key->lo, key->hi);
    else
        snprintf(editor->note, sizeof editor->note, snx_tr("%s is more than %u bytes"), t, (unsigned)key->size - 1);
}
/* what was typed into the focused row: kept, the default for a scale emptied or given 0; one the row does not take
   whole (a number past its range too) is refused, the note saying why and the typing going on, nothing changed */
static void take(GuiEditor* editor) {
    const GuiThemeKey* key = gui_editor_key_of(editor, editor->row);
    int zero = key && key->numeric && !key->whole && zero_text(editor->text);
    if (!key) {
        if (!folder_name(editor->text)) {
            refused(editor, key);
            return;
        }
        snprintf(editor->id, sizeof editor->id, "%s", editor->text);
    } else {
        if (zero && key->def && *key->def) /* a scale is never 0 or empty: its default shows */
            snprintf(editor->text, sizeof editor->text, "%s", key->def);
        if (gui_theme_check(key, editor->text) != 1) {
            refused(editor, key);
            return;
        }
        gui_theme_set(object(editor), key, editor->text);
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
/* A number typed into a setting's row, text added one character at a time: the digits, one '.', two decimals and
   three digits at most and the sign of its range first. Anything else is left out, so no letter (an exponent's e
   included) reaches a row that takes a number. 1 when a character was left out */
static void trim_zeros(char* text) {
    char* at = text;
    char* zero;
    if (*at == '-') ++at;
    zero = at;
    while (*zero == '0') ++zero;
    if (zero == at) return;
    if (*zero == '.' || !*zero) /* one zero stays directly before the point, or as the whole number */
        memmove(at + 1, zero, strlen(zero) + 1);
    else
        memmove(at, zero, strlen(zero) + 1);
}
static int type_number(GuiEditor* editor, const GuiThemeKey* key, const char* text) {
    int dropped = 0;
    for (; *text; ++text) {
        size_t n = strlen(editor->text), digits = 0;
        const char* dot = strchr(editor->text, '.');
        const char* c;
        char ch = *text;
        if (n + 1 >= sizeof editor->text) break;
        for (c = editor->text; *c; ++c) digits += *c >= '0' && *c <= '9';
        if (ch == '-' && !n && key->lo < 0)
            editor->text[n++] = ch;
        else if (ch == '.' && !key->whole && !dot)
            editor->text[n++] = ch;
        else if (ch >= '0' && ch <= '9' && !(dot && (size_t)((editor->text + n) - (dot + 1)) >= 2) &&
                 (key->whole || digits < 3))
            editor->text[n++] = ch;
        else {
            dropped = 1;
            continue;
        }
        editor->text[n] = 0;
        trim_zeros(editor->text);
    }
    return dropped;
}
void gui_editor_type(GuiEditor* editor, const char* text) {
    const GuiThemeKey* key = gui_editor_key_of(editor, editor->row);
    size_t n = strlen(editor->text), add = strlen(text);
    if (!editor->typing || strchr(text, '\n')) return;
    if (key && key->type == GUI_KEY_TEXT && key->numeric) {
        if (type_number(editor, key, text)) {
            if (key->whole)
                snprintf(editor->note, sizeof editor->note,
                         snx_tr("Only the digits of a whole number from %.0f to %.0f"), key->lo, key->hi);
            else
                snprintf(editor->note, sizeof editor->note,
                         snx_tr("Only the digits of a number from %g to %g, three digits and two decimals at most"),
                         key->lo, key->hi);
        }
        return;
    }
    if (n + add >= sizeof editor->text) add = sizeof editor->text - 1 - n;
    while (add && ((unsigned char)text[add] & 0xC0) == 0x80) --add; /* a UTF-8 character whole, or none of it */
    memcpy(editor->text + n, text, add);
    editor->text[n + add] = 0;
}
void gui_editor_takes(const GuiEditor* editor, char* out, size_t size) {
    const GuiThemeKey* key = gui_editor_key_of(editor, editor->row);
    size_t most = sizeof editor->text - 1;
    if (!key)
        snprintf(out, size, "%s", snx_tr("A folder name: letters, digits, spaces, - _ and ., not starting with ."));
    else if (key->type == GUI_KEY_COLOUR)
        snprintf(out, size, "%s", snx_tr("A color: #RRGGBB, as #1A2B3C"));
    else if (key->type == GUI_KEY_NUMBER)
        snprintf(out, size, snx_tr("A whole number from 0 to %lu"), (unsigned long)key->max);
    else if (key->numeric && key->whole && key->def && *key->def) /* nothing typed: the setting's default again */
        snprintf(out, size, snx_tr("A whole number from %.0f to %.0f; empty, the default %s"), key->lo, key->hi,
                 key->def);
    else if (key->numeric && key->whole)
        snprintf(out, size, snx_tr("A whole number from %.0f to %.0f"), key->lo, key->hi);
    else if (key->numeric && key->def && *key->def)
        snprintf(out, size, snx_tr("A number from %g to %g, two decimals at most; empty, the default %s"), key->lo,
                 key->hi, key->def);
    else if (key->numeric)
        snprintf(out, size, snx_tr("A number from %g to %g, two decimals at most"), key->lo, key->hi);
    else
        snprintf(out, size, snx_tr("Text, up to %u bytes"), (unsigned)(key->size - 1 < most ? key->size - 1 : most));
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
    char why[1024] = "";
    /* beside it first, never over one that cannot be read */
    int failed = !path || !text || snx_config_write(path, ".new", text, strlen(text), why, sizeof why);
    if (failed && why[0])
        snprintf(editor->note, sizeof editor->note, snx_tr("Not saved: %s"), why);
    else if (failed)
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
