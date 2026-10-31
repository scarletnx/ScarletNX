/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GUI_MODEL_H
#define SNX_GUI_MODEL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "common/utilities/font.h" /* gui_glyph */

/* the menu's items, in order: the loads, the keys, Settings, Recommended Settings and Share Log, Folders, Exit. The
   games are the Games tab's */
enum {
    GUI_SWITCH,
    GUI_LINUX,
    GUI_3DS,
    GUI_VITA,
    GUI_GBA,
    GUI_KEYS,
    GUI_SETTINGS,    /* the game's .ini settings, the launcher's own for the runs and its App: settings.h */
    GUI_RECOMMENDED, /* Recommended Settings: this computer tested, and the settings that suit it proposed */
    GUI_LOG,         /* the logs in one file, for a bug report: a phone has no terminal to copy them from */
    GUI_FOLDERS,     /* Settings > App > Folders, where each folder opens */
    GUI_EXIT,
    GUI_ITEM_COUNT
};
typedef enum { GUI_UP, GUI_DOWN, GUI_LEFT, GUI_RIGHT, GUI_SELECT, GUI_CANCEL, GUI_PAGE_UP, GUI_PAGE_DOWN } GuiKey;
#define GUI_TEXT_MAX 4
/* the screen over the menu: none, a load's file to pick, the games' browser, the keys', the recompiles' queue */
typedef enum { GUI_LOAD_NONE, GUI_LOAD_DROP, GUI_LOAD_GAMES, GUI_LOAD_KEYS, GUI_LOAD_RECOMPILE } GuiLoadView;
typedef struct {
    int selected; /* keyboard/mouse focus, not activation */
    int active;   /* the last explicitly activated item: the load a picked file goes to */
    bool activated;
    GuiLoadView view;
    bool running, quit;
    /* the font's scale, 1 to GUI_TEXT_MAX; 0 fits the screen (Auto) */
    int text_size;
} GuiMenu;
/* what a key on the menu asks of the GUI: nothing more, the Settings or the logs shared */
typedef enum { GUI_MENU_STAY, GUI_MENU_SETTINGS, GUI_MENU_RECOMMENDED, GUI_MENU_LOG, GUI_MENU_FOLDERS } GuiMenuDo;
GuiMenuDo gui_menu_key(GuiMenu* menu, GuiKey key);
/* The menu's questions, one box for all (main.c's render_ask): shown or not, which one, the choice in focus (the first
   stays first: it keeps things as they are), and whether its second choice takes a second press */
enum { GUI_CONFIRM_KEEP, GUI_CONFIRM_ABORT };
typedef enum {
    GUI_ASK_ABORT,   /* a running run's, when ask_abort says to ask */
    GUI_ASK_EXIT,    /* leaving the menu on a computer */
    GUI_ASK_DEEP,    /* a folder picked with folders in it: are their games listed too */
    GUI_ASK_OUTSIDE, /* a folder picked in Folders outside the app's own storage, which Android slows */
    GUI_ASK_ROOM,    /* a run whose extraction may not fit (gui_room) */
    /* a change to the player's settings or folders, what it changes in the question (main.c's ask_line) */
    GUI_ASK_CLEAN,  /* App's Clean Old Settings: the [Deprecated] sections and old keys taken out */
    GUI_ASK_RESET,  /* Settings' Reset to Defaults: the settings not at their default */
    GUI_ASK_PLACES, /* Folders' Reset to Defaults: the folders picked */
    GUI_ASK_PLACE,  /* a folder picked in Folders, or the Library's recompiled/, back to its default */
    GUI_ASK_FOLDER, /* a game folder taken off the Library */
    GUI_ASK_MOVE,   /* App's Move Old Files: how many entries move, how many stay */
    GUI_ASK_STATE,  /* a save state deleted (the box of a game's save states) */
    GUI_ASK_CACHE,  /* a game's shader cache and code cache deleted (its box's Clean Cache) */
    GUI_ASK_CLOSE,  /* the window closed while a run goes: it ends with the menu */
    GUI_ASK_MADE,   /* a dump, package or ROM a game was made from already: play the game, or recompile it */
    GUI_ASK_ERROR   /* a failure of the system's, said whole (main.c's say_failure): OK alone, which closes it */
} GuiAsk;
typedef struct {
    bool shown;
    GuiAsk ask;
    int selected;
    bool twice; /* the second choice is pressed twice: the first press only arms it */
    bool armed; /* pressed once, its focus kept */
} GuiConfirm;
/* A choice among count, the one in focus (0 first): Up and Down, Left and Right, move round them, Select takes it and
   Cancel goes back. A question's yes and no are two (gui_confirm_key); the setup's steps take as many as they offer */
typedef struct {
    int count, focus;
} GuiChoice;
typedef enum { GUI_CHOICE_STAY, GUI_CHOICE_TAKE, GUI_CHOICE_BACK } GuiChoiceDo;
GuiChoiceDo gui_choice_key(GuiChoice* choice, GuiKey key);
/* The setup's first step, Appearance: Language, Theme and UI scale a row each, as Settings > App has them, the Side
   panel, and Games' View and Icon size as its View box has them, then Next under them. Up and Down move round them all;
   Left and Right change a row's value a step either way, UI scale's open its dialog; Select steps a row's value on and
   opens UI scale's dialog, as Settings > App's Enter does, and goes on from Next; Cancel goes back */
enum {
    GUI_APPEARANCE_LANGUAGE,
    GUI_APPEARANCE_THEME,
    GUI_APPEARANCE_TEXT,
    GUI_APPEARANCE_SIDE,
    GUI_APPEARANCE_VIEW,
    GUI_APPEARANCE_ICONS,
    GUI_APPEARANCE_NEXT,
    GUI_APPEARANCE_ROWS
};
typedef enum {
    GUI_APPEARANCE_STAY,
    GUI_APPEARANCE_CHANGE, /* the row's value, *step (-1 or 1) away */
    GUI_APPEARANCE_SIZES,  /* UI scale's dialog opened */
    GUI_APPEARANCE_ON,     /* the next step */
    GUI_APPEARANCE_BACK
} GuiAppearanceDo;
/* key on the page whose focus is *row (GUI_APPEARANCE_*), moved by it */
GuiAppearanceDo gui_appearance_key(int* row, GuiKey key, int* step);
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
/* a key on a paged list under its head rows: Up and Down go over it whole, from a page's last item to the next
   one's first and back, round from the list's last to the head; Left/Right and PgUp/PgDn turn the page, its focus kept
   on the same row */
GuiBrowserAction gui_browser_key(GuiBrowser* browser, GuiKey key);
/* gui_browser_key over a list with items no one picks (a group's heading; skip says which, by the item's index): Up
   and Down step over them, a page shown with its focus on one moves it to the next item that is not one (else the one
   before), and Select on one asks nothing */
GuiBrowserAction gui_browser_key_skip(GuiBrowser* browser, GuiKey key, bool (*skip)(size_t item, const void* user),
                                      const void* user);

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
