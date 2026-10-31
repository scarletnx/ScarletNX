/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The menu's Settings: the launcher's own rows and the recompiler's (settings.txt's launcher sections: gdbstub, Extract
   to tmpfs, Extract dump, the optimization level, the AOT threads and the LLVM Internals page), then every
   setting of the game's .ini as the runtime lists them (scarletnx --settings, its tables), each a row of the form
   (editor.h), named and its values worded as the game's menu shows them. A setting is the .ini's,
   passed to no run, until one is picked. The picks are kept in a file, an .ini of the game's sections and keys, and go
   to every run as --key=value. A run takes a setting from what that run picks, then from the game's own file of picks
   (to come: today there is only the global one, every game's), then from the global file, and last from the game's
   .ini */
#ifndef SNX_GUI_SETTINGS_H
#define SNX_GUI_SETTINGS_H
#include <stddef.h>
#include "gui/recommend.h"
#include "gui/theme.h"

/* a row that is no setting: a page's, which opens the section/page rows listed after it, and its All, first on that
   page, which steps them together */
enum { GUI_SETTING_PAGE = 1, GUI_SETTING_ALL };
#define GUI_SETTING_TEXT 192 /* a typed value's room: gamepad_map's whole map (pad_map.h) is 165 at most */
typedef struct {
    int word;                           /* a word row's: the word, 0 the .ini's */
    char text[GUI_SETTING_TEXT];        /* a typed row's: empty, the .ini's. A page's and an All's: the value shown */
    char start[GUI_SETTING_TEXT];       /* the value the menu starts with, which a file of picks leaves out */
    char option[GUI_SETTING_TEXT + 72]; /* a run's --key=value; a page's row: the section it is in */
    const char* key;                    /* the .ini's, the row's key being its name in the menus */
    const char** values;                /* a word row's values by word, NULL where the words are the values */
    const char* fallback; /* the runtime's default for the .ini, as the row words it: what it shows while not picked */
    const char* def;      /* that default as the .ini writes it */
    int kind;             /* 0, GUI_SETTING_PAGE or GUI_SETTING_ALL */
    int type_text;        /* the .ini's value is a text, often a folder: gui_settings_debug says its key alone */
    int first, count;     /* the rows a page opens, an All steps */
} GuiSetting;
typedef struct {
    GuiThemeKey* keys;  /* the rows: the launcher's, then the runtime's by section */
    GuiSetting* values; /* their values, which the keys' offsets are in */
    int count;          /* in one block with the list the keys' names and help point into */
    int own;            /* the launcher's own rows and the recompiler's, first (their page's rows among them) */
    char recommended[REC_COUNT][16]; /* recommended.ini's [recommended]: the value Apply gave each key, "" for none */
    char marks[REC_COUNT * 24 + 16]; /* a run's --recommended=key,key */
} GuiSettings;
/* the launcher's own rows and the recompiler's (g_launch_sections, listed by ini_list_text), then the runtime's list of
   the game's settings, a line each (section[/page], key, default, int | float | text, lowest, highest, help, name, the
   values value=word between '|', tabs between); another line is left out. A page's rows are listed after its
   section's others, one page a section. 0, else nonzero when out of memory */
int gui_settings_read(GuiSettings* settings, const char* runtime);
void gui_settings_free(GuiSettings* settings);
/* the row of key set to value, a word or a text as the row takes: 1 when it did */
int gui_settings_set(GuiSettings* settings, const char* key, const char* value);
/* the rows as they are, the launcher's own choices among them, as the values to start from */
void gui_settings_mark(GuiSettings* settings);
/* the picks of a file of them, global or a game's: each key's row set, the others left as they are */
void gui_settings_load(GuiSettings* settings, const char* path);
/* the rows that are not at their start in path, by section; with the launcher's rows alone (the runtime listed none),
   the keys path holds that no row has after them, as they were, and nonzero for a path it cannot read. 0, else
   nonzero */
int gui_settings_save(const GuiSettings* settings, const char* path);
/* what the file of picks at path holds that no row takes, which the next save leaves out while the runtime's rows are
   there: a key no row has, or a value its row does not take, as key=value, ", " between. How many */
int gui_settings_unread(GuiSettings* settings, const char* path, char* out, size_t size);
/* row back to its start (a page's or an All's: their rows), every row for -1 */
void gui_settings_reset(GuiSettings* settings, int row);
/* the rows whose value is not their start: what a reset of every row changes */
/* 1 for row (a page's or an All's: each of its rows) at the value it started with, what Del would put back; else 0 */
int gui_settings_at_start(const GuiSettings* settings, int row);
int gui_settings_changed(const GuiSettings* settings);
/* an All's row stepped (-1 or 1, 0 for a select) with every row it steps: 1, else 0 */
int gui_settings_step_all(GuiSettings* settings, int row, int step);
/* the Alls' values after a change: their rows' word when they share one */
void gui_settings_sync(GuiSettings* settings);
/* row's value as the Settings show it: a pick as it is, followed by [recommended] while it is the value Recommended
   Settings gave it; a row not picked its default and [default] ("Off [default]", "[default]" for an empty one), an
   All's [default] while none of its rows is picked */
void gui_settings_shown(const GuiSettings* settings, int row, char* out, size_t size);
/* the Debug section's settings not at their default for a run of the game whose .ini is at ini (NULL: none read): a
   row's pick, else the file's value, else its default; then gdbstub when the launcher's row is on. With picks 0 the
   file's alone, the picks and gdbstub left out. key=value between ", ", a text's by its key alone, as the game says
   them over its picture (the runtime's options_debug): how many. out cut where it ends, NUL-terminated */
int gui_settings_debug(const GuiSettings* settings, const char* ini, int picks, char* out, size_t size);
/* recommended.ini's [recommended] into the rows: Settings marks a row [recommended] while its pick is the value Apply
   gave it, and a run is told which (gui_settings_marks) */
void gui_settings_recommended(GuiSettings* settings, const RecFile* file);
/* 1 for row picked at the value Recommended Settings' Apply gave it, else 0 */
int gui_settings_is_recommended(const GuiSettings* settings, int row);
/* argv's --recommended=key,key for the settings picked at the value Recommended Settings gave them, which the runtime's
   log marks: its count, 1 or 0 */
size_t gui_settings_marks(GuiSettings* settings, const char** argv);
/* the value picked for key, NULL for the .ini's or no such row */
const char* gui_settings_value(const GuiSettings* settings, const char* key);
/* argv's options for a run: --key=value for each setting picked; with launch the launcher's too, -S -s for the stub,
   --tmpfs and --extract-container. Their count, count + 1 at most */
size_t gui_settings_options(GuiSettings* settings, int launch, const char** argv);
/* Extract to tmpfs: --tmpfs while it is on, else NULL (extractions on disk) */
const char* gui_settings_tmpfs(const GuiSettings* settings);
/* Extract dump: --extract-container while it is on, else NULL (a dump read in place, nothing extracted).
   Both drivers take it */
const char* gui_settings_extract(const GuiSettings* settings);
/* argv's --key=value for each of the recompiler's settings picked (the optimization level, the AOT threads, LLVM
   Internals), which the Switch driver takes for a run (which recompiles when it must) and for R's recompile. Their
   count */
size_t gui_settings_recompile(GuiSettings* settings, const char** argv);
/* argv's --key=value of the game's setting key when it is picked: its count, 1 or 0 */
size_t gui_settings_one(GuiSettings* settings, const char* key, const char** argv);
#endif
