/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The menu's Settings: the launcher's own rows and the recompiler's (settings.txt's launcher sections: gdbstub, Extract
   to tmpfs, Extract dump and the AOT threads), then every setting of
   the game's .ini as the runtime lists them (scarletnx --settings, its tables), each a row of the form (editor.h),
   named and its values worded as the game's menu shows them. A setting is the .ini's, passed to no run, while it is at
   its default: a word row's value is one of its words, the default's marked [default]; one whose default has no word
   (an empty one) has the .ini's as a word of its own, shown [default]. The picks are kept in a file, an .ini of the
   game's sections and keys, and go to every run as --key=value with --launcher-picks=1. A run takes a setting from what
   that run picks, then from the game's own file of picks (<name>.picks beside its .ini, which its menu writes and the
   runtime reads), then from the global file, and last from the game's .ini */
#ifndef SNX_GUI_SETTINGS_H
#define SNX_GUI_SETTINGS_H
#include <stddef.h>
#include "gui/theme.h"

/* a row that is no setting: a page's, which opens the section/page rows listed after it, and its All, first on that
   page, which steps them together; a link, a row of another screen past every setting (gui_settings_link) */
enum { GUI_SETTING_PAGE = 1, GUI_SETTING_ALL, GUI_SETTING_SECRET, GUI_SETTING_LINK };
#define GUI_SETTING_LINKS 24  /* the links past count a list of settings has room for */
#define GUI_SETTING_TEXT 512 /* a typed value's room: the longest of settings.txt's texts the launcher keeps */
typedef struct {
    int word;                           /* a word row's: the word, 0 the .ini's (a plain row's: its first) */
    char text[GUI_SETTING_TEXT];        /* a typed row's: empty, the .ini's. A page's and an All's: the value shown */
    char start[GUI_SETTING_TEXT];       /* the value the menu starts with, which a file of picks leaves out */
    char option[GUI_SETTING_TEXT + 72]; /* a run's --key=value; a page's row: the section it is in */
    const char* key;                    /* the .ini's, the row's key being its name in the menus */
    const char** values;                /* a word row's values by word, NULL where the words are the values */
    const char* fallback; /* the runtime's default for the .ini, as the row words it: what it shows while not picked */
    const char* def;      /* that default as the .ini writes it */
    const char* only;     /* where it does something, when not on this system ("macOS only", ini.h's ini_only) */
    int kind;             /* 0, GUI_SETTING_PAGE, GUI_SETTING_ALL, GUI_SETTING_SECRET: the Eden room's password,
                             which is neither the picks' nor a run's option (users/online.ini holds it), or
                             GUI_SETTING_LINK */
    int plain;            /* a word row whose words are its values alone, no .ini's: its default's is no pick */
    int always;           /* plain, and passed to every run, its start too (gui_settings_always) */
    int type_text;        /* the .ini's value is a text, often a folder: gui_settings_debug says its key alone */
    int needs_build;      /* the build this row needs, which this one has none of: shown, picked by nothing */
    int path; /* a text that is a path (ini.h's INI_PATH): 'd' a folder's, 'f' a file's, 's' one to write's */
    int slider;           /* a plain word row on a slider (ini.h's INI_SLIDER): stepped to its ends, never round */
    /* a word row's value that is none of its words, kept in text and shown as itself as a word after its others: that
       word's index, 0 for none. Only a slider's row and an open one have such a value (the runtime's list's open:
       its words the common values, not every one): a number in its range (number 'i' or 'f', lo to hi), or a text */
    int custom, number, open;
    double lo, hi;
    /* a row of boxes (the runtime's list's boxes): its items, their value, label and about, by item; the value (text)
       is a set of them, a text's separated by commas or spaces, an int's the sum of their bits (number 'i') */
    const char **item_value, **item_label, **item_about;
    int items;
    /* an open row's words with more the launcher found itself (gui_settings_add_words), in one block of its own, NULL
       for none; how many words it had before them */
    const char** grown;
    int base;
    int first, count;     /* the rows a page opens, an All steps */
} GuiSetting;
typedef struct {
    GuiThemeKey* keys;  /* the rows: the launcher's, then the runtime's by section */
    GuiSetting* values; /* their values, which the keys' offsets are in */
    int count;          /* in one block with the list the keys' names and help point into */
    int own;            /* the launcher's own rows and the recompiler's, first (their page's rows among them) */
    int game;           /* a game's own picks (gui_settings_read_game): none of the launcher's rows, every word row's
                           first word Global, no pick. The console it is of, GUI_GAME_*, 0 for the launcher's */
} GuiSettings;
/* the consoles whose games have picks of their own */
enum { GUI_GAME_SWITCH = 1, GUI_GAME_3DS, GUI_GAME_VITA, GUI_GAME_GBA, GUI_GAME_NDS };
/* the launcher's own rows and the recompiler's (g_launch_sections, listed by ini_list_text), then the runtime's list of
   the game's settings, a line each (section[/page], key, default, int | float | text, lowest, highest, help, name, the
   values value=word between '|', where it does something when not on this system, what the build needs, slider for a
   slider's, tabs between); another line is left out. A row this build has none of what it needs is kept and marked, picked by
   nothing. A page's rows are listed after its section's others, one page a section. 0, else nonzero when out of
   memory */
