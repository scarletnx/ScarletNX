/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GUI_MODEL_H
#define SNX_GUI_MODEL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "common/utilities/font.h" /* gui_glyph */

/* the menu's items, in order: the loads, the keys, Settings and Share log, on Android the data folder, Exit. The games
   are the Games tab's */
enum {
    GUI_SWITCH,
    GUI_LINUX,
    GUI_3DS,
    GUI_KEYS,
    GUI_SETTINGS, /* the game's .ini settings, the launcher's own for the runs and its App: settings.h */
    GUI_LOG,      /* the logs in one file, for a bug report: a phone has no terminal to copy them from */
#ifdef __ANDROID__
    GUI_DATA, /* the data folder, the app's own storage, in the system's Files app (dist/android's DataProvider) */
#endif
    GUI_EXIT,
    GUI_ITEM_COUNT
};
typedef enum { GUI_UP, GUI_DOWN, GUI_LEFT, GUI_RIGHT, GUI_SELECT, GUI_CANCEL, GUI_PAGE_UP, GUI_PAGE_DOWN } GuiKey;
#define GUI_TEXT_MAX 4
/* the screen over the menu: none, a load's file to pick, the games' browser, the keys' */
typedef enum { GUI_LOAD_NONE, GUI_LOAD_DROP, GUI_LOAD_GAMES, GUI_LOAD_KEYS } GuiLoadView;
typedef struct {
    int selected; /* keyboard/mouse focus, not activation */
    int active;   /* the last explicitly activated item: the load a picked file goes to */
    bool activated;
    GuiLoadView view;
    bool running, quit;
    int text_size; /* the font's scale, 1 to GUI_TEXT_MAX; 0 fits the screen */
} GuiMenu;
/* what a key on the menu asks of the GUI: nothing more, the Settings or the logs shared */
typedef enum { GUI_MENU_STAY, GUI_MENU_SETTINGS, GUI_MENU_LOG, GUI_MENU_DATA } GuiMenuDo;
GuiMenuDo gui_menu_key(GuiMenu* menu, GuiKey key);
/* The menu's questions, one box for all (main.c's render_ask): shown or not, which one, the choice in focus (the first
   stays first: it keeps things as they are), and whether its second choice takes a second press */
enum { GUI_CONFIRM_KEEP, GUI_CONFIRM_ABORT };
typedef enum {
    GUI_ASK_ABORT,   /* a running run's, when ask_abort says to ask */
    GUI_ASK_EXIT,    /* leaving the menu on a computer */
    GUI_ASK_DEEP,    /* a folder picked with folders in it: are their games listed too */
    GUI_ASK_OUTSIDE, /* a folder picked in Folders outside the app's own storage, which Android slows */
    GUI_ASK_ROOM     /* a run whose extraction may not fit (gui_room) */
} GuiAsk;
typedef struct {
    bool shown;
    GuiAsk ask;
    int selected;
    bool twice; /* the second choice is pressed twice: the first press only arms it */
    bool armed; /* pressed once, its focus kept */
} GuiConfirm;
void gui_confirm_open(GuiConfirm* confirm, GuiAsk ask, bool twice);
/* A key on the question: the arrows move between its choices, Select takes the one in focus and Cancel keeps going,
   both closing it; a question asked twice takes its second choice at the second Select, a move between choices
   undoing the first. True when the second choice is taken */
bool gui_confirm_key(GuiConfirm* confirm, GuiKey key);
/* the pointer on a choice: it takes the focus, a press armed on the other undone */
void gui_confirm_focus(GuiConfirm* confirm, int choice);
const char* gui_setting_description(const GuiMenu* menu);
/* bytes in KB under a MB (a byte is 1 KB, nothing is 0 MB), in MB, or in GB from 1024 MB on */
void gui_size_text(char* out, size_t size, uint64_t bytes);
/* Whether a run's first time with a dump of dump bytes has room: its extraction, about its size, in a folder with
   work_free bytes free and, for a Switch game (copy), its RomFS copied into its folder in games/, about its size
   again, with games_free free; on one disk (shared) both from work_free. need and left say what it takes and what is
   free, on the disk it does not fit on (else the extraction's) */
bool gui_room(uint64_t dump, bool copy, uint64_t work_free, uint64_t games_free, bool shared, uint64_t* need,
              uint64_t* left);
/* the footer's line: RAM in MB or GB, the CPU as a share of all cores and the cores in use (cpu_percent counts one
   core as 100%) */
void gui_usage_text(char* out, size_t size, bool available, uint64_t memory_bytes, double cpu_percent, int cores);
/* how many columns a line over columns too many has slid at ms, a column every step_ms: it waits at each end */
int gui_slide(int over, uint64_t ms, uint32_t step_ms);
/* the bytes of s's first row when it is wrapped at width columns (at least 1; a wide letter's two, gui_text_cols): all
   of it when it fits, else up to the last slash or backslash that fits, else the last space, else the characters that
   fit, one at least */
size_t gui_wrap(const char* s, int width);

typedef struct {
    size_t count, page, page_size;
    size_t head;     /* the rows over the games on every page: Add a folder, then the recent games */
    size_t selected; /* focus: a row of the head under head, else the page's game selected - head */
    size_t active;   /* absolute file index, preserved across page changes and resizing */
    bool activated;
} GuiBrowser;
/* a key's ask: nothing, back to the menu (Cancel), the focused head row, or the focused game run */
typedef enum { GUI_BROWSER_NONE, GUI_BROWSER_BACK, GUI_BROWSER_HEAD, GUI_BROWSER_OPEN } GuiBrowserAction;
size_t gui_browser_pages(const GuiBrowser* browser);
size_t gui_browser_items(const GuiBrowser* browser);
void gui_browser_resize(GuiBrowser* browser, size_t page_size);
GuiBrowserAction gui_browser_key(GuiBrowser* browser, GuiKey key);

#define GUI_MAX_COLS 240
#define GUI_MAX_ROWS 100
typedef struct {
    unsigned char cells[GUI_MAX_ROWS][GUI_MAX_COLS];
    int cols, rows, row, col, escape, utf8_left;
    uint32_t codepoint, utf8_min;
} GuiConsole;
void gui_console_init(GuiConsole* console, int cols, int rows);
void gui_console_resize(GuiConsole* console, int cols, int rows);
void gui_console_feed(GuiConsole* console, const char* data, size_t size);
/* SDL_DROP_FILE is already a path. DROP_TEXT/clipboard may contain a quoted path
   or a local file:// URI. Returns owned UTF-8, never shell-expanded. */
char* gui_drop_path(const char* text, bool text_drop);
/* The path of a file or folder a picker chose: on Android a document or a folder (a tree) of the device's storage, or a
   download it names by path, NULL for any other (an app's, a cloud's); elsewhere the path or file:// URI it gave.
   Owned. */
char* gui_document_path(const char* uri);
/* 1 for text as a prod.keys has it: no NUL byte, and a line "name = hex" at least (a key's name of letters, digits and
   _, its value hexadecimal), what tells it from any other file where it has no name to go by */
int gui_keys_text(const char* data, size_t size);
#endif