int gui_settings_read(GuiSettings* settings, const char* runtime);
/* the rows of a Switch game's own picks, its picks file (<name>.picks beside its .ini) as gui_settings_load and
   gui_settings_save read and write it: the runtime's alone, each with Global first, the value it starts with, which
   no pick has and the launcher's picks then give (no plain row, no slider). As gui_settings_read: 0, else nonzero */
int gui_settings_read_game(GuiSettings* settings, const char* runtime);
/* a game's of console (GUI_GAME_*): a 3DS game's, the settings it has of its own alone (3ds/runtime/main.h's
   snx_ctr_game_setting, its console), a word row's words Global and those its default is not, which Global gives; a
   Vita's, a GBA's or a DS's, the on-screen controls' it has (common/utilities/vpad_consoles.h) and its touch screen's,
   each control by the console's name for it */
int gui_settings_read_console(GuiSettings* settings, const char* runtime, int console);
void gui_settings_free(GuiSettings* settings);
/* row count + i (i under GUI_SETTING_LINKS), past the settings and out of their count, made a link: a row of another
   screen that Settings' search lists, its name, help and value shown, which nothing of the settings reads or writes.
   Its row, or -1 for an i past them or no list read */
int gui_settings_link(GuiSettings* settings, int i, const char* name, const char* about, const char* value);
/* the row of key set to value, a word or a text as the row takes: 1 when it did */
int gui_settings_set(GuiSettings* settings, const char* key, const char* value);
/* the rows as they are, the launcher's own choices among them, as the values to start from */
void gui_settings_mark(GuiSettings* settings);
/* the row of key, a word row of the runtime's, passed to every run from now on: value, then the word picked, never the
   .ini's, which it no longer has. Its [default] marks the value it starts with (gui_settings_mark), which Del and a
   reset put back. 1 when it did, 0 for no such row or a value not of its words */
int gui_settings_always(GuiSettings* settings, const char* key, const char* value);
/* the picks of a file of them, global or a game's: each key's row set, the others left as they are. What it does not
   take is said in old ("; " between, cut where it ends): a key another version's or program's, a line with no = and a
   key given twice, named once a run (config_file.h), a value its row has not, with the row's value now, and a file that
   cannot be read, with the system's reason. What older versions wrote that a plain row reads otherwise too: its .ini, read as
   the value it starts with ("fullscreen=.ini is no longer a value of Fullscreen: fullscreen=0 now"), and its default
   picked, which a run was given over the game's .ini and is now no pick ("docked=1 is the default of TV mode: no run
   is given it over the game's .ini"), and the pick of a key renamed (ini.h's ini_renamed) on its new row while the
   file gives that one none ("vk_descriptors=1 is deprecated: Descriptor buffers is vk_descriptors_mode=1 now"). How
   many, 0 for none */
int gui_settings_load(GuiSettings* settings, const char* path, char* old, size_t size);
/* the rows that are not at their start in path, by section, and the lines of the keys path holds that no row has, as
   they were (config_file.h): another version's or program's, an older version's but a renamed one, whose value its new
   row has. Nonzero for a path it cannot read, which is never written over, and one it cannot write. 0, else nonzero */
int gui_settings_save(const GuiSettings* settings, const char* path);
/* the same, without an older version's keys (ini.h's ini_deprecated and ini_renamed): Clean Old Settings */
int gui_settings_clean(const GuiSettings* settings, const char* path);
/* the keys of an older version the file of picks at path holds, which no row has and gui_settings_clean takes out, as
   key=value, ", " between. How many */
int gui_settings_unread(GuiSettings* settings, const char* path, char* out, size_t size);
/* row back to its start (a page's or an All's: their rows), every row for -1 */
void gui_settings_reset(GuiSettings* settings, int row);
/* the rows whose value is not their start: what a reset of every row changes */
/* 1 for row (a page's or an All's: each of its rows) at the value it started with, what Del would put back; else 0 */
int gui_settings_at_start(const GuiSettings* settings, int row);
int gui_settings_changed(const GuiSettings* settings);
/* A slider's row (GuiSetting's slider): its value a step down (step -1) or up (1), stopping at its ends; set to the
   step nearest to at, thousandths of its way from its first word to its last; and where it is now, the same way. The
   first two: 1 when it changed, 0 at its end or on a row that is no slider's */
int gui_settings_slide(GuiSettings* settings, int row, int step);
int gui_settings_slide_to(GuiSettings* settings, int row, int at);
int gui_settings_knob(const GuiSettings* settings, int row);
/* a number's typed row (GuiThemeKey's numeric) a step down (-1) or up (1) from its value, else its default: a whole
   number's by 1, a float's by a quarter, or a twentieth where its range is 2 or less, as the game's menu steps it, onto
   that step's grid and stopping at its ends. 1 when it changed, 0 at an end or on another row */
int gui_settings_step_number(GuiSettings* settings, int row, int step);
/* row, an open one of words, with count values more after its own, each its own word (the recording devices by name):
   the ones it has already left out, in the place of those a call before added. Its value stays what it is, a word of
   them now where one is. 1, else 0 for another row or out of memory */
int gui_settings_add_words(GuiSettings* settings, int row, const char* const* values, int count);
/* A row of boxes: item i on in the value the row has, else in base, what it has none of its own (the launcher's for a
   game's own, else NULL for the default); item i turned on or off in it, the row's own value then (one with nothing on
   is no value: the default's, or the launcher's, again): 1 when it did. The items a value names that the row has not
   are kept as they are */
int gui_settings_item_on(const GuiSettings* settings, int row, int item, const char* base);
int gui_settings_item_toggle(GuiSettings* settings, int row, int item, const char* base);
/* what a row of boxes shows: how many of its items are on in value, "None" for none, and how many others the value
   names that are none of them ("2 on, 1 other") */
void gui_settings_items_said(const GuiSettings* settings, int row, const char* value, char* out, size_t size);
/* an All's row stepped (-1 or 1, 0 for a select) with every row it steps: 1, else 0 */
int gui_settings_step_all(GuiSettings* settings, int row, int step);
/* the Alls' values after a change: their rows' word when they share one */
void gui_settings_sync(GuiSettings* settings);
/* row's value as the Settings show it: a pick as it is, a row not picked its default and [default] ("Off
   [default]", "[default]" for an empty one), an All's word its rows share or Mixed, [default] after it or alone
   while none of its rows is picked; an always row's word, [default] after the one it starts with */
void gui_settings_shown(const GuiSettings* settings, int row, char* out, size_t size);
/* the Debug section's settings not at their default for a run of the game whose .ini is at ini (NULL: none read): the
   game's own pick (its picks file beside the .ini), else a row's pick, else the file's value, else its default; then
   gdbstub when the launcher's row is on. With picks 0 the game's own alone (its picks and its .ini), the launcher's
   picks and gdbstub left out. key=value between ", ", a text's by its key alone, as the game says
   them over its picture (the runtime's options_debug): how many. out cut where it ends, NUL-terminated */
int gui_settings_debug(const GuiSettings* settings, const char* ini, int picks, char* out, size_t size);
/* a game's own row's value as its Settings show it: a pick as gui_settings_shown says it, else Global and the
   launcher's value of it as its Settings show that ("Global (60%)", "Global (Off [default])") */
void gui_settings_shown_game(const GuiSettings* game, const GuiSettings* global, int row, char* out, size_t size);
/* what the game whose .ini is at ini has of its own: its picks (<name>.picks), key=value, then the GPU driver and
   Freedreno's variables its box picked, kept in the .ini (gpu_driver=value, freedreno_env by its key alone), ", "
   between, cut where out ends: how many */
int gui_settings_game_own(const char* ini, char* out, size_t size);
/* the value picked for key, NULL for the .ini's or no such row */
const char* gui_settings_value(const GuiSettings* settings, const char* key);
/* argv's options for a run of the game whose .ini is at game_ini (NULL: a dump's, or none): --key=value for each
   setting picked and --launcher-picks=1 after them, which puts the game's own picks over them; of the settings the
   recompile takes too (settings.txt's recompile), which it reads from its options alone, the game's own pick where it
   has one. With launch the launcher's too, -S -s for the stub, --tmpfs and --extract-container. Their count, count + 5
   at most */
size_t gui_settings_options(GuiSettings* settings, int launch, const char* game_ini, const char** argv);
/* argv's --key=value for each of the 3DS driver's settings picked: its tables' (keys that start with ctr_) and the
   Switch's it takes too (3ds/runtime/main.h's snx_ctr_shared_setting, vk_memory); with one a game has of its own among
   them (its console), --launcher-picks=1 after them, which puts the game's over it. Its count */
size_t gui_settings_ctr_options(GuiSettings* settings, const char** argv);
/* argv's --key=value for each of the Vita driver's settings picked: its Debug page's (keys that start with vita_). Its
   count */
size_t gui_settings_vita_options(GuiSettings* settings, const char** argv);
/* argv's --key=value for each of the on-screen controls' settings picked (Show controls' always, gui_settings_always)
   and the touch screen's, which a GBA, a DS and a Vita game's driver take (one its console has nothing for said there),
   --launcher-picks=1 after them, which puts the game's own over them. Its count */
size_t gui_settings_controls_options(GuiSettings* settings, const char** argv);
/* the same for a Linux program's run (linux_), an Android app's (android_) and a DS game's (nds_, but the WFC
   service's, which that driver reads from settings.ini) */
size_t gui_settings_linux_options(GuiSettings* settings, const char** argv);
size_t gui_settings_android_options(GuiSettings* settings, const char** argv);
size_t gui_settings_nds_options(GuiSettings* settings, const char** argv);
/* Extract to tmpfs: --tmpfs while it is on, else NULL (extractions on disk) */
const char* gui_settings_tmpfs(const GuiSettings* settings);
/* Extract dump: --extract-container while it is on, else NULL (a dump read in place, nothing extracted).
   Both drivers take it */
const char* gui_settings_extract(const GuiSettings* settings);
/* argv's --key=value for each of the recompiler's settings picked (the AOT threads
   and the like), which the Switch driver takes for a run (which recompiles when it must) and for R's recompile; with
   make, a run that makes the game and plays nothing (-r), each of the game's settings the recompile takes too
   (settings.txt's recompile: NCE among them) picked, which a run gets with the game's options: the game's
   own pick where its picks beside game_ini (NULL: none) have one, else the launcher's. Their count */
size_t gui_settings_recompile(GuiSettings* settings, int make, const char* game_ini, const char** argv);
/* argv's --key=value for each of the recompile's settings every driver that recompiles takes picked: the optimization
   level and the AOT threads, which the 3DS, Vita and GBA drivers read as the Switch's does. Their count */
size_t gui_settings_recompile_shared(GuiSettings* settings, const char** argv);
/* argv's --key=value of the game's setting key when it is picked: its count, 1 or 0 */
size_t gui_settings_one(GuiSettings* settings, const char* key, const char** argv);
/* argv's --gba-color=raw for a GBA game's run, when the launcher's GBA colors (gba_color) is Raw, not its default: 1, else 0 */
size_t gui_settings_gba(GuiSettings* settings, const char** argv);
/* argv's --java-home=DIR for the Android driver when Java home is set: its count, 1 or 0 (empty, the driver's own) */
size_t gui_settings_java_home(GuiSettings* settings, const char** argv);
#endif
