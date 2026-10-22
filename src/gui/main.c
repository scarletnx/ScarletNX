/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include <SDL3/SDL.h>
#include "common/utilities/ini_lines.h"
#ifndef SNX_GUI_NO_MAIN
#include <SDL3/SDL_main.h>
#endif
#include "common/utilities/common.h"
#include "common/utilities/game_ini.h"
#include "common/utilities/mods.h"
#include "common/utilities/demangle.h"
#include "common/utilities/logscan.h"
#include "gui/aot.h"
#include "gui/browser.h"
#include "gui/editor.h"
#include "common/utilities/font.h"
#include "gui/gui.h"
#include "gui/i18n.h"
#include "gui/latin.h"
#include "gui/wide.h"
#include "gui/model.h"
#include "gui/place.h"
#include "gui/ui.h" /* generated from ui.txt by tools/uigen */
#include "gui/gui_process.h"
#include "gui/steps.h"
#include "gui/theme.h"
#include "common/utilities/window_icon.h"
#include "common/utilities/build_info.h"
#include "gui/recent.h"
#include "gui/clean.h"
#include "gui/settings.h"
#include "gui/splash.h"
#include "common/utilities/pad_sdl.h"
#include "common/utilities/pad_screen.h"
#include "common/utilities/users.h"
#include "common/utilities/setup.h"
#ifdef SDL_PLATFORM_ANDROID
#include "gui/android.h"
#endif
#ifdef SDL_PLATFORM_APPLE
#include "gui/apple.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif
/* the checkout the drivers were built from and its build tree, where an older version kept games/ and work/ */
#ifdef SNX_SOURCE_DIR
#define GUI_CHECKOUT SNX_SOURCE_DIR
#define GUI_BUILD    SNX_BUILD_DIR
#else
#define GUI_CHECKOUT NULL
#define GUI_BUILD    NULL
#endif
/* the space folders take, counted on a thread of its own: they may hold many files, on a slow card. The thread's until
   done is set; counted says how many have their bytes. One block: this, each one's bytes, its path's pointer, the
   paths */
#define GUI_SPACE_DIRS                                                                                                 \
    7 /* the data folder's recompiled/, work/, themes/; the XDG folders or the menu's; a picked work/ */
typedef struct {
    int count, counted, done, stop;
    bool games;    /* a file is a game an older version laid out beside the others: its _code.so, _data/... count */
    uint64_t used; /* all of them */
    uint64_t* bytes;
    char** dirs; /* "" none */
} GuiSpace;
/* the editor's screens: the Settings' groups, a group's settings, the App's rows, its game folders, the theme editor,
   the Controllers' button map, the pad drawn (pad_screen.h), the App's folders of data, games and extractions, and
   the User group's users */
enum {
    GUI_FORM_SETTINGS,
    GUI_FORM_GROUP,
    GUI_FORM_APP,
    GUI_FORM_FOLDERS,
    GUI_FORM_THEME,
    GUI_FORM_PAD,
    GUI_FORM_USERS
};
/* Settings > User's values: who the games play as, then each user's nickname as it is typed */
typedef struct {
    char plays[64];
    char nick[SNX_USERS_MAX][SNX_USERS_NICKNAME_MAX + 1];
} GuiUsersForm;
/* Settings > App > Folders' rows, in the order of their keys in app.ini's [folders] */
enum { GUI_PLACE_DATA, GUI_PLACE_GAMES, GUI_PLACE_WORK, GUI_PLACES };
#define GUI_GROUPS_MAX   32
#define GUI_FOLDERS_MAX  16    /* the player's game folders, past recompiled/ and dumps/ */
#define GUI_FOLDER_DEPTH 4     /* how deep the games of a folder kept with its subfolders are looked for */
#define GUI_FOLDER_DEEP  "/**" /* after such a folder's path, in the file of folders */
/* the library a folder of the player's is in, the word that starts its line in folders.txt: Games lists the games
   ready to run of the recompiled ones, the dumps of the dumps ones, and the CIAs (run as they are) of both */
enum { GUI_LIBRARY_RECOMPILED, GUI_LIBRARY_DUMPS, GUI_LIBRARIES };
static const char* const library_words[GUI_LIBRARIES] = {"recompiled", "dumps"};
/* What a row of Folders is: a place (Data, Extractions, the recompiled library's Main), dumps/ (the dumps library's
   Main), a folder of the player's (an Extra), or a library's Add a folder; by group, a box each */
typedef enum { GUI_ROW_PLACE, GUI_ROW_DUMPS, GUI_ROW_FOLDER, GUI_ROW_ADD } GuiFolderRow;
enum { GUI_GROUP_APP, GUI_GROUP_RECOMPILED, GUI_GROUP_DUMPS, GUI_FOLDER_GROUPS };
#define GUI_VIEW_ROWS    24    /* a group's rows shown as a view of them */
#define SNX_GUI_PLAYERS  8     /* Controllers' players, as the runtime's gamepad_p<N>_ keys */
/* Settings > App > Folders' rows, its last aside: the app's two, then each library's Main, its Extras and Add a folder */
#define GUI_FOLDER_ROWS  (GUI_PLACES + 1 + GUI_FOLDERS_MAX + 2)
/* the focused game's runtime JIT cache: none to show, no cache yet, one read, one unreadable */
enum { GUI_AOT_NONE, GUI_AOT_NO_CACHE, GUI_AOT_CACHE, GUI_AOT_BAD };
#define GUI_ICONS     96  /* the games' icons kept drawn: a page's rows and the recent ones, with room */
#define GUI_ICON_SIDE 128 /* the pixels square they are drawn at, scaled to their rows */
/* a game's icon as its rows show it: its program's path, and its texture; NULL for a game without one */
typedef struct {
    char* path;
    SDL_Texture* texture;
} GuiIcon;
/* the dumps whose icons the driver is asked for (-x --icon), on a thread of its own: reading a dump's Control NCA takes
   the keys and the disk, which the window's thread waits for neither. Games puts a dump in (dump_icon_ask); the thread
   asks the driver for each, a dump at a time, into its folder of the cache (dump_icon_dir), and counts the answers,
   which the menu takes to look at those rows again */
#define GUI_DUMP_ASKS 128 /* the dumps waiting at most: a page's and more */
typedef struct {
    SDL_Mutex* lock;
    SDL_Condition* wake;
    SDL_Thread* thread;
    char* driver; /* the program asked, and the data folder its keys are found by */
    char* repo;
    char* dumps[GUI_DUMP_ASKS]; /* each owned, with its folder in the cache beside it */
    char* dirs[GUI_DUMP_ASKS];
    int count, stop;
    unsigned answered; /* how many the driver answered, said or not */
    bool live;         /* the program's: a thread answers (the tests answer with dump_icons_pass) */
} GuiDumpIcons;
/* the App screen's values, as its rows show them, and Folders' (what each row's folder is, past its path) */
typedef struct {
    char theme[64], text[16], language[64], animations[16], side[16], places[48], moves[32];
    char folder[GUI_FOLDER_ROWS][64];
} GuiAppForm;
typedef struct App {
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* font;
    GuiProcess* process;
    GuiMenu menu;
    GuiBrowser browser;
    GuiGames games;
    GuiConsole console;
    GuiUsage usage;
    GuiTheme theme;
    GuiThemes themes;
    GuiSteps steps;
    GuiRecent recent;
    GuiSettings settings; /* read the first time a run or the Settings need them */
    char* settings_file;  /* the picks kept for every game (settings_path); NULL keeps none */
    char* app_file;       /* Settings > App's picks (app.ini), beside it; NULL keeps none */
    GuiSpace* space;      /* ScarletNX's folders, counted when the Settings open; its thread counter until joined */
    SDL_Thread* counter;
    GuiSpace* left_space; /* what older builds left, each one's, for their box */
    SDL_Thread* left_counter;
    uint64_t free_bytes; /* on the data folder's disk, when the count started */
    char* left;          /* what older builds left where this one no longer looks, a line each, lefts of them */
    int lefts;
    bool left_shown;   /* in a box as the menu opens, until a key, a button or a finger closes it */
    bool left_clear;   /* the menu's own files of menu_old among them: the box says how they go (SAY_LEFT_CLEAR) */
    char* menu_old;    /* Android: SDL's folder for the app, its own files, where an older version kept the menu's own
                          files (menu_files); NULL elsewhere */
    Uint64 left_at;    /* when they were looked for */
    char* recent_file; /* where the recent games are kept; NULL keeps none */
    char* run_log;     /* where a run's output goes as it comes, the last run's; NULL keeps none */
    FILE* run_out;
    int recent_named;           /* the run's own name went to its recent game */
    bool recent_unsaved;        /* the list could not be written the last time it changed: the message says so */
    Uint64 clock;               /* fixed time for tests; 0 is the real clock */
    Uint64 fade_ms, leaving_ms; /* a screen fading in from black since; the run fading out since */
    Uint64 entering_ms;         /* the menu fading out since, before a run fades in */
    char* self;    /* the program that runs Switch inputs: the runtime the menu is part of, NULL to look for one */
    char* drivers; /* the folder the other drivers are looked for in first */
    char *cwd, *file, *games_dir, *keys_path;
    char*
        data_dir; /* recompiled/, dumps/, work/ and themes/: the driver's too, which gets it as --repo (snx_data_dir) */
    char* checkout;       /* a build tree's menu's checkout (snx_checkout), where an older version kept games/ */
    char* themes_dirs[2]; /* themes/ in the data folder, then beside the GUI */
    char message[512];
    Uint64 keys_done; /* the keys screen's outcome shown since, 0 for none: then the menu by itself */
    bool keys_ok;
    int cols, rows, scale;
    int auto_scale; /* UI scale Auto's scale, chosen when the window opens: a resize keeps the letters as they were */
    Uint64 sampled;
    bool has_output, finished, aborting, abnormal_exit;
    bool log_due; /* the last run ended abnormally, not by ESC, and its log is not saved since: Share log blinks */
    char row_text[700]; /* the text of the row a list of ui.txt asked for last, until it is drawn (get_row_info) */
    GuiConfirm confirm; /* the question before a run is aborted, when ask_abort says to ask it */
    bool ask_abort;     /* ESC asks first: on a phone it is Back, an edge swipe easily made by mistake */
    int native;
    bool recompile;           /* R on Games: the run recompiles its game (-r) and plays nothing */
    /* B on Games: the focused game's JIT cache (aot.c), and the pack build it starts */
    int aot_state;       /* GUI_AOT_* */
    GuiAotInfo aot_info; /* the cache's header */
    char* aot_focus;     /* the focused game's path the state is for, owned */
    char aot_build_opt;  /* the launcher's opt setting, one char */
    int aot_match;         /* the cache's level is the build's */
    Uint64 aot_checked;    /* when aot_focus was looked at */
    bool aot_pack;         /* the run builds a pack from the cache */
    char* aot_cache;       /* its .jitcache, owned */
    char aot_opt[2];       /* the -O the build uses */
    int aot_hook;          /* --hook-direct-calls */
    Uint64 finger;            /* the finger on the screen (finger_id), 0 for none */
    float finger_x, finger_y; /* where it went down, 0 to 1 across the window */
    float finger_mark;        /* the height a drag last moved the focus at, a row at a time */
    bool pen_held;            /* a pen lifted: its hover leaves the focus where the lift put it until it moves away */
    float pen_x, pen_y;       /* where it lifted, 0 to 1 across the window */
    bool hovering;            /* a mouse or a pen over the window, at hover_x, hover_y of the render: a key of the foot
                                 under it that a click would press is drawn lit */
    float hover_x, hover_y;
    Uint32 picked;                    /* the event a file from the system's picker comes back as */
    SDL_Texture* splash;              /* the boot's splash, its drawing splash_side pixels square, until splash_until */
    const SnxSplashSvg* splash_svg;   /* the boot's pick: Wimsy, or one of dist/splash's */
    SnxSplashDrawing* splash_drawing; /* it, read once; NULL: the icon shows */
    int splash_side;
    /* the user at the head's right (place's badge), the one Settings' user names (users_read): 0 for none */
    char* users_dir; /* the data folder's users/ (open_files'); NULL shows none */
    int user;
    char user_name[SNX_USERS_NICKNAME + 1];
    unsigned char* user_jpeg; /* users/<uid>.jpg, user_jpeg_size bytes; NULL for Wimsy */
    size_t user_jpeg_size;
    SDL_Texture* user_picture; /* drawn user_side pixels square, in the shape it was drawn for */
    GuiIcon icons[GUI_ICONS];  /* the games' icons drawn so far (game_icon), the oldest replaced first */
    GuiDumpIcons dump_icons;   /* the dumps' icons asked of the driver */
    unsigned dump_icons_seen;  /* the answers the rows were drawn after */
    size_t icons_next;
    int user_side, user_shape;
    /* the box a click on the user opens (users_open): each user of users.ini, the focused one picked as Settings' user */
    bool users_shown;
    int users_focus;
    /* Settings > App's UI scale: the sizes in a box over its rows; a size picked replaces size_was until size_until,
       when it goes back unless kept, the question asking meanwhile in size_line */
    bool sizes_shown;
    GuiChoice sizes;
    int size_was;
    Uint64 size_until;
    char size_line[128];
    SDL_Texture* users_pictures[SNX_USERS_MAX]; /* theirs, users_side pixels square, made as the box is drawn */
    int users_side;
    /* the box opening a game with updates, DLCs or mods beside it shows (addons_open): a box to check each of them
       by, the game's addons= and mods_off= when it opened, then Play, which runs as recompile was when it opened */
    bool addons_shown, addons_recompile;
    int addons_focus;
    char *addons_game, *addons_title; /* the game's path and what it is shown as */
    GuiGames addons;
    bool* addons_checked; /* each of addons' */
    char *mods_folder, **mods; /* the game's mods folder and its mods (gui_game_mods), mods_count of them */
    size_t mods_count;
    bool* mods_on; /* each of mods': played, not in mods_off= */
    bool previewing; /* drawing the theme editor's preview, on a copy of the app put back after it */
    Uint64 splash_until;
    GuiRgb splash_page;
    bool editing; /* the editor's screen (form says which) instead of the menu's */
    GuiEditor editor;
    /* a press the colour picker took, until it lifts: its bar's channel + 1, or GUI_PICK_ROWS + 1 off the bars */
    int pick_press;
    int form;        /* GUI_FORM_* */
    int group;       /* the group open, or the one the Settings come back to */
    int first, page; /* a group's: the settings' row of its first, and of the page's row it opened (-1 none) */
    /* a group shown as a view of its rows, Controllers by player: each row's setting, and the player shown (0 every
       player's, 1 to SNX_GUI_PLAYERS) */
    GuiThemeKey view_keys[GUI_VIEW_ROWS];
    int view_rows[GUI_VIEW_ROWS], view_count, player;
    GuiThemeKey peek_keys[GUI_VIEW_ROWS]; /* and the focused group's, in the groups' screen */
    int peek_rows[GUI_VIEW_ROWS];
    int pad_map, pad_swap, pad_player; /* the button map's settings: its map's row and its swap's, for which player */
    int device_listen; /* a player's Controller row waiting for a press on the controller it takes, + 1; 0 for none */
    SDL_Gamepad* listened[16]; /* the controllers open meanwhile: a press comes only from an open one */
    int listened_count;
    bool theme_from_app;       /* the theme editor goes back to the App's rows */
    SnxPadScreen pad;          /* the button map's, on the row gamepad_map, A and B as gamepad_swap_ab says */
    SnxPadDrawing pad_drawing; /* as last drawn: what a click or a tap lands on */
    SDL_Gamepad* gamepad;      /* the pad it shows, the first one plugged in, while it is open */
    bool pads_read;            /* SDL's gamepads started for it */
    unsigned pad_pushed;       /* the triggers and the left stick's directions past half way, a bit each */
    /* the Settings' groups: App, then a group per section of the settings, its first row and rows; a row each */
    int groups, group_first[GUI_GROUPS_MAX], group_rows[GUI_GROUPS_MAX];
    GuiThemeKey group_keys[GUI_GROUPS_MAX];
    char group_name[GUI_GROUPS_MAX][64]; /* each one's key: a copy, the list it is from being freed apart */
    char group_about[GUI_GROUPS_MAX][128];
    GuiAppForm app_form;
    GuiUsersForm users_form;
    GuiThemeKey users_keys[SNX_USERS_MAX + 1]; /* Plays as, then a row a user */
    char users_label[SNX_USERS_MAX][32];
    int language;       /* Settings > App's Language: 0 Auto (the system's), else snx_gui_languages' index + 1 */
    char* games_key;    /* [folders] games = as app.ini had it, recompiled = before it was named so: written again */
    char* text_size_key; /* [app] text_size = as app.ini had it, ui_scale = before it was named so: written again */
    GuiCleaned cleaned; /* what it did, in a box over the screen while cleaned_shown, until a key closes it */
    bool cleaned_shown;
    bool report_move; /* the box shows what Move old files did, not Clean old settings */
    /* app.ini's animations: a screen comes in from the page's colour and a box's shade comes and goes, GUI_FADE_MS
       each; with it off (0) each shows at once */
    bool animations_off;
    int side_box;        /* app.ini's side_box (GuiSide): auto, on or off; F2 shows or hides it */
    unsigned screen_was; /* the screen drawn last (screen_mark), for when another comes */
    bool boxed_was;      /* a box over it then */
    Uint64 switch_ms, box_ms;
    Uint64 closing_ms; /* the short screen of what the menu keeps, shown as it closes, from then; 0 for none */
    bool look_only; /* scarletnx --migrate, =yes left out: nothing is written, app.ini's rewrite of an older key too */
    int files_top, files_shown; /* the box of files over the screen (render_files): its first file, and how many */
    char* folders[GUI_FOLDERS_MAX];     /* the player's game folders, listed after recompiled/ and dumps/ */
    bool folders_deep[GUI_FOLDERS_MAX]; /* each with its subfolders' games too */
    int folders_library[GUI_FOLDERS_MAX]; /* each one's GUI_LIBRARY_* */
    int adding;                           /* the library Add a folder adds to, -1: the one its games say */
    int asked_holds;                      /* folder_asked's libraries, 1 << GUI_LIBRARY_* */
    int folder_count;
    char* folder_asked; /* a folder picked with folders in it, asked whether their games are listed too */
    char* folders_file; /* where they are kept; NULL keeps none */
    GuiThemeKey folder_keys[GUI_FOLDER_ROWS];
    char* library_dumps;    /* dumps/ of the data folder, as Folders' row names it while it is open */
    int folder_off;         /* the game folder GUI_ASK_FOLDER asks about, its index in folders */
    uint64_t games_stamp;   /* what the game folders held when last counted (games_stamp) */
    Uint64 games_checked;   /* when they were last looked at */
    bool recent_all;        /* every recent game shows, not GUI_RECENT_SHOWN and a row for the rest */
    Uint32 picked_folder;   /* the event a folder from the system's picker comes back as */
    SDL_Texture* preview;   /* the editor's: the menu or a run drawn in its theme, at 1x */
    GuiSteps preview_steps; /* the run it shows, over again every GUI_PREVIEW_MS */
    GuiConsole preview_console;

    /* Settings > App > Folders */
    char* home_dir;          /* the data folder snx_data_dir picks: the menu's own files go by it, [folders] or not */
    char* place[GUI_PLACES]; /* app.ini's [folders]: data, games and work picked in Folders; NULL the default */
    char* work_dir;          /* the extractions: [folders] work, else the data folder's work/ in its cache */
    char place_about[GUI_PLACES][384];
    char place_line[GUI_PLACES][4200]; /* the folder in use, whole, under the rows */
    char places_note[3 * 4200];        /* the three, under App's rows */
    int place_row;                     /* the row the folder picker was opened for, -1 none */
    char* own_dir;      /* Android: the app's own storage; a folder outside it is asked about (GUI_ASK_OUTSIDE) */
    char* outside_path; /* the folder asked about, for the row outside_place */
    int outside_place;
    char* room_file; /* a run asked about (GUI_ASK_ROOM): its file, its load and whether it was prepared */
    int room_item;
    bool room_prepared, room_recompile;
    bool room_ok;       /* asked and answered: the run goes */
    GuiChoice dests;    /* Recompile: which of the recompiled library's folders a dump's game goes in */
    bool dests_shown;
    char* dest_file; /* the run asked about, as room_file, and its load */
    int dest_item;
    bool dest_prepared, dest_recompile;
    char* dest_out;     /* the folder picked there: the run's --outdir, until it starts or is not run */
    char room_line[96]; /* the question: what it takes, what is free */
    char ask_line[4400]; /* GUI_ASK_CLEAN's to GUI_ASK_FOLDER's question, with what the change takes or where */
    char error_line[4400];    /* GUI_ASK_ERROR's: the failure, whole */
    uint32_t errors_said[16]; /* the failures it said in the session, by their text's FNV-1a: each said once */
    int errors_count;
    int place_off;       /* the folder GUI_ASK_PLACE puts back to its default */
    uint64_t room_free; /* fixed free space for tests; 0 asks the disk */

    Uint64 preview_since;

    /* the setup (common/utilities/setup.h) in place of the menu while setup_on, a page at a time: the notices, then
       each step setup.ini has left */
    bool setup_on;
    SnxSetup setup; /* setup.ini as it was read, a newer one's numbers kept when it is written */
    int setup_pages[SNX_SETUP_STEPS + 1], setup_count, setup_page;
    GuiChoice setup_choice; /* the page's choices, the one in focus */
    int setup_top;          /* the notice shown, one a page: the last one's OK goes on */
    int setup_language;     /* Settings > App's Language as the setup found it, 0 Auto: kept Auto on the system's */
    int setup_row;          /* the Appearance page's focus, a GUI_APPEARANCE_* row or its Next */
    bool setup_typing;      /* the first user's nickname being typed, setup_name, to go with setup_uid */
    char setup_name[SNX_USERS_NICKNAME + 1];
    uint64_t setup_uid[2];
    int setup_saves;    /* games with saves of the fixed uid, which setup_uid is then */
    bool setup_keys;    /* the folder picker opened for the keys' page */
    bool setup_failed;  /* the message says what did not work */
    bool setup_written; /* users.ini at the user's page: there, its users (-1: setup_why says why it is none) */
    int setup_users;
    char setup_why[512];
    bool setup_save;  /* the Appearance changed, kept in app.ini once done (a portable copy's, when that is picked) */
    bool setup_moved; /* the data folder a portable copy's: what older versions left is looked for again */
#ifdef SDL_PLATFORM_ANDROID
    bool hidden;     /* behind other apps: nothing is drawn, a run is still read */
    bool play_later; /* a game made ready while hidden, which opens once the menu shows */
    char told[340];  /* the run's notification: its step's line, its percent, when; 0 before its first */
    int told_percent;
    Uint64 told_ms;
#endif
} App;

/* the menu's item i: a row of ui.txt's Entries, in model.h's order (a negative size if not); its text shown
   translated */
#define GUI_ENTRY(item) (UI_MENU_ENTRIES_LOAD_SWITCH_NSP_XCI_NRO + (item))
typedef char gui_entries_in_order
    [GUI_ENTRY(GUI_LINUX) == UI_MENU_ENTRIES_LOAD_LINUX_ARM_ELF && GUI_ENTRY(GUI_3DS) == UI_MENU_ENTRIES_LOAD_3DS_CIA &&
             GUI_ENTRY(GUI_KEYS) == UI_MENU_ENTRIES_SETUP_DEVICE_KEYS &&
             GUI_ENTRY(GUI_SETTINGS) == UI_MENU_ENTRIES_SETTINGS && GUI_ENTRY(GUI_LOG) == UI_MENU_ENTRIES_SHARE_LOG &&
#ifdef SDL_PLATFORM_ANDROID
             GUI_ENTRY(GUI_DATA) == UI_MENU_ENTRIES_OPEN_DATA_FOLDER &&
#endif
             GUI_ENTRY(GUI_EXIT) == UI_MENU_ENTRIES_EXIT && GUI_ENTRY(GUI_ITEM_COUNT) == UI_MENU_ITEMS
         ? 1
         : -1];
static const char* entry_text(int item) { return ui_menu_texts + ui_menu_items[GUI_ENTRY(item)].text; }
#define GUI_RECENT_SHOWN 3 /* the recent games Games shows before a row that shows the rest */
/* what the screens say to press: a phone has a finger, the back button and the system's file picker, nothing to drop;
   a computer has the picker too (on Linux the desktop's portal, else zenity), on a screen of its own as the keys' */
#ifdef SDL_PLATFORM_ANDROID
#define SAY_MENU      SNX_TR_KEYS("Tap: select. Refresh: tap it. Back: go back.")
#define SAY_GAMES     SNX_TR_KEYS("Tap: open. Swipe: turn the page. Recompile: tap it. Refresh: tap it. Back: go back.")
#define SAY_GAMES_AOT SNX_TR_KEYS("AOT pack: tap it.")
#define SAY_KEYS_HOW   SNX_TR_KEYS("Back: go back.")
#define SAY_KEYS_WAIT  SNX_TR_NOOP("Pick prod.keys")
#define SAY_PICK_BOX   SNX_TR_NOOP("tap the box to open the file picker")
#define SAY_FILE_WAIT  SNX_TR_NOOP("Pick a file")
#define SAY_DONE       SNX_TR_NOOP("Tap to return to menu")
#define SAY_ABORT      SNX_TR_NOOP("Tap here to abort and return to menu")
#define SAY_NO_GAMES   SNX_TR_NOOP("No games in recompiled/ or dumps/: put a game's .nro, .xci or .nsp in dumps/.")
#define SAY_EDITOR     SNX_TR_KEYS("Tap a key to change, type or pick it. Back: go back.")
#define SAY_PICKER     SNX_TR_KEYS("Drag a bar, or tap #RRGGBB to type it. Tap outside: keep. Back: put back.")
#define SAY_SETTINGS   SAY_EDITOR
#define SAY_GROUPS     SNX_TR_KEYS("Tap a group to open it. Back: go back.")
#define SAY_APP        SNX_TR_KEYS("Tap a row to change or open it. Back: go back.")
#define SAY_FOLDERS    SNX_TR_KEYS("Add a folder: tap it. Tap an Extra to take it off. Refresh: tap it. Back: go back.")
#define SAY_PLACES     SNX_TR_KEYS("Tap a folder to pick another. Refresh: tap it. Back: go back.")
#define SAY_PAD            SNX_TR_KEYS("Tap a control; tap it again for a press. Back: go back.")
#define SAY_USERS          SNX_TR_KEYS("Tap a user to rename it. Back: go back.")
#else
#define SAY_MENU       SNX_TR_KEYS("Arrows/mouse: move. Space/Enter/click: select.")
#define SAY_GAMES                                                                                                      \
    SNX_TR_KEYS("Up/Down: select. Enter/Space: open. R: recompile. M: mods folder. Left/Right/PgUp/PgDown: page. "  \
                "F5: refresh.")
#define SAY_GAMES_AOT SNX_TR_KEYS("B: build AOT pack.")
#define SAY_PICK_BOX   SNX_TR_NOOP("click the box (or Enter) to open the file picker")
#define SAY_KEYS_HOW   SNX_TR_KEYS("Ctrl+V: paste a path, or drop the file on the window. ESC: back.")
#define SAY_KEYS_WAIT  SNX_TR_NOOP("Pick or drop prod.keys")
#define SAY_FILE_WAIT  SNX_TR_NOOP("Pick or drop a file")
#define SAY_DONE       SNX_TR_NOOP("Press [ESC] to return to menu")
#define SAY_ABORT      SNX_TR_NOOP("Press [ESC] to abort and return to menu")
#define SAY_NO_GAMES   SNX_TR_NOOP("No games in recompiled/, dumps/ or the libraries of Settings > App > Folders.")
#define SAY_EDITOR     SNX_TR_KEYS("Up/Down: key. Left/Right: change. Enter: edit. Ctrl+S: save. ESC: back.")
#define SAY_PICKER     SNX_TR_KEYS("Up/Down: row. Left/Right: 1. PgUp/PgDn: 16. Enter: keep. ESC: put back.")
#define SAY_SETTINGS   SNX_TR_KEYS("Up/Down: setting. Left/Right: change. Enter: type. Del: default. ESC: back.")
#define SAY_GROUPS     SNX_TR_KEYS("Up/Down: group. Enter: open. ESC: back.")
#define SAY_APP        SNX_TR_KEYS("Up/Down: row. Left/Right: change. Enter: change or open. ESC: back.")
#define SAY_FOLDERS                                                                                                    \
    SNX_TR_KEYS("Add a folder: pick, drop or paste one. Del: take an Extra off. F5: refresh. ESC: back.")
#define SAY_PLACES SNX_TR_KEYS("Up/Down: folder. Enter: pick another. Del: the default. F5: refresh. ESC: back.")
#define SAY_PAD    SNX_TR_KEYS("Arrows/pad: move, change. Enter/A: select. Del: the .ini's. ESC: back.")
#define SAY_USERS  SNX_TR_KEYS("Up/Down: user. Left/Right: plays as. Enter: type a nickname. ESC: back.")
#endif
/* a failure of the system's (a file not written, a picker that did not open): in a box over the screen, whole, until
   OK, once in the session; the tips or the note keep it after. Over a run or another question, those alone */
static void say_failure(App* app, const char* text) {
    uint32_t hash = 2166136261u;
    const char* c;
    int i;
    if (app->menu.running || app->finished || app->confirm.shown) return;
    for (c = text; *c; ++c) hash = (hash ^ (unsigned char)*c) * 16777619u;
    for (i = 0; i < app->errors_count && i < (int)SDL_arraysize(app->errors_said); ++i)
        if (app->errors_said[i] == hash) return;
    app->errors_said[app->errors_count++ % (int)SDL_arraysize(app->errors_said)] = hash;
    snprintf(app->error_line, sizeof app->error_line, "%s", text);
    gui_confirm_open(&app->confirm, GUI_ASK_ERROR, false);
}
static SDL_Color rgb(GuiRgb c) {
    SDL_Color out = {c.r, c.g, c.b, 255};
    return out;
}
static Uint64 now_ms(const App* app) { return app->clock ? app->clock : SDL_GetTicks(); }
#define GUI_KEYS_OK_MS     3000  /* how long the keys screen shows they were installed, then the menu */
#define GUI_KEYS_FAILED_MS 10000 /* and why they were not, the menu's line saying it after */
/* the keys screen's outcome, gone back to the menu once shown long enough; the seconds it still shows, 0 for none */
static int keys_left(App* app) {
    Uint64 shown = app->keys_ok ? GUI_KEYS_OK_MS : GUI_KEYS_FAILED_MS, now = now_ms(app);
    if (!app->keys_done || app->menu.view != GUI_LOAD_KEYS) {
        app->keys_done = 0;
        return 0;
    }
    if (now - app->keys_done >= shown) {
        app->keys_done = 0;
        app->menu.view = GUI_LOAD_NONE;
        return 0;
    }
    return (int)((shown - (now - app->keys_done) + 999) / 1000);
}
static void glyph(App* app, float x, float y, int c);
/* the glyphs a row of the font's texture: 512 pixels wide, so 8192 of them (CP437's, the Latin letters and 3780 wide
   ones) fit 2048 high, where 16 a row would take 8192 */
#define GUI_FONT_ROW 64
/* the font's glyphs: CP437's 256, then the Latin letters past it, then each wide letter's two halves
   (gui_menu_glyph) */
static SDL_Texture* create_font(SDL_Renderer* renderer) {
    int glyphs = 256 + gui_latin_count + 2 * gui_wide_count;
    SDL_Surface* surface =
        SDL_CreateSurface(GUI_FONT_ROW * 8, (glyphs + GUI_FONT_ROW - 1) / GUI_FONT_ROW * 16, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return NULL;
    for (int glyph = 0; glyph < glyphs; ++glyph) {
        const uint8_t* rows = glyph < 256                     ? gui_vga_font + glyph * 16
                              : glyph < 256 + gui_latin_count ? gui_latin_font + (glyph - 256) * 16
                                                              : gui_wide_font + (glyph - 256 - gui_latin_count) * 16;
        for (int y = 0; y < 16; ++y) {
            Uint32* line = (Uint32*)((char*)surface->pixels + ((glyph / GUI_FONT_ROW) * 16 + y) * surface->pitch);
            for (int x = 0; x < 8; ++x)
                line[(glyph % GUI_FONT_ROW) * 8 + x] = (rows[y] & (0x80 >> x)) ? 0xffffffffu : 0;
        }
    }
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (texture) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    }
    return texture;
}
/* the font's glyph c (gui_menu_glyph's, or a CP437 byte), its corner at x, y in pixels, w by h a column: a wide one's
   halves side by side */
static void glyph_sized(App* app, float x, float y, int c, float w, float h) {
    for (int half = 0; half < gui_glyph_cols(c); ++half) {
        SDL_FRect source = {(float)((c + half) % GUI_FONT_ROW * 8), (float)((c + half) / GUI_FONT_ROW * 16), 8, 16};
        SDL_FRect destination = {x + (float)half * w, y, w, h};
        SDL_RenderTexture(app->renderer, app->font, &source, &destination);
    }
}
static void glyph(App* app, float x, float y, int c) {
    glyph_sized(app, x, y, c, 8.0f * (float)app->scale, 16.0f * (float)app->scale);
}
/* text from a pixel position, cols columns of it at most (a wide letter that would cross them left out), alpha its
   opacity, cut at the last column; where it ends */
static float text_px(App* app, float x, float y, const char* value, size_t cols, SDL_Color color, float alpha) {
    size_t used = 0;
    float w = 8.0f * (float)app->scale, limit = (float)(app->cols - 1) * w;
    SDL_SetTextureColorMod(app->font, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(app->font, (Uint8)(alpha * 255));
    while (*value) {
        int c = gui_menu_glyph(gui_utf8_step(&value)), n = gui_glyph_cols(c);
        if (used + (size_t)n > cols) break;
        if (x + (float)n * w <= limit) glyph(app, x, y, c);
        used += (size_t)n;
        x += (float)n * w;
    }
    SDL_SetTextureAlphaMod(app->font, 255);
    return x;
}
/* text at a cell, a line too long for its row sliding as the theme says */
static void text(App* app, int x, int y, const char* value, SDL_Color color) {
    int skip = gui_slide(gui_text_cols(value) - (app->cols - 1 - x), now_ms(app), app->theme.scroll_ms);
    text_px(app, (float)(x * 8 * app->scale), (float)(y * 16 * app->scale), gui_cols_skip(value, skip), SIZE_MAX, color,
            1);
}
static int cell_h(const App* app) { return 16 * app->scale; }
static int cell_w(const App* app) { return 8 * app->scale; }
static void prompt_in(App* app, GuiRect r, int y, GuiPromptLine line, const char* value, SDL_Color color);
/* a line of the loading screen that waits on the user, on row y as the theme's prompt places it */
static void prompt(App* app, GuiPromptLine line, int x, int y, const char* value, SDL_Color color) {
    GuiRect r = gui_rect(x * cell_w(app), y * cell_h(app), (app->cols - 1 - x) * cell_w(app), cell_h(app));
    prompt_in(app, r, r.y, line, value, color);
}
/* the part of the window no system bar or camera covers, in the renderer's pixels: everything is drawn in it */
static void set_area(App* app) {
    SDL_Rect r;
    int ww, wh, ow, oh;
    SDL_GetWindowSize(app->window, &ww, &wh);
    SDL_GetRenderOutputSize(app->renderer, &ow, &oh);
    if (ww > 0 && wh > 0 && SDL_GetWindowSafeArea(app->window, &r)) {
        r.x = r.x * ow / ww;
        r.w = r.w * ow / ww;
        r.y = r.y * oh / wh;
        r.h = r.h * oh / wh;
        SDL_SetRenderViewport(app->renderer, &r);
    } else
        SDL_SetRenderViewport(app->renderer, NULL);
}
static void fill(App* app, float x, float y, float w, float h, GuiRgb c, Uint8 alpha) {
    SDL_FRect r = {x, y, w, h};
    SDL_SetRenderDrawBlendMode(app->renderer, alpha == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app->renderer, c.r, c.g, c.b, alpha);
    SDL_RenderFillRect(app->renderer, &r);
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_NONE);
}
static void area(const App* app, int* width, int* height) {
    SDL_Rect r;
    SDL_GetRenderViewport(app->renderer, &r);
    *width = r.w;
    *height = r.h;
}
/* rows of the whole width, as a band behind text: on to the window's edges, over the camera's cutout beside the
   text and what is left below the last row */
static void band(App* app, int row, int rows, GuiRgb c) {
    SDL_Rect safe;
    int width, height, bottom = row + rows == app->rows && app->rows < GUI_MAX_ROWS;
    float top;
    SDL_GetRenderViewport(app->renderer, &safe);
    SDL_GetRenderOutputSize(app->renderer, &width, &height);
    top = row ? (float)(safe.y + row * 16 * app->scale) : 0;
    SDL_SetRenderViewport(app->renderer, NULL);
    fill(app, 0, top, (float)width, (bottom ? (float)height : (float)(safe.y + (row + rows) * 16 * app->scale)) - top,
         c, 255);
    SDL_SetRenderViewport(app->renderer, &safe);
}
/* a line between two rows, across the text area */
static void rule(App* app, int row, GuiRgb c) {
    float s = (float)app->scale;
    fill(app, 8 * s, (float)(row * 16) * s - s, (float)((app->cols - 2) * 8) * s, s, c, 255);
}
static void event(App* app, const SDL_Event* e);
#ifdef SNX_XDG
/* ~/.switch/prod.keys, where hactool and other tools keep them */
static char* home_keys_path(void) {
    const char* home = getenv("HOME");
    if (!home || !home[0]) return NULL;
    char* directory = snx_path_join(home, ".switch");
    char* path = directory ? snx_path_join(directory, "prod.keys") : NULL;
    free(directory);
    return path;
}
/* the menu whose folder is the user's data folder (snx_data_dir), not a portable copy's */
static bool installed(const App* app) {
    char* user = snx_user_data_dir();
    bool same = user && app->data_dir && !strcmp(user, app->data_dir);
    free(user);
    return same;
}
#endif
/* the menu's own files: those of its config folder, then those of its state folder */
static const char* const menu_files[2][4] = {{"settings.ini", "app.ini", "folders.txt", "gamecontrollerdb.txt"},
                                             {"recent.txt", "last-run.log", "", ""}};
/* the folder of kind for the menu's own files (menu_files), made: its data folder's (snx_app_dir: the user's, a
   portable copy's, Android's in the app's own storage), else the user's. Made with make; NULL when none can be */
static char* menu_folder(const App* app, int kind, bool make) {
    char* folder = snx_menu_dir(app->home_dir ? app->home_dir : app->data_dir, kind);
    if (folder && make && snx_mkdirs(folder)) {
        free(folder);
        folder = NULL;
    }
    return folder;
}
/* where the keys are installed (snx_keys_dir): the user's data folder on an XDG system, else ~/.switch */
static char* device_keys_path(const App* app) {
    char *dir = snx_keys_dir(app->data_dir), *path = dir ? snx_path_join(dir, "prod.keys") : NULL;
    free(dir);
    return path;
}
/* the prod.keys the driver reads: those installed here, else with the user's data folder the ones in ~/.switch */
static char* keys_in_use(const App* app) {
    if (app->keys_path && snx_is_file(app->keys_path)) return snx_strdup(app->keys_path);
#ifdef SNX_XDG
    char* home = installed(app) ? home_keys_path() : NULL;
    if (home && snx_is_file(home)) return home;
    free(home);
#endif
    return NULL;
}
/* the footer from row: the usage, then the working folder, which recompiled/ and themes/ are found in. The theme's bar
   style puts them on a band of ink over rows rows, its plain one in soft and ink; a rule above when it says so */
/* what ScarletNX's folders take, counted on a thread, and what is left on their disk; "" before the first count */
static void disk_text(const App* app, char* out, size_t size) {
    char used[32], left[32];
    out[0] = 0;
    if (!app->space) return;
    gui_size_text(left, sizeof left, app->free_bytes);
    if (__atomic_load_n(&app->space->done, __ATOMIC_ACQUIRE)) {
        gui_size_text(used, sizeof used, app->space->used);
        snprintf(out, size, snx_tr("Disk: %s, %s free"), used, left);
    } else
        snprintf(out, size, snx_tr("Disk: counting, %s free"), left);
}
static void footer(App* app, int row, int rows, GuiRgb page, GuiRgb ink, GuiRgb soft) {
    char usage[256], disk[96];
    int bar = app->theme.footer == GUI_STYLE_BAR;
    size_t n;
    if (bar) band(app, row, rows, ink);
    if (app->theme.footer_rule) rule(app, row, soft);
    gui_usage_text(usage, sizeof(usage), app->usage.available, app->usage.memory_bytes, app->usage.cpu_percent,
                   SDL_GetNumLogicalCPUCores());
    disk_text(app, disk, sizeof disk);
    n = strlen(usage);
    if (disk[0]) snprintf(usage + n, sizeof usage - n, "  %s", disk);
    text(app, 1, row, usage, rgb(bar ? page : soft));
    text(app, 1, row + 1, snx_tr("CWD:"), rgb(bar ? page : soft));
    text(app, 2 + gui_text_cols(snx_tr("CWD:")), row + 1, app->cwd ? app->cwd : snx_tr("(unavailable)"),
         rgb(bar ? page : ink));
}
/* the screen a file is picked or dropped on: the keys', and a load option's */
static int pick_screen(const App* app) { return app->menu.view == GUI_LOAD_KEYS || app->menu.view == GUI_LOAD_DROP; }
/* the menu's screen, a run or not */
static GuiScreen menu_screen(const App* app) {
    return app->menu.view == GUI_LOAD_GAMES  ? GUI_SCREEN_GAMES
           : app->menu.view == GUI_LOAD_KEYS ? GUI_SCREEN_KEYS
                                             : GUI_SCREEN_MENU;
}
static GuiScreen screen(const App* app) {
    if (app->menu.running || app->finished)
        return (app->native && !gui_steps_last(&app->steps, "RECOMPILE")) || gui_steps_last(&app->steps, "LAUNCH")
                   ? GUI_SCREEN_LOADING
                   : GUI_SCREEN_COMPILING;
    return menu_screen(app);
}
/* the theme of that id; what its theme.ini has that it does not take said in the open form's note, else in the
   message: added to it as the menu opens (add), else in its place, so a theme that takes all says nothing */
static void theme_load(App* app, const char* id, bool add) {
    char said[sizeof app->message];
    gui_theme_load(&app->theme, (const char* const*)app->themes_dirs, 2, id, said, sizeof said);
    if (app->editing) {
        if (said[0]) snprintf(app->editor.note, sizeof app->editor.note, "%s", said);
    } else if (add) {
        size_t m = strlen(app->message);
        if (said[0]) snprintf(app->message + m, sizeof app->message - m, "%s%s", m ? " " : "", said);
    } else
        snprintf(app->message, sizeof app->message, "%s", said);
    if (said[0] && app->setup_on) app->setup_failed = true; /* the setup's foot in the failure colour */
}
/* the theme step themes after or before the one shown, round the list */
static void next_theme(App* app, int step) {
    size_t i;
    const char* const* dirs = (const char* const*)app->themes_dirs;
    gui_themes_scan(&app->themes, dirs, 2);
    i = 0;
    while (i < app->themes.count && strcmp(app->themes.ids[i], app->theme.id)) ++i;
    if (app->themes.count) theme_load(app, app->themes.ids[(i + app->themes.count + step) % app->themes.count], false);
}
/* app.ini's [folders] keys, in the order of Settings > App > Folders' rows */
static const char* const place_names[GUI_PLACES] = {"data", "recompiled", "work"};
static const char* const place_labels[GUI_PLACES] = {SNX_TR_NOOP("Data"), SNX_TR_NOOP("Recompiled"),
                                                     SNX_TR_NOOP("Extractions")};
/* side_box's words in app.ini, GuiSide's order, and as Settings > App shows them */
static const char* const side_words[] = {"auto", "on", "off"};
static const char* const side_names[] = {SNX_TR_NOOP("Auto"), SNX_TR_NOOP("On"), SNX_TR_NOOP("Off")};
/* ui_scale's quarters of Auto (-1 to -3) as app.ini names them */
static const char* const text_quarters[] = {"0.25", "0.5", "0.75"};
/* Settings > App's picks, kept at once in app_file for the next start. The App's note, or the menu's message, says when
   they could not be; 0, else nonzero */
static int app_save(App* app) {
    char *text, *dir, *staged;
    size_t size = 256;
    int n, failed, i;
    if (!app->app_file || app->look_only) return 0;
    for (i = 0; i < GUI_PLACES; ++i) size += 16 + (app->place[i] ? strlen(app->place[i]) : 0);
    text = (char*)snx_alloc(size);
    n = snprintf(text, size, "; Settings > App in the launcher, as they were left\n[app]\ntheme = %s\n", app->theme.id);
    if (app->menu.text_size > 0)
        n += snprintf(text + n, size - (size_t)n, "ui_scale = %d\n", app->menu.text_size);
    else if (app->menu.text_size < 0)
        n += snprintf(text + n, size - (size_t)n, "ui_scale = %s\n", text_quarters[-app->menu.text_size - 1]);
    else
        n += snprintf(text + n, size - (size_t)n, "ui_scale = auto\n");
    n += snprintf(text + n, size - (size_t)n, "language = %s\n",
                  app->language ? snx_gui_languages[app->language - 1].code : "auto");
    n += snprintf(text + n, size - (size_t)n, "animations = %d\n", !app->animations_off);
    n += snprintf(text + n, size - (size_t)n, "side_box = %s\n", side_words[app->side_box]);
    /* Folders' section once one is picked; an empty key is the default */
    if (app->place[GUI_PLACE_DATA] || app->place[GUI_PLACE_GAMES] || app->place[GUI_PLACE_WORK]) {
        n += snprintf(text + n, size - (size_t)n, "[folders]\n");
        for (i = 0; i < GUI_PLACES; ++i)
            n += snprintf(text + n, size - (size_t)n, "%s = %s\n", place_names[i], app->place[i] ? app->place[i] : "");
    }
    /* staged beside it, then put in place: a crash mid-write leaves the old picks */
    dir = snx_dirname(app->app_file);
    staged = snx_format("%s.new", app->app_file);
    failed = !dir || !staged || snx_mkdirs(dir) || snx_write_file(staged, text, (size_t)n) ||
             snx_replace(staged, app->app_file);
    if (failed && staged) remove(staged);
    if (failed && app->editing) {
        snprintf(app->editor.note, sizeof app->editor.note,
                 snx_tr("Not kept for the next start: %s could not be written"), app->app_file);
        say_failure(app, app->editor.note);
    } else if (failed) {
        snprintf(app->message, sizeof app->message, snx_tr("Not kept for the next start: %s could not be written"),
                 app->app_file);
        say_failure(app, app->message);
    }
    free(dir);
    free(staged);
    free(text);
    return failed;
}
static void layout(App* app);
static void places_apply(App* app);
static size_t games_listed(const App* app);
/* the language of the system's locales, the most wanted first, that the menu has; English for none */
static int language_auto(void) {
    char names[8][32];
    const char* list[8];
    int count = 0, i, n = 0;
    SDL_Locale** locales = SDL_GetPreferredLocales(&count);
    for (i = 0; locales && i < count && n < 8; ++i) {
        snprintf(names[n], sizeof names[n], "%s%s%s", locales[i]->language, locales[i]->country ? "_" : "",
                 locales[i]->country ? locales[i]->country : "");
        list[n] = names[n];
        ++n;
    }
    SDL_free(locales);
    return snx_tr_pick(list, n);
}
/* the language Settings > App picked, or Auto's, in use */
static void language_apply(App* app) { snx_tr_use(app->language ? app->language - 1 : language_auto()); }
/* what app_save kept, read: applied as the menu opens (apply), else only looked at. A value the menu does not know, or a
   theme no longer there, leaves what the menu has, named in bad as key = value; how many. A language named in another
   form of its tag (pt_BR, which builds before the tags wrote) is that language, the form read kept in renamed */
static int app_read(App* app, bool apply, char* bad, size_t size, char* renamed, size_t renamed_size) {
    size_t bytes = 0, i;
    char *text = app->app_file ? (char*)snx_read_file(app->app_file, &bytes) : NULL, *key, *value;
    char* places[GUI_PLACES] = {NULL, NULL, NULL};
    const char* const* dirs = (const char* const*)app->themes_dirs;
    int scale = app->menu.text_size, language = app->language, n = 0, count = 0, moved = 0;
    bool still = app->animations_off;
    int side = app->side_box;
    SnxIni ini;
    if (size) bad[0] = 0;
    if (renamed_size) renamed[0] = 0;
    if (!text) return 0;
    snx_ini_start(&ini, text);
    while (snx_ini_next(&ini, &key, &value)) {
        char* end = NULL;
        long number = strtol(value, &end, 10);
        int place = GUI_PLACES;
        if (!strcmp(ini.section, "folders")) {
            for (place = 0; place < GUI_PLACES && strcmp(key, place_names[place]); ++place) {}
            if (place == GUI_PLACES && !strcmp(key, "games")) { /* recompiled = as an older version named it */
                place = GUI_PLACE_GAMES;
                if (apply) {
                    free(app->games_key);
                    app->games_key = snx_strdup(value);
                }
            }
        }
        if (place < GUI_PLACES && (!*value || snx_path_is_full(value))) { /* nothing is the default */
            free(places[place]);
            places[place] = *value ? snx_strdup(value) : NULL;
            continue;
        } else if (!strcmp(ini.section, "app") && !strcmp(key, "theme")) {
            gui_themes_scan(&app->themes, dirs, 2);
            for (i = 0; i < app->themes.count && strcmp(app->themes.ids[i], value); ++i) {}
            if (i < app->themes.count) {
                if (apply) theme_load(app, value, true);
                continue;
            }
        } else if (!strcmp(ini.section, "app") && (!strcmp(key, "ui_scale") || !strcmp(key, "text_size"))) {
            int q, taken = 1;
            for (q = 0; q < 3 && strcmp(value, text_quarters[q]); ++q) {}
            if (!strcmp(value, "auto"))
                scale = 0;
            else if (q < 3)
                scale = -(q + 1);
            else if (*value && !*end && number >= 1 && number <= GUI_TEXT_MAX)
                scale = (int)number;
            else
                taken = 0;
            if (taken && apply && !strcmp(key, "text_size")) { /* ui_scale = as an older version named it */
                free(app->text_size_key);
                app->text_size_key = snx_strdup(value);
            }
            if (taken) continue;
        } else if (!strcmp(ini.section, "app") && !strcmp(key, "side_box")) {
            int w;
            for (w = 0; w < 3 && strcmp(value, side_words[w]); ++w) {}
            if (w < 3) {
                side = w;
                continue;
            }
        } else if (!strcmp(ini.section, "app") && !strcmp(key, "animations")) {
            if (!strcmp(value, "0") || !strcmp(value, "1")) {
                still = !strcmp(value, "0");
                continue;
            }
        } else if (!strcmp(ini.section, "app") && !strcmp(key, "language")) {
            int found = snx_tr_find(value);
            if (!strcmp(value, "auto") || found >= 0) {
                language = found + 1;
                if (found >= 0 && strcmp(value, snx_gui_languages[found].code) && renamed_size)
                    snprintf(renamed, renamed_size, "%s", value);
                continue;
            }
        }
        ++count;
        if (n) n += snprintf(bad + n, size - (size_t)n, ", ");
        if (n < (int)size && strcmp(ini.section, "app")) /* a key outside [app] says where it is */
            n += snprintf(bad + n, size - (size_t)n, "[%s] ", ini.section);
        if (n < (int)size) n += snprintf(bad + n, size - (size_t)n, "%s = %s", key, value);
        if (n >= (int)size) n = (int)size - 1;
    }
    free(text);
    if (apply && scale != app->menu.text_size) {
        app->menu.text_size = scale;
        layout(app);
    }
    if (apply && language != app->language) {
        app->language = language;
        language_apply(app);
    }
    if (apply) app->animations_off = still;
    if (apply && side != app->side_box) {
        app->side_box = side;
        layout(app);
    }
    for (i = 0; i < GUI_PLACES; ++i) {
        if (apply && (places[i] ? !app->place[i] || strcmp(places[i], app->place[i]) : app->place[i] != NULL)) {
            free(app->place[i]);
            app->place[i] = places[i];
            places[i] = NULL;
            moved = 1;
        }
        free(places[i]);
    }
    if (moved) places_apply(app);
    return count;
}
/* app.ini as the menu opens, what it does not take named in the message */
static void app_load(App* app) {
    char bad[384], renamed[64];
    app_read(app, true, bad, sizeof bad, renamed, sizeof renamed);
    /* an older form of the language's tag: the tag in its place, the file's other lines as they were, said once */
    if (renamed[0] && app->language) {
        const char* code = snx_gui_languages[app->language - 1].code;
        char why[512];
        size_t m = strlen(app->message);
        snprintf(app->message + m, sizeof app->message - m, "%s", m ? " " : "");
        m = strlen(app->message);
        if (snx_ini_put(app->app_file, "app", "language", code, why, sizeof why))
            snprintf(app->message + m, sizeof app->message - m, snx_tr("%s: language = %s read as %s, not written: %s"),
                     app->app_file, renamed, code, why);
        else
            snprintf(app->message + m, sizeof app->message - m, snx_tr("%s: language = %s is now %s"), app->app_file,
                     renamed, code);
    }
    if (bad[0]) {
        size_t m = strlen(app->message);
        snprintf(app->message + m, sizeof app->message - m, "%s", m ? " " : "");
        m = strlen(app->message);
        snprintf(app->message + m, sizeof app->message - m, snx_tr("Not used from %s: %s"), app->app_file, bad);
    }
    /* [folders] games = and [app] text_size =, the names an older version wrote: written again as recompiled = and
       ui_scale =, each said once */
    if (app->games_key || app->text_size_key) {
        char said[sizeof app->message];
        int failed;
        snprintf(said, sizeof said, "%s", app->message);
        failed = app_save(app);
        if (!failed && app->games_key) {
            size_t m = strlen(said);
            snprintf(said + m, sizeof said - m, "%s", m ? " " : "");
            m = strlen(said);
            snprintf(said + m, sizeof said - m, snx_tr("%s: [folders] games = %s is now recompiled = %s"),
                     app->app_file, app->games_key, app->games_key);
        }
        if (!failed && app->text_size_key) {
            size_t m = strlen(said);
            snprintf(said + m, sizeof said - m, "%s", m ? " " : "");
            m = strlen(said);
            snprintf(said + m, sizeof said - m, snx_tr("%s: text_size = %s is now ui_scale = %s"), app->app_file,
                     app->text_size_key, app->text_size_key);
        }
        if (!failed) snprintf(app->message, sizeof app->message, "%s", said);
        free(app->games_key);
        app->games_key = NULL;
        free(app->text_size_key);
        app->text_size_key = NULL;
    }
}
/* a list that could not be kept: the message says so, after what it says already, until it is */
static void recent_note(App* app) {
    char note[sizeof app->message], *at;
    size_t n = strlen(app->message);
    if (!app->recent_file) return;
    snprintf(note, sizeof note, snx_tr("Recent games not kept: %s could not be written."), app->recent_file);
    at = strstr(app->message, note);
    if (app->recent_unsaved && !at)
        snprintf(app->message + n, sizeof app->message - n, "%s%s", n ? " " : "", note);
    else if (!app->recent_unsaved && at) { /* with the space that set it apart */
        size_t cut = strlen(note);
        if (at > app->message)
            --at, ++cut;
        else if (at[cut] == ' ')
            ++cut;
        memmove(at, at + cut, strlen(at + cut) + 1);
    }
}
/* the recent games shown, and whether a row under them shows the rest */
static int recent_shown(const App* app) {
    return app->recent_all ? app->recent.count : SDL_min(app->recent.count, GUI_RECENT_SHOWN);
}
static int recent_more(const App* app) { return recent_shown(app) < app->recent.count; }
/* the rows over Games' games: Add a folder, then the recent games and the row that shows the rest */
static size_t games_head(const App* app) { return 1 + (size_t)(recent_shown(app) + recent_more(app)); }
/* the list as the menu and the file have it */
static void recent_changed(App* app) {
    app->browser.head = games_head(app);
    gui_browser_resize(&app->browser, app->browser.page_size);
    if (app->recent_file) app->recent_unsaved = gui_recent_save(&app->recent, app->recent_file) != 0;
    recent_note(app);
}
static void recent_open(App* app, char* file) {
    free(app->recent_file);
    app->recent_file = file;
    gui_recent_load(&app->recent, file);
    app->browser.head = games_head(app);
}
static void menu_label(const App* app, int item, char* label, size_t size) {
    snprintf(label, size, "%s%s", app->theme.marker, snx_tr(entry_text(item)));
}
/* still the menu, fading out, while a run starts */
static int entering(const App* app) {
    return app->entering_ms && now_ms(app) < app->entering_ms + app->theme.fade_out_ms;
}
/* the rows Auto keeps at the least: a phone held wide is short, and its menu needs room for its items */
#ifdef SDL_PLATFORM_ANDROID
#define GUI_AUTO_ROWS 20
#else
#define GUI_AUTO_ROWS 16
#endif
/* UI scale Auto's scale: twice the display's own (Android's dpi / 160), so a computer at 100% gets 2x and a phone's
   letters come out as big to the eye, as long as rows rows fit in a window height pixels high */
static int pick_auto_scale(float display_scale, int height, int rows) {
    int scale = SDL_max(1, (int)(2.0f * display_scale));
    while (scale > 1 && height / (16 * scale) < rows) --scale;
    return scale;
}
/* a list of boxes in a column of the screen: its box, the cursor's margin before the boxes and the arrows' column after
   them when entries are hidden, each box's height and the pitch from one to the next, the first shown and how many */
typedef struct {
    GuiRect at;
    int count, focus, margin, arrows, item, pitch, first, shown;
} GuiList;
/* what a screen has besides its title, list and foot: the status box (unless the footer's style is none), the tabs
   (a run has none) */
enum { GUI_SHOWS_STATUS = 1, GUI_SHOWS_TABS = 2 };
static void place_screen(App* app, GuiPlace* p, const char* keys, int list_cols, int shows);
static const char* shown_keys(const App* app, char* out, size_t size);
static void aot_status_update(App* app);
/* a screen's keys, Tab's after them */
#define SAY_TAB SNX_TR_KEYS("Tab: screen. F2: side box.")
static const char* screen_keys(const App* app, const char* keys, char* out, size_t size) {
    int aot = app->menu.view == GUI_LOAD_GAMES &&
              (app->aot_state == GUI_AOT_CACHE || app->aot_state == GUI_AOT_BAD);
#ifdef SDL_PLATFORM_ANDROID
    if (aot) {
        snprintf(out, size, "%s %s", snx_tr(keys), snx_tr(SAY_GAMES_AOT));
        return out; /* a tap on a tab */
    }
    return snx_tr(keys);
#else
    if (aot) {
        snprintf(out, size, "%s %s %s", snx_tr(keys), snx_tr(SAY_GAMES_AOT), snx_tr(SAY_TAB));
        return out;
    }
    snprintf(out, size, "%s %s", snx_tr(keys), snx_tr(SAY_TAB));
    return out;
#endif
}
static void games_place(App* app, GuiPlace* p) {
    size_t i;
    int longest = gui_text_cols(snx_tr("Recent games"));
    char keys[256];
    for (i = 0; i < app->games.count; ++i) longest = SDL_max(longest, gui_text_cols(app->games.shown[i]));
    for (i = 0; i < (size_t)recent_shown(app); ++i) longest = SDL_max(longest, gui_text_cols(app->recent.game[i].name));
    place_screen(app, p, shown_keys(app, keys, sizeof keys),
                 longest + 2 + gui_text_cols(app->theme.marker) + gui_text_cols(app->theme.cursor),
                 GUI_SHOWS_STATUS | GUI_SHOWS_TABS);
}
/* a list entry's box: its text under a frame's width, the frame at its foot over the font's last row, which is empty,
   so many fit; its text's row in it */
static int item_h(const App* app) { return cell_h(app) + app->scale; }
/* the boxes spread over the column, the theme's gap at most between two */
static void list_place(const App* app, GuiList* l, GuiRect at, int count, int focus) {
    int s = app->scale, gap = (int)app->theme.gap;
    l->at = at;
    l->count = count;
    l->focus = focus;
    l->margin = app->theme.cursor[0] ? (gui_text_cols(app->theme.cursor) * cell_w(app)) : 0;
    l->item = item_h(app);
    l->pitch = gui_place_pitch(at.h, count, l->item + SDL_min(2, gap) * s, l->item + gap * s);
    l->shown = gui_place_list(at.h, l->item, l->pitch, count, focus, app->theme.scroll == GUI_SCROLL_CENTER, &l->first);
    l->arrows = l->shown < count ? cell_w(app) : 0;
}
/* the list's entries before the page's games: "Recent games" and the recent rows, none without recent games */
static int games_lead(const App* app) { return app->browser.head > 1 ? (int)app->browser.head : 0; }
/* the games a page of the boxed list holds: those that fit under Add a folder and the recent games */
static size_t games_page(App* app) {
    GuiPlace p;
    GuiRect rects[UI_GAMES_ITEMS];
    games_place(app, &p);
    ui_games_layout(p.list, app->scale, rects);
    return (size_t)SDL_max(1, (int)ui_games_shown(rects, app->scale, UI_GAMES_ROWS) - games_lead(app));
}
/* a box head's height: its label, two pixels of the text above and below */
static int head_h(const App* app) { return cell_h(app) + 4 * app->scale; }
/* r without a column at either side: where a box's text goes */
static GuiRect inner(const App* app, GuiRect r) { return gui_rect(r.x + cell_w(app), r.y, r.w - 2 * cell_w(app), r.h); }
/* the output box's cells under its head */
static void run_console(App* app, const GuiPlace* p, int* cols, int* rows) {
    GuiRect r = inner(app, p->list);
    *cols = SDL_max(1, r.w / cell_w(app));
    *rows = SDL_max(1, (p->list.h - head_h(app) - 4 * app->scale) / cell_h(app));
}
/* ---- a run of the console layout: the driver's output in the wide box, beside it what runs, its file and how it
   ended, the status under them; the file in the tips, the ESC line where the keys go, as the theme's prompt frames it */
static void run_place(App* app, GuiPlace* p) {
    int width, height;
    area(app, &width, &height);
    place_screen(app, p, snx_tr(SAY_ABORT), width / cell_w(app), GUI_SHOWS_STATUS); /* its longest ESC line */
}
static void layout(App* app) {
    int run;
    int width, height, whole_width, whole_height;
    set_area(app);
    area(app, &width, &height);
    /* Auto's letters are the opening window's, measured whole: a phone's system bars may still show over it then, and
       the safe area be short. The window then keeps room for 16 rows of them */
    if (!app->auto_scale && SDL_GetWindowSizeInPixels(app->window, &whole_width, &whole_height) && whole_height > 0) {
        app->auto_scale = pick_auto_scale(SDL_GetWindowDisplayScale(app->window), whole_height, GUI_AUTO_ROWS);
        SDL_SetWindowMinimumSize(
            app->window, 800,
            SDL_max(480, (int)SDL_ceilf(256.0f * (float)app->auto_scale / SDL_GetWindowPixelDensity(app->window))));
    }
    app->scale = app->menu.text_size > 0   ? app->menu.text_size
                 : app->menu.text_size < 0 ? SDL_max(1, (app->auto_scale * -app->menu.text_size + 2) / 4)
                                           : app->auto_scale;
    /* as long as 16 rows still fit: Auto's always do on a computer, a phone's not while Fullscreen is off */
    while (app->scale > 1 && height / (16 * app->scale) < 16) --app->scale;
    if (app->scale < 1) app->scale = 1;
    app->cols = SDL_min(width / (8 * app->scale), GUI_MAX_COLS);
    app->rows = SDL_min(height / (16 * app->scale), GUI_MAX_ROWS);
    run = (app->menu.running || app->finished) && !entering(app);
    gui_browser_resize(&app->browser,
                       app->menu.view == GUI_LOAD_GAMES ? games_page(app) : (size_t)SDL_max(1, app->rows - 10));
    if (run && app->theme.run != GUI_RUN_STEPS) { /* the run's output box */
        GuiPlace p;
        int cols, rows;
        run_place(app, &p);
        run_console(app, &p, &cols, &rows);
        gui_console_resize(&app->console, cols, rows);
    }
}
/* an arc of a circle, from and sweep in degrees, thickness pixels inward */
static void arc(App* app, float cx, float cy, float r, float from, float sweep, float thickness, GuiRgb c) {
    float t;
    SDL_FPoint p[49];
    SDL_SetRenderDrawColor(app->renderer, c.r, c.g, c.b, 255);
    for (t = 0; t < thickness; t += 1.0f) {
        int i;
        for (i = 0; i <= 48; ++i) {
            float a = (from + sweep * (float)i / 48) * SDL_PI_F / 180;
            p[i].x = cx + (r - t) * SDL_cosf(a);
            p[i].y = cy + (r - t) * SDL_sinf(a);
        }
        SDL_RenderLines(app->renderer, p, 49);
    }
}
/* the ring that turns while a run goes: a thin outer ring, a thicker inner one and a bar going round it */
static void ring(App* app, float cx, float cy) {
    float r = 7.0f * (float)app->scale, inner = r * 0.62f, thick = SDL_max(1.0f, 1.5f * (float)app->scale);
    float angle = app->finished ? 0 : (float)((now_ms(app) / 4) % 360);
    arc(app, cx, cy, r, 0, 360, SDL_max(1.0f, 0.5f * (float)app->scale), app->theme.run_colors.dim);
    arc(app, cx, cy, inner, 0, 360, thick, app->theme.run_colors.soft);
    arc(app, cx, cy, inner, angle, 70, thick, app->theme.run_colors.ink);
}
/* "", ".", "..", "..." round and round; "..." once the run is over */
static const char* dots(const App* app) {
    static const char* const turn[] = {"", ".", "..", "..."};
    return app->finished ? "..." : turn[(now_ms(app) / 400) % 4];
}
static void launch(App* app, char* absolute, bool prepared);
static void render_dests(App* app);
/* the game's name as the run knows it: the runner's banner, else the file's own, in capitals */
static void run_name(const App* app, char* out, size_t size) {
    const char* q;
    size_t n;
    const char* dot;
    const GuiStep *launch = gui_steps_last(&app->steps, "LAUNCH"), *made = gui_steps_last(&app->steps, "RECOMPILE");
    /* a game made already is named by its title ID: its recompile's line says what it is shown as */
    const char* name = launch ? launch->detail : app->native && made ? made->detail : app->file ? app->file : "";
    for (q = name; *q; ++q)
        if (*q == '/' || *q == '\\') name = q + 1;
    n = 0;
    dot = launch || (app->native && made) ? NULL : strrchr(name, '.');
    for (q = name; *q && q != dot && n + 1 < size; ++q) out[n++] = *q >= 'a' && *q <= 'z' ? (char)(*q - 32) : *q;
    out[n] = 0;
}
/* a step's line: its label, its detail in capitals and its status */
static size_t step_runs(const GuiStep* s, int how, Uint64 now, char run[3][112], size_t length[3]) {
    char* q;
    size_t total;
    int r;
    snprintf(run[0], 112, "%s:", snx_tr(s->label));
    snprintf(run[1], 112, " %s", s->detail);
    for (q = run[1]; *q; ++q)
        if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 32);
    if (s->state == GUI_STEP_FAILED)
        snprintf(run[2], 112, "%s", snx_tr("...FAILED"));
    else if (s->state == GUI_STEP_DONE)
        snprintf(run[2], 112, "%s", snx_tr("...OK"));
    else {
        char progress[64];
        gui_step_progress(s, how, now, progress, sizeof progress);
        snprintf(run[2], 112, "%s%s", progress[0] ? " " : "", progress);
    }
    total = 0;
    for (r = 0; r < 3; ++r) total += length[r] = (size_t)gui_text_cols(run[r]);
    return total;
}
/* a row cut to the columns it has: a step's detail, keeping its label and state, or a line, ending in "..." */
static void clip_row(char run[3][112], size_t length[3], int note, int room) {
    int cut;
    size_t marker, other, left, at;
    size_t total = length[0] + length[1] + length[2];
    if (room < 8 || total <= (size_t)room) return;
    cut = note ? 0 : 1;
    marker = note ? 3 : 0;
    other = total - length[cut];
    left = (size_t)room > other + marker ? (size_t)room - other - marker : 0;
    at = gui_cols_fit(run[cut], (int)left);
    snprintf(run[cut] + at, 112 - at, "%s", note ? "..." : "");
    length[cut] = (size_t)gui_text_cols(run[cut]);
}
/* how far each of rows rows has typed at now, total[r] columns that came at shown[r]: async, each from when it came;
   sync, one after the other. A slow row types a column every typing_ms, any other the whole row in typing_ms, and
   one above first, off the screen, at once. Returns the row being typed, -1 for none */
static int type_rows(const GuiTheme* theme, int rows, int first, const size_t* total, const Uint64* shown,
                     const int* slow, Uint64 now, size_t* typed) {
    int r, typing = -1;
    Uint64 free_at = 0;
    for (r = 0; r < rows; ++r) {
        Uint64 start, span = slow[r] ? (Uint64)theme->typing_ms * total[r] : theme->typing_ms;
        typed[r] = total[r];
        if (r < first || theme->line_in != GUI_LINE_TYPING || !theme->typing_ms) continue;
        start = theme->typing_sync ? SDL_max(shown[r], free_at) : shown[r];
        free_at = start + span;
        if (now < start)
            typed[r] = 0;
        else if (now - start < span)
            typed[r] = (size_t)((now - start) * total[r] / span);
        if (now >= start && typed[r] < total[r]) typing = r;
    }
    return typing;
}
static void foot(App* app, const GuiPlace* p, const char* tips, SDL_Color color, const char* keys);
static int hint_pick(const char* key, int col);
/* a run as a loading screen: the title, the name and the ring; the steps typed down the page, their status last;
   the progress bar and its percentage */
static void render_steps(App* app) {
    /* the rows: each step, then every line it kept when detailed, folded once it is done when collapse says so;
       each ends before the right edge, with room for a step's dots and the cursor */
    enum { ROWS = GUI_STEPS_MAX + GUI_NOTES_MAX, SEGMENTS = 40 };
    static char runs[ROWS][3][112];
    static size_t lengths[ROWS][3], typed[ROWS], totals[ROWS];
    static const GuiStep* owner[ROWS];
    static int is_note[ROWS], slow[ROWS];
    static Uint64 shown[ROWS];
    static const GuiRgb white = {255, 255, 255};
    const GuiTheme* theme = &app->theme;
    const GuiPalette* colors = &theme->run_colors;
    /* since: a line that came while the menu faded out comes in with the run */
    Uint64 now = now_ms(app), since = app->fade_ms;
    float cw = 8.0f * (float)app->scale, ch = 16.0f * (float)app->scale;
    int width, height, name_col, col, y, i, r, rows = 0, last_step = -1, typing, top = 3, bottom, fit, first, cursor;
    int foot = theme->footer != GUI_STYLE_NONE ? 2 : 0; /* the footer's rows, over the ESC line */
    const char* title;
    char name[96], head[128];
    SDL_Color ink, soft, failure;
    area(app, &width, &height);
    if (theme->texture)
        for (y = 3; y < height; y += 4) fill(app, 0, (float)y, (float)width, 1, white, 14);
    ink = rgb(colors->ink);
    soft = rgb(colors->soft);
    failure = rgb(colors->failure);

    run_name(app, name, sizeof name);
    col = theme->ring == GUI_RING_LEFT ? 4 : 1;
    title = theme->title[screen(app)];
    name_col = col + gui_text_cols(title) + (title[0] ? 1 : 0);
    if (theme->ring == GUI_RING_LEFT) ring(app, cw * 2, ch * 1.5f);
    if (theme->ring == GUI_RING_RIGHT) ring(app, (float)width - cw * 2, ch * 1.5f);
    if (theme->ring == GUI_RING_NEAR_TEXT) /* after the name and the dots' whole width, so it stays put */
        ring(app, cw * (float)(name_col + gui_text_cols(name) + 4) + 7.0f * (float)app->scale, ch * 1.5f);
    text(app, col, 1, title, ink);
    snprintf(head, sizeof head, "%s%s", name, dots(app));
    text(app, name_col, 1, head, soft);
    if (app->finished) text(app, 1, 2, app->message, app->abnormal_exit ? failure : soft);
    for (i = 0; i < app->steps.count; ++i) {
        int n;
        const GuiNote* notes[GUI_NOTES_MAX];
        int k;
        const GuiStep* s = &app->steps.step[i];
        owner[rows] = s;
        is_note[rows] = 0;
        shown[rows] = SDL_max(s->shown_ms, since);
        step_runs(s, theme->progress, now, runs[rows], lengths[rows]);
        clip_row(runs[rows], lengths[rows], 0, app->cols - 6);
        last_step = rows++;
        if (!theme->detailed || (theme->collapse && s->state == GUI_STEP_DONE)) continue;

        n = gui_step_notes(&app->steps, s, notes, GUI_NOTES_MAX);
        for (k = 0; k < n; ++k, ++rows) {
            owner[rows] = s;
            is_note[rows] = 1;
            shown[rows] = SDL_max(notes[k]->shown_ms, since);
            snprintf(runs[rows][0], sizeof runs[rows][0], "%s", notes[k]->text);
            runs[rows][1][0] = runs[rows][2][0] = 0;
            lengths[rows][0] = (size_t)gui_text_cols(runs[rows][0]);
            lengths[rows][1] = lengths[rows][2] = 0;
            clip_row(runs[rows], lengths[rows], 1, app->cols - 5);
        }
    }
    bottom = (theme->bar ? app->rows - 5 : app->rows - 2) - foot;
    fit = SDL_max(1, bottom - top);
    first = SDL_max(0, rows - fit);
    /* the newest row of a step still running types readably; a done step's, and one a newer row of its step passed,
       at once but for a glimpse: a fast log keeps the cursor on its latest lines */
    for (r = 0; r < rows; ++r) {
        totals[r] = lengths[r][0] + lengths[r][1] + lengths[r][2];
        slow[r] = owner[r]->state == GUI_STEP_ACTIVE && (r + 1 == rows || owner[r + 1] != owner[r]);
    }
    typing = type_rows(theme, rows, first, totals, shown, slow, now, typed);
    cursor = typing >= 0 ? typing : last_step;
    for (r = first; r < rows; ++r) {
        int working, blink, k;
        size_t left;
        const GuiStep* s = owner[r];
        int last = s == &app->steps.step[app->steps.count - 1];
        int failed = s->state == GUI_STEP_FAILED, active = s->state == GUI_STEP_ACTIVE;
        size_t total = lengths[r][0] + lengths[r][1] + lengths[r][2];
        Uint64 age = now > shown[r] ? now - shown[r] : 0;
        int moving = theme->line_in == GUI_LINE_SLIDE || theme->line_in == GUI_LINE_FADE;
        float p = moving && theme->line_in_ms ? SDL_min(1.0f, (float)age / (float)theme->line_in_ms) : 1.0f;
        float x = cw * (is_note[r] ? 3 : 1) -
                  (theme->line_in == GUI_LINE_SLIDE ? 16.0f * (float)app->scale * (1 - p) * (1 - p) : 0);
        float y = ch * (float)(top + r - first);
        SDL_Color color[3];
        color[0] = failed ? failure : last ? ink : soft;
        color[1] = failed ? failure : last || active ? ink : soft;
        color[2] = failed ? failure : ink;
        if (is_note[r]) color[0] = soft;
        left = typed[r];
        for (k = 0; k < 3; ++k) {
            size_t drawn = SDL_min(left, lengths[r][k]);
            x = text_px(app, x, y, runs[r][k], drawn, color[k], p);
            left -= drawn;
        }
        if (!is_note[r] && typed[r] == total && active) x = text_px(app, x, y, dots(app), 3, ink, p);
        blink = !theme->blink_ms || (now / theme->blink_ms) % 2 == 0;
        working = typing == r || (typing < 0 && active);
        if (r == cursor && working && blink) text_px(app, x, y, "_", 1, ink, p);
    }
    if (theme->bar) {
        int k, percent = gui_steps_percent(&app->steps);
        int filled = percent < 0 ? 0 : (percent * SEGMENTS + 50) / 100;
        float span = (float)(app->cols - 8) * cw, gap = 2.0f * (float)app->scale;
        float seg = (span - gap * (SEGMENTS - 1)) / SEGMENTS, sy = ch * (float)(app->rows - foot - 3) + ch * 0.3f;
        char number[16];
        fill(app, cw, ch * (float)(app->rows - foot - 4) + ch / 2, (float)(app->cols - 2) * cw, (float)app->scale,
             colors->dim, 255);
        for (k = 0; k < SEGMENTS; ++k) {
            SDL_FRect r = {cw + (float)k * (seg + gap), sy, seg, ch * 0.4f};
            if (k < filled)
                fill(app, r.x, r.y, r.w, r.h, colors->ink, 255);
            else {
                SDL_SetRenderDrawColor(app->renderer, colors->dim.r, colors->dim.g, colors->dim.b, 255);
                SDL_RenderRect(app->renderer, &r);
            }
        }
        if (percent < 0)
            snprintf(number, sizeof number, " --%%");
        else
            snprintf(number, sizeof number, "%3d%%", percent);
        text(app, app->cols - 6, app->rows - foot - 3, number, ink);
        text(app, 1, app->rows - foot - 2, name, ink);
    }
    if (foot) footer(app, app->rows - 3, 2, colors->page, colors->ink, colors->soft);
    if (app->finished)
        prompt(app, app->abnormal_exit ? GUI_PROMPT_FAILED : GUI_PROMPT_DONE, 1, app->rows - 1, snx_tr(SAY_DONE), soft);
    else if (app->aborting)
        prompt(app, GUI_PROMPT_ABORTING, 1, app->rows - 1, snx_tr("Aborting process tree..."), rgb(colors->warning));
    else
        text(app, 1, app->rows - 1, snx_tr(SAY_ABORT), soft);
}
/* the whole window darkened, alpha of 255, the cutout beside the text too: the fades, and a box over the screen */
#define GUI_FADE_MS 150 /* a screen's fade in, a box's shade coming or going */
static void shade(App* app, Uint8 alpha);
/* how far a fade from at is, 0 to GUI_FADE_MS long: 1 done (or none: animations off, never started) */
static float faded(const App* app, Uint64 at) {
    Uint64 now = now_ms(app);
    if (app->animations_off || !at || now >= at + GUI_FADE_MS) return 1.0f;
    return (float)(now > at ? now - at : 0) / (float)GUI_FADE_MS;
}
/* a box's shade over the screen under it, coming in as the box opens */
static void box_shade(App* app) { shade(app, (Uint8)(128 * faded(app, app->box_ms))); }
static void shade(App* app, Uint8 alpha) {
    static const GuiRgb black = {0, 0, 0};
    SDL_Rect safe;
    int width, height;
    SDL_GetRenderViewport(app->renderer, &safe);
    SDL_GetRenderOutputSize(app->renderer, &width, &height);
    SDL_SetRenderViewport(app->renderer, NULL);
    fill(app, 0, 0, (float)width, (float)height, black, alpha);
    SDL_SetRenderViewport(app->renderer, &safe);
}
/* the screen as one number, which another screen changes: a tab, a form, a group, a load's screen, the setup's page;
   never 0, which screen_was holds before the first frame */
static unsigned screen_mark(const App* app) {
    return (unsigned)app->editing | (unsigned)app->form << 1 | (unsigned)app->menu.view << 5 |
           (unsigned)app->setup_on << 9 | (unsigned)app->setup_page << 10 | (unsigned)app->group << 16 |
           (unsigned)(app->page + 1) << 22 | 1u << 31;
}
/* what changed since the last frame: a screen come, whose fade starts now, a box opened or closed, whose shade does */
static void fades_mark(App* app) {
    unsigned mark = screen_mark(app);
    bool boxed = app->confirm.shown || app->sizes_shown || app->users_shown || app->addons_shown ||
                 app->cleaned_shown || app->left_shown || app->dests_shown;
    if (mark != app->screen_was && app->screen_was) app->switch_ms = SDL_max(now_ms(app), 1);
    if (boxed != app->boxed_was) app->box_ms = SDL_max(now_ms(app), 1);
    app->screen_was = mark;
    app->boxed_was = boxed;
}
/* over the frame drawn: the new screen coming in from the page's colour, a box's shade going once it closed */
static void fades_draw(App* app) {
    float in = faded(app, app->switch_ms), out = faded(app, app->box_ms);
    SDL_Rect safe;
    int width, height;
    if (in < 1.0f) {
        SDL_GetRenderViewport(app->renderer, &safe);
        SDL_GetRenderOutputSize(app->renderer, &width, &height);
        SDL_SetRenderViewport(app->renderer, NULL);
        fill(app, 0, 0, (float)width, (float)height, app->theme.page, (Uint8)(255 * (1.0f - in)));
        SDL_SetRenderViewport(app->renderer, &safe);
    }
    if (out < 1.0f && !app->boxed_was) shade(app, (Uint8)(128 * (1.0f - out)));
}
/* the screen fading in from black, or the menu or the run fading out to it */
static void overlay(App* app) {
    const GuiTheme* theme = &app->theme;
    Uint64 now = now_ms(app);
    float a = 0;
    if (entering(app))
        a = (float)(now > app->entering_ms ? now - app->entering_ms : 0) / (float)theme->fade_out_ms;
    else if (app->leaving_ms && theme->fade_out_ms)
        a = SDL_min(1.0f, (float)(now > app->leaving_ms ? now - app->leaving_ms : 0) / (float)theme->fade_out_ms);
    else if (app->fade_ms && theme->fade_in_ms && now < app->fade_ms + theme->fade_in_ms)
        a = 1.0f - (float)(now > app->fade_ms ? now - app->fade_ms : 0) / (float)theme->fade_in_ms;
    if (a > 0) shade(app, (Uint8)(a * 255));
}
static const char* ask_question(const App* app) {
    switch (app->confirm.ask) {
        case GUI_ASK_DEEP: return snx_tr("List the games in its subfolders too?");
        case GUI_ASK_EXIT: return snx_tr("Exit ScarletNX?");
        case GUI_ASK_OUTSIDE: return snx_tr("Outside the app, Android checks every file access: games may run slower.");
        case GUI_ASK_ROOM: return app->room_line;
        case GUI_ASK_TEXT: return app->size_line;
        case GUI_ASK_CLEAN:
        case GUI_ASK_RESET:
        case GUI_ASK_PLACES:
        case GUI_ASK_PLACE:
        case GUI_ASK_FOLDER:
        case GUI_ASK_MOVE: return app->ask_line;
        case GUI_ASK_ERROR: return app->error_line;
        case GUI_ASK_CLOSE:
            return screen(app) == GUI_SCREEN_COMPILING
                       ? snx_tr("A recompile goes: exiting ScarletNX stops it, its work lost.")
                       : snx_tr("A game runs: exiting ScarletNX ends it.");
        default:
            return screen(app) == GUI_SCREEN_COMPILING ? snx_tr("Abort the recompile? Its work is lost.")
                                                       : snx_tr("Abort the run?");
    }
}
/* app->left's lines, Share log's: English whatever the menu shows */
#define SAY_LEFT_BY    " was left by an older version: this one keeps it in "
#define SAY_LEFT_TITLE SNX_TR_NOOP("Left by older versions")
/* how the menu's own files an older Android version left go (left_clear): out of the player's reach, and the app in
   testing, its whole storage is cleared */
#define SAY_LEFT_CLEAR                                                                                                 \
    SNX_TR_NOOP("While the app is in testing, Android's Settings > Apps > ScarletNX > Storage > Clear storage takes "  \
                "the menu's files above, with every game, save and key in the app")
/* a line of app->left: the file where an older build left it, and where this one keeps it */
static void left_paths(const char* line, char* old, char* now, size_t size) {
    const char* by = strstr(line, SAY_LEFT_BY);
    snprintf(old, size, "%.*s", (int)(by - line), line);
    by += strlen(SAY_LEFT_BY);
    snprintf(now, size, "%.*s", (int)strcspn(by, "\n"), by);
}
static void list_item(App* app, const GuiList* l, int i, const char* label, int focused, int activated);
static void list_marks(App* app, const GuiList* l);
static int list_hit(const GuiList* l, float x, float y, bool touch);
static void fill_rect(App* app, GuiRect r, GuiRgb c) {
    if (r.w > 0 && r.h > 0) fill(app, (float)r.x, (float)r.y, (float)r.w, (float)r.h, c, 255);
}
static void outline(App* app, GuiRect r, GuiRgb c) {
    int s = app->scale;
    fill_rect(app, gui_rect(r.x, r.y, r.w, s), c);
    fill_rect(app, gui_rect(r.x, r.y + r.h - s, r.w, s), c);
    fill_rect(app, gui_rect(r.x, r.y, s, r.h), c);
    fill_rect(app, gui_rect(r.x + r.w - s, r.y, s, r.h), c);
}
/* value from pixel x, y up to the right edge of r: a line too long for it slides as the theme says */
static void text_in(App* app, GuiRect r, int x, int y, const char* value, SDL_Color color) {
    int w = cell_w(app), room = (r.x + r.w - x) / w, skip;
    if (room <= 0) return;
    skip = gui_slide(gui_text_cols(value) - room, now_ms(app), app->theme.scroll_ms);
    value = gui_cols_skip(value, skip);
    SDL_SetTextureColorMod(app->font, color.r, color.g, color.b);
    while (*value) {
        int c = gui_menu_glyph(gui_utf8_step(&value)), n = gui_glyph_cols(c);
        if (n > room) break;
        glyph(app, (float)x, (float)y, c);
        room -= n;
        x += n * w;
    }
}
/* a box framed in the dim colour, filled with the page's (fill) or the frame alone (line); with a head when label is
   not NULL, its label on a band of ink (fill) or over a rule (line). Where its text starts */
static int pane(App* app, GuiRect r, const char* label) {
    const GuiTheme* t = &app->theme;
    int s = app->scale, filled = t->panels == GUI_PANELS_FILL;
    if (filled) fill_rect(app, r, t->page);
    if (label && filled) fill_rect(app, gui_rect(r.x, r.y, r.w, head_h(app)), t->ink);
    if (label && !filled) fill_rect(app, gui_rect(r.x, r.y + head_h(app) - s, r.w, s), t->dim);
    outline(app, r, t->dim);
    if (label) text_in(app, inner(app, r), r.x + cell_w(app), r.y + 2 * s, label, rgb(filled ? t->page : t->ink));
    return r.y + (label ? head_h(app) : 0) + 2 * s;
}
/* the box over the darkened screen, on the page colour whatever the theme's boxes, so nothing shows through it */
static int over_box(App* app, GuiRect r, const char* label) {
    box_shade(app);
    fill_rect(app, r, app->theme.page);
    return pane(app, r, label);
}
/* a box over the screen, cols columns of text wide and h pixels high, in its middle */
static GuiRect over_rect(const App* app, int cols, int h) {
    int width, height, w;
    area(app, &width, &height);
    w = SDL_min((cols + 2) * cell_w(app), width - 2 * cell_w(app));
    return gui_rect((width - w) / 2, SDL_max(0, (height - h) / 2), w, h);
}
/* the bytes of s's first row at width columns: a path's up to a slash (gui_wrap), other text's up to a space or after a
   wide letter, as Chinese breaks between any two */
static size_t wrap_at(const char* s, int width, int path) {
    const char *p = s, *space = NULL;
    int n = 0;
    if (path) return gui_wrap(s, width);
    while (*p) {
        const char* next = p;
        int cols = gui_glyph_cols(gui_menu_glyph(gui_utf8_step(&next)));
        if ((n += cols) > width) break;
        p = next;
        if (*p == ' ' || cols == 2) space = p;
    }
    return !*p ? strlen(s) : space ? (size_t)(space - s) : gui_wrap(s, width);
}
/* s from pixel x, y wrapped to the right edge of r (path: at its slashes), a row under the other while they fit above
   its bottom, the last that fits sliding what is left, or on one row that slides with the theme's [body] text =
   slide: the rows it took */
static int wrap_in(App* app, GuiRect r, int x, int y, const char* s, int path, SDL_Color color) {
    char row[1100];
    int width = (r.x + r.w - x) / cell_w(app), rows = 0;
    if (width <= 0) return 0;
    if (app->theme.text == GUI_TEXT_SLIDE) {
        if (!*s || y + cell_h(app) > r.y + r.h) return 0;
        text_in(app, r, x, y, s, color);
        return 1;
    }
    while (*s && y + (rows + 1) * cell_h(app) <= r.y + r.h) {
        size_t n = wrap_at(s, width, path), next = n;
        while (s[next] == ' ') ++next;
        if (s[next] && y + (rows + 2) * cell_h(app) > r.y + r.h) { /* no row for the rest: cut, it would be lost */
            text_in(app, r, x, y + rows * cell_h(app), s, color);
            return rows + 1;
        }
        snprintf(row, sizeof row, "%.*s", (int)n, s);
        text_in(app, r, x, y + rows * cell_h(app), row, color);
        for (s += n; *s == ' ';) ++s; /* a row starts at a word */
        ++rows;
    }
    return rows;
}
/* the rows s takes wrapped to width columns, one where the theme slides it */
static int wrap_rows(const App* app, const char* s, int width, int path) {
    int rows = 0;
    if (width <= 0 || app->theme.text == GUI_TEXT_SLIDE) return 1;
    for (; *s; ++rows)
        for (s += wrap_at(s, width, path); *s == ' ';) ++s;
    return SDL_max(rows, 1);
}
/* a box of files over the screen, two lines each, as many as fit from app->files_top, how many more, then OK,
   focused: the folders and files an older version left (report 0), each where it is (-) and the space it takes; or
   what Clean old settings did (1), each file and what was done to it or why not. Up/Down go over them, another key
   closes it */
/* the space the older version's i-th folder, old, takes, or that it is still counted */
static void left_size(const App* app, int i, const char* old, char* out, size_t size) {
    const GuiSpace* space = app->left_space;
    if (space && i < __atomic_load_n(&space->counted, __ATOMIC_ACQUIRE) && !strcmp(space->dirs[i], old))
        gui_size_text(out, size, space->bytes[i]);
    else
        snprintf(out, size, "%s", snx_tr("counting"));
}
/* the i-th file, at: its two lines, their colours, whether the second is a path; the next file, NULL after the last */
static const char* files_entry(const App* app, int report, int i, const char* at, char* a, char* b, size_t size,
                               SDL_Color* ca, SDL_Color* cb, int* path) {
    SDL_Color failure = rgb(app->theme.failure), ink = rgb(app->theme.ink);
    const char* next;
    bool ok;
    if (!report) {
        left_paths(at, a, b, size);
        left_size(app, i, a, b, size);
        *ca = failure;
        *cb = rgb(app->theme.soft);
        *path = 0;
        next = strchr(at, '\n') + 1;
        return *next ? next : NULL;
    }
    next = gui_cleaned_entry(at, &ok, a, b, size);
    *ca = ok ? ink : failure;
    *cb = ok ? rgb(app->theme.soft) : failure;
    *path = 0;
    return next;
}
static void render_files(App* app, int report) {
    char a[4400], b[4400], label[96];
    const char *first = report ? app->cleaned.text : app->left, *at = first;
    int width, height, cols, room, used = 0, shown, i, y, path, s = app->scale, item = item_h(app), hint;
    int count = report ? app->cleaned.count : app->lefts, done = app->cleaned.done, top;
    SDL_Color ca, cb;
    GuiRect r, text;
    GuiList ok;
    area(app, &width, &height);
    cols = width / cell_w(app) - 6;
    hint = !report && app->left_clear ? wrap_rows(app, snx_tr(SAY_LEFT_CLEAR), cols, 0) : 0; /* under them, before OK */
    /* the first shown, kept among them when there are fewer since */
    top = app->files_top = SDL_clamp(app->files_top, 0, SDL_max(count - 1, 0));
    for (i = 0; at && i < top; ++i) at = files_entry(app, report, i, at, a, b, sizeof a, &ca, &cb, &path);
    first = at;
    /* the files' rows, and one for how many more */
    room = (height - head_h(app) - item - 24 * s) / cell_h(app) - 2 - hint;
    for (shown = 0; at && top + shown < count; ++shown) {
        int need;
        at = files_entry(app, report, top + shown, at, a, b, sizeof a, &ca, &cb, &path);
        need = wrap_rows(app, a, cols - 2, 1) + wrap_rows(app, b, cols - 2, path);
        if (shown && used + need > room) break;
        used += need;
    }
    app->files_shown = shown;
    r = over_rect(app, cols, head_h(app) + (used + (top + shown < count) + hint) * cell_h(app) + item + 16 * s);
    if (!report)
        snprintf(label, sizeof label, "%s", snx_tr(SAY_LEFT_TITLE));
    else if (app->report_move)
        snprintf(label, sizeof label, snx_tr("Move old files: %d done, %d not"), done, count - done);
    else if (count > done)
        snprintf(label, sizeof label, snx_tr("Clean old settings: %d done, %d not"), done, count - done);
    else
        snprintf(label, sizeof label, snx_tr("Clean old settings: %d done"), done);
    y = over_box(app, r, label) + 2 * s;
    text = inner(app, r);
    for (i = 0, at = first; at && i < shown; ++i) {
        at = files_entry(app, report, top + i, at, a, b, sizeof a, &ca, &cb, &path);
        text_in(app, text, text.x, y, "-", ca);
        if (top && !i) text_in(app, r, text.x + text.w, y, "\x1e", rgb(app->theme.soft)); /* more above: its margin */
        y += wrap_in(app, text, text.x + 2 * cell_w(app), y, a, 1, ca) * cell_h(app);
        y += wrap_in(app, text, text.x + 2 * cell_w(app), y, b, path, cb) * cell_h(app);
    }
    if (top + shown < count) {
        snprintf(label, sizeof label, snx_tr("and %d more, which Share log lists"), count - top - shown);
        text_in(app, text, text.x, y, label, rgb(app->theme.soft));
        y += cell_h(app);
    }
    if (hint) wrap_in(app, text, text.x, y, snx_tr(SAY_LEFT_CLEAR), 0, rgb(app->theme.ink));
    list_place(app, &ok, gui_rect(text.x, r.y + r.h - item - 6 * s, SDL_min(text.w, 26 * cell_w(app)), item), 1, 0);
    snprintf(label, sizeof label, "%s%s", app->theme.marker, snx_tr("OK"));
    list_item(app, &ok, 0, label, true, false);
}
/* ---- a question and its choices, as the theme's options-dialog places them: in a box in the middle of their area, the
   question over the choices; or the question in a box at the area's foot, a tab with its label on the box's top edge,
   and the choices at its right above it. The questions over a screen and the setup's steps ---- */
typedef struct {
    GuiRect box;  /* the question's: the whole dialog in the middle, or the box at the foot */
    GuiRect tab;  /* at the foot: the label on the box's top edge; no size without a label */
    GuiList list; /* the choices */
} GuiChoices;
static void choices_place(App* app, GuiRect area, const char* question, const char* tab, const char* const* labels,
                          int count, int focus, GuiChoices* c) {
    int s = app->scale, cw = cell_w(app), item = item_h(app), list_h = count * (item + 6 * s), longest = 16, i, rows;
    int marker = gui_text_cols(app->theme.marker), tab_w = tab && tab[0] ? gui_text_cols(tab) + 2 : 0;
    memset(c, 0, sizeof *c);
    for (i = 0; i < count; ++i) longest = SDL_max(longest, gui_text_cols(labels[i]) + marker + 2);
    if (app->theme.dialog == GUI_DIALOG_YORHA) {
        int margin = app->theme.cursor[0] ? gui_text_cols(app->theme.cursor) * cw : 0;
        int w = SDL_min(SDL_max(longest, 24) * cw + margin, area.w), top;
        rows = wrap_rows(app, question, (area.w - 4 * cw) / cw, 0);
        c->box = gui_rect(area.x, area.y + area.h - (rows + 2) * cell_h(app), area.w, (rows + 2) * cell_h(app));
        if (tab_w) c->tab = gui_rect(c->box.x + 2 * cw, c->box.y - cell_h(app), tab_w * cw, cell_h(app) + 4 * s);
        top = (tab_w ? c->tab.y : c->box.y) - cell_h(app) / 2 - list_h;
        list_place(app, &c->list, gui_rect(area.x + area.w - w - 2 * cw, top, w, list_h), count, focus);
        return;
    }
    { /* in the middle, as wide as the question or the longest choice */
        int cols = SDL_max(gui_text_cols(question) + tab_w, longest), w, h;
        w = SDL_min((cols + 2) * cw, area.w - 2 * cw);
        rows = wrap_rows(app, question, (w - 2 * cw - tab_w * cw) / cw, 0);
        h = rows * cell_h(app) + list_h + 12 * s;
        c->box = gui_rect(area.x + (area.w - w) / 2, area.y + SDL_max(0, (area.h - h) / 2), w, h);
        list_place(app, &c->list,
                   gui_rect(c->box.x + cw, c->box.y + rows * cell_h(app) + 10 * s, c->box.w - 2 * cw, list_h), count,
                   focus);
    }
}
/* them drawn where choices_place put them, the focused one as the lists draw theirs */
static void choices_draw(App* app, const GuiChoices* c, const char* question, const char* tab,
                         const char* const* labels, int count, int focus) {
    const GuiTheme* t = &app->theme;
    char label[256];
    int i, s = app->scale, filled = t->panels == GUI_PANELS_FILL;
    if (t->dialog == GUI_DIALOG_YORHA) {
        fill_rect(app, c->box, filled ? t->ink : t->page);
        if (!filled) outline(app, c->box, t->dim);
        wrap_in(app, gui_rect(c->box.x + 2 * cell_w(app), c->box.y, c->box.w - 4 * cell_w(app), c->box.h),
                c->box.x + 2 * cell_w(app), c->box.y + cell_h(app), question, 0, rgb(filled ? t->page : t->ink));
        if (c->tab.h) {
            fill_rect(app, c->tab, t->page);
            outline(app, c->tab, filled ? t->ink : t->dim);
            text_in(app, c->tab, c->tab.x + cell_w(app), c->tab.y + 2 * s, tab, rgb(t->ink));
        }
    } else {
        fill_rect(app, c->box, t->page);
        pane(app, c->box, NULL);
        wrap_in(app,
                gui_rect(c->box.x, c->box.y, c->box.w - (tab && tab[0] ? gui_text_cols(tab) + 2 : 0) * cell_w(app),
                         c->box.h),
                c->box.x + cell_w(app), c->box.y + 4 * s, question, 0, rgb(t->warning));
        if (tab && tab[0])
            text_in(app, c->box, c->box.x + c->box.w - (gui_text_cols(tab) + 1) * cell_w(app), c->box.y + 4 * s, tab,
                    rgb(t->soft));
    }
    for (i = c->list.first; i < c->list.first + c->list.shown && i < count; ++i) {
        snprintf(label, sizeof label, "%s%s", t->marker, labels[i]);
        list_item(app, &c->list, i, label, focus == i, false);
    }
    list_marks(app, &c->list);
}
/* the pixel x, y on the dialog: the choice under it (-1 for none), and whether it is on the dialog at all */
static int choices_hit(const GuiChoices* c, float x, float y, bool* on) {
    const GuiRect* r[3];
    int i;
    r[0] = &c->box;
    r[1] = &c->tab;
    r[2] = &c->list.at;
    *on = false;
    for (i = 0; i < 3; ++i)
        *on = *on || (x >= (float)r[i]->x && x < (float)(r[i]->x + r[i]->w) && y >= (float)r[i]->y &&
                      y < (float)(r[i]->y + r[i]->h));
    return list_hit(&c->list, x, y, true);
}
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
#define SAY_AGAIN SNX_TR_NOOP("tap again")
#else
#define SAY_AGAIN SNX_TR_NOOP("press again")
#endif
/* the questions over a run, the menu or a form (GuiAsk's order): the first choice keeps things as they are */
static const char* const ask_choices[][2] = {{SNX_TR_NOOP("Keep going"), SNX_TR_NOOP("Abort")},
                                             {SNX_TR_NOOP("Stay"), SNX_TR_NOOP("Exit")},
                                             {SNX_TR_NOOP("Only this folder"), SNX_TR_NOOP("With subfolders")},
                                             {SNX_TR_NOOP("Back"), SNX_TR_NOOP("Use it")},
                                             {SNX_TR_NOOP("Back"), SNX_TR_NOOP("Run anyway")},
                                             {SNX_TR_NOOP("Back"), SNX_TR_NOOP("Keep")},
                                             {SNX_TR_NOOP("Back"), SNX_TR_NOOP("Clean")},
                                             {SNX_TR_NOOP("Back"), SNX_TR_NOOP("Reset")},
                                             {SNX_TR_NOOP("Back"), SNX_TR_NOOP("Reset")},
                                             {SNX_TR_NOOP("Back"), SNX_TR_NOOP("Reset")},
                                             {SNX_TR_NOOP("Back"), SNX_TR_NOOP("Take it off")},
                                             {SNX_TR_NOOP("Back"), SNX_TR_NOOP("Move")},
                                             {SNX_TR_NOOP("Back"), SNX_TR_NOOP("Exit")},
                                             {SNX_TR_NOOP("OK"), NULL}};
/* the question shown, over the whole screen in the middle or at its foot, a choice asked twice saying so once pressed
   (its label in second): how many choices it has, one for a failure's OK */
static int ask_place(App* app, GuiChoices* c, const char** labels, char* second, size_t size) {
    int width, height, count;
    GuiAsk ask = app->confirm.ask;
    area(app, &width, &height);
    labels[0] = snx_tr(ask_choices[ask][0]);
    count = ask_choices[ask][1] ? 2 : 1;
    if (count == 2)
        snprintf(second, size, "%s%s%s", snx_tr(ask_choices[ask][1]), app->confirm.armed ? ": " : "",
                 app->confirm.armed ? snx_tr(SAY_AGAIN) : "");
    labels[1] = count == 2 ? second : NULL;
    choices_place(app,
                  app->theme.dialog == GUI_DIALOG_YORHA
                      ? gui_rect(cell_w(app), cell_h(app) / 2, width - 2 * cell_w(app), height - cell_h(app))
                      : gui_rect(0, 0, width, height),
                  ask_question(app), NULL, labels, count, app->confirm.selected, c);
    return count;
}
static void render_ask(App* app) {
    GuiChoices c;
    const char* labels[2];
    char second[96];
    int count = ask_place(app, &c, labels, second, sizeof second);
    box_shade(app); /* over another screen */
    choices_draw(app, &c, ask_question(app), NULL, labels, count, app->confirm.selected);
}
/* the boot's splash: a drawing of Wimsy's, picked at random, drawn a third of the window's shorter side on a theme's
   splash page, then the menu as the theme brings it in */
static int splash_size(const App* app) {
    SDL_Rect view;
    SDL_GetRenderViewport(app->renderer, &view);
    return SDL_max(1, SDL_min(view.w, view.h) / 3);
}
/* the drawing at side pixels; the program's icon when it cannot be, said once */
static SDL_Texture* splash_texture(App* app, int side) {
    SDL_Texture* texture = NULL;
    void* pixels = app->splash_drawing ? SDL_malloc((size_t)side * (size_t)side * 4) : NULL;
    if (pixels && !snx_splash_draw(app->splash_drawing, side, pixels, side * 4)) {
        texture = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, side, side);
        if (texture && (!SDL_UpdateTexture(texture, NULL, pixels, side * 4) ||
                        !SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED))) {
            SDL_DestroyTexture(texture);
            texture = NULL;
        }
    }
    SDL_free(pixels);
    if (!texture && app->splash_drawing) {
        fprintf(stderr, "scarletnx: the splash's %s could not be drawn %d pixels square, so the icon shows\n",
                app->splash_svg->name, side);
        snx_splash_close(app->splash_drawing);
        app->splash_drawing = NULL;
    }
    if (!texture) {
        SDL_Surface* icon = snx_window_icon_surface();
        texture = icon ? SDL_CreateTextureFromSurface(app->renderer, icon) : NULL;
        SDL_DestroySurface(icon);
    }
    return texture;
}
/* the splash over, its time up or nothing to show: the menu comes in as the theme brings it */
static void splash_end(App* app) {
    if (app->splash) SDL_DestroyTexture(app->splash);
    app->splash = NULL;
    snx_splash_close(app->splash_drawing);
    app->splash_drawing = NULL;
    app->fade_ms = app->theme.fade_in_ms ? SDL_max(now_ms(app), 1) : 0;
}
/* svg's drawing on t's splash page until its time */
static void splash_with(App* app, const GuiTheme* t, const SnxSplashSvg* svg) {
    app->splash_svg = svg;
    app->splash_drawing = snx_splash_open(svg);
    if (!app->splash_drawing)
        fprintf(stderr, "scarletnx: the splash's %s could not be read, so the icon shows\n", svg->name);
    app->splash_side = splash_size(app);
    app->splash = splash_texture(app, app->splash_side);
    if (!app->splash) {
        snx_splash_close(app->splash_drawing);
        app->splash_drawing = NULL;
        return;
    }
    app->splash_page = t->splash_page;
    app->splash_until = SDL_GetTicks() + t->splash_ms;
}
static void splash_start(App* app) {
    const GuiTheme* t = gui_splash_theme(&app->theme, (int)SDL_rand(100));
    if (t->splash_ms) splash_with(app, t, &snx_splash_svgs[SDL_rand(snx_splash_svg_count)]);
}
static int splash_shown(App* app) {
    SDL_Rect view;
    int side;
    if (!app->splash) return 0;
    if (now_ms(app) >= app->splash_until) {
        splash_end(app);
        return 0;
    }
    side = splash_size(app);
    if (side != app->splash_side) { /* the window changed: drawn again at its size */
        SDL_DestroyTexture(app->splash);
        app->splash_side = side;
        app->splash = splash_texture(app, side);
        if (!app->splash) {
            splash_end(app);
            return 0;
        }
    }
    SDL_GetRenderViewport(app->renderer, &view);
    SDL_SetRenderDrawColor(app->renderer, app->splash_page.r, app->splash_page.g, app->splash_page.b, 255);
    SDL_RenderClear(app->renderer);
    {
        SDL_FRect at = {(float)((view.w - side) / 2), (float)((view.h - side) / 2), (float)side, (float)side};
        SDL_RenderTexture(app->renderer, app->splash, NULL, &at);
    }
    SDL_RenderPresent(app->renderer);
    return 1;
}
#ifdef SDL_PLATFORM_ANDROID
#define SAY_MENU_KEYS SAY_MENU
#else
#define SAY_MENU_KEYS SNX_TR_KEYS("Arrows/mouse: move. Space/Enter/click: select. F5: refresh. ESC: exit.")
#endif
/* the keys the foot of the screen shown says, Tab's after them: what it draws, and what a click on one presses */
static bool folders_row_place(const App* app);
static const char* shown_keys(const App* app, char* out, size_t size) {
    const char* keys = SAY_MENU_KEYS;
    if (app->editing)
        keys = app->form == GUI_FORM_PAD        ? SAY_PAD
               : app->editor.picking            ? SAY_PICKER
               : app->form == GUI_FORM_THEME    ? SAY_EDITOR
               : app->form == GUI_FORM_SETTINGS ? SAY_GROUPS
               : app->form == GUI_FORM_APP      ? SAY_APP
               : app->form == GUI_FORM_FOLDERS  ? (folders_row_place(app) ? SAY_PLACES : SAY_FOLDERS)
               : app->form == GUI_FORM_USERS    ? SAY_USERS
                                                : SAY_SETTINGS;
    else if (app->menu.view == GUI_LOAD_GAMES)
        keys = SAY_GAMES;
    else if (pick_screen(app))
        keys = SAY_KEYS_HOW;
    return screen_keys(app, keys, out, size);
}
/* the colour picker's box, its rows and the bars of the first three, in pixels */
typedef struct {
    GuiRect box, rows[GUI_PICK_ROWS], bars[3];
} GuiPicker;
static int item_text_y(const App* app, GuiRect b) { return b.y + app->scale; }
static GuiRect list_box(const GuiList* l, int i) {
    return gui_rect(l->at.x + l->margin, l->at.y + (i - l->first) * l->pitch, l->at.w - l->margin - l->arrows, l->item);
}
static void touch(App* app, float x, float y);
/* the entry under pixel x, y, -1 for none; a finger takes the whole width */
static int list_hit(const GuiList* l, float x, float y, bool touch) {
    int i;
    for (i = l->first; i < l->first + l->shown; ++i) {
        GuiRect b = list_box(l, i);
        if (y >= (float)b.y && y < (float)(b.y + b.h + (l->pitch - l->item)) &&
            (touch ? x >= (float)l->at.x && x < (float)(l->at.x + l->at.w) : x >= (float)b.x && x < (float)(b.x + b.w)))
            return i;
    }
    return -1;
}
/* an entry as a box of its own: on the dim colour (fill) or framed in it (line); the focused one inverted on the focus
   colour with a rule over and under it (fill), or framed and written in it; the cursor in the margin. focused 2: that
   look without the cursor, for the group whose rows have the focus */
static void list_item(App* app, const GuiList* l, int i, const char* label, int focused, int activated) {
    const GuiTheme* t = &app->theme;
    GuiRect b = list_box(l, i);
    int s = app->scale, filled = t->panels == GUI_PANELS_FILL, inverted = focused && t->focus_bar;
    if (inverted)
        fill_rect(app, b, t->focus);
    else if (filled)
        fill_rect(app, b, t->dim);
    if (!inverted && (!filled || focused)) outline(app, b, focused ? t->focus : t->dim);
    if (focused && filled && l->pitch - l->item >= 6 * s) { /* where the gap has room for them */
        fill_rect(app, gui_rect(b.x, b.y - 3 * s, b.w, s), t->focus);
        fill_rect(app, gui_rect(b.x, b.y + b.h + 2 * s, b.w, s), t->focus);
    }
    text_in(app, inner(app, b), b.x + cell_w(app) / 2, item_text_y(app, b), label,
            rgb(inverted    ? t->page
                : activated ? t->active
                : focused   ? t->focus
                            : t->ink));
    if (focused == 1 && t->cursor[0]) text_in(app, l->at, l->at.x, item_text_y(app, b), t->cursor, rgb(t->focus));
}
/* arrows past the boxes' right edge: up beside entry first, down beside entry last */
static void list_arrows(App* app, const GuiList* l, int first, int last, int up, int down) {
    int x = l->at.x + l->at.w - cell_w(app);
    if (up) text_in(app, l->at, x, item_text_y(app, list_box(l, first)), "\x1e", rgb(app->theme.soft));
    if (down) text_in(app, l->at, x, item_text_y(app, list_box(l, last)), "\x1f", rgb(app->theme.soft));
}
/* while entries are hidden above or below the ones shown; a list showing none (its column gone) has no entry to mark */
static void list_marks(App* app, const GuiList* l) {
    if (!l->shown) return;
    list_arrows(app, l, l->first, l->first + l->shown - 1, l->first > 0, l->first + l->shown < l->count);
}
/* a theme colour by ui.txt's name (UiColor), else dflt */
static GuiRgb ui_colour(const App* app, unsigned color, GuiRgb dflt) {
    const GuiTheme* t = &app->theme;
    const GuiRgb named[] = {t->page, t->ink, t->soft, t->focus, t->active, t->failure, t->dim, t->warning, t->added};
    return color > UI_COLOR_NONE && color <= sizeof named / sizeof *named ? named[color - 1] : dflt;
}
/* a row's box: the row less the theme's cursor, which is drawn before it */
static GuiRect ui_row_box(const App* app, GuiRect r) {
    int margin = app->theme.cursor[0] ? gui_text_cols(app->theme.cursor) * cell_w(app) : 0;
    return gui_rect(r.x + margin, r.y, r.w - margin, r.h);
}
/* a row of a scene of ui.txt, in list_item's look: the theme's cursor and the arrows of hidden rows inside it, the box
   between them; the failure colour fills it (every other blink_ms with UI_ATTR_BLINK, steady with 0), the active
   colour writes the one activated last, and an open one (UI_ATTR_OPEN) has the focus's look without the cursor; a
   title (UI_ATTR_TITLE) is on a band of ink in a theme of filled boxes, else over a rule */
/* a row's text's top: as item_text_y has it in a row a cell and a pixel high, in the middle of a taller one */
static int ui_text_y(const App* app, GuiRect b) { return b.y + (b.h - cell_h(app) + app->scale) / 2; }
static int user_side(const App* app, int h);
void ui_draw_row(App* app, GuiRect r, const char* text, const void* image, unsigned color, unsigned attr,
                 bool focused) {
    const GuiTheme* t = &app->theme;
    int look = focused || (attr & UI_ATTR_OPEN), filled = t->panels == GUI_PANELS_FILL, inverted = look && t->focus_bar;
    int failing =
        (attr & UI_ATTR_FAILURE) && !((attr & UI_ATTR_BLINK) && t->blink_ms && (now_ms(app) / t->blink_ms) % 2);
    GuiRect b = ui_row_box(app, r);
    char label[400];
    if (attr & UI_ATTR_LABEL) { /* between rows, as "Recent games": no box, no marker */
        text_in(app, b, b.x, ui_text_y(app, b), text, rgb(t->soft));
        return;
    }
    if (attr & UI_ATTR_TITLE) { /* the whole row's, cursor's margin too: over its group, as a box's head */
        if (filled)
            fill_rect(app, r, t->ink);
        else
            fill_rect(app, gui_rect(r.x, r.y + r.h - app->scale, r.w, app->scale), t->dim);
        text_in(app, r, r.x + cell_w(app) / 2, ui_text_y(app, r), text, rgb(filled ? t->page : t->ink));
        return;
    }
    snprintf(label, sizeof label, "%s%s", t->marker, text);
    if (failing || inverted)
        fill_rect(app, b, failing ? t->failure : t->focus);
    else if (filled)
        fill_rect(app, b, t->dim);
    if (!failing && !inverted && (!filled || look)) outline(app, b, look ? t->focus : t->dim);
    {
        SDL_Color ink = rgb(failing || inverted     ? t->page
                            : attr & UI_ATTR_ACTIVE ? t->active
                            : look                  ? t->focus
                                                    : ui_colour(app, color, t->ink));
        int x = b.x + cell_w(app) / 2, side = user_side(app, b.h);
        if (image && side > 0) { /* the marker, the image as big as the head's user picture, then the text */
            SDL_FRect at;
            text_in(app, inner(app, b), x, ui_text_y(app, b), t->marker, ink);
            x += gui_text_cols(t->marker) * cell_w(app);
            at = (SDL_FRect){(float)x, (float)(b.y + 2 * app->scale), (float)side, (float)side};
            SDL_RenderTexture(app->renderer, (SDL_Texture*)image, NULL, &at);
            text_in(app, inner(app, b), x + side + cell_w(app) / 2, ui_text_y(app, b), text, ink);
        } else
            text_in(app, inner(app, b), x, ui_text_y(app, b), label, ink);
    }
    if (focused && t->cursor[0]) text_in(app, r, r.x, ui_text_y(app, b), t->cursor, rgb(t->focus));
    if (attr & UI_ATTR_MORE_UP) /* beside the down arrow when one row shows */
        text_in(app, b, b.x + b.w - ((attr & UI_ATTR_MORE_DOWN) ? 2 : 1) * cell_w(app), ui_text_y(app, b), "\x1e",
                rgb(failing || inverted ? t->page : t->soft));
    if (attr & UI_ATTR_MORE_DOWN)
        text_in(app, b, b.x + b.w - cell_w(app), ui_text_y(app, b), "\x1f",
                rgb(failing || inverted ? t->page : t->soft));
}
/* a label of a scene of ui.txt, in the soft colour unless it names one */
void ui_draw_label(App* app, GuiRect r, const char* text, unsigned color) {
    text_in(app, r, r.x, item_text_y(app, r), text, rgb(ui_colour(app, color, app->theme.soft)));
}
const char* ui_tr(const char* english) { return snx_tr(english); }
/* The keys of a screen: "Keys: what they do." sentences, each drawn as the keys, inverted (the theme's focus bar) or in
   the focus colour, and what they do; a row holds as many as fit, against its right edge */
static int hint_next(const char** s, char* key, char* what, size_t size) {
    const char *p = *s, *end, *colon;
    size_t n;
    while (*p == ' ') ++p;
    if (!*p) return 0;
    if (*p == '~') ++p; /* a sentence its screen cannot do now: drawn greyed, pressed by nothing (hint_off) */
    end = strstr(p, ". ");
    if (!end) end = p + strlen(p) - (p[strlen(p) - 1] == '.');
    colon = strstr(p, ": ");
    if (colon && colon > end) colon = NULL;
    n = colon ? (size_t)(colon - p) : 0;
    snprintf(key, size, "%.*s", (int)n, p);
    p = colon ? colon + 2 : p;
    snprintf(what, size, "%.*s", (int)(end - p), p);
    *s = *end ? end + 1 : end;
    return 1;
}
static int hint_off(const char* s) {
    while (*s == ' ') ++s;
    return *s == '~';
}
static int hint_width(const char* key, const char* what) {
    return gui_text_cols(key) + (key[0] ? 1 : 0) + gui_text_cols(what);
}
/* a click or a tap, for hints() to say which sentence with keys it is on: their keys, and how many columns into the
   sentence (-1: the margin before it); key empty for none */
typedef struct {
    float x, y;
    char key[128];
    int col;
} GuiHintHit;
/* the rows keys take in width columns; with draw, drawn from pixel y in r, on the foot's band of ink when band; with
   hit, the sentence its pixel is on, a column of the gap either side of one its own */
static int hints(App* app, const char* keys, int width, GuiRect r, int y, int draw, int band, GuiHintHit* hit) {
    const GuiTheme* t = &app->theme;
    char key[128], what[128];
    const char* row = keys;
    int rows = 0, s = app->scale, lit, off;
    for (;;) {
        const char *at = row, *end = row, *u = row;
        int used = 0, x;
        while (hint_next(&at, key, what, sizeof key)) {
            int w = hint_width(key, what) + (used ? 2 : 0);
            if (used && used + w > width) break;
            used += w;
            end = at;
        }
        if (end == row) break;
        for (x = r.x + r.w - used * cell_w(app);
             (draw || hit) && u != end && (off = hint_off(u), hint_next(&u, key, what, sizeof key));) {
            int cy = y + rows * cell_h(app), k = gui_text_cols(key), w = hint_width(key, what) * cell_w(app);
            if (hit && k && !off && hit->y >= (float)cy && hit->y < (float)(cy + cell_h(app)) &&
                hit->x >= (float)(x - cell_w(app)) && hit->x < (float)(x + w + cell_w(app))) {
                snprintf(hit->key, sizeof hit->key, "%s", key);
                hit->col = (int)SDL_floorf((hit->x - (float)x) / (float)cell_w(app));
            }
            lit = draw && k && !off && app->hovering && app->hover_y >= (float)cy &&
                  app->hover_y < (float)(cy + cell_h(app)) && app->hover_x >= (float)(x - cell_w(app)) &&
                  app->hover_x < (float)(x + w + cell_w(app)) &&
                  hint_pick(key, (int)SDL_floorf((app->hover_x - (float)x) / (float)cell_w(app))) >= 0;
            if (lit) /* the whole sentence, as a button under the pointer: inverted */
                fill_rect(app, gui_rect(x - s, cy, w + 2 * s, cell_h(app)), band ? t->page : t->focus);
            else if (draw && k && !off && t->focus_bar)
                fill_rect(app, gui_rect(x - s, cy, k * cell_w(app) + 2 * s, cell_h(app)), band ? t->soft : t->focus);
            if (draw && k)
                text_in(app, r, x, cy, key,
                        rgb(off                    ? t->soft
                            : lit                  ? (band ? t->ink : t->page)
                            : t->focus_bar || band ? t->page
                                                   : t->focus));
            if (draw)
                text_in(app, r, x + (k + (k ? 1 : 0)) * cell_w(app), cy, what,
                        rgb(off    ? t->soft
                            : lit  ? (band ? t->ink : t->page)
                            : band ? t->page
                                   : t->ink));
            x += w + 2 * cell_w(app);
        }
        ++rows;
        row = end;
    }
    return SDL_max(rows, 1);
}
/* ---- the foot and the tips: the keys' box, the tips' beside it or on a row under the head ---- */
/* the foot's letters, a size under the rest's but 1 at the least: what draws or measures the keys, and the tips when
   they are in the foot, does it with app->scale set to them, then puts the menus' back */
static int foot_scale(const App* app) { return SDL_max(1, app->scale - 1); }
/* the text's room in a box of them: past the bar at its left in a box, else the box's own width */
static GuiRect foot_text(const App* app, GuiRect r, int box) {
    int s = app->scale;
    return box ? gui_rect(r.x + 3 * cell_w(app), r.y + 2 * s, r.w - 4 * cell_w(app), r.h - 4 * s)
               : gui_rect(r.x, r.y + 2 * s, r.w, r.h - 4 * s);
}
/* one of them: with box a pane with a bar down its left, on the foot's band when band; where its text goes */
static GuiRect foot_box(App* app, GuiRect r, int box, int band) {
    const GuiTheme* t = &app->theme;
    int s = app->scale;
    if (!r.h) return r;
    if (box && !band) pane(app, r, NULL);
    if (box)
        fill_rect(app, gui_rect(r.x + 2 * s, r.y + 2 * s, 3 * s, r.h - 4 * s),
                  band                           ? t->page
                  : t->panels == GUI_PANELS_FILL ? t->ink
                                                 : t->soft);
    return foot_text(app, r, box);
}
static void foot(App* app, const GuiPlace* p, const char* tips, SDL_Color color, const char* keys) {
    const GuiTheme* t = &app->theme;
    int band = t->footer == GUI_STYLE_BAR, below = p->tips.h && p->tips.y == p->keys.y, s = app->scale;
    int small = foot_scale(app);
    GuiRect foot = below ? gui_rect(p->tips.x, p->keys.y, p->keys.x + p->keys.w - p->tips.x, p->keys.h) : p->keys;
    GuiRect text;
    if (!p->keys.h) return;
    if (band) fill_rect(app, foot, t->ink);
    if (t->footer_rule) fill_rect(app, gui_rect(foot.x, foot.y - cell_h(app) / 4 - s / 2, foot.w, s), t->soft);
    if (below && band && !(color.r == t->failure.r && color.g == t->failure.g && color.b == t->failure.b))
        color = rgb(t->page); /* on the band but a failure */
    if (below) app->scale = small;
    text = foot_box(app, p->tips, t->tips_box, below && band);
    if (below && t->tips_wrap)
        wrap_in(app, text, text.x, text.y, tips, 0, color);
    else if (p->tips.h) /* one row: a long one slides */
        text_in(app, text, text.x, text.y, tips, color);
    app->scale = small;
    text = foot_box(app, p->keys, t->footer_box, band);
    hints(app, keys, text.w / cell_w(app), text, text.y, 1, band, NULL);
    app->scale = s;
}
/* the foot of a screen: the message while there is one, a failure's in its colour, else about */
static void screen_foot(App* app, const GuiPlace* p, const char* about, const char* keys) {
    const GuiTheme* t = &app->theme;
    foot(app, p, app->message[0] ? app->message : about,
         rgb(app->message[0] ? (app->abnormal_exit ? t->failure : t->focus) : t->ink), keys);
}
/* ---- the user at the head's right: the one Settings' user names, its nickname and its picture in a box ---- */
/* Settings' user: the pick, or its default, 1; read from settings.ini until the Settings are */
static int settings_user(const App* app) {
    const char* value = NULL;
    char *text = NULL, *key, *at;
    size_t size;
    int user = 1;
    if (app->settings.keys)
        value = gui_settings_value(&app->settings, "user");
    else if (app->settings_file && snx_is_file(app->settings_file)) {
        SnxIni ini;
        text = (char*)snx_read_file(app->settings_file, &size);
        if (text) snx_ini_start(&ini, text);
        while (text && snx_ini_next(&ini, &key, &at))
            if (!strcmp(key, "user")) value = at;
    }
    if (value) user = SDL_atoi(value);
    free(text);
    return user;
}
/* the users of users_dir, for the head: the one games play as, or none with why in the message */
static void users_read(App* app) {
    char why[512];
    const SnxUser* u;
    const unsigned char* jpeg;
    uint32_t size;
    app->user = 0;
    free(app->user_jpeg);
    app->user_jpeg = NULL;
    app->user_jpeg_size = 0;
    SDL_DestroyTexture(app->user_picture);
    app->user_picture = NULL;
    if (!app->users_dir) return;
    if (snx_users_load(app->users_dir, settings_user(app), why, sizeof why)) {
        snprintf(app->message, sizeof app->message, snx_tr("No user shown: %s"), why);
        return;
    }
    u = snx_users_selected();
    jpeg = snx_users_picture(u, &size);
    if (jpeg && (app->user_jpeg = malloc(size)) != NULL) {
        memcpy(app->user_jpeg, jpeg, size);
        app->user_jpeg_size = size;
    }
    snprintf(app->user_name, sizeof app->user_name, "%s", u->nickname);
    app->user = snx_users_selected_number();
}
/* jpeg (size bytes; NULL for Wimsy) side pixels square in the theme's shape, a texture of its own; a JPEG that does
   not draw is Wimsy, and *failed says so */
static SDL_Texture* picture_texture(App* app, const unsigned char* jpeg, size_t size, int side, bool* failed) {
    int round = app->theme.picture == GUI_PICTURE_ROUND;
    SDL_Texture* texture;
    void* pixels = side > 0 ? malloc((size_t)side * (size_t)side * 4) : NULL;
    *failed = false;
    if (!pixels) return NULL;
    if (snx_splash_picture(jpeg, size, side, round, pixels, side * 4)) {
        *failed = jpeg != NULL;
        snx_splash_picture(NULL, 0, side, round, pixels, side * 4);
    }
    texture = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, side, side);
    if (texture && (!SDL_UpdateTexture(texture, NULL, pixels, side * 4) ||
                    !SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED))) {
        SDL_DestroyTexture(texture);
        texture = NULL;
    }
    free(pixels);
    return texture;
}
/* the icon of the game whose program is at path, as its row shows it: its icon.jpg drawn once, then kept; NULL for
   a game without one. One that does not draw is said, with its file, and shows none */
/* the folder in the cache a dump's icon goes in: icons/ of the menu's cache, named by a hash of the dump's path, size
   and time and of the keys' (prod.keys), so a dump replaced, or keys put in after a dump could not be read, asks again.
   Owned; NULL when the dump is not there */
static char* dump_icon_dir(const App* app, const char* dump) {
    SDL_PathInfo info, keys = {0};
    uint64_t h = 1469598103934665603ull; /* FNV-1a */
    const char* c;
    char *cache, *icons, name[17], *dir;
    if (!SDL_GetPathInfo(dump, &info) || info.type != SDL_PATHTYPE_FILE) return NULL;
    for (c = dump; *c; ++c) h = (h ^ (unsigned char)*c) * 1099511628211ull;
    h = (h ^ info.size) * 1099511628211ull;
    h = (h ^ (uint64_t)info.modify_time) * 1099511628211ull;
    if (app->keys_path) SDL_GetPathInfo(app->keys_path, &keys); /* none: zeros */
    h = (h ^ keys.size) * 1099511628211ull;
    h = (h ^ (uint64_t)keys.modify_time) * 1099511628211ull;
    snprintf(name, sizeof name, "%016llx", (unsigned long long)h);
    cache = menu_folder(app, SNX_USER_CACHE, false);
    icons = cache ? snx_path_join(cache, "icons") : NULL;
    dir = icons ? snx_path_join(icons, name) : NULL;
    free(icons);
    free(cache);
    return dir;
}
/* one dump asked of the driver: its icon.jpg in dir, else why.txt there with what the driver said */
static void dump_icon_answer(const GuiDumpIcons* d, const char* dump, const char* dir) {
    const char* args[] = {d->driver, "--repo", d->repo, "-x", "--icon", "--out", dir, "--", dump, NULL};
    SDL_Process* run;
    char *said = NULL, *why, *icon;
    size_t size = 0;
    int status = -1;
    if (snx_mkdirs(dir)) return; /* asked again next time: nowhere to answer */
    run = SDL_CreateProcess(args, true);
    if (run) said = (char*)SDL_ReadProcess(run, &size, &status);
    icon = snx_path_join(dir, "icon.jpg");
    if (!run || status || !snx_is_file(icon)) {
        why = snx_path_join(dir, "why.txt");
        if (!run)
            snx_write_file(why, SDL_GetError(), strlen(SDL_GetError()));
        else
            snx_write_file(why, said ? said : "", said ? strlen(said) : 0);
        free(why);
    }
    free(icon);
    SDL_free(said);
    if (run) SDL_DestroyProcess(run);
}
/* the dumps asked so far, here: what the thread does, and the tests' look of their own */
static void dump_icons_pass(GuiDumpIcons* d) {
    char *dump, *dir;
    if (!d->lock) return;
    for (;;) {
        SDL_LockMutex(d->lock);
        if (!d->count || __atomic_load_n(&d->stop, __ATOMIC_RELAXED)) {
            SDL_UnlockMutex(d->lock);
            return;
        }
        dump = d->dumps[0];
        dir = d->dirs[0];
        memmove(d->dumps, d->dumps + 1, (size_t)--d->count * sizeof *d->dumps);
        memmove(d->dirs, d->dirs + 1, (size_t)d->count * sizeof *d->dirs);
        SDL_UnlockMutex(d->lock);
        dump_icon_answer(d, dump, dir);
        free(dump);
        free(dir);
        SDL_LockMutex(d->lock);
        ++d->answered;
        SDL_UnlockMutex(d->lock);
    }
}
static int SDLCALL dump_icons_run(void* data) {
    GuiDumpIcons* d = (GuiDumpIcons*)data;
    while (!__atomic_load_n(&d->stop, __ATOMIC_RELAXED)) {
        dump_icons_pass(d);
        SDL_LockMutex(d->lock);
        if (!d->count && !__atomic_load_n(&d->stop, __ATOMIC_RELAXED)) SDL_WaitCondition(d->wake, d->lock);
        SDL_UnlockMutex(d->lock);
    }
    return 0;
}
static char* find_driver(const App* app, int item);
/* a dump's icon to ask the driver for, once: false when it cannot be asked (no driver, the queue full) */
static bool dump_icon_ask(App* app, const char* dump, const char* dir) {
    GuiDumpIcons* d = &app->dump_icons;
    bool asked = false;
    int i;
    if (!d->lock) {
        char* driver = find_driver(app, GUI_SWITCH);
        if (!driver || !app->data_dir || !(d->lock = SDL_CreateMutex()) || !(d->wake = SDL_CreateCondition())) {
            free(driver);
            return false;
        }
        d->driver = driver;
        d->repo = snx_strdup(app->data_dir);
    }
    SDL_LockMutex(d->lock);
    for (i = 0; i < d->count && strcmp(d->dirs[i], dir); ++i) {}
    if (i == d->count && d->count < GUI_DUMP_ASKS) {
        d->dumps[d->count] = snx_strdup(dump);
        d->dirs[d->count++] = snx_strdup(dir);
        asked = true;
        SDL_SignalCondition(d->wake);
    }
    SDL_UnlockMutex(d->lock);
    return asked || i < d->count;
}
/* the asker's thread, answering from now on: the program's alone (tests answer with dump_icons_pass) */
static void dump_icons_start(App* app) {
    GuiDumpIcons* d = &app->dump_icons;
    if (d->lock && !d->thread && !(d->thread = SDL_CreateThread(dump_icons_run, "dump icons", d)))
        snprintf(app->message, sizeof app->message, snx_tr("The dumps' icons are not asked for (%s)"), SDL_GetError());
}
static void dump_icons_stop(App* app) {
    GuiDumpIcons* d = &app->dump_icons;
    int i;
    __atomic_store_n(&d->stop, 1, __ATOMIC_RELAXED);
    if (d->lock) {
        SDL_LockMutex(d->lock);
        SDL_SignalCondition(d->wake);
        SDL_UnlockMutex(d->lock);
    }
    if (d->thread) SDL_WaitThread(d->thread, NULL);
    for (i = 0; i < d->count; ++i) {
        free(d->dumps[i]);
        free(d->dirs[i]);
    }
    free(d->driver);
    free(d->repo);
    if (d->wake) SDL_DestroyCondition(d->wake);
    if (d->lock) SDL_DestroyMutex(d->lock);
    *d = (GuiDumpIcons){0};
}
/* the icon file of the game or dump at path, owned: a game's own icon.jpg (gui_game_icon); a dump's in its folder of
   the cache, asked of the driver when it has neither it nor the driver's why. NULL while there is none */
static char* icon_file(App* app, const char* path) {
    char *dir, *file, *why;
    const char* leaf = path + strlen(path);
    while (leaf > path && leaf[-1] != '/' && leaf[-1] != '\\') --leaf;
    if (!gui_is_dump(leaf)) return gui_game_icon(path);
    if (!(dir = dump_icon_dir(app, path))) return NULL;
    file = snx_path_join(dir, "icon.jpg");
    why = snx_path_join(dir, "why.txt");
    if (!snx_is_file(file)) {
        if (!snx_is_file(why)) {
            if (dump_icon_ask(app, path, dir) && app->dump_icons.live) dump_icons_start(app);
        }
        free(file);
        file = NULL;
    }
    free(why);
    free(dir);
    return file;
}
static SDL_Texture* game_icon(App* app, const char* path) {
    GuiIcon* icon;
    char* file;
    unsigned char* jpeg;
    void* pixels;
    size_t size = 0, i;
    for (i = 0; i < GUI_ICONS; ++i)
        if (app->icons[i].path && !strcmp(app->icons[i].path, path)) return app->icons[i].texture;
    icon = &app->icons[app->icons_next];
    app->icons_next = (app->icons_next + 1) % GUI_ICONS;
    free(icon->path);
    SDL_DestroyTexture(icon->texture);
    *icon = (GuiIcon){snx_strdup(path), NULL};
    file = icon_file(app, path);
    jpeg = file ? snx_read_file(file, &size) : NULL;
    pixels = jpeg ? malloc((size_t)GUI_ICON_SIDE * GUI_ICON_SIDE * 4) : NULL;
    if (pixels && snx_splash_picture(jpeg, size, GUI_ICON_SIDE, 0, pixels, GUI_ICON_SIDE * 4))
        snprintf(app->message, sizeof app->message, snx_tr("%s could not be drawn: the game shows no icon"), file);
    else if (pixels) {
        icon->texture = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
                                          GUI_ICON_SIDE, GUI_ICON_SIDE);
        if (icon->texture && (!SDL_UpdateTexture(icon->texture, NULL, pixels, GUI_ICON_SIDE * 4) ||
                              !SDL_SetTextureBlendMode(icon->texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED))) {
            SDL_DestroyTexture(icon->texture);
            icon->texture = NULL;
        }
    }
    free(pixels);
    free(jpeg);
    free(file);
    return icon->texture;
}
/* the rows without an icon looked at again: the driver answered for a dump */
static void icons_forget_none(App* app) {
    size_t i;
    for (i = 0; i < GUI_ICONS; ++i)
        if (app->icons[i].path && !app->icons[i].texture) {
            free(app->icons[i].path);
            app->icons[i].path = NULL;
        }
}
/* every icon game_icon kept: drawn again from their files when shown, as a game's may have changed */
static void icons_free(App* app) {
    size_t i;
    for (i = 0; i < GUI_ICONS; ++i) {
        free(app->icons[i].path);
        SDL_DestroyTexture(app->icons[i].texture);
        app->icons[i] = (GuiIcon){NULL, NULL};
    }
    app->icons_next = 0;
}
/* the head's user's picture, side pixels square, kept while its size and the theme's shape are the same. A JPEG that
   does not draw shows Wimsy, said. The preview's, at its own size and theme, is a texture of its own that the caller
   destroys: the app it draws on is put back after it, so nothing the app keeps is touched */
static SDL_Texture* user_texture(App* app, int side) {
    SDL_Texture* texture;
    bool failed;
    if (!app->previewing && app->user_picture && app->user_side == side && app->user_shape == (int)app->theme.picture)
        return app->user_picture;
    texture = picture_texture(app, app->user_jpeg, app->user_jpeg_size, side, &failed);
    if (app->previewing) return texture;
    if (failed) {
        snprintf(app->message, sizeof app->message, snx_tr("%s's picture could not be drawn: Wimsy shows"),
                 app->user_name);
        free(app->user_jpeg);
        app->user_jpeg = NULL;
        app->user_jpeg_size = 0;
    }
    SDL_DestroyTexture(app->user_picture);
    app->user_picture = texture;
    app->user_side = side;
    app->user_shape = (int)app->theme.picture;
    return texture;
}
/* a box of the user's, r, h pixels high: framed as the boxes are (filled with the page's colour too, a theme's filled
   ones), on the head's band in its colours swapped; the nickname a column in, the picture against its right two pixels
   in, the box's frame round both */
static int user_side(const App* app, int h) { return h - 4 * app->scale; }
static void draw_user(App* app, GuiRect r, int band) {
    const GuiTheme* t = &app->theme;
    int s = app->scale, side = user_side(app, r.h);
    SDL_Texture* picture;
    if (!r.h || side <= 0) return;
    if (!band && t->panels == GUI_PANELS_FILL) fill_rect(app, r, t->page);
    outline(app, r, band ? t->page : t->dim);
    text_in(app, gui_rect(r.x, r.y, r.w - side - 2 * s - cell_w(app), r.h), r.x + cell_w(app),
            r.y + (r.h - cell_h(app)) / 2, app->user_name, rgb(band ? t->page : t->ink));
    picture = user_texture(app, side);
    if (picture) {
        SDL_FRect at = {(float)(r.x + r.w - 2 * s - side), (float)(r.y + 2 * s), (float)side, (float)side};
        SDL_RenderTexture(app->renderer, picture, NULL, &at);
        if (app->previewing) SDL_DestroyTexture(picture);
    }
}
/* the box's width h pixels high: a column, the nickname, a column, the picture and its two pixels; 0 with no user to
   show */
static int user_width(const App* app, int h) {
    if (!app->user || app->setup_on || app->theme.picture == GUI_PICTURE_NONE || user_side(app, h) <= 0) return 0;
    return (gui_text_cols(app->user_name) + 2) * cell_w(app) + user_side(app, h) + 2 * app->scale;
}
/* ---- the box of users a click on the head's opens: each user of users.ini, the one picked games' user ---- */
static void users_close(App* app) {
    int i;
    for (i = 0; i < SNX_USERS_MAX; ++i) {
        SDL_DestroyTexture(app->users_pictures[i]);
        app->users_pictures[i] = NULL;
    }
    app->users_shown = false;
}
static void users_open(App* app) {
    if (!app->user) return;
    users_close(app);
    app->users_shown = true;
    app->users_focus = app->user - 1;
}
/* the box over the screen, as wide as its title or the longest nickname with its picture; a row each, the focused
   one */
#define SAY_USERS_TITLE SNX_TR_NOOP("Who plays the games")
static void users_boxes(App* app, GuiRect* r, GuiList* rows) {
    int i, s = app->scale, item = item_h(app), h = g_user_count * (item + 6 * s);
    int longest = SDL_max(16, gui_text_cols(snx_tr(SAY_USERS_TITLE)) + 2);
    for (i = 0; i < g_user_count; ++i)
        longest = SDL_max(longest, gui_text_cols(g_users[i].nickname) + gui_text_cols(app->theme.marker) + 4);
    *r = over_rect(app, longest, head_h(app) + h + 12 * s);
    list_place(app, rows, gui_rect(r->x + cell_w(app), r->y + head_h(app) + 6 * s, r->w - 2 * cell_w(app), h),
               g_user_count, app->users_focus);
}
static void render_users(App* app) {
    GuiRect r;
    GuiList rows;
    char label[SNX_USERS_NICKNAME + 24];
    int i, s = app->scale;
    users_boxes(app, &r, &rows);
    over_box(app, r, snx_tr(SAY_USERS_TITLE));
    for (i = rows.first; i < rows.first + rows.shown; ++i) {
        GuiRect b = list_box(&rows, i);
        int side = b.h - 4 * s;
        snprintf(label, sizeof label, "%s%s", app->theme.marker, g_users[i].nickname);
        list_item(app, &rows, i, label, app->users_focus == i, i == app->user - 1);
        if (side <= 0) continue;
        if (!app->users_pictures[i] || app->users_side != side) {
            uint32_t size;
            const unsigned char* jpeg = snx_users_picture(&g_users[i], &size);
            bool failed;
            SDL_DestroyTexture(app->users_pictures[i]);
            app->users_pictures[i] = picture_texture(app, jpeg, size, side, &failed);
        }
        if (app->users_pictures[i]) {
            SDL_FRect at = {(float)(b.x + b.w - 2 * s - side), (float)(b.y + 2 * s), (float)side, (float)side};
            SDL_RenderTexture(app->renderer, app->users_pictures[i], NULL, &at);
        }
    }
    app->users_side = rows.shown ? list_box(&rows, rows.first).h - 4 * s : 0;
}
/* user n (1-based) picked: Settings' user, kept in settings.ini at once, and the head shows them */
static GuiSettings* settings_of(App* app);
static void users_pick(App* app, int n) {
    char value[8];
    GuiSettings* settings = settings_of(app);
    snprintf(value, sizeof value, "%d", n);
    if (!gui_settings_set(settings, "user", value))
        snprintf(app->message, sizeof app->message, "%s", snx_tr("No user picked: the Settings have no user"));
    else {
        gui_settings_sync(settings);
        if (app->settings_file && gui_settings_save(settings, app->settings_file)) {
            snprintf(app->message, sizeof app->message, snx_tr("Not saved: %s could not be written"),
                     app->settings_file);
            say_failure(app, app->message);
        }
    }
    users_close(app);
    users_read(app);
}
/* a key on the box: Up and Down move, Select picks, Back closes it */
static void users_key(App* app, GuiKey key) {
    if (key == GUI_UP || key == GUI_DOWN)
        app->users_focus = (app->users_focus + (key == GUI_UP ? g_user_count - 1 : 1)) % g_user_count;
    else if (key == GUI_SELECT)
        users_pick(app, app->users_focus + 1);
    else if (key == GUI_CANCEL)
        users_close(app);
}
/* the pointer on it: the row under it takes the focus, a click picks it; a click off the box closes it */
static void users_pointer(App* app, float x, float y, bool click, bool touch) {
    GuiRect r;
    GuiList rows;
    int i;
    users_boxes(app, &r, &rows);
    if ((i = list_hit(&rows, x, y, touch)) >= 0) {
        app->users_focus = i;
        if (click) users_pick(app, i + 1);
    } else if (click && (x < (float)r.x || x >= (float)(r.x + r.w) || y < (float)r.y || y >= (float)(r.y + r.h)))
        users_close(app);
}
/* ---- the box of a game's updates and DLCs (gui_game_addons) and mods (gui_game_mods), which opening the game shows,
   as the game's addons= and mods_off= have them picked: its updates as a choice of one (each version installed,
   newest first, or No update), its DLCs and mods a box to check each by, and Open mods folder; then Save, which keeps
   them in addons= and mods_off=, and Play, which keeps them and plays the game with them, from the picked update's
   program, else its own ---- */
static void addons_close(App* app) {
    gui_games_free(&app->addons);
    free(app->addons_game);
    free(app->addons_title);
    free(app->addons_checked);
    snx_free_strings(app->mods, app->mods_count);
    free(app->mods_folder);
    free(app->mods_on);
    app->addons_game = app->addons_title = app->mods_folder = NULL;
    app->addons_checked = app->mods_on = NULL;
    app->mods = NULL;
    app->mods_count = 0;
    app->addons_shown = false;
}
/* folder as a file:// URL, owned: its bytes other than letters, digits and -._~/: written %XX, a \\ as / */
static char* file_url(const char* folder) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = strlen(folder), i, at = 0;
    char* url = (char*)malloc(8 + 3 * n + 1);
    if (!url) return NULL;
    memcpy(url, "file://", 7);
    at = 7;
    if (folder[0] != '/' && folder[0] != '\\') url[at++] = '/'; /* C:/... */
    for (i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)folder[i];
        if (c == '\\') c = '/';
        if (SDL_isalnum(c) || strchr("-._~/:", c)) {
            url[at++] = (char)c;
        } else {
            url[at++] = '%';
            url[at++] = hex[c >> 4];
            url[at++] = hex[c & 15];
        }
    }
    url[at] = 0;
    return url;
}
/* folder opened in the system's file manager, made first when it isn't there: app->message says where it is, or why
   it could not be opened */
static void show_folder(App* app, const char* folder) {
    char why[256], *url;
    if (snx_mkdirs(folder)) {
        snprintf(app->message, sizeof app->message, snx_tr("Cannot make %s: %s"), folder,
                 snx_os_error(why, sizeof why));
        return;
    }
    url = file_url(folder);
    if (url && SDL_OpenURL(url))
        snprintf(app->message, sizeof app->message, snx_tr("Opened %s"), folder);
    else
        snprintf(app->message, sizeof app->message, snx_tr("Could not open %s: %s"), folder,
                 url ? SDL_GetError() : snx_tr("out of memory"));
    free(url);
}
/* the game's folder and name, owned, from its program's path */
static void addons_where(const App* app, char** folder, char** name) {
    const char* leaf;
    *folder = snx_dirname(app->addons_game);
    for (leaf = *folder + strlen(*folder); leaf > *folder && leaf[-1] != '/' && leaf[-1] != '\\'; --leaf) continue;
    *name = snx_strdup(leaf);
}
/* addons' i's entry, its name after <game>_ */
static const char* addon_entry(const App* app, size_t i) {
    const char* under = strchr(app->addons.names[i], '_');
    return under ? under + 1 : app->addons.names[i];
}
/* what addons' i is to its game: SNX_ADDON_UPDATE or SNX_ADDON_DLC */
static int addon_kind(const App* app, size_t i) {
    char game[17];
    SnxU64 own = 0, id = 0;
    snprintf(game, sizeof game, "%s", app->addons.names[i]);
    if (!snx_title_parse(game, &own) || !snx_addon_entry_id(own, addon_entry(app, i), &id)) return SNX_ADDON_NONE;
    return snx_addon_kind(own, id);
}
/* the updates, first in addons (gui_game_addons lists them so) */
static size_t addons_updates(const App* app) {
    size_t n = 0;
    while (n < app->addons.count && addon_kind(app, n) == SNX_ADDON_UPDATE) n++;
    return n;
}
/* the box's rows: the updates and No update (with any update), the DLCs, the mods, Open mods folder, Save, then Play */
enum { ROW_NO_UPDATE = -1, ROW_SAVE = -2, ROW_PLAY = -3, ROW_MODS_FOLDER = -4 };
static int addons_rows(const App* app) {
    return (int)(app->addons.count + app->mods_count) + (addons_updates(app) ? 1 : 0) + 3;
}
/* what row is: its add-on's index in addons, a mod's (mods' index past addons' count), or ROW_NO_UPDATE,
   ROW_MODS_FOLDER, ROW_SAVE or ROW_PLAY */
static int addons_row(const App* app, int row) {
    int updates = (int)addons_updates(app), rows = addons_rows(app);
    if (row == rows - 1) return ROW_PLAY;
    if (row == rows - 2) return ROW_SAVE;
    if (row == rows - 3) return ROW_MODS_FOLDER;
    if (row < updates) return row;
    if (updates && row == updates) return ROW_NO_UPDATE;
    return row - (updates ? 1 : 0);
}
/* a mod's row's item (addons_row): its index in mods, else -1 */
static int addons_mod(const App* app, int item) {
    return item >= (int)app->addons.count ? item - (int)app->addons.count : -1;
}
static void addons_open(App* app, const char* game, const char* title) {
    char *folder, *name, *ini, *text;
    SnxAddon* list;
    size_t count, i, j;
    addons_close(app);
    gui_game_addons(game, &app->addons);
    app->mods_count = gui_game_mods(game, &app->mods_folder, &app->mods);
    if (!app->addons.count && !app->mods_count) {
        addons_close(app);
        return;
    }
    app->addons_game = snx_strdup(game);
    app->addons_title = snx_strdup(title);
    app->addons_checked = (bool*)calloc(app->addons.count + 1, sizeof *app->addons_checked);
    app->mods_on = (bool*)calloc(app->mods_count + 1, sizeof *app->mods_on);
    list = (SnxAddon*)malloc(SNX_ADDONS_MAX * sizeof *list);
    if (!app->addons_checked || !app->mods_on || !list) {
        free(list);
        addons_close(app);
        return;
    }
    addons_where(app, &folder, &name);
    ini = snx_game_ini(folder, name);
    text = snx_ini_value(ini, "addons");
    count = snx_addons_parse(text, name, list, SNX_ADDONS_MAX);
    for (i = 0; i < app->addons.count; ++i)
        for (j = 0; j < count; ++j)
            if (!strcmp(addon_entry(app, i), list[j].entry)) app->addons_checked[i] = true;
    free(text);
    text = snx_ini_value(ini, "mods_off");
    for (i = 0; i < app->mods_count; ++i) app->mods_on[i] = !snx_mod_off(text, app->mods[i]);
    free(text);
    free(ini);
    free(folder);
    free(name);
    free(list);
    app->addons_recompile = app->recompile;
    app->addons_focus = 0;
    app->addons_shown = true;
}
static void addons_label(const App* app, int row, char* label, size_t size) {
    int item = addons_row(app, row);
    size_t i;
    bool none = true;
    for (i = 0; i < addons_updates(app); ++i) none = none && !app->addons_checked[i];
    if (addons_mod(app, item) >= 0)
        snprintf(label, size, "%s%s %s", app->theme.marker, app->mods_on[addons_mod(app, item)] ? "[x]" : "[ ]",
                 app->mods[addons_mod(app, item)]);
    else if (item >= 0)
        snprintf(label, size, "%s%s %s", app->theme.marker,
                 addon_kind(app, (size_t)item) == SNX_ADDON_UPDATE ? (app->addons_checked[item] ? "(*)" : "( )")
                                                                   : (app->addons_checked[item] ? "[x]" : "[ ]"),
                 app->addons.shown[item]);
    else if (item == ROW_NO_UPDATE)
        snprintf(label, size, "%s%s %s", app->theme.marker, none ? "(*)" : "( )", snx_tr("No update"));
    else if (item == ROW_MODS_FOLDER)
        snprintf(label, size, "%s%s", app->theme.marker, snx_tr("Open mods folder"));
    else if (item == ROW_SAVE)
        snprintf(label, size, "%s%s", app->theme.marker, snx_tr("Save"));
    else
        snprintf(label, size, "%s%s", app->theme.marker, app->addons_recompile ? snx_tr("Recompile") : snx_tr("Play"));
}
static void addons_boxes(App* app, GuiRect* r, GuiList* rows) {
    int i, s = app->scale, item = item_h(app), count = addons_rows(app), h = count * (item + 6 * s);
    int longest = SDL_max(16, (int)SDL_utf8strlen(app->addons_title) + 2);
    char label[160];
    for (i = 0; i < count; ++i) {
        addons_label(app, i, label, sizeof label);
        longest = SDL_max(longest, (int)SDL_utf8strlen(label) + 4);
    }
    *r = over_rect(app, longest, head_h(app) + h + 12 * s);
    list_place(app, rows, gui_rect(r->x + cell_w(app), r->y + head_h(app) + 6 * s, r->w - 2 * cell_w(app), h), count,
               app->addons_focus);
}
static void render_addons(App* app) {
    GuiRect r;
    GuiList rows;
    char label[160];
    int i;
    addons_boxes(app, &r, &rows);
    over_box(app, r, app->addons_title);
    for (i = rows.first; i < rows.first + rows.shown; ++i) {
        addons_label(app, i, label, sizeof label);
        list_item(app, &rows, i, label, app->addons_focus == i, false);
    }
}
/* what is picked kept as the game's addons= and mods_off=: 1, else 0 with app->message saying why. The picked
   update's program, absolute and owned, in *program when it is not NULL (NULL with no update picked) */
static int addons_save(App* app, char** program) {
    char *folder, *name, *ini, *text, *off = snx_strdup("");
    SnxAddon* list = (SnxAddon*)calloc(app->addons.count + 1, sizeof *list);
    size_t i, count = 0;
    int saved;
    if (program) *program = NULL;
    addons_where(app, &folder, &name);
    for (i = 0; list && i < app->addons.count; ++i)
        if (app->addons_checked[i]) {
            snprintf(list[count++].entry, sizeof list->entry, "%s", addon_entry(app, i));
            if (program && addon_kind(app, i) == SNX_ADDON_UPDATE) *program = snx_absolute(app->addons.paths[i]);
        }
    for (i = 0; i < app->mods_count; ++i)
        if (!app->mods_on[i]) {
            char* more = snx_format("%s%s%s", off, off[0] ? "/" : "", app->mods[i]);
            free(off);
            off = more;
        }
    ini = snx_game_ini(folder, name);
    text = list ? snx_addons_text(list, count) : NULL;
    saved = text && !snx_ini_record(ini, "addons", text) && !snx_ini_record(ini, "mods_off", off);
    if (!saved) {
        snprintf(app->message, sizeof app->message, snx_tr("Not saved: %s could not be written"), ini);
        say_failure(app, app->message);
    }
    free(off);
    free(text);
    free(ini);
    free(list);
    free(folder);
    free(name);
    return saved;
}
static void launch(App* app, char* absolute, bool prepared);
/* row pressed: an update picked (the others not: one runs at a time), No update, a DLC's or a mod's box checked or
   not; Open mods folder, the folder opened; Save, the picks kept and the box closed; or Play, the picks kept and the
   game played (or recompiled) through the driver, from the picked update's program. A mod's exefs/ changes the
   program, which the driver recompiles before it plays */
static void addons_press(App* app, int row) {
    int item = addons_row(app, row), selected = app->menu.selected;
    size_t i;
    char* path;
    bool recompile = app->recompile;
    if (addons_mod(app, item) >= 0) {
        app->mods_on[addons_mod(app, item)] = !app->mods_on[addons_mod(app, item)];
        return;
    }
    if (item == ROW_MODS_FOLDER) {
        if (app->mods_folder) show_folder(app, app->mods_folder);
        return;
    }
    if (item >= 0 && addon_kind(app, (size_t)item) == SNX_ADDON_DLC) {
        app->addons_checked[item] = !app->addons_checked[item];
        return;
    }
    if (item >= 0 || item == ROW_NO_UPDATE) {
        for (i = 0; i < addons_updates(app); ++i) app->addons_checked[i] = (int)i == item;
        return;
    }
    if (!addons_save(app, item == ROW_PLAY ? &path : NULL)) return;
    if (item == ROW_SAVE) {
        snprintf(app->message, sizeof app->message, snx_tr("%s: its updates, DLCs and mods kept"), app->addons_title);
        addons_close(app);
        return;
    }
    if (!path) path = snx_absolute(app->addons_game);
    app->recompile = app->addons_recompile;
    addons_close(app);
    app->menu.selected = GUI_SWITCH;
    launch(app, path, true);
    if (!app->menu.running) app->menu.selected = selected;
    app->recompile = recompile;
}
/* a key on the box: Up and Down move, Select presses the row, Back closes it, nothing kept */
static void addons_key(App* app, GuiKey key) {
    int count = addons_rows(app);
    if (key == GUI_UP || key == GUI_DOWN)
        app->addons_focus = (app->addons_focus + (key == GUI_UP ? count - 1 : 1)) % count;
    else if (key == GUI_SELECT)
        addons_press(app, app->addons_focus);
    else if (key == GUI_CANCEL)
        addons_close(app);
}
/* the pointer on it: the row under it takes the focus, a click presses it; a click off the box closes it */
static void addons_pointer(App* app, float x, float y, bool click, bool touch) {
    GuiRect r;
    GuiList rows;
    int i;
    addons_boxes(app, &r, &rows);
    if ((i = list_hit(&rows, x, y, touch)) >= 0) {
        app->addons_focus = i;
        if (click) addons_press(app, i);
    } else if (click && (x < (float)r.x || x >= (float)(r.x + r.w) || y < (float)r.y || y >= (float)(r.y + r.h)))
        addons_close(app);
}
/* ---- the head: the tabs, and the title after them or under them ---- */
static int title_size(const App* app) { return (int)SDL_clamp(app->theme.title_size, 1, 3); }
/* value from pixel x, y in letters size times the text's, cut at the right edge of r */
static void big_text(App* app, GuiRect r, int x, int y, const char* value, int size, SDL_Color color) {
    float w = 8.0f * (float)(app->scale * size), h = 16.0f * (float)(app->scale * size);
    SDL_SetTextureColorMod(app->font, color.r, color.g, color.b);
    while (*value) {
        int c = gui_menu_glyph(gui_utf8_step(&value)), n = gui_glyph_cols(c);
        if ((float)x + (float)n * w > (float)(r.x + r.w)) break;
        glyph_sized(app, (float)x, (float)y, c, w, h);
        x += n * (int)w;
    }
}
#define SAY_SETTINGS_TITLE SNX_TR_NOOP("SCARLETNX  /  SETTINGS")
static const char* screen_title(const App* app, char* out, size_t size) {
    char* c;
    if (app->setup_on && !app->editing) return snx_tr("SCARLETNX  /  SETUP");
    if (!app->editing)
        return app->theme
            .title[(app->menu.running || app->finished) && !entering(app) ? screen(app) : menu_screen(app)];
    if (app->form == GUI_FORM_THEME)
        snprintf(out, size, snx_tr("SCARLETNX  /  THEME EDITOR  /  %s"), app->editor.id);
    else if (app->form == GUI_FORM_PAD) {
        if (app->pad_player) /* a player's own */
            snprintf(out, size, snx_tr("SCARLETNX  /  SETTINGS  /  %s  /  PLAYER %d  /  %s"),
                     snx_tr(app->group_keys[app->group].key), app->pad_player,
                     snx_tr(app->settings.keys[app->page].key));
        else
            snprintf(out, size, snx_tr("SCARLETNX  /  SETTINGS  /  %s  /  %s"), snx_tr(app->group_keys[app->group].key),
                     snx_tr(app->settings.keys[app->page].key));
        for (c = out; *c; ++c) *c = (char)SDL_toupper((unsigned char)*c);
    } else if (app->form == GUI_FORM_FOLDERS)
        snprintf(out, size, "%s", snx_tr("SCARLETNX  /  SETTINGS  /  FOLDERS"));
    else
        snprintf(out, size, "%s", snx_tr(SAY_SETTINGS_TITLE));
    return out;
}
/* the status box: the usage, the disk and the working folder, wrapped to width columns; with draw, drawn in r */
static int status_rows(App* app, int width, GuiRect r, int y, int draw) {
    char usage[256], disk[96], cwd[1100], *apart;
    const char* lines[4];
    int i, rows = 0;
    SDL_Color soft = rgb(app->theme.soft), ink = rgb(app->theme.ink);
    gui_usage_text(usage, sizeof usage, app->usage.available, app->usage.memory_bytes, app->usage.cpu_percent,
                   SDL_GetNumLogicalCPUCores());
    disk_text(app, disk, sizeof disk);
    snprintf(cwd, sizeof cwd, "%s %s", snx_tr("CWD:"), app->cwd ? app->cwd : snx_tr("(unavailable)"));
    lines[0] = usage;
    lines[1] = "";
    lines[2] = disk;
    lines[3] = cwd;
    /* the usage too wide for a row: the memory and the CPU a row each, at the two spaces between them, so a row breaks
       between two values and never inside one ("(0.5 cores)") */
    if (gui_text_cols(usage) > width && (apart = strstr(usage, "  ")) != NULL) {
        *apart = 0;
        lines[1] = apart + 2;
    }
    for (i = 0; i < 4; ++i) {
        if (!lines[i][0]) continue;
        if (draw)
            rows += wrap_in(app, r, r.x, y + rows * cell_h(app), lines[i], i == 3, i == 3 ? ink : soft);
        else
            rows += wrap_rows(app, lines[i], width, i == 3);
    }
    return rows;
}
/* the screens the tabs go round, Tab and Shift+Tab or a click on one: the menu, Games and the Settings */
enum { GUI_TAB_MENU, GUI_TAB_GAMES, GUI_TAB_SETTINGS, GUI_TABS };
static const char* const tab_names[GUI_TABS] = {SNX_TR_NOOP("Menu"), SNX_TR_NOOP("Games"), SNX_TR_NOOP("Settings")};
static int tab_shown(const App* app) {
    if (app->setup_on && !app->editing) return app->setup_page;
    if (app->editing) return GUI_TAB_SETTINGS;
    return app->menu.view == GUI_LOAD_GAMES ? GUI_TAB_GAMES : GUI_TAB_MENU;
}
/* the tabs while the setup shows: its pages, the notices' then each step's, numbered */
enum { SETUP_NOTICE = SNX_SETUP_STEPS }; /* a page's: a step's, or the notices' */
static const char* const setup_step_names[SNX_SETUP_STEPS] = {SNX_TR_NOOP("Appearance"), SNX_TR_NOOP("Install"),
                                                              SNX_TR_NOOP("Folders"), SNX_TR_NOOP("Keys"),
                                                              SNX_TR_NOOP("User")};
static int tab_count(const App* app) { return app->setup_on && !app->editing ? app->setup_count : GUI_TABS; }
/* page i's number among the steps, from 1; 0 for the notices' */
static int setup_number(const App* app, int i) {
    return app->setup_pages[i] == SETUP_NOTICE ? 0 : i + (app->setup_pages[0] != SETUP_NOTICE);
}
static void tab_label(const App* app, int i, char* out, size_t size) {
    if (!app->setup_on || app->editing)
        snprintf(out, size, "%s", snx_tr(tab_names[i]));
    else if (app->setup_pages[i] == SETUP_NOTICE)
        snprintf(out, size, "%s", snx_tr("Notice"));
    else
        snprintf(out, size, "%d %s", setup_number(app, i), snx_tr(setup_step_names[app->setup_pages[i]]));
}
/* tab i in the row of tabs: two columns wider than its name, a column after the one before it */
static GuiRect tab_rect(const App* app, GuiRect row, int i) {
    char label[64];
    int x = row.x, k;
    for (k = 0; k < i; ++k) {
        tab_label(app, k, label, sizeof label);
        x += (gui_text_cols(label) + 3) * cell_w(app);
    }
    tab_label(app, i, label, sizeof label);
    return gui_rect(x, row.y, (gui_text_cols(label) + 2) * cell_w(app), row.h);
}
/* the tabs, the one shown as the focused box is; on the head's band in its colours swapped */
static void draw_tabs(App* app, GuiRect row, int band) {
    const GuiTheme* t = &app->theme;
    GuiRgb ink = band ? t->page : t->ink, page = band ? t->ink : t->page, focus = band ? t->page : t->focus;
    int i, s = app->scale, shown = tab_shown(app);
    char label[64];
    if (!row.h) return;
    for (i = 0; i < tab_count(app); ++i) {
        GuiRect r = tab_rect(app, row, i);
        int on = i == shown, inverted = on && t->focus_bar;
        if (inverted)
            fill_rect(app, r, focus);
        else if (t->panels == GUI_PANELS_FILL)
            fill_rect(app, r, t->dim);
        if (!inverted && (t->panels == GUI_PANELS_OUTLINE || on)) outline(app, r, on ? focus : t->dim);
        tab_label(app, i, label, sizeof label);
        text_in(app, r, r.x + cell_w(app), r.y + 2 * s, label, rgb(inverted ? page : on ? focus : ink));
    }
}
/* the page's faint grid behind the boxes, in the dim colour */
static void backdrop(App* app) {
    const GuiTheme* t = &app->theme;
    int width, height, s = app->scale, x, y;
    if (!t->grid) return;
    area(app, &width, &height);
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app->renderer, t->dim.r, t->dim.g, t->dim.b, 72);
    for (x = 0; x < width; x += 6 * s) SDL_RenderLine(app->renderer, (float)x, 0, (float)x, (float)height);
    for (y = 0; y < height; y += 6 * s) SDL_RenderLine(app->renderer, 0, (float)y, (float)width, (float)y);
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_NONE);
}
/* title past its first n parts, each ending at a '/' (the spaces after it left out), or its last part */
static const char* title_tail(const char* title, int n) {
    const char* slash;
    for (; n > 0 && (slash = strchr(title, '/')) != NULL; --n)
        for (title = slash + 1; *title == ' ';) ++title;
    return title;
}
static int title_px(const App* app, const char* title, int size) {
    return gui_text_cols(title) * size * cell_w(app) + (app->theme.title_shadow ? size * app->scale : 0);
}
/* the pixels a title of letters size times the text's has in the head, as gui_place gives them: beside the tabs and
   the user, or with the user on a row under the tabs */
static int head_room(const App* app, int size) {
    GuiRect none = {0, 0, 0, 0}, last = tab_rect(app, none, tab_count(app) - 1);
    int width, height, tabs = cell_h(app) + 4 * app->scale, title = size * cell_h(app), badge;
    if (app->theme.title_shadow) title += size * app->scale;
    area(app, &width, &height);
    badge = user_width(app, app->theme.head_place == GUI_HEAD_TABS ? SDL_max(title, tabs) : title);
    width -= 2 * cell_w(app) + (badge ? badge + 2 * cell_w(app) : 0);
    return app->theme.head_place == GUI_HEAD_TABS ? width - (last.x + last.w) - 2 * cell_w(app) : width;
}
/* The head's title over the tabs' screens and its letters' size: a window too narrow for the main screens' titles
   (the menu's, Games', the keys' and the Settings') whole at the theme's size takes a size smaller, down to the
   text's, and only then leaves out the same first parts of every one, "SCARLETNX  /" first, so the head stays one
   and the same from screen to screen. The size is the English titles', so the head is as big in every language: a
   translation too long for it leaves out first parts too. A screen deeper in the Settings leaves out what more it
   must, and a last part still too wide is cut. The setup's head, its steps for tabs, keeps its title as it is */
static const char* head_title(const App* app, bool tabs, char* out, size_t size, int* letters) {
    const char *titles[4], *english[4];
    char own[160];
    const char* title = screen_title(app, own, sizeof own);
    int big = title_size(app), parts = 0, room = 0, i;
    *letters = big;
    snprintf(out, size, "%s", title);
    if (!tabs || !title[0] || (app->setup_on && !app->editing)) return out; /* the setup's steps are its tabs */
    titles[0] = english[0] = app->theme.title[GUI_SCREEN_MENU];
    titles[1] = english[1] = app->theme.title[GUI_SCREEN_GAMES];
    titles[2] = english[2] = app->theme.title[GUI_SCREEN_KEYS];
    english[3] = SAY_SETTINGS_TITLE;
    titles[3] = snx_tr(SAY_SETTINGS_TITLE);
    for (parts = 0; parts < 4; ++parts) {
        for (*letters = big; *letters >= 1; --*letters) {
            room = head_room(app, *letters);
            for (i = 0; i < 4 && title_px(app, title_tail(english[i], parts), *letters) <= room; ++i) {}
            if (i == 4) break;
        }
        if (*letters >= 1) break;
    }
    if (parts == 4) { /* not even their last parts fit: no title on any, the tabs say which screen it is */
        *letters = 1;
        out[0] = 0;
        return out;
    }
    for (parts = 0; parts < 3; ++parts) { /* at that size, the first parts the translations leave out, alike */
        for (i = 0; i < 4 && title_px(app, title_tail(titles[i], parts), *letters) <= room; ++i) {}
        if (i == 4) break;
    }
    title = title_tail(title, parts);
    while (title_px(app, title, *letters) > room && strchr(title, '/')) title = title_tail(title, 1);
    snprintf(out, size, "%.*s", (int)gui_cols_fit(title, SDL_max(room, 0) / (*letters * cell_w(app))), title);
    return out;
}
static void chrome(App* app, const GuiPlace* p) {
    const GuiTheme* t = &app->theme;
    int band = t->head == GUI_STYLE_BAR, size, s = app->scale;
    char out[160];
    const char* title = head_title(app, p->tabs.h > 0, out, sizeof out, &size);
    SDL_SetRenderDrawColor(app->renderer, t->page.r, t->page.g, t->page.b, 255);
    SDL_RenderClear(app->renderer);
    backdrop(app);
    if (!p->head.h) return;
    if (band) fill_rect(app, gui_rect(p->head.x, p->head.y - 2 * s, p->head.w, p->head.h + 4 * s), t->ink);
    if (t->head_rule)
        fill_rect(app, gui_rect(p->head.x, p->head.y + p->head.h + cell_h(app) / 4 - s / 2, p->head.w, s), t->dim);
    draw_tabs(app, p->tabs, band);
    draw_user(app, p->badge, band);
    if (!p->title.h) return;
    if (t->title_shadow)
        big_text(app, p->title, p->title.x + size * s, p->title.y + size * s, title, size, rgb(t->dim));
    big_text(app, p->title, p->title.x, p->title.y, title, size, rgb(band ? t->page : t->ink));
}
static void place_screen(App* app, GuiPlace* p, const char* keys, int list_cols, int shows) {
    const GuiTheme* t = &app->theme;
    GuiPlaceIn in;
    GuiRect none = {0, 0, 0, 0};
    int width, height, s = app->scale, size;
    char out[160];
    const char* title = head_title(app, shows & GUI_SHOWS_TABS, out, sizeof out, &size);
    area(app, &width, &height);
    memset(&in, 0, sizeof in);
    in.width = width;
    in.height = height;
    in.cw = cell_w(app);
    in.ch = cell_h(app);
    if (shows & GUI_SHOWS_TABS) {
        GuiRect last = tab_rect(app, none, tab_count(app) - 1);
        in.tabs = cell_h(app) + 4 * s;
        in.tabs_w = last.x + last.w;
    }
    if (title[0]) {
        int shadow = t->title_shadow ? size * s : 0;
        in.title = size * cell_h(app) + shadow;
        in.title_w = gui_text_cols(title) * size * cell_w(app) + shadow;
        in.title_beside = t->head_place == GUI_HEAD_TABS;
    }
    /* the user as high as the head's row: the title's, or the tabs' with none */
    in.badge = in.title ? SDL_max(in.title, in.title_beside ? in.tabs : 0) : in.tabs;
    in.badge_w = user_width(app, in.badge);
    if (!in.badge_w) in.badge = 0;
    if (t->tips_place == GUI_TIPS_TOP && !app->setup_on) in.tips = cell_h(app) + 4 * s; /* the setup's: the foot's */
    in.list_cols = list_cols ? SDL_min(SDL_max(list_cols, 20), SDL_max(20, (width / cell_w(app) - 2) * 3 / 5)) : 0;
    in.fixed = t->sizing == GUI_SIZING_FIXED && list_cols && list_cols < width / cell_w(app); /* a list, not a run */
    in.side = app->side_box;
    app->scale = foot_scale(app);
    { /* the widest key, which a row cannot break */
        const char* at = keys;
        char key[128], what[128];
        while (hint_next(&at, key, what, sizeof key))
            in.keys_w = SDL_max(in.keys_w, (hint_width(key, what) + (t->footer_box ? 4 : 0)) * cell_w(app));
    }
    in.foot = cell_h(app);
    gui_place(&in, p); /* the columns, which the foot's rows and the status box's follow */
    in.foot = SDL_max(in.tips || !t->tips_wrap ? 1 : 2,
                      hints(app, keys, foot_text(app, p->keys, t->footer_box).w / cell_w(app), none, 0, 0, 0, NULL)) *
                  cell_h(app) +
              4 * app->scale;
    app->scale = s;
    if (shows & GUI_SHOWS_STATUS && in.list_cols && t->footer != GUI_STYLE_NONE) /* at the side's width */
        in.status =
            head_h(app) + 4 * s + status_rows(app, inner(app, p->side).w / cell_w(app), none, 0, 0) * cell_h(app);
    gui_place(&in, p);
}
static void status_box(App* app, GuiRect r) {
    GuiRect text = inner(app, r);
    int y;
    if (!r.h) return;
    y = pane(app, r, snx_tr("Status"));
    status_rows(app, text.w / cell_w(app), gui_rect(text.x, y, text.w, r.y + r.h - y), y, 1);
}
/* ---- the menu: the items, ui.txt's scene Menu; beside them the focused one ---- */
typedef struct {
    GuiPlace place;
    GuiRect rects[UI_MENU_ITEMS];
    UiList lists[UI_MENU_ITEMS]; /* the entries' at UI_MENU_ENTRIES: row i is item i */
} GuiMenuBoxes;
/* what an item is about, for the tips and the side box */
static const char* item_about(const App* app, int item) {
    static const char* const loads[] = {SNX_TR_NOOP("Recompiles a game's .nsp, .xci or .nro, then runs it"),
                                        SNX_TR_NOOP("Runs an ARM64 Linux program on the CPU"),
                                        SNX_TR_NOOP("Opens a 3DS .cia")};
    if (item >= GUI_SWITCH && item <= GUI_3DS) return snx_tr(loads[item - GUI_SWITCH]);
    if (item == GUI_KEYS) return snx_tr("Installs prod.keys, which games are extracted with");
    if (item == GUI_EXIT) return snx_tr("Closes ScarletNX");
    {
        GuiMenu menu = app->menu;
        menu.selected = item;
        return gui_setting_description(&menu);
    }
}
static void menu_boxes(App* app, GuiMenuBoxes* m) {
    int i, longest = 0;
    char label[160], keys[256];
    for (i = 0; i < GUI_ITEM_COUNT; ++i) {
        menu_label(app, i, label, sizeof label);
        longest = SDL_max(longest, gui_text_cols(label));
    }
    place_screen(app, &m->place, shown_keys(app, keys, sizeof keys), longest + 2 + gui_text_cols(app->theme.cursor),
                 GUI_SHOWS_STATUS | GUI_SHOWS_TABS);
    ui_menu_layout(m->place.list, app->scale, m->rects);
    memset(m->lists, 0, sizeof m->lists);
    m->lists[UI_MENU_ENTRIES].focus = (size_t)app->menu.selected;
    ui_menu_follow(m->rects, app->scale, UI_MENU_ENTRIES, GUI_ITEM_COUNT, &m->lists[UI_MENU_ENTRIES]);
}
/* the last run's output, under what the side box says */
static void last_output(App* app, GuiRect r, int y) {
    SDL_Color soft = rgb(app->theme.soft);
    int i, x;
    if (!app->has_output || entering(app)) return;
    y += cell_h(app);
    y += wrap_in(app, r, r.x, y, snx_tr("Last output (no scrollback):"), 0, soft) * cell_h(app);
    SDL_SetTextureColorMod(app->font, app->theme.ink.r, app->theme.ink.g, app->theme.ink.b);
    for (i = 0; i < app->console.rows && y + cell_h(app) <= r.y + r.h; ++i, y += cell_h(app))
        for (x = 0; x < app->console.cols && (x + 1) * cell_w(app) <= r.w; ++x)
            if (app->console.cells[i][x] != ' ')
                glyph(app, (float)(r.x + x * cell_w(app)), (float)y, app->console.cells[i][x]);
}
/* the side box: the focused item, and for the keys the ones in use; the last run's output under it. What the item
   does is the tips', not said twice */
static void menu_side(App* app, const GuiMenuBoxes* m) {
    int item = (int)m->lists[UI_MENU_ENTRIES].focus, y;
    GuiRect r = inner(app, m->place.side);
    char line[1100];
    if (!m->place.side.h) return;
    y = pane(app, m->place.side, snx_tr(entry_text(item)));
    r = gui_rect(r.x, y, r.w, m->place.side.y + m->place.side.h - y);
    if (item == GUI_KEYS) {
        char* in_use = keys_in_use(app);
        snprintf(line, sizeof line, snx_tr("In use: %s"), in_use ? in_use : snx_tr("none"));
        y += wrap_in(app, r, r.x, y, line, 1, rgb(in_use ? app->theme.soft : app->theme.failure)) * cell_h(app);
        free(in_use);
    }
    last_output(app, r, y);
}
/* the entries' attributes: the one activated last in the active colour; blinking in the failure colour, Setup device
   keys while no prod.keys is in use, so a first start shows what to do first, and Share log after a run crashed until
   its log is saved */
unsigned ui_menu_entries_attr_cb(App* app, size_t row) {
    unsigned attr = app->menu.activated && (size_t)app->menu.active == row ? UI_ATTR_ACTIVE : 0;
    if (row == GUI_KEYS) {
        char* in_use = keys_in_use(app);
        if (!in_use) attr |= UI_ATTR_FAILURE | UI_ATTR_BLINK;
        free(in_use);
    }
    if (row == GUI_LOG && app->log_due) attr |= UI_ATTR_FAILURE | UI_ATTR_BLINK;
    return attr;
}
static void draw_menu(App* app) {
    GuiMenuBoxes m;
    char keys[256];
    menu_boxes(app, &m);
    chrome(app, &m.place);
    ui_menu_draw(app, m.rects, app->scale, m.lists);
    menu_side(app, &m);
    status_box(app, m.place.status);
    screen_foot(app, &m.place, item_about(app, (int)m.lists[UI_MENU_ENTRIES].focus),
                shown_keys(app, keys, sizeof keys));
}
/* ---- Games: Add a folder, then "Recent games" over the recent rows and a page of the games, as many as the list's
   box holds ---- */
typedef struct {
    GuiPlace place;
    GuiRect rects[UI_GAMES_ITEMS];
    UiList
        lists[UI_GAMES_ITEMS]; /* Add's, and Rows': the label and the recent rows under games_lead, then the page's */
    size_t start;              /* the page's first game */
    int lead;                  /* games_lead */
} GuiGamesBoxes;
static void games_boxes(App* app, GuiGamesBoxes* g) {
    size_t selected = app->browser.selected, head = app->browser.head;
    games_place(app, &g->place);
    ui_games_layout(g->place.list, app->scale, g->rects);
    g->start = app->browser.page * app->browser.page_size;
    g->lead = games_lead(app);
    memset(g->lists, 0, sizeof g->lists);
    g->lists[UI_GAMES_ADD].focus = selected ? SIZE_MAX : 0;
    g->lists[UI_GAMES_ROWS].focus = !selected         ? SIZE_MAX
                                    : selected < head ? selected
                                                      : (size_t)g->lead + selected - head;
    /* a page under the recent rows that does not fit (a short screen, a page of one) scrolls to the focus; with Add a
       folder focused the rows stay at their top */
    if (selected)
        ui_games_follow(g->rects, app->scale, UI_GAMES_ROWS, (size_t)g->lead + gui_browser_items(&app->browser),
                        &g->lists[UI_GAMES_ROWS]);
}
static void game_label(const App* app, const GuiGames* games, size_t i, char* out, size_t size);
static void menu_key(App* app, GuiKey key);
/* Rows' row i as the browser counts it (0 is Add a folder): a recent row, else a game of the page */
static size_t games_selected(const App* app, size_t row) {
    size_t lead = (size_t)games_lead(app);
    return row < lead ? row : app->browser.head + row - lead;
}
/* Rows' row: "Recent games", the recent rows and their show more, a page's headings and games; a game in the active
   colour once activated, the pages before and after marked on the first game and the last */
bool ui_games_rows_get_row_info(App* app, size_t row, int width, UiRowInfo* info) {
    char* label = app->row_text;
    size_t lead = (size_t)games_lead(app), items = gui_browser_items(&app->browser), i, start;
    (void)width; /* a game's name slides when it is too long, as the menu's rows */
    start = app->browser.page * app->browser.page_size;
    if (row == 0 && lead) {
        info->label = snx_tr("Recent games");
        info->attr = UI_ATTR_LABEL;
        return true;
    }
    if (row < lead) {
        int index = (int)row - 1;
        snprintf(label, sizeof app->row_text, "%s",
                 index < recent_shown(app) ? app->recent.game[index].name : snx_tr("...show more"));
        info->label = label;
        if (index < recent_shown(app) && app->recent.game[index].driver == GUI_SWITCH)
            info->image = game_icon(app, app->recent.game[index].path);
        return true;
    }
    i = row - lead;
    if (i >= items) return false;
    if (gui_games_heading(&app->games, start + i)) { /* Recompiled, Dumps */
        info->label = app->games.shown[start + i];
        info->attr = UI_ATTR_LABEL;
        return true;
    }
    game_label(app, &app->games, start + i, label, sizeof app->row_text);
    info->label = label + strlen(app->theme.marker); /* ui_draw_row puts the marker */
    info->image = game_icon(app, app->games.paths[start + i]);
    if (app->browser.activated && app->browser.active == start + i) info->attr |= UI_ATTR_ACTIVE;
    if (i == 0 && app->browser.page > 0) info->attr |= UI_ATTR_MORE_UP;
    if (i + 1 == items && app->browser.page + 1 < gui_browser_pages(&app->browser)) info->attr |= UI_ATTR_MORE_DOWN;
    return true;
}
/* a row hit: the focus on it, and a click opens it; "Recent games" is no row to take */
void ui_games_rows_row_action_cb(App* app, size_t row, uint32_t hit) {
    if (row == 0 && games_lead(app)) return;
    app->browser.selected = games_selected(app, row);
    if (hit & UI_HIT_CLICK) menu_key(app, GUI_SELECT);
}
void ui_games_add_add_a_folder_action_cb(App* app, uint32_t hit) {
    app->browser.selected = 0;
    if (hit & UI_HIT_CLICK) menu_key(app, GUI_SELECT);
}
/* the focused row of Games: what it is, about it, and a line more for a recent game's or a game's kind */
static bool game_was_old(const App* app, const char* path);
/* game i's kind, by its file's name: ready, a dump or a CIA; its updates, DLCs and mods; the games/ of an older
   version that holds it, as its row's (games/) says */
static void game_kind(const App* app, const GuiGames* games, size_t i, char* kind, size_t size) {
    const char* name = games->names[i];
    size_t n;
    if (games->addons[i])
        snprintf(kind, size, snx_tr("Ready to run, with %zu update(s), DLC(s) or mod(s) to pick from"), games->addons[i]);
    else
        snprintf(kind, size, "%s",
                 gui_is_cia(name)    ? snx_tr("3DS CIA, opened with scarletnx-3ds")
                 : gui_is_dump(name) ? snx_tr("A dump, prepared when it runs")
                                     : snx_tr("Ready to run"));
    n = strlen(kind);
    if (game_was_old(app, games->paths[i]))
        snprintf(kind + n, size - n, "%s",
                 snx_tr(", in the games/ an older version made: new games go to recompiled/"));
}
static const char* games_focus(App* app, const GuiGamesBoxes* g, const char** about, char* kind, size_t size) {
    static const char* const kinds[] = {"Switch", "Linux ARM", "3DS"}; /* names, never translated */
    size_t selected = app->browser.selected, head = app->browser.head;
    kind[0] = 0;
    if (!selected) {
        *about = snx_tr("Pick a folder of games or dumps, listed after recompiled/ and dumps/");
        return snx_tr("Add a folder");
    }
    if (selected < head && (int)selected - 1 < recent_shown(app)) {
        const GuiRecentGame* r = &app->recent.game[selected - 1];
        snprintf(kind, size, "%s, %s", kinds[r->driver >= GUI_SWITCH && r->driver <= GUI_3DS ? r->driver : 0],
                 r->prepared ? snx_tr("ready to run") : snx_tr("prepared when it runs"));
        *about = r->path;
        return r->name;
    }
    if (selected < head) {
        *about = snx_tr("The recent games not shown");
        return snx_tr("...show more");
    }
    if (gui_games_heading(&app->games, g->start + selected - head)) { /* a group's heading */
        *about = "";
        return app->games.shown[g->start + selected - head];
    }
    game_kind(app, &app->games, g->start + selected - head, kind, size);
    *about = app->games.paths[g->start + selected - head];
    return app->games.shown[g->start + selected - head];
}
/* a game's row in Games: its name, how many updates, DLCs and mods it has, and (games/) when an older version's
   games/ holds it */
static void game_label(const App* app, const GuiGames* games, size_t i, char* out, size_t size) {
    const char* was = game_was_old(app, games->paths[i]) ? snx_tr("  (games/)") : "";
    if (games->addons[i]) /* its updates, DLCs and mods, which opening it offers */
        snprintf(out, size, "%s%s  +%zu%s", app->theme.marker, games->shown[i], games->addons[i], was);
    else
        snprintf(out, size, "%s%s%s", app->theme.marker, games->shown[i], was);
}
static void draw_games(App* app) {
    GuiGamesBoxes g;
    GuiRect r;
    char line[1100], keys[256], kind[192];
    const char *about, *name;
    size_t i;
    SDL_Color soft = rgb(app->theme.soft), ink = rgb(app->theme.ink), failure = rgb(app->theme.failure);
    int y;
    aot_status_update(app);
    games_boxes(app, &g);
    chrome(app, &g.place);
    ui_games_draw(app, g.rects, app->scale, g.lists);
    /* beside it the focused row: what it is and its path, whole; the page, and where the games are looked for. The
       tips say what it is (a game's kind) or does, which the box does not say again */
    name = games_focus(app, &g, &about, kind, sizeof kind);
    r = inner(app, g.place.side);
    if (g.place.side.h) {
        y = pane(app, g.place.side, name);
        r = gui_rect(r.x, y, r.w, g.place.side.y + g.place.side.h - y);
        y += wrap_in(app, r, r.x, y, about, app->browser.selected != 0, ink) * cell_h(app);
        if (kind[0]) y += wrap_in(app, r, r.x, y, kind, 0, soft) * cell_h(app);
        if (app->aot_state == GUI_AOT_CACHE) {
            char size[32];
            gui_size_text(size, sizeof size, app->aot_info.size);
            snprintf(line, sizeof line, snx_tr("JIT cache: %s, O%c"), size, app->aot_info.opt);
            y += wrap_in(app, r, r.x, y, line, 0, soft) * cell_h(app);
            snprintf(line, sizeof line, snx_tr("AOT build: O%c%s"), app->aot_build_opt,
                     app->aot_match ? "" : snx_tr(" (levels differ)"));
            y += wrap_in(app, r, r.x, y, line, 0, app->aot_match ? soft : failure) * cell_h(app);
        } else if (app->aot_state == GUI_AOT_NO_CACHE) {
            y += wrap_in(app, r, r.x, y, snx_tr("JIT cache: none yet. Play once to collect one."), 0, soft) *
                 cell_h(app);
        } else if (app->aot_state == GUI_AOT_BAD) {
            y += wrap_in(app, r, r.x, y, snx_tr("JIT cache: unreadable. Play once to rebuild it."), 0, failure) *
                 cell_h(app);
        }
        y += cell_h(app);
        if (!app->games.count)
            y += wrap_in(app, r, r.x, y, snx_tr(SAY_NO_GAMES), 0, soft) * cell_h(app);
        else {
            snprintf(line, sizeof line, snx_tr("Page %zu/%zu, %zu game(s)"), app->browser.page + 1,
                     gui_browser_pages(&app->browser), games_listed(app));
            y += wrap_in(app, r, r.x, y, line, 0, soft) * cell_h(app);
        }
        y += wrap_in(app, r, r.x, y, app->games_dir ? app->games_dir : "recompiled/", 1, soft) * cell_h(app);
        {
            char* dumps = app->data_dir ? snx_path_join(app->data_dir, "dumps") : NULL;
            y += wrap_in(app, r, r.x, y, dumps ? dumps : "dumps/", 1, soft) * cell_h(app);
            free(dumps);
        }
        for (i = 0; i < (size_t)app->folder_count; ++i)
            y += wrap_in(app, r, r.x, y, app->folders[i], 1, soft) * cell_h(app);
        last_output(app, r, y);
    }
    status_box(app, g.place.status);
    screen_foot(app, &g.place, kind[0] ? kind : about, shown_keys(app, keys, sizeof keys));
}
/* a line that waits on the user, at pixel row y of r: plain at its left, or framed, placed and blinking as the theme's
   prompt says */
static void prompt_in(App* app, GuiRect r, int y, GuiPromptLine line, const char* value, SDL_Color color) {
    char framed[160];
    const GuiPrompt* p = &app->theme.prompt;
    int x = r.x, w;
    if (p->lines[line]) {
        if (p->blink && app->theme.blink_ms && (now_ms(app) / app->theme.blink_ms) % 2) return;
        snprintf(framed, sizeof framed, "%s%s%s", p->open, value, p->close);
        value = framed;
        w = gui_text_cols(framed) * cell_w(app);
        if (p->align == GUI_ALIGN_CENTER) x = r.x + (r.w - w) / 2;
        if (p->align == GUI_ALIGN_RIGHT) x = r.x + r.w - w;
    }
    text_in(app, r, SDL_max(r.x, x), y, value, color);
}
/* ---- the screen a file is picked or dropped on: one box with where the keys go and the ones in use, the line that
   waits in its middle; ESC, or the foot's, goes back */
typedef struct {
    GuiPlace place;
    int prompt; /* the waiting line's pixel row */
} GuiPickBoxes;
static void pick_boxes(App* app, GuiPickBoxes* k) {
    char keys[256];
    place_screen(app, &k->place, shown_keys(app, keys, sizeof keys), 0, GUI_SHOWS_TABS);
    k->prompt = k->place.list.y + k->place.list.h / 2;
}
static void draw_pick(App* app) {
    GuiPickBoxes k;
    GuiRect r;
    bool keys = app->menu.view == GUI_LOAD_KEYS;
    char line[1100], hints_line[256], *in_use = keys ? keys_in_use(app) : NULL;
    int y;
    pick_boxes(app, &k);
    chrome(app, &k.place);
    y = pane(app, k.place.list, snx_tr(entry_text(keys ? GUI_KEYS : app->menu.active)));
    r = inner(app, k.place.list);
    r = gui_rect(r.x, y, r.w, r.y + r.h - y);
    if (keys) {
        snprintf(line, sizeof line, snx_tr("Installs to: %s"),
                 app->keys_path ? app->keys_path : snx_tr("Home directory unavailable."));
        y += wrap_in(app, r, r.x, y, line, 1, rgb(app->theme.ink)) * cell_h(app);
    }
    if (keys && keys_left(app)) { /* what the last try did, and when the menu comes back */
        GuiRgb said = app->keys_ok ? app->theme.focus : app->theme.failure;
        y += cell_h(app);
        y += wrap_in(app, r, r.x, y, app->keys_ok ? snx_tr("Installed") : snx_tr("Not installed"), 0, rgb(said)) *
             cell_h(app);
        y += wrap_in(app, r, r.x, y, app->message, 1, rgb(app->keys_ok ? app->theme.ink : said)) * cell_h(app);
        snprintf(line, sizeof line,
                 app->keys_ok ? snx_tr("Back to the menu in %d s")
                              : snx_tr("Back to the menu in %d s, or pick another file"),
                 keys_left(app));
        y += wrap_in(app, r, r.x, y, line, 0, rgb(app->theme.soft)) * cell_h(app);
        if (app->keys_ok) { /* the keys just installed: nothing to warn about, nor to pick */
            free(in_use);
            in_use = NULL;
        }
    }
    if (in_use && app->keys_path && !strcmp(in_use, app->keys_path))
        y += wrap_in(app, r, r.x, y, snx_tr("Keys already setup. This will overwrite the previous ones"), 0,
                     rgb(app->theme.failure)) *
             cell_h(app);
    else if (in_use) {
        snprintf(line, sizeof line, snx_tr("Keys in use: %s. Keys dropped here are read before them"), in_use);
        y += wrap_in(app, r, r.x, y, line, 1, rgb(app->theme.soft)) * cell_h(app);
    }
    free(in_use);
    if (!keys || !app->keys_done || !app->keys_ok)
        prompt_in(app, r, SDL_max(y, k.prompt), GUI_PROMPT_KEYS, snx_tr(keys ? SAY_KEYS_WAIT : SAY_FILE_WAIT),
                  rgb(app->theme.focus));
    snprintf(line, sizeof line, "%s: %s", snx_tr(entry_text(keys ? GUI_KEYS : app->menu.active)), snx_tr(SAY_PICK_BOX));
    screen_foot(app, &k.place, line, shown_keys(app, hints_line, sizeof hints_line));
}
static void draw_run(App* app) {
    GuiPlace p;
    GuiRect r, text;
    const GuiTheme* t = &app->theme;
    int y, x, i, cols, rows, s = app->scale;
    GuiPromptLine line =
        app->finished ? (app->abnormal_exit ? GUI_PROMPT_FAILED : GUI_PROMPT_DONE) : GUI_PROMPT_ABORTING;
    run_place(app, &p);
    chrome(app, &p);
    y = pane(app, p.list, snx_tr("Output"));
    r = inner(app, p.list);
    run_console(app, &p, &cols, &rows);
    SDL_SetTextureColorMod(app->font, t->ink.r, t->ink.g, t->ink.b);
    for (i = 0; i < SDL_min(rows, app->console.rows); ++i)
        for (x = 0; x < SDL_min(cols, app->console.cols); ++x)
            if (app->console.cells[i][x] != ' ')
                glyph(app, (float)(r.x + x * cell_w(app)), (float)(y + i * cell_h(app)), app->console.cells[i][x]);
    y = pane(app, p.side, snx_tr(entry_text(app->menu.selected)));
    text = inner(app, p.side);
    text = gui_rect(text.x, y, text.w, p.side.y + p.side.h - y);
    y += wrap_in(app, text, text.x, y, app->file ? app->file : "", 1, rgb(t->ink)) * cell_h(app);
    if (app->finished)
        wrap_in(app, text, text.x, y + cell_h(app) / 2, app->message, 0,
                rgb(app->abnormal_exit ? t->failure : t->focus));
    else if (app->aborting)
        wrap_in(app, text, text.x, y + cell_h(app) / 2, snx_tr("Aborting process tree..."), 0, rgb(t->failure));
    status_box(app, p.status);
    foot(app, &p, app->file ? app->file : "", rgb(t->soft), "");
    app->scale = foot_scale(app); /* the ESC line where the keys go, in their letters */
    text = foot_text(app, p.keys, t->footer_box);
    if (app->finished || app->aborting)
        prompt_in(app, text, text.y, line, app->finished ? snx_tr(SAY_DONE) : snx_tr("Aborting process tree..."),
                  rgb(t->failure));
    else
        text_in(app, text, text.x + text.w - gui_text_cols(snx_tr(SAY_ABORT)) * cell_w(app), text.y, snx_tr(SAY_ABORT),
                rgb(t->failure));
    app->scale = s;
}
/* the menu's screen or a run's, in app->theme, on the render target as it is; the boxes over it */
static void draw_screen(App* app) {
    const GuiTheme* t = &app->theme;
    int idle = (!app->menu.running && !app->finished) || entering(app);
    if (!idle && t->run == GUI_RUN_STEPS) {
        SDL_SetRenderDrawColor(app->renderer, t->run_colors.page.r, t->run_colors.page.g, t->run_colors.page.b, 255);
        SDL_RenderClear(app->renderer);
        render_steps(app);
    } else if (!idle)
        draw_run(app);
    else if (app->menu.view == GUI_LOAD_GAMES)
        draw_games(app);
    else if (pick_screen(app))
        draw_pick(app);
    else
        draw_menu(app);
    if (app->confirm.shown) render_ask(app);
    if (idle && app->dests_shown && !app->confirm.shown) render_dests(app);
    if (idle && app->users_shown && !app->confirm.shown) render_users(app);
    if (idle && app->addons_shown && !app->confirm.shown) render_addons(app);
    if (idle && app->left_shown && app->lefts) render_files(app, 0);
    overlay(app);
}
static const char* driver_name(int item) {
    switch (item) {
#ifdef SDL_PLATFORM_ANDROID
        /* the app's libraries folder only takes lib*.so: the drivers go in as ones */
        case GUI_SWITCH: return "libscarletnx-run.so";
        case GUI_LINUX: return "libscarletnx-gnuabi.so";
        case GUI_3DS: return "libscarletnx-3ds.so";
#else
        case GUI_SWITCH: return "scarletnx";
        case GUI_LINUX: return "scarletnx-gnuabi";
        case GUI_3DS: return "scarletnx-3ds";
#endif
        default: return NULL;
    }
}
static char* find_driver(const App* app, int item) {
    const char* name = driver_name(item);
    if (app->self && item == GUI_SWITCH) return snx_strdup(app->self);
    if (!name) return NULL;
#ifdef _WIN32
    char* filename = snx_format("%s.exe", name);
#else
    char* filename = snx_strdup(name);
#endif
    char* local = snx_path_join(app->drivers, filename);
    char* found = snx_find_tool(local);
    free(filename);
    free(local);
    return found ? found : snx_find_tool(name);
}
/* what argv prints, NULL unless it ends with 0, killed after ten seconds; owned */
static char* capture(App* app, const char* const* argv) {
    char buffer[4096], *out = snx_strdup("");
    size_t size = 0;
    int status = 1, n;
    Uint64 until = SDL_GetTicks() + 10000;
    if (!gui_process_start(app->process, argv, app->message, sizeof(app->message))) status = -1;
    while (status >= 0 && !gui_process_finished(app->process, &status) && SDL_GetTicks() < until + 5000) {
        char* grown;
        n = gui_process_read(app->process, buffer, sizeof buffer);
        if (n > 0 && (grown = realloc(out, size + (size_t)n + 1)) != NULL) {
            out = grown;
            memcpy(out + size, buffer, (size_t)n);
            out[size += (size_t)n] = 0;
        } else if (n <= 0)
            SDL_Delay(1);
        if (SDL_GetTicks() > until) gui_process_terminate(app->process);
    }
    if (status) {
        free(out);
        return NULL;
    }
    return out;
}
/* The Settings' list freed, with what was made from it: its groups (their rows are the list's), a group's view, and
   a form on its rows, which would read the list freed. The only way the list goes, so nothing outlives it */
static void settings_forget(App* app) {
    if ((app->settings.values && app->editor.object == (char*)app->settings.values) ||
        app->editor.keys == app->group_keys || app->editor.keys == app->view_keys)
        gui_editor_form(&app->editor, NULL, 0, &app->app_form, NULL);
    gui_settings_free(&app->settings);
    app->groups = 0;
    app->view_count = 0;
}
char* (*snx_menu_settings)(void) = NULL;
/* the Settings' rows: the runtime's list after the launcher's own, read the first time from the runtime the menu is
   built into, else from the program that runs Switch inputs */
static GuiSettings* settings_of(App* app) {
    char* driver = NULL;
    char* list;
    if (app->settings.keys) return &app->settings;
    if (snx_menu_settings) /* this program's own: every section at once, with no program to ask */
        list = snx_menu_settings();
    else {
        driver = find_driver(app, GUI_SWITCH);
#ifdef SDL_PLATFORM_IOS /* no program to ask: the game's .ini keeps its settings */
        list = NULL;
#else
        const char* argv[] = {driver, "--settings", NULL};
        list = driver ? capture(app, argv) : NULL;
#endif
    }
    gui_settings_read(&app->settings, list ? list : "");
    free(list);
    free(driver);
    /* the launcher's own choices win over the game's .ini, as its items did: a phone plays on the whole screen, its
       pad shown */
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
    gui_settings_set(&app->settings, "fullscreen", "1");
    gui_settings_set(&app->settings, "controls", "1");
#else
    gui_settings_set(&app->settings, "fullscreen", "0");
    gui_settings_set(&app->settings, "controls", "0");
#endif
    gui_settings_mark(&app->settings);
    gui_settings_load(&app->settings, app->settings_file);
    return &app->settings;
}
/* ---- the theme editor's preview: the menu or a run in the theme being edited ---- */
#define GUI_PREVIEW_MS 8000
static const char preview_run[] = "Extracting A Game.nsp\n"
                                  "Extracted 3 NCAs, the program's 4 modules\n"
                                  "Recompiling A Game (4 modules)\n"
                                  "Functions: 48211  instructions: 2915002\n"
                                  "LLVM: 5 code units and the module descriptor\n"
                                  "  compiled block 1/6: code_000, 9874 functions (8.1 s)\n"
                                  "  compiled block 2/6: code_001, 10022 functions (8.4 s)\n"
                                  "  compiled block 3/6: code_002, 9567 functions (7.9 s)\n";
/* the menu or a run drawn in the theme being edited, at 1x, in app->preview; false when it cannot be made */
static bool preview_texture(App* app) {
    static App saved; /* the whole app, put back after: drawing the preview borrows it */
    int run = app->editor.preview != GUI_SCREEN_MENU;
    Uint64 now = now_ms(app);
    if (!app->preview) {
        app->preview = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 640, 400);
        if (!app->preview) return false;
    }
    if (!app->preview_since || now - app->preview_since >= GUI_PREVIEW_MS) {
        app->preview_since = SDL_max(now, 1);
        gui_steps_reset(&app->preview_steps);
        gui_steps_feed(&app->preview_steps, preview_run, sizeof preview_run - 1, app->preview_since);
        gui_console_init(&app->preview_console, 78, 17);
        gui_console_feed(&app->preview_console, preview_run, sizeof preview_run - 1);
    }
    saved = *app;
    app->previewing = true;
    app->theme = app->editor.theme;
    app->scale = 1;
    app->cols = 80;
    app->rows = 25;
    app->menu.view = GUI_LOAD_NONE;
    app->menu.running = run;
    app->editing = false; /* the menu's own tabs */
    app->finished = app->aborting = app->abnormal_exit = app->confirm.shown = false;
    /* the box of users is the editor's: its pictures, made or replaced on this copy, would go with it */
    app->users_shown = false;
    app->addons_shown = false;
    app->native = 0;
    app->entering_ms = app->leaving_ms = 0;
    app->fade_ms = run ? app->preview_since : 0; /* a run fades in from black, as after the menu */
    app->message[0] = 0;
    app->has_output = run;
    if (run) {
        app->steps = app->preview_steps;
        app->console = app->preview_console;
        app->file = (char*)"dumps/A Game.nsp";
        app->menu.selected = GUI_SWITCH;
    }
    SDL_SetRenderTarget(app->renderer, app->preview);
    SDL_SetRenderViewport(app->renderer, NULL);
    draw_screen(app);
    SDL_SetRenderTarget(app->renderer, NULL);
    *app = saved;
    set_area(app);
    return true;
}
static void screen_tab(App* app, int step);
static int tab_hit(App* app, float x, float y);
static bool user_hit(App* app, float x, float y);
/* the editor on the theme the menu shows: a folder of themes/ keeps its file's extras and its name, a built-in is
   saved as a new theme */
static void open_editor(App* app) {
    GuiThemeFile file;
    size_t size = 0;
    char* text = gui_theme_text((const char* const*)app->themes_dirs, 2, app->theme.id, &size);
    int from_file = text && gui_theme_file_read(&file, text, size);
    free(text);
    gui_editor_start(&app->editor, &app->theme, from_file ? &file : NULL, from_file ? app->theme.id : "mytheme");
    if (from_file) gui_theme_file_free(&file);
    app->editing = true;
    app->form = GUI_FORM_THEME;
    app->preview_since = 0;
}
/* the bytes of path, a folder or a file; for a game laid out beside the others its files beside it too */
static uint64_t space_of(const char* path, bool games, const int* stop) {
    static const char* const beside[] = {"_code.so", "_code.dll", ".exe", ".abi", ".hints", ".ini", ".log", "_data"};
    uint64_t bytes = snx_is_dir(path) ? gui_tree_size(path, stop) : snx_file_size(path);
    size_t i;
    for (i = 0; games && path[0] && !snx_is_dir(path) && i < sizeof beside / sizeof beside[0]; ++i) {
        char* file = snx_format("%s%s", path, beside[i]);
        bytes += snx_is_dir(file) ? gui_tree_size(file, stop) : snx_file_size(file);
        free(file);
    }
    return bytes;
}
static int SDLCALL count_space(void* data) {
    GuiSpace* space = (GuiSpace*)data;
    for (int i = 0; i < space->count; ++i) {
        space->bytes[i] = space->dirs[i][0] ? space_of(space->dirs[i], space->games, &space->stop) : 0;
        space->used += space->bytes[i];
        __atomic_store_n(&space->counted, i + 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&space->done, 1, __ATOMIC_RELEASE);
    return 0;
}
static void space_end(GuiSpace** space, SDL_Thread** counter) {
    if (*counter) {
        __atomic_store_n(&(*space)->stop, 1, __ATOMIC_RELAXED);
        SDL_WaitThread(*counter, NULL);
        *counter = NULL;
    }
    free(*space);
    *space = NULL;
}
/* count folders' space (NULL or "" none) in place of the count before, on a thread; here if none can start */
static void space_count(GuiSpace** space, SDL_Thread** counter, char* const* dirs, int count, bool games) {
    size_t size = sizeof **space + (size_t)count * (sizeof(uint64_t) + sizeof(char*));
    char* at;
    int i;
    space_end(space, counter);
    for (i = 0; i < count; ++i) size += (dirs[i] ? strlen(dirs[i]) : 0) + 1;
    if (!(*space = (GuiSpace*)calloc(1, size))) return;
    (*space)->count = count;
    (*space)->games = games;
    (*space)->bytes = (uint64_t*)(*space + 1);
    (*space)->dirs = (char**)((*space)->bytes + count);
    for (i = 0, at = (char*)((*space)->dirs + count); i < count; at += strlen(at) + 1, ++i)
        (*space)->dirs[i] = strcpy(at, dirs[i] ? dirs[i] : "");
    if (!(*counter = SDL_CreateThread(count_space, "space", *space))) count_space(*space);
}
static void space_stop(App* app) { space_end(&app->space, &app->counter); }
/* the space left on the disk of path, or of the folder it will be made in */
static uint64_t free_space(const char* path) {
    char* at = snx_strdup(path);
    uint64_t left;
    while (!snx_is_dir(at)) {
        char* up = snx_dirname(at);
        int top = !strcmp(up, at);
        free(at);
        at = up;
        if (top) break;
    }
    left = snx_free_space(at);
    free(at);
    return left;
}
static void space_start(App* app) {
    /* settings, logs and caches, the menu's files among them; an older version's work/ is the data folder's */
    char* dirs[GUI_SPACE_DIRS] = {snx_strdup(app->games_dir),
                                  snx_path_join(app->data_dir, "work"),
                                  app->themes_dirs[0] ? snx_strdup(app->themes_dirs[0]) : NULL,
                                  menu_folder(app, SNX_USER_CONFIG, false),
                                  menu_folder(app, SNX_USER_STATE, false),
                                  menu_folder(app, SNX_USER_CACHE, false),
                                  app->place[GUI_PLACE_WORK] ? snx_strdup(app->place[GUI_PLACE_WORK]) : NULL};
    int i;
    app->free_bytes = free_space(app->data_dir);
    space_count(&app->space, &app->counter, dirs, GUI_SPACE_DIRS, false);
    for (i = 0; i < GUI_SPACE_DIRS; ++i) free(dirs[i]);
}
/* ---- the Settings: its groups, App then a group per section of the settings; a group's rows; the App's ---- */
static const GuiThemeKey app_keys[] = {
    {"", SNX_TR_NOOP("Theme"), GUI_KEY_ACTION, offsetof(GuiAppForm, theme), sizeof(((GuiAppForm*)0)->theme), NULL, 0,
     SNX_TR_NOOP("Built-in themes, then each themes/<name>/theme.ini")},
    {"", SNX_TR_NOOP("UI scale"), GUI_KEY_ACTION, offsetof(GuiAppForm, text), sizeof(((GuiAppForm*)0)->text), NULL, 0,
     SNX_TR_NOOP("Picked in a box, kept once said so. Auto fits the screen, its parts are smaller")},
    {"", SNX_TR_NOOP("Language"), GUI_KEY_ACTION, offsetof(GuiAppForm, language), sizeof(((GuiAppForm*)0)->language),
     NULL, 0, SNX_TR_NOOP("The menu's words. Auto: the system's language, English when the menu has none of its")},
    {"", SNX_TR_NOOP("Animations"), GUI_KEY_ACTION, offsetof(GuiAppForm, animations),
     sizeof(((GuiAppForm*)0)->animations), NULL, 0,
     SNX_TR_NOOP("A screen fading in, a box's shade coming and going, 0.15 s each. Off: at once")},
    {"", SNX_TR_NOOP("Side box"), GUI_KEY_ACTION, offsetof(GuiAppForm, side), sizeof(((GuiAppForm*)0)->side), NULL, 0,
     SNX_TR_NOOP("The box beside the list: Auto where both fit, On under the list where they do not, Off never. F2")},
    {"", SNX_TR_NOOP("Theme editor"), GUI_KEY_ACTION, 0, 0, NULL, 0,
     SNX_TR_NOOP("This theme's keys with a preview, saved as themes/<name>/theme.ini")},
    {"", SNX_TR_NOOP("Folders"), GUI_KEY_ACTION, offsetof(GuiAppForm, places), sizeof(((GuiAppForm*)0)->places), NULL,
     0, SNX_TR_NOOP("The data and the extractions, and the libraries Games lists: recompiled games and dumps")},
    {"", SNX_TR_NOOP("Clean old settings"), GUI_KEY_ACTION, 0, 0, NULL, 0,
     SNX_TR_NOOP("The settings the runtime and the launcher no longer read, taken out once asked")},
    {"", SNX_TR_NOOP("Move old files"), GUI_KEY_ACTION, offsetof(GuiAppForm, moves), sizeof(((GuiAppForm*)0)->moves),
     NULL, 0,
     SNX_TR_NOOP("What an older version left where this one no longer looks, moved where it keeps it once asked")},
    {"", SNX_TR_NOOP("Reset all"), GUI_KEY_ACTION, 0, 0, NULL, 0,
     SNX_TR_NOOP("Every setting of the groups after App back to its default, once asked")}};
enum {
    GUI_APP_THEME,
    GUI_APP_TEXT,
    GUI_APP_LANGUAGE,
    GUI_APP_ANIMATIONS,
    GUI_APP_SIDE,
    GUI_APP_EDITOR,
    GUI_APP_PLACES,
    GUI_APP_CLEAN,
    GUI_APP_MOVE,
    GUI_APP_RESET,
    GUI_APP_ROWS
};
/* path as App's note says it: from the data folder when it is in it (recompiled/), else whole */
static void data_relative(const App* app, const char* path, char* out, size_t size) {
    size_t n = app->data_dir ? strlen(app->data_dir) : 0;
    while (n > 1 && (app->data_dir[n - 1] == '/' || app->data_dir[n - 1] == '\\')) --n;
    if (path && n && !strncmp(path, app->data_dir, n) && (path[n] == '/' || path[n] == '\\') && path[n + 1])
        snprintf(out, size, "%s%c", path + n + 1, path[n]);
    else
        snprintf(out, size, "%s", path ? path : "");
}
/* a text_size as Settings show it: Auto, a part of Auto with the scale it gives, or a scale */
static void size_name(const App* app, int size, char* out, size_t n) {
    if (size > 0)
        snprintf(out, n, "%dx", size);
    else if (size < 0)
        snprintf(out, n, snx_tr("%s of Auto: %dx"), text_quarters[-size - 1],
                 SDL_max(1, (app->auto_scale * -size + 2) / 4));
    else
        snprintf(out, n, "%s", snx_tr("Auto"));
}
static void app_values(App* app) {
    GuiAppForm* f = &app->app_form;
    char games[4200], work[4200];
    int moved;
    snprintf(f->theme, sizeof f->theme, "%s", app->theme.name);
    size_name(app, app->menu.text_size, f->text, sizeof f->text);
    if (app->language)
        snprintf(f->language, sizeof f->language, "%s", snx_gui_languages[app->language - 1].name);
    else /* and what it is now */
        snprintf(f->language, sizeof f->language, snx_tr("Auto: %s"), snx_gui_languages[snx_tr_language()].name);
    snprintf(f->animations, sizeof f->animations, "%s", app->animations_off ? snx_tr("Off") : snx_tr("On"));
    snprintf(f->side, sizeof f->side, "%s", snx_tr(side_names[app->side_box]));
    moved = (app->place[GUI_PLACE_DATA] != NULL) + (app->place[GUI_PLACE_GAMES] != NULL) +
            (app->place[GUI_PLACE_WORK] != NULL);
    if (moved && app->folder_count)
        snprintf(f->places, sizeof f->places, snx_tr("%d picked, %d Extra(s)"), moved, app->folder_count);
    else if (moved)
        snprintf(f->places, sizeof f->places, snx_tr("%d picked"), moved);
    else if (app->folder_count)
        snprintf(f->places, sizeof f->places, snx_tr("Defaults, %d Extra(s)"), app->folder_count);
    else
        snprintf(f->places, sizeof f->places, "%s", snx_tr("Defaults"));
    if (app->lefts) /* as the menu last looked: when it opened, or App did */
        snprintf(f->moves, sizeof f->moves, snx_tr("%d left"), app->lefts);
    else
        snprintf(f->moves, sizeof f->moves, "%s", snx_tr("Nothing left"));
    /* the three folders under the rows, as Folders' value has no room for them; one in the data folder from it */
    data_relative(app, app->games_dir, games, sizeof games);
    data_relative(app, app->work_dir, work, sizeof work);
    snprintf(app->places_note, sizeof app->places_note, "%s: %s  %s: %s  %s: %s", snx_tr(place_labels[0]),
             app->data_dir ? app->data_dir : "", snx_tr(place_labels[1]), games, snx_tr(place_labels[2]), work);
}
/* ---- the player's game folders: listed after recompiled/ and dumps/, kept a line each in folders_file ---- */
/* what a folder holds, as Games lists it: 1 << GUI_LIBRARY_RECOMPILED for games ready to run, 1 << GUI_LIBRARY_DUMPS
   for dumps or CIAs (down its subfolders with deep); nothing, or no such folder, the dumps' */
static int folder_holds(const char* path, bool deep) {
    GuiGames games;
    char error[8];
    int holds = 0;
    memset(&games, 0, sizeof games);
    if (gui_games_add_deep(&games, path, GUI_GAMES_RUN, deep ? GUI_FOLDER_DEPTH : 0, error, sizeof error) &&
        games.count)
        holds |= 1 << GUI_LIBRARY_RECOMPILED;
    gui_games_free(&games);
    memset(&games, 0, sizeof games);
    if (gui_games_add_deep(&games, path, GUI_GAMES_DUMP | GUI_GAMES_CIA, deep ? GUI_FOLDER_DEPTH : 0, error,
                           sizeof error) &&
        games.count)
        holds |= 1 << GUI_LIBRARY_DUMPS;
    gui_games_free(&games);
    return holds ? holds : 1 << GUI_LIBRARY_DUMPS;
}
static void folder_keep(App* app, const char* path, int library, bool deep) {
    if (app->folder_count == GUI_FOLDERS_MAX) return;
    app->folders_deep[app->folder_count] = deep;
    app->folders_library[app->folder_count] = library;
    app->folders[app->folder_count++] = snx_strdup(path);
}
static int folders_save(App* app);
/* folders.txt, a line a folder: its library's word, a space and its path, GUI_FOLDER_DEEP after a folder kept with
   its subfolders. A line an older version wrote has the path alone: the folder goes in the library of what it holds, in
   both for both, the file written again so and said once, each folder named with where it went */
static void folders_load(App* app) {
    size_t size = 0, said = 0;
    char *text = app->folders_file ? (char*)snx_read_file(app->folders_file, &size) : NULL, *line, *next;
    char moved[sizeof app->message];
    moved[0] = 0;
    for (line = text; line && *line && app->folder_count < GUI_FOLDERS_MAX; line = next) {
        size_t n, deep_n = sizeof GUI_FOLDER_DEEP - 1;
        int library = -1, l;
        bool deep;
        next = line + strcspn(line, "\n");
        if (*next) *next++ = 0;
        if (!*line) continue;
        for (l = 0; l < GUI_LIBRARIES; ++l) {
            n = strlen(library_words[l]);
            if (!strncmp(line, library_words[l], n) && line[n] == ' ') {
                library = l;
                line += n + 1;
                break;
            }
        }
        n = strlen(line);
        deep = n > deep_n && !strcmp(line + n - deep_n, GUI_FOLDER_DEEP);
        if (deep) line[n - deep_n] = 0;
        if (library >= 0) {
            folder_keep(app, line, library, deep);
            continue;
        }
        { /* an older version's line */
            int holds = folder_holds(line, deep);
            for (l = 0; l < GUI_LIBRARIES; ++l)
                if (holds & (1 << l)) folder_keep(app, line, l, deep);
            if (said < sizeof moved)
                said += (size_t)snprintf(moved + said, sizeof moved - said, "%s%s: %s", said ? ", " : "", line,
                                         holds == (1 << GUI_LIBRARY_RECOMPILED) ? snx_tr("recompiled")
                                         : holds == (1 << GUI_LIBRARY_DUMPS)    ? snx_tr("dumps")
                                                                                : snx_tr("recompiled and dumps"));
        }
    }
    free(text);
    if (moved[0]) {
        size_t m = strlen(app->message);
        char note[sizeof app->message];
        if (folders_save(app))
            snprintf(note, sizeof note,
                     snx_tr("%s: each folder of an older version's lines read as %s, not written: %s"),
                     app->folders_file, moved, app->editor.note);
        else
            snprintf(note, sizeof note, snx_tr("%s: each folder of an older version's lines is in a library now: %s"),
                     app->folders_file, moved);
        snprintf(app->message + m, sizeof app->message - m, "%s%s", m ? " " : "", note);
    }
}
/* 0, else the note says why not */
static int folders_save(App* app) {
    size_t size = 1, n;
    char *text, *dir;
    int i, failed;
    if (!app->folders_file) return 0;
    for (i = 0; i < app->folder_count; ++i) size += strlen(app->folders[i]) + sizeof GUI_FOLDER_DEEP + 12;
    text = (char*)malloc(size);
    for (i = 0, n = 0; text && i < app->folder_count; ++i)
        n += (size_t)snprintf(text + n, size - n, "%s %s%s\n", library_words[app->folders_library[i]], app->folders[i],
                              app->folders_deep[i] ? GUI_FOLDER_DEEP : "");
    dir = snx_dirname(app->folders_file);
    failed = !text || !dir || snx_mkdirs(dir) || snx_write_file(app->folders_file, text, n);
    if (failed) {
        snprintf(app->editor.note, sizeof app->editor.note, snx_tr("Not saved: %s could not be written"),
                 app->folders_file);
        say_failure(app, app->editor.note);
    }
    free(dir);
    free(text);
    return failed;
}
static void open_folders(App* app, int row);
static int folder_row_of(const App* app, int kind, int at);
/* the dumps library's Main row: dumps/ of the data folder, which goes with it */
static const char dumps_row[] =
    SNX_TR_NOOP("Dumps put here are listed, its subfolders' too: .nsp, .xci and .nro files. It stays on the list");
/* whether path is dumps/ of the data folder, which the Library always lists */
static bool is_dumps_dir(const App* app, const char* path) {
    char* dumps = app->data_dir ? snx_path_join(app->data_dir, "dumps") : NULL;
    bool same = dumps && path && snx_same_file(dumps, path);
    free(dumps);
    return same;
}
/* a folder the player picked, dropped or pasted: a folder that is there, once. What happened goes on the folders'
   screen, or in the menu's message when the picker answered after the player left it */
static void folder_add(App* app, const char* path, int library, bool deep) {
    char *absolute = path ? snx_absolute(path) : NULL, note[sizeof app->editor.note];
    int i, added = 0, here = app->editing && app->form == GUI_FORM_FOLDERS;
    for (i = 0;
         absolute && i < app->folder_count && (strcmp(app->folders[i], absolute) || app->folders_library[i] != library);
         ++i) {}
    if (!absolute || !snx_is_dir(absolute))
        snprintf(note, sizeof note, snx_tr("Not a folder: %s"), path ? path : snx_tr("(none)"));
    else if (i < app->folder_count ||
             (library == GUI_LIBRARY_RECOMPILED && app->games_dir && !strcmp(absolute, app->games_dir)) ||
             (library == GUI_LIBRARY_DUMPS && is_dumps_dir(app, absolute)))
        snprintf(note, sizeof note,
                 library == GUI_LIBRARY_DUMPS ? snx_tr("Already in the dumps library: %s")
                                              : snx_tr("Already in the recompiled library: %s"),
                 absolute);
    else if (app->folder_count == GUI_FOLDERS_MAX)
        snprintf(note, sizeof note, snx_tr("At most %d folders"), GUI_FOLDERS_MAX);
    else {
        folder_keep(app, absolute, library, deep);
        added = 1;
        snprintf(note, sizeof note, "%s",
                 folders_save(app) ? app->editor.note
                 : library == GUI_LIBRARY_RECOMPILED
                     ? (deep ? snx_tr("Added to the recompiled library, with its subfolders")
                             : snx_tr("Added to the recompiled library"))
                     : (deep ? snx_tr("Added to the dumps library, with its subfolders")
                             : snx_tr("Added to the dumps library")));
    }
    free(absolute);
    if (here && added) open_folders(app, folder_row_of(app, GUI_ROW_FOLDER, app->folder_count - 1)); /* its row */
    if (here)
        snprintf(app->editor.note, sizeof app->editor.note, "%s", note);
    else
        snprintf(app->message, sizeof app->message, snx_tr("Folders: %s"), note);
}
static void folder_remove(App* app, int folder) {
    char note[sizeof app->editor.note], *path = app->folders[folder];
    int row = folder_row_of(app, GUI_ROW_FOLDER, folder); /* the row after it takes its place */
    memmove(app->folders + folder, app->folders + folder + 1, (size_t)(app->folder_count - folder - 1) * sizeof(char*));
    memmove(app->folders_deep + folder, app->folders_deep + folder + 1,
            (size_t)(app->folder_count - folder - 1) * sizeof(bool));
    memmove(app->folders_library + folder, app->folders_library + folder + 1,
            (size_t)(app->folder_count - folder - 1) * sizeof(int));
    --app->folder_count;
    if (folders_save(app))
        snprintf(note, sizeof note, "%s", app->editor.note);
    else
        snprintf(note, sizeof note, snx_tr("%s taken off the list. The folder keeps its games"), path);
    free(path);
    open_folders(app, row);
    snprintf(app->editor.note, sizeof app->editor.note, "%s", note);
}
/* a game folder taken off the Library asked about first (GUI_ASK_FOLDER): only the list changes */
static void folder_off_ask(App* app, int folder) {
    app->folder_off = folder;
    snprintf(app->ask_line, sizeof app->ask_line,
             app->folders_library[folder] == GUI_LIBRARY_RECOMPILED
                 ? snx_tr("Take %s off the recompiled library? The folder keeps its games.")
                 : snx_tr("Take %s off the dumps library? The folder keeps its games."),
             app->folders[folder]);
    gui_confirm_open(&app->confirm, GUI_ASK_FOLDER, false);
}
/* path into each library holds names, with its subfolders (deep) or not */
static void folder_add_to(App* app, const char* path, int holds, bool deep) {
    int l;
    for (l = 0; l < GUI_LIBRARIES; ++l)
        if (holds & (1 << l)) folder_add(app, path, l, deep);
}
/* a folder picked, to the library Add a folder was for, else to the one of what it holds (both for both): one with
   folders in it asks first whether their games are listed too (Only this folder, With subfolders); another is added
   as it is */
static void folder_ask(App* app, const char* path) {
    char* absolute = path ? snx_absolute(path) : NULL;
    int holds = app->adding >= 0 ? 1 << app->adding : absolute ? folder_holds(absolute, true) : 1 << GUI_LIBRARY_DUMPS;
    if (absolute && snx_is_dir(absolute) && gui_has_folders(absolute, GUI_GAMES_RUN | GUI_GAMES_CIA)) {
        free(app->folder_asked);
        app->folder_asked = absolute;
        app->asked_holds = holds;
        gui_confirm_open(&app->confirm, GUI_ASK_DEEP, false);
        return;
    }
    free(absolute);
    folder_add_to(app, path, holds, false);
}
/* its answer: a choice taken adds the folder as it says, with its subfolders (deep) or not; Back adds nothing */
static void folder_answer(App* app, bool taken, bool deep) {
    char* path = app->folder_asked;
    app->folder_asked = NULL;
    if (taken)
        folder_add_to(app, path, app->asked_holds, deep);
    else if (app->editing && app->form == GUI_FORM_FOLDERS)
        snprintf(app->editor.note, sizeof app->editor.note, "%s", snx_tr("Not added"));
    free(path);
}
/* the system's folder picker answers on its own thread, as the file picker */
static void SDLCALL folder_picked(void* userdata, const char* const* files, int filter) {
    SDL_Event e;
    (void)filter;
    SDL_zero(e);
    e.type = *(const Uint32*)userdata;
    e.user.code = files == NULL; /* an error, which SDL_GetError names */
    e.user.data1 = files && files[0] ? SDL_strdup(files[0]) : NULL;
    e.user.data2 = files ? NULL : SDL_strdup(SDL_GetError());
    SDL_PushEvent(&e);
}
/* a group's row of the Settings, its help the keys it holds */
static void group_add(App* app, const char* name, int first, const char* about) {
    int g = app->groups++;
    GuiThemeKey* k = &app->group_keys[g];
    memset(k, 0, sizeof *k);
    k->section = "";
    snprintf(app->group_name[g], sizeof app->group_name[g], "%s", name);
    k->key = app->group_name[g];
    k->type = GUI_KEY_ACTION;
    k->about = app->group_about[g];
    snprintf(app->group_about[g], sizeof app->group_about[g], "%s", about);
    app->group_first[g] = first;
    app->group_rows[g] = 0;
}
/* the name a section's group has in the launcher: Overlay for the runtime's On-screen controls, whose page in the game
   and settings.ini's [header] keep that name */
static const char* group_title(const char* section) {
    return strcmp(section, "On-screen controls") ? section : SNX_TR_NOOP("Overlay");
}
static void settings_groups(App* app) {
    GuiSettings* s = settings_of(app);
    int r;
    app->groups = 0;
    group_add(app, SNX_TR_NOOP("App"), 0, snx_tr("The theme, the menu's letters, the theme editor and the folders"));
    for (r = 0; r < s->count; ++r) {
        int g = app->groups - 1;
        char* about = app->group_about[g];
        size_t n = strlen(about);
        if (strchr(s->keys[r].section, '/')) continue; /* a page's: its row in the group opens it */
        /* past the most groups the rest go in the last one: every row is still reached */
        if (g == 0 ||
            (strcmp(s->keys[r].section, s->keys[app->group_first[g]].section) && app->groups < GUI_GROUPS_MAX)) {
            group_add(app, group_title(s->keys[r].section), r, "");
            g = app->groups - 1;
            about = app->group_about[g];
            n = 0;
        }
        snprintf(about + n, sizeof app->group_about[g] - n, "%s%s", n ? ", " : "", s->keys[r].key);
        ++app->group_rows[g];
    }
}
/* the groups, row on the focus */
static void settings_top(App* app, int row) {
    settings_groups(app);
    gui_editor_form(&app->editor, app->group_keys, app->groups, &app->app_form, NULL);
    app->editor.flat = true;
    app->editor.row = SDL_min(row, gui_editor_rows(&app->editor) - 1);
    app->form = GUI_FORM_SETTINGS;
    app->editing = true;
}
static void open_app(App* app, int row) {
    app_values(app);
    gui_editor_form(&app->editor, app_keys, GUI_APP_ROWS, &app->app_form, NULL);
    app->editor.flat = true;
    app->editor.row = row;
    app->form = GUI_FORM_APP;
    app->group = 0;
    app->editing = true;
}
/* ---- Settings > App > Folders: the data folder, recompiled/ and the extractions, each picked or its default ---- */
/* snx_folder_unfit's reasons, shown translated */
static const char* const unfit_reasons[] = {SNX_TR_NOOP("is not there"), SNX_TR_NOOP("is not a folder"),
                                            SNX_TR_NOOP("cannot be written"),
                                            SNX_TR_NOOP("is on a disk that runs no programs (noexec)")};
static const char* unfit_text(const char* why) {
    size_t i;
    for (i = 0; i < SDL_arraysize(unfit_reasons); ++i)
        if (!strcmp(why, unfit_reasons[i])) return snx_tr(unfit_reasons[i]);
    return why;
}
static const char* const place_holds[GUI_PLACES] = {SNX_TR_NOOP("recompiled/, dumps/, themes/ and prod.keys"),
                                                    SNX_TR_NOOP("The recompiled games"),
                                                    SNX_TR_NOOP("Each dump's extraction")};
#ifdef SDL_PLATFORM_ANDROID
#define PLACE_HOW SNX_TR_NOOP("Tap: pick another. The last row: all back to the defaults")
#else
#define PLACE_HOW SNX_TR_NOOP("Enter: pick another. Del: the default")
#endif
/* the folders as Folders has them: each one picked, else its default from the data folder, the menu's own one first */
/* the folder place takes while none is picked, with the data folder in use (its own default: home_dir): owned */
static char* place_default(const App* app, int place) {
    if (place == GUI_PLACE_DATA) return app->home_dir ? snx_strdup(app->home_dir) : NULL;
    if (!app->data_dir) return NULL;
    if (place == GUI_PLACE_GAMES) return snx_recompiled_dir(app->data_dir);
    char* work = snx_path_join(app->data_dir, "work"); /* where the driver puts them by itself (snx_work_dir) */
    char* cache = work ? snx_user_path(work, SNX_USER_CACHE) : NULL;
    free(work);
    return cache;
}
static void places_apply(App* app) {
    const char* data = app->place[GUI_PLACE_DATA] ? app->place[GUI_PLACE_DATA] : app->home_dir;
    char* copy;
    if (!data) return;
    copy = snx_strdup(data);
    free(app->data_dir);
    app->data_dir = copy;
    free(app->games_dir);
    app->games_dir =
        app->place[GUI_PLACE_GAMES] ? snx_strdup(app->place[GUI_PLACE_GAMES]) : place_default(app, GUI_PLACE_GAMES);
    free(app->themes_dirs[0]);
    app->themes_dirs[0] = snx_path_join(app->data_dir, "themes");
    { /* where dumps go to be listed: made here, so the player finds it; one that cannot be made is said here, once */
        char* dumps = snx_path_join(app->data_dir, "dumps");
        if (dumps && !app->look_only && !snx_is_dir(dumps) && snx_mkdirs(dumps)) {
            snprintf(app->message, sizeof app->message, snx_tr("Could not make %s"), dumps);
            say_failure(app, app->message);
        }
        free(dumps);
    }
    free(app->work_dir);
    app->work_dir =
        app->place[GUI_PLACE_WORK] ? snx_strdup(app->place[GUI_PLACE_WORK]) : place_default(app, GUI_PLACE_WORK);
    free(app->keys_path);
    app->keys_path = device_keys_path(app);
}
static const char* place_path(const App* app, int place) {
    return place == GUI_PLACE_DATA ? app->data_dir : place == GUI_PLACE_GAMES ? app->games_dir : app->work_dir;
}
/* a folder outside the app's own storage on Android, where every file access goes through its checks (FUSE) */
static bool outside_app(const App* app, const char* path) {
    return app->own_dir && !snx_path_within(path, app->own_dir);
}
static const char* const group_titles[GUI_FOLDER_GROUPS] = {
    SNX_TR_NOOP("The app's folders"), SNX_TR_NOOP("Recompiled library"), SNX_TR_NOOP("Dumps library")};
/* row r's kind (GuiFolderRow), its place, folder or library in *at and its group in *group; -1 past the rows (the
   last, All back to the defaults, is the form's own) */
static int folder_row(const App* app, int r, int* at, int* group) {
    static const int app_places[] = {GUI_PLACE_DATA, GUI_PLACE_WORK};
    int i, n = 0, l;
    *group = GUI_GROUP_APP;
    for (i = 0; i < 2; ++i)
        if (n++ == r) {
            *at = app_places[i];
            return GUI_ROW_PLACE;
        }
    for (l = 0; l < GUI_LIBRARIES; ++l) {
        *group = l == GUI_LIBRARY_RECOMPILED ? GUI_GROUP_RECOMPILED : GUI_GROUP_DUMPS;
        if (n++ == r) {
            *at = l == GUI_LIBRARY_RECOMPILED ? GUI_PLACE_GAMES : -1;
            return l == GUI_LIBRARY_RECOMPILED ? GUI_ROW_PLACE : GUI_ROW_DUMPS;
        }
        for (i = 0; i < app->folder_count; ++i)
            if (app->folders_library[i] == l && n++ == r) {
                *at = i;
                return GUI_ROW_FOLDER;
            }
        if (n++ == r) {
            *at = l;
            return GUI_ROW_ADD;
        }
    }
    *at = -1;
    return -1;
}
static int folder_rows(const App* app) {
    int r = 0, at, group;
    while (folder_row(app, r, &at, &group) >= 0) ++r;
    return r;
}
/* the row of kind at at, else -1 */
static int folder_row_of(const App* app, int kind, int at) {
    int r, a, group, k;
    for (r = 0; (k = folder_row(app, r, &a, &group)) >= 0; ++r)
        if (k == kind && a == at) return r;
    return -1;
}
/* the folder row r is, whole: a place's in use, dumps/, an Extra; NULL for Add a folder and the last */
static const char* folder_row_path(const App* app, int r) {
    int at, group, kind = folder_row(app, r, &at, &group);
    return kind == GUI_ROW_PLACE    ? place_path(app, at)
           : kind == GUI_ROW_DUMPS  ? app->library_dumps
           : kind == GUI_ROW_FOLDER ? app->folders[at]
                                    : NULL;
}
/* Folders' screen lists each group's title before its rows (every group has some) and the last row after the
   groups: the list's entries, row r's, and entry e's row (-1 a title, its group in *group; the last's group
   GUI_FOLDER_GROUPS) */
static int folder_entries(const App* app) { return folder_rows(app) + GUI_FOLDER_GROUPS + 1; }
static int folder_entry(const App* app, int r) {
    int at, group, rows = folder_rows(app);
    if (r >= rows) return rows + GUI_FOLDER_GROUPS;
    folder_row(app, r, &at, &group);
    return r + group + 1;
}
static int folder_entry_row(const App* app, int e, int* group) {
    int r, at, rows = folder_rows(app);
    for (r = 0; r < rows; ++r) {
        folder_row(app, r, &at, group);
        if (folder_entry(app, r) == e) return r;
        if (folder_entry(app, r) - 1 == e && (!r || folder_entry(app, r - 1) + 1 < folder_entry(app, r))) return -1;
    }
    *group = GUI_FOLDER_GROUPS;
    return rows;
}
/* the focused row of Folders a place's: Enter picks another, Del its default */
static bool folders_row_place(const App* app) {
    int at, group;
    return folder_row(app, app->editor.row, &at, &group) == GUI_ROW_PLACE;
}
/* the library row r's group lists, -1 for the app's folders and the last */
static int folder_row_library(const App* app, int r) {
    int at, group;
    folder_row(app, r, &at, &group);
    return group == GUI_GROUP_RECOMPILED ? GUI_LIBRARY_RECOMPILED : group == GUI_GROUP_DUMPS ? GUI_LIBRARY_DUMPS : -1;
}
static const char extra_row[] = SNX_TR_NOOP("Del or Enter asks to take it off the library; the folder keeps its games");
static const char add_recompiled_row[] =
    SNX_TR_NOOP("Pick, drop or paste a folder of recompiled games: Games lists them too");
static const char add_dumps_row[] = SNX_TR_NOOP("Pick, drop or paste a folder of dumps: Games lists them too");
/* Settings > App > Folders, every folder on one screen, a box a group: the app's (Data, Extractions), then each
   library's Main, its Extras and Add a folder; the last row puts the places back to their defaults. A row is its
   name, its folder cut in its middle and what it is; above, what it holds, under the boxes its folder whole */
static void open_folders(App* app, int row) {
    int r, rows, at, group;
    free(app->library_dumps);
    app->library_dumps = app->data_dir ? snx_path_join(app->data_dir, "dumps") : NULL;
    rows = folder_rows(app);
    for (r = 0; r < rows; ++r) {
        GuiThemeKey* k = &app->folder_keys[r];
        int kind = folder_row(app, r, &at, &group);
        const char* path = folder_row_path(app, r);
        char* value = app->app_form.folder[r];
        size_t size = sizeof app->app_form.folder[r];
        memset(k, 0, sizeof *k);
        k->section = "";
        k->type = GUI_KEY_ACTION;
        k->at = offsetof(GuiAppForm, folder) + (size_t)r * sizeof app->app_form.folder[0];
        k->size = sizeof app->app_form.folder[0];
        value[0] = 0;
        if (kind == GUI_ROW_PLACE) {
            const char* why = path && app->place[at] ? snx_folder_unfit(path, at == GUI_PLACE_GAMES) : NULL;
            char left[32];
            k->key = at == GUI_PLACE_GAMES ? SNX_TR_NOOP("Main") : place_labels[at]; /* shown translated */
            k->about = app->place_about[at];
            gui_size_text(left, sizeof left, path ? free_space(path) : 0);
            snprintf(value, size, "%s",
                     why                                        ? unfit_text(why)
                     : app->place[at] && outside_app(app, path) ? snx_tr("outside the app")
                     : app->place[at]                           ? snx_tr("picked")
                                                                : snx_tr("default"));
            snprintf(app->place_about[at], sizeof app->place_about[at], snx_tr("%s, %s free. %s"),
                     snx_tr(place_holds[at]), left, snx_tr(PLACE_HOW));
            snprintf(app->place_line[at], sizeof app->place_line[at], "%s%s%s", path ? path : snx_tr("(none)"),
                     why ? " " : "", why ? unfit_text(why) : "");
        } else if (kind == GUI_ROW_ADD) {
            k->key = SNX_TR_NOOP("Add a folder");
            k->about = at == GUI_LIBRARY_RECOMPILED ? add_recompiled_row : add_dumps_row;
        } else {
            k->key = kind == GUI_ROW_DUMPS ? SNX_TR_NOOP("Main") : SNX_TR_NOOP("Extra");
            k->about = kind == GUI_ROW_DUMPS ? dumps_row : extra_row;
            snprintf(value, size, "%s",
                     !path || !snx_is_dir(path)                       ? snx_tr("not found")
                     : kind == GUI_ROW_DUMPS || app->folders_deep[at] ? snx_tr("+ subfolders")
                                                                      : "");
        }
    }
    gui_editor_form(&app->editor, app->folder_keys, rows, &app->app_form, SNX_TR_NOOP("All back to the defaults"));
    app->editor.flat = true;
    app->editor.row = SDL_min(row, gui_editor_rows(&app->editor) - 1);
    app->form = GUI_FORM_FOLDERS;
    app->editing = true;
}
/* a place's note: on the form while it shows, else in the menu's message */
static void place_note(App* app, const char* note) {
    if (app->editing && app->form == GUI_FORM_FOLDERS)
        snprintf(app->editor.note, sizeof app->editor.note, "%s", note);
    else
        snprintf(app->message, sizeof app->message, snx_tr("Folders: %s"), note);
}
/* a place picked, absolute, or its default (NULL), kept in app.ini */
static void place_use(App* app, int place, char* absolute) {
    char note[sizeof app->editor.note];
    free(app->place[place]);
    app->place[place] = absolute;
    places_apply(app);
    space_start(app);
    if (app->editing && app->form == GUI_FORM_FOLDERS) open_folders(app, folder_row_of(app, GUI_ROW_PLACE, place));
    if (app->app_file && app_save(app)) {
        snprintf(note, sizeof note, snx_tr("Used, not kept for the next start: %s could not be written"),
                 app->app_file);
        say_failure(app, note);
    } else
        snprintf(note, sizeof note,
                 absolute ? snx_tr("%s: %s. The folder before keeps what it has") : snx_tr("%s: %s, the default"),
                 snx_tr(place_labels[place]), place_path(app, place));
    place_note(app, note);
}
/* Del on a place's row, or on the Library's recompiled/: a folder picked goes back to its default once a box asks
   (GUI_ASK_PLACE), only the setting changing; the default itself says so */
static void place_default_ask(App* app, int place) {
    char* fallback = place_default(app, place);
    const char* none = snx_tr("(none)");
    if (!app->place[place]) {
        char note[sizeof app->editor.note];
        snprintf(note, sizeof note, snx_tr("%s: %s, the default"), snx_tr(place_labels[place]), place_path(app, place));
        place_note(app, note);
    } else {
        app->place_off = place;
        snprintf(app->ask_line, sizeof app->ask_line, snx_tr("%s back to its default, %s? %s keeps what it has."),
                 snx_tr(place_labels[place]), fallback ? fallback : none, app->place[place]);
        gui_confirm_open(&app->confirm, GUI_ASK_PLACE, false);
    }
    free(fallback);
}
/* a place picked (path), or its default (NULL). Only where the folders are changes: nothing in them is moved. A folder
   that cannot take what goes there is refused, never replaced by the default; one outside the app is asked about */
static void place_set(App* app, int place, const char* path) {
    char *absolute = path ? snx_absolute(path) : NULL, note[sizeof app->editor.note];
    const char* why = path ? (absolute ? snx_folder_unfit(absolute, place == GUI_PLACE_GAMES) : "is not there") : NULL;
    if (why) {
        snprintf(note, sizeof note, snx_tr("Not changed: %s %s"), path, unfit_text(why));
        free(absolute);
        place_note(app, note);
        return;
    }
    if (absolute && outside_app(app, absolute)) {
        free(app->outside_path);
        app->outside_path = absolute;
        app->outside_place = place;
        gui_confirm_open(&app->confirm, GUI_ASK_OUTSIDE, true);
        return;
    }
    place_use(app, place, absolute);
}
/* its answer: Use it, pressed twice, takes the folder; Back keeps the one in use */
static void outside_answer(App* app, bool taken) {
    char* path = app->outside_path;
    app->outside_path = NULL;
    if (taken)
        place_use(app, app->outside_place, path);
    else {
        free(path);
        place_note(app, snx_tr("Not changed"));
    }
}
/* the form's last row: the folders picked named in a box (GUI_ASK_PLACES) before they go back to their defaults; none
   picked, the defaults in use named */
static void places_reset_ask(App* app) {
    char list[sizeof app->ask_line];
    size_t n = 0;
    int picked = 0;
    for (int i = 0; i < GUI_PLACES; ++i) picked += app->place[i] != NULL;
    for (int i = 0; i < GUI_PLACES; ++i) {
        const char* path = picked ? app->place[i] : place_path(app, i);
        if ((!picked || path) && n < sizeof list)
            n += (size_t)snprintf(list + n, sizeof list - n, "%s%s: %s", n ? ", " : "", snx_tr(place_labels[i]),
                                  path ? path : snx_tr("(none)"));
    }
    if (!picked) {
        char note[sizeof app->editor.note];
        snprintf(note, sizeof note, snx_tr("They are the defaults: %s"), list);
        place_note(app, note);
        return;
    }
    snprintf(app->ask_line, sizeof app->ask_line,
             snx_tr("Back to the defaults: %s? The folders before keep what they have."), list);
    gui_confirm_open(&app->confirm, GUI_ASK_PLACES, false);
}
/* its answer: every place back to its default */
static void places_reset(App* app) {
    for (int i = 0; i < GUI_PLACES; ++i) {
        free(app->place[i]);
        app->place[i] = NULL;
    }
    places_apply(app);
    space_start(app);
    if (app->editing && app->form == GUI_FORM_FOLDERS) open_folders(app, folder_rows(app)); /* the last row */
    if (app->app_file && app_save(app)) {
        char note[sizeof app->editor.note];
        snprintf(note, sizeof note, snx_tr("Used, not kept for the next start: %s could not be written"),
                 app->app_file);
        place_note(app, note);
        say_failure(app, note);
    } else
        place_note(app, snx_tr("All back to the defaults. The folders before keep what they have"));
}
/* a run's folders picked in Folders, each still able to take what goes there: else out says which, and why */
static bool places_fit(const App* app, char* out, size_t size) {
    int i;
    for (i = 0; i < GUI_PLACES; ++i) {
        const char* why = app->place[i] ? snx_folder_unfit(app->place[i], i == GUI_PLACE_GAMES) : NULL;
        if (why) {
            snprintf(out, size, snx_tr("%s folder %s %s. Settings > App > Folders picks another."),
                     snx_tr(place_labels[i]), app->place[i], unfit_text(why));
            return false;
        }
    }
    return true;
}
/* a Switch homebrew's .nro, which runs without an extraction */
static bool nro_file(const char* path) {
    size_t n = strlen(path);
    return n > 4 && path[n - 4] == '.' && SDL_tolower((unsigned char)path[n - 3]) == 'n' &&
           SDL_tolower((unsigned char)path[n - 2]) == 'r' && SDL_tolower((unsigned char)path[n - 1]) == 'o';
}
/* a dump's extraction in the picked extractions folder, as the driver names it in its own: <work>[/<kind>]/<name> */
static char* dump_out(const App* app, const char* file, const char* kind) {
    char *name = snx_dump_name(file), *dir = kind ? snx_path_join(app->work_dir, kind) : snx_strdup(app->work_dir);
    char* out = snx_path_join(dir, name);
    free(name);
    free(dir);
    return out;
}
/* the games of recompiled/ made from a dump: each one's program, as Games lists it, and the dump its .ini's source=
   names, else its folder's <name>.source, the link an older version made (snx_game_source) */
typedef struct {
    char* program;
    char* dump;
} MadeGame;
typedef struct {
    MadeGame* game;
    size_t count;
} MadeGames;
static void made_read(const App* app, MadeGames* made) {
    char** names = NULL;
    size_t count = 0, i;
    *made = (MadeGames){0};
    if (!app->games_dir || snx_list_dir(app->games_dir, &names, &count) || !count) return;
    made->game = (MadeGame*)snx_alloc(count * sizeof *made->game);
    for (i = 0; i < count; ++i) {
        char *program = gui_folder_game(app->games_dir, names[i]), *folder, *dump;
        int recorded = 0;
        if (!program) continue;
        folder = snx_path_join(app->games_dir, names[i]);
        dump = snx_game_source(folder, names[i], &recorded);
        free(folder);
        if (!recorded) {
            free(program);
            free(dump);
            continue;
        }
        made->game[made->count++] = (MadeGame){program, dump};
    }
    snx_free_strings(names, count);
}
/* the program of the game made from dump, else NULL */
static const char* made_program(const MadeGames* made, const char* dump) {
    size_t i;
    for (i = 0; i < made->count; ++i)
        if (snx_same_file(made->game[i].dump, dump)) return made->game[i].program;
    return NULL;
}
static void made_free(MadeGames* made) {
    size_t i;
    for (i = 0; i < made->count; ++i) {
        free(made->game[i].program);
        free(made->game[i].dump);
    }
    free(made->game);
    *made = (MadeGames){0};
}
/* a game of recompiled/ made from file already */
static bool made_from(const App* app, const char* file) {
    MadeGames made;
    bool found;
    made_read(app, &made);
    found = made_program(&made, file) != NULL;
    made_free(&made);
    return found;
}
/* the recent rows as Games has their games: a dump made into a game of recompiled/ since runs that game's program
   from then on, and a game of a folder of its own goes by the name Games shows it by, its control.nacp's */
static void recent_as_games(App* app) {
    MadeGames made;
    bool changed = false;
    int i;
    made_read(app, &made);
    for (i = app->recent.count - 1; i >= 0; --i) { /* from the oldest: an entry dropped moves none still to look at */
        const GuiRecentGame* r = &app->recent.game[i];
        const char* program = r->driver == GUI_SWITCH && !r->prepared ? made_program(&made, r->path) : NULL;
        if (program && gui_recent_made(&app->recent, i, program)) changed = true;
    }
    made_free(&made);
    for (i = 0; i < app->recent.count; ++i) {
        char* shown = app->recent.game[i].driver == GUI_SWITCH && gui_game_folder(app->recent.game[i].path)
                          ? gui_game_shown(app->recent.game[i].path)
                          : NULL;
        if (shown && gui_recent_rename(&app->recent, app->recent.game[i].path, shown)) changed = true;
        free(shown);
    }
    if (changed) recent_changed(app);
}
/* room on the disks for file's run (gui_room), not extracted yet; else room_line says what it takes. With Extract game
   container off a dump is read in place and takes no room. On, a Switch dump is extracted straight into its game's
   folder, or the temporary folder with Extract to tmpfs; only into an extractions folder picked is it kept, and its
   game made from a copy. The free space is room_free in tests */
static bool room_fits(App* app, const char* file) {
    bool nx = app->menu.selected == GUI_SWITCH, tmpfs = nx && gui_settings_tmpfs(settings_of(app));
    if (!tmpfs && !gui_settings_extract(settings_of(app))) return true; /* read in place: nothing extracted */
    bool kept = !nx || (app->place[GUI_PLACE_WORK] && !tmpfs), fits = true;
    char *out, *temp = tmpfs ? snx_temp_root() : NULL, need_text[32], left_text[32];
    const char* games = app->dest_out ? app->dest_out : app->games_dir; /* where the game goes */
    const char* work = temp ? temp : kept ? app->work_dir : games;
    uint64_t need = 0, left = 0;
    if (nx && nro_file(file)) { /* no extraction */
        free(temp);
        return true;
    }
    out = dump_out(app, file, nx ? NULL : "3ds");
    if (kept ? !snx_is_dir(out) : !made_from(app, file)) /* else extracted already, and its game made with it */
        fits = gui_room(snx_file_size(file), nx && kept, app->room_free ? app->room_free : free_space(work),
                        app->room_free ? app->room_free : free_space(games), snx_same_disk(work, games), &need, &left);
    free(temp);
    if (!fits) {
        gui_size_text(need_text, sizeof need_text, need);
        gui_size_text(left_text, sizeof left_text, left);
        snprintf(app->room_line, sizeof app->room_line, snx_tr("Needs about %s, %s free."), need_text, left_text);
    }
    free(out);
    return fits;
}
static void open_folder_picker(App* app) {
    if (!app->picked_folder) app->picked_folder = SDL_RegisterEvents(1);
    if (!app->picked_folder) return;
#ifdef SDL_PLATFORM_ANDROID
    { /* SDL has no folder picker there: the system's own, through the app */
        char message[sizeof app->editor.note - 40];
        if (gui_android_pick_folder(folder_picked, &app->picked_folder, message, sizeof message))
            ;
        else if (app->editing) {
            snprintf(app->editor.note, sizeof app->editor.note, snx_tr("The folder picker could not open: %s"),
                     message);
            say_failure(app, app->editor.note);
        } else { /* Games' Add a folder */
            snprintf(app->message, sizeof app->message, snx_tr("Folders: the folder picker could not open: %s"),
                     message);
            say_failure(app, app->message);
        }
    }
#elif defined(SDL_PLATFORM_IOS)
    { /* nor there: the document picker (dist/apple) */
        char message[sizeof app->editor.note - 40];
        if (gui_apple_pick(app->window, 1, folder_picked, &app->picked_folder, message, sizeof message))
            ;
        else if (app->editing) {
            snprintf(app->editor.note, sizeof app->editor.note, snx_tr("The folder picker could not open: %s"),
                     message);
            say_failure(app, app->editor.note);
        } else {
            snprintf(app->message, sizeof app->message, snx_tr("Folders: the folder picker could not open: %s"),
                     message);
            say_failure(app, app->message);
        }
    }
#else
    SDL_ShowOpenFolderDialog(folder_picked, &app->picked_folder, app->window, NULL, false);
#endif
}
/* a folder dropped or pasted on Folders' focused row: a place takes it, a library's row adds it to that library;
   the last row says where it goes */
static void folders_take(App* app, const char* path) {
    int at, group, kind = folder_row(app, app->editor.row, &at, &group),
                   library = folder_row_library(app, app->editor.row);
    if (kind == GUI_ROW_PLACE)
        place_set(app, at, path);
    else if (library >= 0) {
        app->adding = library;
        folder_ask(app, path);
        app->adding = -1;
    } else
        snprintf(app->editor.note, sizeof app->editor.note, "%s",
                 snx_tr("Drop it on the folder it takes the place of, or on a library's row"));
}
/* Select (with del, Del) on a row of Folders: a place picks another folder (Del: asks before its default); dumps/ says
   what it is; an Extra asks before it is taken off; Add a folder opens the picker for its library */
static void folder_select(App* app, int row, bool del) {
    int at, group, kind = folder_row(app, row, &at, &group);
    if (kind == GUI_ROW_PLACE && del)
        place_default_ask(app, at);
    else if (kind == GUI_ROW_PLACE) {
        app->place_row = at;
        open_folder_picker(app);
    } else if (kind == GUI_ROW_DUMPS)
        snprintf(app->editor.note, sizeof app->editor.note, "%s", snx_tr(dumps_row));
    else if (kind == GUI_ROW_FOLDER)
        folder_off_ask(app, at);
    else if (kind == GUI_ROW_ADD && !del) {
        app->adding = at;
        open_folder_picker(app);
    }
}
/* the setting a row of the open group or page is, and the row a setting is (-1: none) */
static int setting_of(const App* app, int row) { return app->view_count ? app->view_rows[row] : app->first + row; }
static int row_of(const App* app, int setting) {
    int r;
    if (!app->view_count) return setting - app->first;
    for (r = 0; r < app->view_count && app->view_rows[r] != setting; ++r) {}
    return r < app->view_count ? r : -1;
}
/* a player's key, gamepad_p<N>_name: N, else 0 */
static int player_key(const char* key, const char* name) {
    int n = 0, at = 0;
    if (!key || sscanf(key, "gamepad_p%d_%n", &n, &at) != 1 || !at || (name && strcmp(key + at, name))) return 0;
    return n >= 1 && n <= SNX_GUI_PLAYERS ? n : 0;
}
/* group g's rows, wherever they are: its section's own, and its pages' rows (a second page's comes after the first
   page's rows) */
static int in_group(const App* app, int g, int r) {
    return !strcmp(app->settings.keys[r].section, app->settings.keys[app->group_first[g]].section);
}
/* group g's page row of the players (the runtime's "Player" page), -1 for none */
static int players_row(const App* app, int g) {
    int r;
    for (r = 0; g > 0 && g < app->groups && r < app->settings.count; ++r)
        if (in_group(app, g, r) && app->settings.values[r].kind == GUI_SETTING_PAGE &&
            !strcmp(app->settings.keys[r].key, "Player"))
            return r;
    return -1;
}
/* Controllers by player, as Eden's tabs, into rows: its Player row first; every player's rows for player 0, player
   N's own for N with the rows no player has its own of. How many, 0 for a group without players */
static int player_rows(App* app, int g, int p, int* rows) {
    int players = players_row(app, g), r, n = 0;
    GuiSetting* row;
    if (players < 0) return 0;
    row = &app->settings.values[players];
    rows[n++] = players;
    for (r = row->first; p && r < row->first + row->count && n < GUI_VIEW_ROWS; ++r)
        if (player_key(app->settings.values[r].key, NULL) == p) rows[n++] = r; /* the player's, its page's */
    for (r = 0; r < app->settings.count && n < GUI_VIEW_ROWS; ++r) {
        const char* key = app->settings.values[r].key ? app->settings.values[r].key : "";
        if (r == players || !in_group(app, g, r)) continue;
        if (p && (app->settings.values[r].kind == GUI_SETTING_PAGE || !strcmp(key, "gamepad_swap_ab") ||
                  !strcmp(key, "gamepad_rumble")))
            continue; /* each player has its own of these */
        rows[n++] = r;
    }
    return n;
}
/* the open group as a view of its rows by player, its Player row saying which (< All > or < N >) */
static void group_view(App* app, int g) {
    int r, players = players_row(app, g);
    app->view_count = player_rows(app, g, app->player, app->view_rows);
    if (!app->view_count) return;
    if (app->player)
        snprintf(app->settings.values[players].text, sizeof app->settings.values[players].text, "< %d >", app->player);
    else
        snprintf(app->settings.values[players].text, sizeof app->settings.values[players].text, "< %s >",
                 snx_tr("All"));
    for (r = 0; r < app->view_count; ++r) app->view_keys[r] = app->settings.keys[app->view_rows[r]];
    app->view_keys[0].about =
        SNX_TR_NOOP("Each player's own, as Eden's tabs: Left/Right go round All and players 1 to 8");
    gui_editor_form(&app->editor, app->view_keys, app->view_count, app->settings.values, NULL);
    app->editor.flat = true;
}
#define SAY_SETUP_NICKNAME SNX_TR_NOOP("nickname 4 to 20 of A-Z a-z 0-9 space . _ -")
/* the User group: its one setting, who the games play as, shown with every user of users.ini to rename */
static bool users_group(const App* app, int g) {
    return g > 0 && g < app->groups && !strcmp(app->group_keys[g].key, "User");
}
/* its rows, Plays as then a user each with its nickname: how many */
static int users_fill(App* app) {
    int i, plays = settings_user(app);
    snprintf(app->users_form.plays, sizeof app->users_form.plays, snx_tr("User %d: %s"), plays,
             plays >= 1 && plays <= g_user_count ? g_users[plays - 1].nickname : snx_tr("none in users.ini"));
    app->users_keys[0] =
        (GuiThemeKey){"",
                      SNX_TR_NOOP("Plays as"),
                      GUI_KEY_ACTION,
                      offsetof(GuiUsersForm, plays),
                      sizeof app->users_form.plays,
                      NULL,
                      0,
                      SNX_TR_NOOP("The user the games play as: Left/Right, or Enter to pick by picture")};
    for (i = 0; i < g_user_count && i < SNX_USERS_MAX; ++i) {
        snprintf(app->users_label[i], sizeof app->users_label[i], snx_tr("User %d"), i + 1);
        snprintf(app->users_form.nick[i], sizeof app->users_form.nick[i], "%s", g_users[i].nickname);
        app->users_keys[i + 1] = (GuiThemeKey){
            "",
            app->users_label[i],
            GUI_KEY_TEXT,
            offsetof(GuiUsersForm, nick) + (size_t)i * sizeof app->users_form.nick[0],
            sizeof app->users_form.nick[0],
            NULL,
            0,
            SNX_TR_NOOP(
                "Its nickname, as the games and Eden's rooms show it: Enter types it, 4 to 20 of A-Z a-z 0-9 space "
                ". _ -")};
    }
    return i + 1;
}
static void open_users(App* app, int g, int row) {
    int n = users_fill(app);
    gui_editor_form(&app->editor, app->users_keys, n, &app->users_form, NULL);
    app->editor.flat = true;
    app->editor.row = SDL_clamp(row, 0, n - 1);
    app->form = GUI_FORM_USERS;
    app->group = g;
    app->page = -1;
    app->view_count = 0;
    app->editing = true;
}
/* Plays as a step either way, round the users, kept as Settings' user */
static void plays_as_step(App* app, int step) {
    char value[16];
    int n = SDL_max(g_user_count, 1), user = (settings_user(app) - 1 + n + (step < 0 ? -1 : 1)) % n + 1;
    snprintf(value, sizeof value, "%d", user);
    gui_settings_set(&app->settings, "user", value);
    app->editor.dirty = true;
}
/* a nickname typed: users.ini written with it, the head's too; one the rule refuses or that cannot be written said, the
   nickname as it was */
static void users_commit(App* app) {
    int number = app->editor.row;
    char why[512], *nick;
    app->editor.dirty = false;
    if (number < 1 || number > g_user_count) return;
    nick = app->users_form.nick[number - 1];
    if (!strcmp(nick, g_users[number - 1].nickname)) return;
    if (!snx_users_nickname_valid(nick))
        snprintf(app->editor.note, sizeof app->editor.note, snx_tr("%s is not a nickname: %s"), nick,
                 snx_tr(SAY_SETUP_NICKNAME));
    else if (!app->users_dir || snx_users_rename(app->users_dir, number, nick, why, sizeof why)) {
        snprintf(app->editor.note, sizeof app->editor.note, snx_tr("User %d keeps %s: %s"), number,
                 g_users[number - 1].nickname, app->users_dir ? why : snx_tr("no users/ folder"));
        say_failure(app, app->editor.note);
    } else {
        snprintf(app->editor.note, sizeof app->editor.note, snx_tr("User %d is %s now"), number, nick);
        users_read(app); /* the head shows the nickname */
    }
    users_fill(app); /* the rows as users.ini says */
}
static void open_group(App* app, int g) {
    if (!g) {
        open_app(app, 0);
        return;
    }
    if (users_group(app, g)) {
        open_users(app, g, 0);
        return;
    }
    app->view_count = 0;
    gui_editor_form(&app->editor, app->settings.keys + app->group_first[g], app->group_rows[g], app->settings.values,
                    NULL);
    app->editor.flat = true;
    app->form = GUI_FORM_GROUP;
    app->group = g;
    app->first = app->group_first[g];
    app->page = -1;
    group_view(app, g);
}
static void open_pad(App* app, int row);
static bool pad_page(const App* app, int row);
/* a page's rows (settings.h), from its row in the group; the button map's, the pad drawn */
static void open_page(App* app, int row) {
    const GuiSetting* page = &app->settings.values[row];
    if (pad_page(app, row)) {
        open_pad(app, row);
        return;
    }
    gui_editor_form(&app->editor, app->settings.keys + page->first, page->count, app->settings.values, NULL);
    app->editor.flat = true;
    app->view_count = 0;
    app->first = page->first;
    app->page = row;
}
static void open_settings(App* app) {
    app->group = 0;
    settings_top(app, 0);
    space_start(app);
}
static void leave_editor(App* app) {
    gui_editor_stop(&app->editor);
    app->editing = false;
    SDL_StopTextInput(app->window);
}
static void pad_close(App* app);
/* ESC on a screen: a group's and the App's go back to the groups, the theme editor to where it was opened */
static void device_stop(App* app);
static void leave_form(App* app) {
    int form = app->form;
    device_stop(app);
    leave_editor(app);
    if (form == GUI_FORM_PAD) pad_close(app);
    if ((form == GUI_FORM_GROUP || form == GUI_FORM_PAD) && app->page >= 0) { /* a page: back to its row */
        int row = app->page;
        open_group(app, app->group);
        app->editor.row = SDL_max(row_of(app, row), 0);
        app->editing = true;
    } else if (form == GUI_FORM_GROUP || form == GUI_FORM_APP || form == GUI_FORM_USERS)
        settings_top(app, app->group);
    else if (form == GUI_FORM_FOLDERS)
        open_app(app, GUI_APP_PLACES);
    else if (form == GUI_FORM_THEME && app->theme_from_app) {
        app->theme_from_app = false;
        open_app(app, GUI_APP_EDITOR);
    }
}
static void clean_row(App* app);
static void move_row(App* app);
static void settings_reset_ask(App* app);
/* ---- UI scale: picked in a box over the App's rows, then kept once said so, else back by itself after a while, as
   a screen mode is: a size can leave the menu too small or too big to read ---- */
#define GUI_TEXT_KEEP_MS 15000
static const char* const size_labels[GUI_TEXT_MAX + 4] = {SNX_TR_NOOP("0.25 of Auto"),
                                                          SNX_TR_NOOP("0.5 of Auto"),
                                                          SNX_TR_NOOP("0.75 of Auto"),
                                                          SNX_TR_NOOP("Auto"),
                                                          "1x",
                                                          "2x",
                                                          "3x",
                                                          "4x"};
static int size_of_choice(int i) { return i < 3 ? -(i + 1) : i - 3; }
static void sizes_open(App* app) {
    app->sizes.count = GUI_TEXT_MAX + 4;
    app->sizes.focus = app->menu.text_size < 0 ? -app->menu.text_size - 1 : app->menu.text_size + 3;
    app->sizes_shown = true;
}
static void sizes_place(App* app, GuiChoices* c, const char** labels) {
    int width, height, i;
    area(app, &width, &height);
    for (i = 0; i < app->sizes.count; ++i) labels[i] = snx_tr(size_labels[i]);
    choices_place(app,
                  app->theme.dialog == GUI_DIALOG_YORHA
                      ? gui_rect(cell_w(app), cell_h(app) / 2, width - 2 * cell_w(app), height - cell_h(app))
                      : gui_rect(0, 0, width, height),
                  snx_tr("UI scale"), NULL, labels, app->sizes.count, app->sizes.focus, c);
}
static void render_sizes(App* app) {
    GuiChoices c;
    const char* labels[GUI_TEXT_MAX + 4];
    sizes_place(app, &c, labels);
    box_shade(app); /* over the App's rows */
    choices_draw(app, &c, snx_tr("UI scale"), NULL, labels, app->sizes.count, app->sizes.focus);
}
static void text_left(App* app);
/* size in use now, asked about until GUI_TEXT_KEEP_MS from now */
static void size_take(App* app, int size) {
    app->sizes_shown = false;
    if (size == app->menu.text_size) return;
    app->size_was = app->menu.text_size;
    app->menu.text_size = size;
    layout(app);
    app->size_until = now_ms(app) + GUI_TEXT_KEEP_MS;
    text_left(app);
    gui_confirm_open(&app->confirm, GUI_ASK_TEXT, false);
    app_values(app);
}
static void sizes_key(App* app, GuiKey key) {
    switch (gui_choice_key(&app->sizes, key)) {
        case GUI_CHOICE_TAKE: size_take(app, size_of_choice(app->sizes.focus)); break;
        case GUI_CHOICE_BACK: app->sizes_shown = false; break;
        default: break;
    }
}
/* a size under the pointer takes the focus, a click takes it; a click off the box closes it */
static void sizes_pointer(App* app, float x, float y, bool click) {
    GuiChoices c;
    const char* labels[GUI_TEXT_MAX + 4];
    bool on;
    int i;
    sizes_place(app, &c, labels);
    if ((i = choices_hit(&c, x, y, &on)) >= 0) {
        app->sizes.focus = i;
        if (click) size_take(app, size_of_choice(i));
    } else if (click && !on)
        app->sizes_shown = false;
}
/* the new size kept in app.ini (the setup's once it is done), or the one before it back (Back, ESC, or no answer in
   time) */
static void size_answer(App* app, bool keep) {
    app->size_until = 0;
    if (keep && app->setup_on)
        app->setup_save = true;
    else if (keep)
        app_save(app);
    else {
        app->menu.text_size = app->size_was;
        layout(app);
    }
    app_values(app);
}
/* while a new size waits for its answer: the question's line with the seconds left, and the size before it back once
   they run out */
static void text_left(App* app) {
    Uint64 now = now_ms(app);
    char was[64];
    if (!app->size_until) return;
    if (now >= app->size_until) {
        app->confirm.shown = false;
        size_answer(app, false);
        return;
    }
    size_name(app, app->size_was, was, sizeof was);
    snprintf(app->size_line, sizeof app->size_line, snx_tr("Keep this UI scale? Back to %s in %d s"), was,
             (int)((app->size_until - now + 999) / 1000));
}
/* the App's rows: the theme and the language go round, either way, UI scale opens its box, and the editor opens */
static void app_action(App* app, int row, int step) {
    if (row == GUI_APP_THEME)
        next_theme(app, step < 0 ? -1 : 1);
    else if (row == GUI_APP_TEXT) {
        sizes_open(app);
        return;
    } else if (row == GUI_APP_ANIMATIONS) {
        app->animations_off = !app->animations_off;
    } else if (row == GUI_APP_SIDE) {
        app->side_box = (app->side_box + (step < 0 ? 2 : 1)) % 3;
        layout(app);
    } else if (row == GUI_APP_LANGUAGE) {
        app->language = (app->language + (step < 0 ? snx_gui_language_count : 1)) % (snx_gui_language_count + 1);
        language_apply(app);
    } else if (row == GUI_APP_EDITOR && !step) {
        app->theme_from_app = true;
        open_editor(app);
        return;
    } else if (row == GUI_APP_PLACES && !step) {
        open_folders(app, 0);
        return;
    } else if (row == GUI_APP_CLEAN) {
        if (!step) clean_row(app);
        return;
    } else if (row == GUI_APP_MOVE) {
        if (!step) move_row(app);
        return;
    } else if (row == GUI_APP_RESET) {
        if (!step) settings_reset_ask(app);
        return;
    }
    if (row == GUI_APP_THEME || row == GUI_APP_TEXT || row == GUI_APP_LANGUAGE || row == GUI_APP_ANIMATIONS ||
        row == GUI_APP_SIDE)
        app_save(app);
    app_values(app);
}
/* saved in themes/ of the data folder, and the menu's theme from then on */
static void save_editor(App* app) {
    const char* const* dirs = (const char* const*)app->themes_dirs;
    if (gui_editor_save(&app->editor, app->themes_dirs[0])) return;
    gui_themes_scan(&app->themes, dirs, 2);
    theme_load(app, app->editor.id, false);
    app_save(app);
}
/* settings.ini written as the settings are, a failure in the note and its box; the head and Plays as after it */
static void write_settings(App* app) {
    gui_settings_sync(&app->settings);
    if (app->settings_file && gui_settings_save(&app->settings, app->settings_file)) {
        snprintf(app->editor.note, sizeof app->editor.note, snx_tr("Not saved: %s could not be written"),
                 app->settings_file);
        say_failure(app, app->editor.note);
    }
    if (app->user != settings_user(app)) users_read(app); /* the head shows the user picked */
    if (app->form == GUI_FORM_USERS) users_fill(app);     /* Plays as says it too */
}
/* a Settings pick, kept at once */
static void keep_settings(App* app) {
    if ((app->form != GUI_FORM_GROUP && app->form != GUI_FORM_SETTINGS && app->form != GUI_FORM_PAD &&
         app->form != GUI_FORM_USERS) ||
        !app->editor.dirty)
        return;
    app->editor.dirty = false;
    write_settings(app);
}
static int setting_row(const App* app, const char* key) {
    int i;
    for (i = 0; i < app->settings.count; ++i)
        if (app->settings.values[i].key && !strcmp(app->settings.values[i].key, key)) return i;
    return -1;
}
/* the page gamepad_map is on */
static bool pad_page(const App* app, int row) {
    const GuiSetting* page = &app->settings.values[row];
    int map = setting_row(app, "gamepad_map");
    return map >= 0 && map >= page->first && map < page->first + page->count;
}
/* the pad's name, its labels and what it holds now */
static void pad_sync(App* app) {
    SnxPadScreen* p = &app->pad;
    const char* name = app->gamepad ? SDL_GetGamepadName(app->gamepad) : NULL;
    size_t n = 0;
    int c;
    if (app->gamepad && !name) name = "Controller";
    while (name && *name && n + 1 < sizeof p->pad) /* in the font's glyphs, as the page's other texts */
        p->pad[n++] = (char)gui_glyph(SDL_StepUTF8(&name, NULL));
    p->pad[n] = 0;
    p->held = 0;
    if (!app->gamepad) {
        memset(p->labels, 0, sizeof p->labels);
        p->symmetric = 0;
        return;
    }
    snx_pad_labels(app->gamepad, p->labels);
    p->symmetric = snx_pad_symmetric(app->gamepad);
    for (c = 0; c < SNX_PAD_CONTROLS; ++c)
        if (snx_pad_held(app->gamepad, c)) p->held |= 1u << c;
}
/* the map as gamepad_map says, A and B as gamepad_swap_ab does; a map it cannot read is named, and left out. Why there is
   no pad stays */
static void pad_start(App* app) {
    const GuiSettings* s = &app->settings;
    const char* swap = app->pad_swap >= 0 ? gui_settings_value(s, s->values[app->pad_swap].key) : NULL;
    int map = app->pad_map, on = swap ? atoi(swap) : -1;
    char error[64], note[sizeof app->pad.note];
    snprintf(note, sizeof note, "%s", app->pad.note);
    snx_pad_screen_start(&app->pad);
    snprintf(app->pad.note, sizeof app->pad.note, "%s", note);
    if (on < 0) { /* a player's "As All": every player's */
        swap = gui_settings_value(s, "gamepad_swap_ab");
        on = swap && atoi(swap);
    }
    app->pad.swap_ab = on;
    if (snx_pad_map_read(&app->pad.own, s->values[map].text, error, sizeof error))
        snprintf(app->editor.note, sizeof app->editor.note,
                 snx_tr("gamepad_map's \"%s\" is not a control and the button it sends (south:B)"), error);
    pad_sync(app);
}
/* the SDL mappings the page and the games read: SDL_GAMECONTROLLERCONFIG_FILE's, else gamecontrollerdb.txt beside
   settings.ini; NULL without a settings file */
static char* pad_mappings_file(const App* app) {
    const char* named = SDL_GetHint(SDL_HINT_GAMECONTROLLERCONFIG_FILE);
    char *folder, *path;
    if (named && *named) return snx_strdup(named);
    folder = app->settings_file ? snx_dirname(app->settings_file) : NULL;
    path = folder ? snx_path_join(folder, "gamecontrollerdb.txt") : NULL;
    free(folder);
    return path;
}
#ifndef SDL_PLATFORM_ANDROID
/* the mappings file the page names, for the games the launcher starts: one outside its games/ (a folder the player
   added) would read the user's otherwise. A file the player named stays theirs */
static void pad_mappings_share(const App* app) {
    char* file;
    if (SDL_getenv_unsafe("SDL_GAMECONTROLLERCONFIG_FILE")) return;
    file = pad_mappings_file(app);
    if (file && snx_is_file(file)) SDL_setenv_unsafe("SDL_GAMECONTROLLERCONFIG_FILE", file, 1);
    free(file);
}
#endif
/* the first pad plugged in, when none is open */
static void pad_attach(App* app) {
    SDL_JoystickID* ids;
    int i, n = 0;
    if (app->gamepad || !app->pads_read) return;
    ids = SDL_GetGamepads(&n);
    for (i = 0; ids && i < n && !app->gamepad; i++) {
        app->gamepad = SDL_OpenGamepad(ids[i]);
        if (!app->gamepad) /* the page's texts, pad_screen.c's as the game's menu draws them: English, in the font's bytes */
            snprintf(app->pad.note, sizeof app->pad.note, "The controller could not be opened: %s", SDL_GetError());
    }
    SDL_free(ids);
    if (app->gamepad) app->pad.note[0] = 0; /* an earlier failed/unmapped pad no longer describes this one */
    if (!app->gamepad && !n) {              /* none, or a joystick SDL has no mapping for, named */
        SDL_JoystickID* joysticks = SDL_GetJoysticks(&n);
        app->pad.note[0] = 0;
        if (joysticks && n > 0) {
            char guid[33], *file = pad_mappings_file(app);
            SDL_GUIDToString(SDL_GetJoystickGUIDForID(joysticks[0]), guid, sizeof guid);
            snprintf(app->pad.note, sizeof app->pad.note, "%s has no SDL mapping: add a line for GUID %s to %s",
                     SDL_GetJoystickNameForID(joysticks[0]) ? SDL_GetJoystickNameForID(joysticks[0]) : "The joystick",
                     guid, file ? file : "gamecontrollerdb.txt");
            free(file);
        }
        SDL_free(joysticks);
    }
    app->pad_pushed = 0;
    pad_sync(app);
}
static void pad_load_mappings(App* app) {
    const char* named = SDL_GetHint(SDL_HINT_GAMECONTROLLERCONFIG_FILE);
    char* path;
    if (named && *named) return; /* SDL loaded the explicit file at subsystem startup */
    path = pad_mappings_file(app);
    if (path && snx_is_file(path) && SDL_AddGamepadMappingsFromFile(path) < 0) {
        snprintf(app->editor.note, sizeof app->editor.note, snx_tr("SDL mappings could not be read: %s"),
                 SDL_GetError());
        say_failure(app, app->editor.note);
    }
    free(path);
}
static void render(App* app);
/* the button map of map's row (row: what to come back to), A and B as swap's row says, for player (0: every one) */
static void open_pad_on(App* app, int row, int map, int swap, int player) {
    leave_editor(app);
    app->editing = true;
    app->form = GUI_FORM_PAD;
    app->page = row;
    app->pad_map = map;
    app->pad_swap = swap;
    app->pad_player = player;
    app->editor.note[0] = 0;
    app->pad.note[0] = 0;
    pad_start(app);
    render(app); /* SDL looks for pads for most of a second: the page shows first, saying so */
    /* two Joy-Cons one pad, or two, as the games take them (gamepad_joycon_pair, 1 when not picked) */
    {
        const char* pair = gui_settings_value(&app->settings, "gamepad_joycon_pair");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS, pair && !atoi(pair) ? "0" : "1");
    }
    app->pads_read = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (!app->pads_read)
        snprintf(app->pad.note, sizeof app->pad.note, "Controllers cannot be read: %s", SDL_GetError());
    else
        pad_load_mappings(app);
    pad_attach(app);
}
/* the Button map page's: every player's map */
static void open_pad(App* app, int row) {
    open_pad_on(app, row, setting_row(app, "gamepad_map"), setting_row(app, "gamepad_swap_ab"), 0);
}
/* player N's map row's, its A and B as its own swap says */
static void open_player_pad(App* app, int map) {
    char key[32];
    int p = player_key(app->settings.values[map].key, "map");
    snprintf(key, sizeof key, "gamepad_p%d_swap_ab", p);
    open_pad_on(app, map, map, setting_row(app, key), p);
}
static void pad_close(App* app) {
    if (app->gamepad) SDL_CloseGamepad(app->gamepad);
    app->gamepad = NULL;
    if (app->pads_read) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    app->pads_read = false;
}
/* what the page did: the map kept at once, as the Settings' other picks */
static void pad_did(App* app, int did) {
    GuiSettings* s = &app->settings;
    int map = app->pad_map;
    if (did == SNX_PAD_DID_LEAVE) {
        leave_form(app);
        return;
    }
    if (did != SNX_PAD_DID_MAP) return;
    snx_pad_map_write(&app->pad.own, s->values[map].text, sizeof s->values[map].text);
    app->editor.note[0] = 0; /* a map it could not read is replaced */
    app->editor.dirty = true;
    keep_settings(app);
}
/* a control of the pad: while the page waits, the press it waits for; else the key the console's button it sends
   is in the game's menu: A selects, B goes back, the D-pad moves */
static void pad_control(App* app, int control) {
    static const int KEYS[SNX_PAD_BUTTONS] = {
        SNX_PAD_KEY_SELECT, SNX_PAD_KEY_BACK, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, SNX_PAD_KEY_LEFT, SNX_PAD_KEY_UP,
        SNX_PAD_KEY_RIGHT,  SNX_PAD_KEY_DOWN};
    SnxPadMap map;
    int bit;
    if (control < 0) return;
    if (app->pad.listening) {
        pad_did(app, snx_pad_screen_press(&app->pad, control));
        return;
    }
    snx_pad_screen_map(&app->pad, &map);
    bit = map.bit[control];
    if (bit >= 0 && bit < SNX_PAD_BUTTONS && KEYS[bit] >= 0) pad_did(app, snx_pad_screen_key(&app->pad, KEYS[bit]));
}
/* a trigger past SNX_PAD_TRIGGER is its control pressed, the left stick past half way the D-pad: once as each crosses */
static void pad_axis(App* app, const SDL_GamepadAxisEvent* a) {
    int side;
    if (a->axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a->axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
        unsigned bit = 1u << (a->axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        bool on = a->value > SNX_PAD_TRIGGER, was = (app->pad_pushed & bit) != 0;
        app->pad_pushed = on ? app->pad_pushed | bit : app->pad_pushed & ~bit;
        if (on && !was) pad_control(app, a->axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER ? SNX_PAD_RT : SNX_PAD_LT);
        return;
    }
    if (a->axis != SDL_GAMEPAD_AXIS_LEFTX && a->axis != SDL_GAMEPAD_AXIS_LEFTY) return;
    for (side = 0; side < 2; ++side) {
        bool y = a->axis == SDL_GAMEPAD_AXIS_LEFTY, on = side ? a->value > 16384 : a->value < -16384;
        unsigned bit = 4u << (2 * y + side);
        bool was = (app->pad_pushed & bit) != 0;
        app->pad_pushed = on ? app->pad_pushed | bit : app->pad_pushed & ~bit;
        if (on && !was)
            pad_did(app, snx_pad_screen_key(&app->pad, y ? (side ? SNX_PAD_KEY_DOWN : SNX_PAD_KEY_UP)
                                                         : (side ? SNX_PAD_KEY_RIGHT : SNX_PAD_KEY_LEFT)));
    }
}
static bool menu_key_of(const SDL_KeyboardEvent* k, GuiKey* key);
/* the page's keys; Del puts the map back to the .ini's */
static void pad_keyboard(App* app, const SDL_KeyboardEvent* k) {
    GuiKey key;
    if (k->key == SDLK_TAB && !k->repeat) { /* the next screen, as the foot says, the pads closed */
        screen_tab(app, k->mod & SDL_KMOD_SHIFT ? -1 : 1);
        return;
    }
    if (k->key == SDLK_DELETE || k->key == SDLK_BACKSPACE) {
        int focus = app->pad.focus;
        gui_settings_reset(&app->settings, app->pad_map);
        app->editor.note[0] = 0;
        pad_start(app);
        app->pad.focus = focus;
        app->editor.dirty = true;
        keep_settings(app);
        return;
    }
    if (!menu_key_of(k, &key)) return;
    switch (key) {
        case GUI_UP: pad_did(app, snx_pad_screen_key(&app->pad, SNX_PAD_KEY_UP)); break;
        case GUI_DOWN: pad_did(app, snx_pad_screen_key(&app->pad, SNX_PAD_KEY_DOWN)); break;
        case GUI_LEFT: pad_did(app, snx_pad_screen_key(&app->pad, SNX_PAD_KEY_LEFT)); break;
        case GUI_RIGHT: pad_did(app, snx_pad_screen_key(&app->pad, SNX_PAD_KEY_RIGHT)); break;
        case GUI_SELECT: pad_did(app, snx_pad_screen_key(&app->pad, SNX_PAD_KEY_SELECT)); break;
        case GUI_CANCEL: pad_did(app, snx_pad_screen_key(&app->pad, SNX_PAD_KEY_BACK)); break;
        default: break;
    }
}
/* the page's events: keys, its pad's presses, pads plugged in and out; false for what the rest takes */
/* every controller there, open while a Controller row waits: one already open stays as it is */
static void device_open(App* app) {
    SDL_JoystickID* ids;
    int i, k, n = 0;
    ids = SDL_GetGamepads(&n);
    for (i = 0; ids && i < n && app->listened_count < (int)SDL_arraysize(app->listened); ++i) {
        for (k = 0; k < app->listened_count && SDL_GetGamepadID(app->listened[k]) != ids[i]; ++k) {}
        if (k == app->listened_count && (app->listened[k] = SDL_OpenGamepad(ids[i])) != NULL) ++app->listened_count;
    }
    SDL_free(ids);
}
/* a player's Controller row: Enter waits for a press on a controller, which the player then takes, by its GUID ("/2"
   after it for the second of two alike), as Eden's Input Device. ESC keeps the one it has */
/* a Controller row's controller as the row shows it */
static const char* device_shown(const GuiSetting* v) { return v->text[0] ? v->text : snx_tr("Any [default]"); }
static void device_listen(App* app, int row) {
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        snprintf(app->editor.note, sizeof app->editor.note, snx_tr("Controllers cannot be read: %s"), SDL_GetError());
        say_failure(app, app->editor.note);
        return;
    }
    pad_load_mappings(app);
    app->device_listen = row + 1;
    device_open(app);
    snprintf(app->editor.note, sizeof app->editor.note,
             snx_tr("Press a button on player %d's controller. ESC keeps %s"),
             player_key(app->settings.values[row].key, "device"), device_shown(&app->settings.values[row]));
}
static void device_stop(App* app) {
    if (!app->device_listen) return;
    app->device_listen = 0;
    while (app->listened_count) SDL_CloseGamepad(app->listened[--app->listened_count]);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}
/* the controller pressed, the player's from now on */
static void device_take(App* app, SDL_JoystickID id) {
    GuiSetting* v = &app->settings.values[app->device_listen - 1];
    SDL_JoystickID* ids;
    char guid[33], one[33];
    const char* name = SDL_GetGamepadNameForID(id);
    int i, n = 0, alike = 0;
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof guid);
    ids = SDL_GetGamepads(&n);
    for (i = 0; ids && i < n && ids[i] != id; ++i) { /* the ones alike before it */
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(ids[i]), one, sizeof one);
        alike += !SDL_strcasecmp(one, guid);
    }
    SDL_free(ids);
    if (alike)
        snprintf(v->text, sizeof v->text, "%s/%d", guid, alike + 1);
    else
        snprintf(v->text, sizeof v->text, "%s", guid);
    snprintf(app->editor.note, sizeof app->editor.note, snx_tr("Player %d: %s"), player_key(v->key, "device"),
             name ? name : snx_tr("the controller pressed"));
    device_stop(app);
    app->editor.dirty = true;
    keep_settings(app);
}
/* while a Controller row waits: a press takes its controller, ESC stops; every other event waits too */
static bool device_event(App* app, const SDL_Event* e) {
    GuiKey key;
    if (!app->device_listen) return false;
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
        device_take(app, e->gbutton.which);
    else if (e->type == SDL_EVENT_GAMEPAD_ADDED) /* plugged in while it waits: its press counts too */
        device_open(app);
    else if (e->type == SDL_EVENT_KEY_DOWN && menu_key_of(&e->key, &key) && key == GUI_CANCEL) {
        const GuiSetting* v = &app->settings.values[app->device_listen - 1];
        device_stop(app);
        snprintf(app->editor.note, sizeof app->editor.note, snx_tr("Player %d keeps %s"), player_key(v->key, "device"),
                 device_shown(v));
    } else
        return e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_TEXT_INPUT ||
               e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_FINGER_DOWN;
    return true;
}
static bool pad_event(App* app, const SDL_Event* e) {
    SDL_JoystickID ours = app->gamepad ? SDL_GetGamepadID(app->gamepad) : 0;
    switch (e->type) {
        case SDL_EVENT_KEY_DOWN: pad_keyboard(app, &e->key); return true;
        case SDL_EVENT_TEXT_INPUT: return true;
        case SDL_EVENT_JOYSTICK_ADDED: /* one SDL has no mapping for is named, and no longer once it is out */
        case SDL_EVENT_JOYSTICK_REMOVED:
        case SDL_EVENT_GAMEPAD_ADDED: pad_attach(app); return true;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (ours && e->gdevice.which == ours) {
                SDL_CloseGamepad(app->gamepad);
                app->gamepad = NULL;
                app->pad.listening = 0;
                pad_attach(app);
            }
            return true;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (ours && e->gbutton.which == ours) pad_control(app, snx_pad_of_button(e->gbutton.button));
            return true;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (ours && e->gaxis.which == ours) pad_axis(app, &e->gaxis);
            return true;
        default: return false;
    }
}
/* a text of the page, in the font's glyphs (CP437) a byte each */
static void pad_text(App* app, const SnxPadShape* p, GuiRgb c) {
    float x = p->x;
    const char* s;
    SDL_SetTextureColorMod(app->font, c.r, c.g, c.b);
    for (s = p->text; *s; ++s, x += 8.0f * (float)app->scale) glyph(app, x, p->y, (unsigned char)*s);
}
/* a round part of the pad */
static void pad_disc(App* app, const SnxPadShape* p, GuiRgb c) {
    enum { SIDES = 32 };
    SDL_Vertex v[SIDES + 1];
    int index[SIDES * 3], k;
    float r = SDL_min(p->w, p->h) / 2;
    SDL_FColor color = {(float)c.r / 255, (float)c.g / 255, (float)c.b / 255, 1};
    for (k = 0; k <= SIDES; ++k) {
        float a = 2 * SDL_PI_F * (float)k / SIDES;
        v[k].position.x = p->x + p->w / 2 + (k ? r * SDL_cosf(a) : 0);
        v[k].position.y = p->y + p->h / 2 + (k ? r * SDL_sinf(a) : 0);
        v[k].color = color;
        v[k].tex_coord.x = v[k].tex_coord.y = 0;
    }
    for (k = 0; k < SIDES; ++k) {
        index[3 * k] = 0;
        index[3 * k + 1] = k + 1;
        index[3 * k + 2] = k + 1 < SIDES ? k + 2 : 1;
    }
    SDL_RenderGeometry(app->renderer, NULL, v, SIDES + 1, index, SIDES * 3);
}
/* the page in the theme's colours: the panels a little darker than the page, the focus as the menus' and what is held
   on the pad in the active colour */
static void render_pad(App* app) {
    const GuiTheme* theme = &app->theme;
    float cw = 8.0f * (float)app->scale, ch = 16.0f * (float)app->scale;
    char keys[256];
    GuiPlace place;
    GuiRect r;
    int i;
    /* the pad drawn in the box between the head and the foot */
    place_screen(app, &place, shown_keys(app, keys, sizeof keys), 0, GUI_SHOWS_TABS);
    chrome(app, &place);
    r = inner(app, place.list);
    pad_sync(app);
    snx_pad_screen_lay(&app->pad, (float)r.x, (float)r.y, (float)r.w, (float)r.h, cw, ch, &app->pad_drawing);
    for (i = 0; i < app->pad_drawing.n; ++i) {
        const SnxPadShape* p = &app->pad_drawing.shape[i];
        GuiRgb on = p->style == SNX_PAD_STYLE_FOCUS ? theme->focus : theme->active;
        bool marked = p->style == SNX_PAD_STYLE_FOCUS || p->style == SNX_PAD_STYLE_LIT;
        if (p->kind == SNX_PAD_SHAPE_TEXT)
            pad_text(app, p,
                     marked                           ? theme->page
                     : p->style == SNX_PAD_STYLE_SOFT ? theme->soft
                     : p->style == SNX_PAD_STYLE_NOTE ? theme->failure
                                                      : theme->ink);
        else if (p->kind == SNX_PAD_SHAPE_DISC)
            pad_disc(app, p, marked ? on : theme->soft);
        else if (marked)
            fill(app, p->x, p->y, p->w, p->h, on, 255);
        else
            fill(app, p->x, p->y, p->w, p->h, theme->soft, p->kind == SNX_PAD_SHAPE_BAR ? 255 : 0x40);
    }
    foot(app, &place,
         app->editor.note[0] ? app->editor.note
         : !app->pads_read   ? snx_tr("Looking for controllers...")
                             : "",
         rgb(app->editor.note[0] ? theme->failure : theme->soft), keys);
}
static void editor_key(App* app, GuiKey key) {
    GuiEditorDo what;
    /* a player's map row opens its button map, as every player's Button map page does; its Controller row waits for
       a press on the controller it takes */
    if (app->form == GUI_FORM_GROUP && key == GUI_SELECT && !app->editor.typing &&
        player_key(app->settings.values[setting_of(app, app->editor.row)].key, "map")) {
        open_player_pad(app, setting_of(app, app->editor.row));
        return;
    }
    if (app->form == GUI_FORM_GROUP && key == GUI_SELECT && !app->editor.typing &&
        player_key(app->settings.values[setting_of(app, app->editor.row)].key, "device")) {
        device_listen(app, setting_of(app, app->editor.row));
        return;
    }
    what = gui_editor_key(&app->editor, key);
    if (what == GUI_EDITOR_ACTION && app->form == GUI_FORM_SETTINGS && !app->editor.step) {
        open_group(app, app->editor.row);
        return;
    }
    if (what == GUI_EDITOR_ACTION && app->form == GUI_FORM_GROUP) { /* a page's row opens it, an All steps its rows */
        int row = setting_of(app, app->editor.row);
        if (app->settings.values[row].kind == GUI_SETTING_PAGE && !strcmp(app->settings.keys[row].key, "Player")) {
            /* the player shown, round All and 1 to 8 */
            app->player = (app->player + (app->editor.step < 0 ? SNX_GUI_PLAYERS : 1)) % (SNX_GUI_PLAYERS + 1);
            group_view(app, app->group);
            return;
        }
        if (app->settings.values[row].kind == GUI_SETTING_PAGE && !app->editor.step)
            open_page(app, row);
        else if (gui_settings_step_all(&app->settings, row, app->editor.step)) {
            app->editor.dirty = true;
            keep_settings(app);
        }
        return;
    }
    if (what == GUI_EDITOR_ACTION && app->form == GUI_FORM_APP) {
        app_action(app, app->editor.row, app->editor.step);
        return;
    }
    if (app->form == GUI_FORM_FOLDERS) {
        if (what == GUI_EDITOR_ACTION && !app->editor.step)
            folder_select(app, app->editor.row, false);
        else if (what == GUI_EDITOR_SAVE) /* All back to the defaults */
            places_reset_ask(app);
        if (what != GUI_EDITOR_LEAVE) return;
    }
    if (app->form == GUI_FORM_USERS) { /* Plays as steps or picks; a nickname typed is written */
        if (what == GUI_EDITOR_ACTION && app->editor.step)
            plays_as_step(app, app->editor.step);
        else if (what == GUI_EDITOR_ACTION)
            users_open(app);
        else if (app->editor.dirty && app->editor.row)
            users_commit(app);
    }
    if (what == GUI_EDITOR_SAVE && app->form == GUI_FORM_THEME) save_editor(app);
    keep_settings(app);
    if (what == GUI_EDITOR_LEAVE) {
        leave_form(app);
        return;
    }
#ifdef SDL_PLATFORM_ANDROID
    if (app->form == GUI_FORM_GROUP) { /* the menu on the whole screen too, as the games, unless fullscreen is 0 */
        const char* fullscreen = gui_settings_value(&app->settings, "fullscreen");
        SDL_SetWindowFullscreen(app->window, !fullscreen || strcmp(fullscreen, "0"));
    }
#endif
    if (app->editor.typing && !SDL_TextInputActive(app->window))
        SDL_StartTextInput(app->window); /* a phone's keyboard comes up */
    else if (!app->editor.typing && SDL_TextInputActive(app->window))
        SDL_StopTextInput(app->window);
}
/* keys and typed text: while a value is typed, Space and the letters are its text */
static void editor_input(App* app, const SDL_Event* e) {
    GuiKey key;
    if (e->type == SDL_EVENT_TEXT_INPUT) {
        gui_editor_type(&app->editor, e->text.text);
        return;
    }
    if (app->form == GUI_FORM_THEME && e->key.key == SDLK_S && (e->key.mod & SDL_KMOD_CTRL) && !e->key.repeat) {
        save_editor(app);
        return;
    }
    /* Del, or Backspace when nothing is typed: a setting back to its default */
    if (app->form == GUI_FORM_GROUP && !app->editor.typing &&
        (e->key.key == SDLK_DELETE || e->key.key == SDLK_BACKSPACE)) {
        gui_settings_reset(&app->settings, setting_of(app, app->editor.row));
        app->editor.dirty = true;
        keep_settings(app);
        return;
    }
    /* Folders: Del puts a place back to its default or takes an Extra off, Ctrl+V gives the row the folder pasted */
    if (app->form == GUI_FORM_FOLDERS && (e->key.key == SDLK_DELETE || e->key.key == SDLK_BACKSPACE)) {
        folder_select(app, app->editor.row, true);
        return;
    }
    if (app->form == GUI_FORM_FOLDERS && e->key.key == SDLK_V && (e->key.mod & SDL_KMOD_CTRL) && !e->key.repeat) {
        char* clipboard = SDL_GetClipboardText();
        char* path = gui_drop_path(clipboard ? clipboard : "", true);
        if (path)
            folders_take(app, path);
        else
            snprintf(app->editor.note, sizeof app->editor.note, "%s", snx_tr("Paste one local folder's path"));
        free(path);
        SDL_free(clipboard);
        return;
    }
    if (e->key.key == SDLK_TAB && !e->key.repeat && !app->editor.typing) {
        screen_tab(app, e->key.mod & SDL_KMOD_SHIFT ? -1 : 1);
        return;
    }
    if (app->form == GUI_FORM_THEME && e->key.key == SDLK_P && (e->key.mod & SDL_KMOD_CTRL) && !e->key.repeat) {
        app->editor.preview = app->editor.preview == GUI_SCREEN_MENU ? GUI_SCREEN_COMPILING : GUI_SCREEN_MENU;
        app->preview_since = 0;
        return;
    }
    if (app->editor.typing) {
        if (e->key.key == SDLK_BACKSPACE) gui_editor_erase(&app->editor);
        if (e->key.key == SDLK_V && (e->key.mod & SDL_KMOD_CTRL)) {
            char* clipboard = SDL_GetClipboardText();
            gui_editor_type(&app->editor, clipboard ? clipboard : "");
            SDL_free(clipboard);
        }
        if (e->key.key == SDLK_SPACE) return;
    }
    if (menu_key_of(&e->key, &key)) editor_key(app, key);
}
/* ---- the Settings and the theme editor: the groups on the left, the open or the focused group's rows on the
   right; the theme editor's keys on the left, the preview on the right. The two lists are ui.txt's scenes: FormList
   in the list's column, FormSide in the side box under its head (Folders': the whole screen, its groups' titles
   among its rows) ---- */
typedef struct {
    GuiPlace place;
    GuiRect left[UI_FORM_LIST_ITEMS], right[UI_FORM_SIDE_ITEMS];            /* FormList's rectangles, FormSide's */
    UiList left_lists[UI_FORM_LIST_ITEMS], right_lists[UI_FORM_SIDE_ITEMS]; /* the row each shows first, its focus */
    GuiRect info; /* the right box's foot, for a line about its rows */
} GuiFormBoxes;
/* the group the groups' list shows open, -1 for none: the one being edited, App for its folders */
static int form_group(const App* app) {
    if (app->form == GUI_FORM_GROUP || app->form == GUI_FORM_USERS) return app->group;
    return app->form == GUI_FORM_APP || app->form == GUI_FORM_FOLDERS ? 0 : -1;
}
/* the columns a form's row label takes, the widest's */
static int form_labels(const GuiEditor* e) {
    char label[64];
    int r, labels = 8;
    for (r = 0; r < gui_editor_rows(e); ++r) {
        gui_editor_label(e, r, label, sizeof label);
        labels = SDL_max(labels, gui_text_cols(label));
    }
    return labels;
}
/* Folders': the widest name of the rows with a folder; Add a folder and the last go long */
static int folder_labels(const App* app) {
    char label[256];
    int r, labels = 0;
    for (r = 0; r < folder_rows(app); ++r)
        if (folder_row_path(app, r)) {
            gui_editor_label(&app->editor, r, label, sizeof label);
            labels = SDL_max(labels, gui_text_cols(label));
        }
    return labels;
}
/* the line at the foot of a group's rows, about what they hold, NULL on the screens with none */
static const char* form_note(const App* app) {
    if (app->form == GUI_FORM_APP) return app->places_note;
    if (app->form == GUI_FORM_FOLDERS) { /* the focused folder whole, its row cut to fit */
        int at, group, kind = folder_row(app, app->editor.row, &at, &group);
        return kind == GUI_ROW_PLACE ? app->place_line[at]
               : kind == GUI_ROW_ADD ? snx_tr("Each library's Extras are listed after its Main")
               : kind >= 0           ? folder_row_path(app, app->editor.row)
                                     : snx_tr("Data, Extractions and the recompiled library's Main");
    }
    if (app->form != GUI_FORM_SETTINGS && app->form != GUI_FORM_GROUP) return NULL;
    return app->settings.count > app->settings.own ? snx_tr("For the next runs. [default]: as the game's .ini says")
                                                  : snx_tr("The runtime listed no settings");
}
/* the right box's rows: on the groups, the focused group's, to see before opening it, made in peek; else the
   editor's */
static const GuiEditor* form_rows(App* app, GuiEditor* peek) {
    int g = app->editor.row;
    if (app->form != GUI_FORM_SETTINGS) return &app->editor;
    memset(peek, 0, sizeof *peek);
    if (g == 0) {
        app_values(app);
        gui_editor_form(peek, app_keys, GUI_APP_ROWS, &app->app_form, NULL);
    } else if (users_group(app, g))
        gui_editor_form(peek, app->users_keys, users_fill(app), &app->users_form, NULL);
    else {
        int n = player_rows(app, g, 0, app->peek_rows), r; /* Controllers: every player's */
        for (r = 0; r < n; ++r) app->peek_keys[r] = app->settings.keys[app->peek_rows[r]];
        if (n)
            gui_editor_form(peek, app->peek_keys, n, app->settings.values, NULL);
        else
            gui_editor_form(peek, app->settings.keys + app->group_first[g], app->group_rows[g], app->settings.values,
                            NULL);
    }
    peek->flat = true;
    peek->row = -1;
    return peek;
}
static void form_boxes(App* app, GuiFormBoxes* f) {
    const char* note = form_note(app);
    char keys[256];
    int rows, top, count;
    int theme = app->form == GUI_FORM_THEME, width, height, g;
    int longest = 0;
    area(app, &width, &height);
    for (g = 0; g < app->groups; ++g) longest = SDL_max(longest, gui_text_cols(snx_tr(app->group_keys[g].key)));
    place_screen(app, &f->place, shown_keys(app, keys, sizeof keys),
                 theme ? width / cell_w(app)
                       : longest + 2 + gui_text_cols(app->theme.marker) + gui_text_cols(app->theme.cursor),
                 GUI_SHOWS_TABS);
    if (app->form == GUI_FORM_FOLDERS) { /* the whole screen, as a load's: its boxes the groups, none beside them */
        int top = f->place.list.y,
            bottom = SDL_max(f->place.list.y + f->place.list.h,
                             SDL_max(f->place.side.y + f->place.side.h, f->place.status.y + f->place.status.h));
        f->place.side = gui_rect(f->place.head.x, top, f->place.head.w, bottom - top);
        f->place.list = gui_rect(f->place.head.x, top, 0, 0);
        f->place.status = gui_rect(0, 0, 0, 0);
    } else if (!theme && app->form != GUI_FORM_SETTINGS && (!f->place.side.w || !f->place.side.h)) {
        f->place.side = f->place.list; /* no side box: the open group's rows the whole column, the groups out */
        f->place.list = gui_rect(f->place.list.x, f->place.list.y, 0, 0);
    }
    rows = note ? wrap_rows(app, note, inner(app, f->place.side).w / cell_w(app), 0) : 1; /* the note whole */
    f->info = gui_rect(f->place.side.x, f->place.side.y + f->place.side.h - rows * cell_h(app) - 4 * app->scale,
                       f->place.side.w, rows * cell_h(app) + 2 * app->scale);
    top = f->place.side.y + (app->form == GUI_FORM_FOLDERS ? 2 * app->scale : head_h(app) + 4 * app->scale);
    ui_form_list_layout(f->place.list, app->scale, f->left);
    ui_form_side_layout(gui_rect(f->place.side.x + cell_w(app), top, f->place.side.w - 2 * cell_w(app),
                                 f->info.y - top - 2 * app->scale),
                        app->scale, f->right);
    memset(f->left_lists, 0, sizeof f->left_lists);
    memset(f->right_lists, 0, sizeof f->right_lists);
    /* the list the keys move in follows its focus; the groups' the open one, which has the focus's look alone */
    count = theme ? gui_editor_rows(&app->editor) : app->groups;
    g = theme || app->form == GUI_FORM_SETTINGS ? app->editor.row : form_group(app);
    f->left_lists[UI_FORM_LIST_ROWS].focus = g >= 0 ? (size_t)g : SIZE_MAX;
    if (g >= 0)
        ui_form_list_follow(f->left, app->scale, UI_FORM_LIST_ROWS, (size_t)count, &f->left_lists[UI_FORM_LIST_ROWS]);
    if (!theme && app->form != GUI_FORM_SETTINGS) f->left_lists[UI_FORM_LIST_ROWS].focus = SIZE_MAX;
    if (theme || app->form == GUI_FORM_SETTINGS) { /* nothing, or the rows to see before opening their group */
        f->right_lists[UI_FORM_SIDE_ROWS].focus = SIZE_MAX;
        return;
    }
    count = app->form == GUI_FORM_FOLDERS ? folder_entries(app) : gui_editor_rows(&app->editor);
    f->right_lists[UI_FORM_SIDE_ROWS].focus =
        (size_t)(app->form == GUI_FORM_FOLDERS ? folder_entry(app, app->editor.row) : app->editor.row);
    ui_form_side_follow(f->right, app->scale, UI_FORM_SIDE_ROWS, (size_t)count, &f->right_lists[UI_FORM_SIDE_ROWS]);
}
/* the columns of text a row width pixels wide has past its marker's half cell (ui_draw_row's) */
static int form_room(const App* app, int width) {
    return (ui_row_box(app, gui_rect(0, 0, width, 0)).w - cell_w(app) * 3 / 2) / cell_w(app);
}
/* a row of a form as its box shows it, its marker first: its label, the value past the widest label (labels
   columns); one being typed ends where the typing is, its start giving way, in room columns */
static void form_line(App* app, const GuiEditor* e, int r, int labels, int room, char* line, size_t size) {
    char label[256], value[400];
    const GuiThemeKey* k = gui_editor_key_of(e, r);
    const char* path;
    int typing = e->typing && r == e->row, pad;
    size_t n;
    gui_editor_label(e, r, label, sizeof label);
    if (typing)
        snprintf(value, sizeof value, "%s_", e->text);
    else if (k && e->object == (char*)app->settings.values) { /* a setting: a pick, else its default and [default] */
        int row = e->keys == app->view_keys   ? app->view_rows[k - app->view_keys]
                  : e->keys == app->peek_keys ? app->peek_rows[k - app->peek_keys]
                                              : (int)(k - app->settings.keys);
        gui_settings_shown(&app->settings, row, value, sizeof value);
        if (player_key(app->settings.values[row].key, "device") && !app->settings.values[row].text[0])
            snprintf(value, sizeof value, "%s", snx_tr("Any [default]")); /* the next to connect */
        else if (player_key(app->settings.values[row].key, "map")) /* it opens the button map, as All's page does */
            snprintf(value, sizeof value, "%s",
                     app->settings.values[row].text[0] ? snx_tr("Its own >") : snx_tr("As All >"));
    } else
        gui_editor_value(e, r, value, sizeof value);
    if (k && e->keys == app->folder_keys && (path = folder_row_path(app, r)) != NULL) {
        /* its folder, cut in its middle (whole under the boxes), then what it is */
        char state[64], cut[300];
        int fit;
        snprintf(state, sizeof state, "%s", value);
        fit = room - gui_text_cols(app->theme.marker) - labels - 1 - (state[0] ? gui_text_cols(state) + 2 : 0);
        gui_path_fit(path, SDL_max(fit, 4), cut, sizeof cut);
        snprintf(value, sizeof value, "%s%s%s", cut, state[0] ? "  " : "", state);
    }
    pad = SDL_max(labels - gui_text_cols(label), 0); /* the labels' columns, not their bytes */
    if (value[0])
        snprintf(line, size, "%s%s%*s %s", app->theme.marker, label, pad, "", value);
    else /* no value to line up: padding would make a short row too long for its box, and slide */
        snprintf(line, size, "%s%s", app->theme.marker, label);
    n = (size_t)gui_text_cols(line);
    if (typing && room > 4 && n > (size_t)room) { /* the typed end in view */
        const char* tail = gui_cols_skip(value, (int)(n - (size_t)room + 3));
        snprintf(line, size, "%s%s%*s ...%s", app->theme.marker, label, pad, "", tail);
    }
}
/* row r of e as a list of ui.txt asks for it, in a row width pixels wide: false past its last */
static bool form_row_info(App* app, const GuiEditor* e, size_t r, int labels, int width, UiRowInfo* info) {
    char* line = app->row_text;
    if (r >= (size_t)gui_editor_rows(e)) return false;
    form_line(app, e, (int)r, labels, form_room(app, width), line, sizeof app->row_text);
    info->label = line + strlen(app->theme.marker); /* ui_draw_row puts the marker */
    return true;
}
/* FormList's row: the theme editor's keys; else the groups, the one open beside them in the focus's look */
bool ui_form_list_rows_get_row_info(App* app, size_t row, int width, UiRowInfo* info) {
    if (app->form == GUI_FORM_THEME)
        return form_row_info(app, &app->editor, row, form_labels(&app->editor), width, info);
    if (row >= (size_t)app->groups) return false;
    info->label = snx_tr(app->group_keys[row].key);
    if (app->form != GUI_FORM_SETTINGS && (int)row == form_group(app)) info->attr = UI_ATTR_OPEN;
    return true;
}
/* FormSide's row: the open or the focused group's; Folders' groups each under its title, the last after them; none
   in the theme editor, whose preview is there */
bool ui_form_side_rows_get_row_info(App* app, size_t row, int width, UiRowInfo* info) {
    GuiEditor peek;
    const GuiEditor* e;
    int r, group;
    if (app->form == GUI_FORM_THEME) return false;
    if (app->form != GUI_FORM_FOLDERS) {
        e = form_rows(app, &peek);
        return form_row_info(app, e, row, form_labels(e), width, info);
    }
    if (row >= (size_t)folder_entries(app)) return false;
    if ((r = folder_entry_row(app, (int)row, &group)) < 0) { /* a group's title, over its rows */
        info->label = snx_tr(group_titles[group]);
        info->attr = UI_ATTR_TITLE;
        return true;
    }
    return form_row_info(app, &app->editor, (size_t)r, folder_labels(app), width, info);
}
/* what the bar says of the focused row: the note of the last key, else the row's help */
static const char* form_about(const App* app, SDL_Color* color) {
    const GuiEditor* e = &app->editor;
    const GuiThemeKey* k = gui_editor_key_of(e, e->row);
    *color = rgb(e->note[0] ? (e->leaving ? app->theme.failure : app->theme.focus) : app->theme.ink);
    if (e->note[0]) return e->note;
    if (k) return snx_tr(k->about);
    if (app->form == GUI_FORM_FOLDERS) return snx_tr("Each folder back to its default. What is in them stays");
    return e->object                     ? ""
           : e->row == GUI_EDITOR_FOLDER ? snx_tr("The folder of themes/ it is saved in")
                                         : snx_tr("Ctrl+S saves too");
}
/* a colour swatch framed in soft, so a page-coloured one shows */
static void swatch(App* app, GuiRect r, GuiRgb c) {
    fill_rect(app, r, app->theme.soft);
    fill_rect(app, gui_rect(r.x + app->scale, r.y + app->scale, r.w - 2 * app->scale, r.h - 2 * app->scale), c);
}
/* the theme editor's colour rows: the colour after the row's text, in its box while it fits there */
static void form_swatches(App* app, const GuiFormBoxes* f) {
    const GuiEditor* e = &app->editor;
    const UiList* l = &f->left_lists[UI_FORM_LIST_ROWS];
    int labels = form_labels(e), cw = cell_w(app), s = app->scale;
    size_t i, n = ui_form_list_shown(f->left, s, UI_FORM_LIST_ROWS);
    for (i = 0; i < n && l->first + i < (size_t)gui_editor_rows(e); ++i) {
        int r = (int)(l->first + i), x;
        const GuiThemeKey* k = gui_editor_key_of(e, r);
        GuiRect row = ui_form_list_row(f->left, s, UI_FORM_LIST_ROWS, i), b = ui_row_box(app, row);
        char line[700];
        if (!k || k->type != GUI_KEY_COLOUR) continue;
        form_line(app, e, r, labels, form_room(app, row.w), line, sizeof line);
        x = b.x + cw / 2 + gui_text_cols(line) * cw + cw;
        if (x + 2 * cw <= b.x + b.w - cw) /* a cell short of the box's end, where its arrows go */
            swatch(app, gui_rect(x, ui_text_y(app, b) + 2 * s, 2 * cw, cell_h(app) - 5 * s),
                   *(const GuiRgb*)((const char*)(e->object ? e->object : (const char*)&e->theme) + k->at));
    }
}
/* Folders' groups, a box each as far as it shows: from its title down to its last row shown, half a cell wider
   either side; drawn under them */
static void folder_frames(App* app, const GuiFormBoxes* f) {
    const UiList* l = &f->right_lists[UI_FORM_SIDE_ROWS];
    size_t i, n = ui_form_side_shown(f->right, app->scale, UI_FORM_SIDE_ROWS);
    int g, group, cw = cell_w(app);
    for (g = 0; g < GUI_FOLDER_GROUPS; ++g) {
        GuiRect a = {0}, z = {0}, box;
        for (i = 0; i < n && l->first + i < (size_t)folder_entries(app); ++i) {
            folder_entry_row(app, (int)(l->first + i), &group);
            if (group != g) continue;
            z = ui_form_side_row(f->right, app->scale, UI_FORM_SIDE_ROWS, i);
            if (!a.h) a = z;
        }
        if (!a.h) continue;
        box = gui_rect(a.x - cw / 2, a.y, a.w + cw, z.y + z.h - a.y);
        if (app->theme.panels == GUI_PANELS_FILL) fill_rect(app, box, app->theme.page);
        outline(app, box, app->theme.dim);
    }
}
static void draw_picker(App* app);
static void draw_form(App* app) {
    GuiFormBoxes f;
    char keys[256], line[1100];
    const char* about;
    SDL_Color color;
    int theme = app->form == GUI_FORM_THEME;
    form_boxes(app, &f);
    chrome(app, &f.place);
    ui_form_list_draw(app, f.left, app->scale, f.left_lists);
    if (theme) { /* the preview, 16 by 10, and what switches it */
        GuiRect p = inner(app, f.place.side);
        int w = SDL_min(p.w, (f.info.y - f.place.side.y - 2 * cell_h(app)) * 16 / 10), y;
        form_swatches(app, &f);
        y = pane(app, f.place.side, snx_tr("Preview"));
        if (w > 0 && preview_texture(app)) {
            SDL_FRect box = {(float)(p.x + (p.w - w) / 2), (float)y, (float)w, (float)(w * 10 / 16)};
            SDL_RenderTexture(app->renderer, app->preview, NULL, &box);
            text_in(app, p, p.x, y + w * 10 / 16 + 2 * app->scale,
                    app->editor.preview != GUI_SCREEN_MENU ? snx_tr("A run (Ctrl+P: the menu)")
                                                           : snx_tr("The menu (Ctrl+P: a run)"),
                    rgb(app->theme.soft));
        }
        if (w > 0) y += w * 10 / 16 + 2 * app->scale + cell_h(app);
        snprintf(line, sizeof line, snx_tr("Saves in %s"),
                 app->themes_dirs[0] ? app->themes_dirs[0] : snx_tr("(no themes folder)"));
        wrap_in(app, gui_rect(p.x, y, p.w, f.place.side.y + f.place.side.h - y), p.x, y, line, 1, rgb(app->theme.soft));
    } else {
        int g = app->form == GUI_FORM_SETTINGS ? app->editor.row : app->group;
        if (app->form == GUI_FORM_FOLDERS) /* its groups' titles are in its rows */
            folder_frames(app, &f);
        else
            pane(app, f.place.side,
                 app->form == GUI_FORM_GROUP && app->page >= 0 ? snx_tr(app->settings.keys[app->page].key)
                                                               : snx_tr(app->group_keys[g].key));
        ui_form_side_draw(app, f.right, app->scale, f.right_lists);
        if (form_note(app))
            wrap_in(app, inner(app, f.info), f.info.x + cell_w(app), f.info.y, form_note(app), 0, rgb(app->theme.soft));
    }
    draw_picker(app);
    about = form_about(app, &color);
    foot(app, &f.place, about, color, shown_keys(app, keys, sizeof keys));
}
/* ---- the colour picker: over the theme editor's keys, so the preview shows the colour as it changes;
   a bar per channel, then the colour typed beside it and the one it had ---- */
static void picker_place(App* app, GuiPicker* p) {
    int s = app->scale, cw = cell_w(app), item = cell_h(app) + 6 * s, x0, x1, top, width, height, w, h, i;
    GuiFormBoxes f;
    area(app, &width, &height);
    form_boxes(app, &f);
    x0 = f.place.list.x;
    x1 = f.place.list.x + f.place.list.w;
    top = head_h(app) + 2 * s; /* under the pane's head */
    w = SDL_min(x1 - x0, 46 * cw);
    h = top + GUI_PICK_ROWS * item + 4 * s;
    p->box = gui_rect(x0 + (x1 - x0 - w) / 2, SDL_max(0, (height - h) / 2), w, h);
    for (i = 0; i < GUI_PICK_ROWS; ++i)
        p->rows[i] = gui_rect(p->box.x + cw, p->box.y + top + i * item, p->box.w - 2 * cw, item);
    for (i = 0; i < 3; ++i) /* the channel's letter before it, its value after it */
        p->bars[i] = gui_rect(p->rows[i].x + 3 * cw, p->rows[i].y + 3 * s, p->rows[i].w - 8 * cw, item - 6 * s);
}
/* the value of a bar's strip x pixels in: a strip is a pixel at 1x, the first 0 and the last 255 (a bar is whole
   half cells, so its inside is whole strips) */
static int strip_value(const App* app, GuiRect b, int x) {
    int s = app->scale, w = b.w - 2 * s;
    x = x < 0 ? 0 : x >= w ? w - 1 : x;
    return w > s ? x / s * s * 255 / (w - s) : 0;
}
static uint8_t* channel(GuiRgb* c, int i) { return i == GUI_PICK_RED ? &c->r : i == GUI_PICK_GREEN ? &c->g : &c->b; }
static void draw_picker(App* app) {
    static const char* const names[3] = {"R", "G", "B"};
    GuiEditor* e = &app->editor;
    const GuiTheme* t = &app->theme;
    const GuiThemeKey* k = gui_editor_key_of(e, e->row);
    GuiRgb *c = gui_editor_colour(e), shade;
    GuiPicker p;
    char line[96];
    int i, x, was, s = app->scale, cw = cell_w(app);
    if (!e->picking || !c) return;
    picker_place(app, &p);
    snprintf(line, sizeof line, "[%s] %s", k->section, k->key);
    over_box(app, p.box, line);
    for (i = 0; i < GUI_PICK_ROWS; ++i) {
        GuiRect r = p.rows[i];
        int focused = e->channel == i, band = focused && t->focus_bar, y = r.y + 3 * s;
        SDL_Color ink = rgb(band ? t->page : focused ? t->focus : t->ink);
        if (band) fill_rect(app, r, t->focus);
        if (i == GUI_PICK_HEX) { /* the colour as typed, the one it is now and the one it had */
            if (e->typing)
                snprintf(line, sizeof line, "%s_", e->text);
            else
                gui_editor_value(e, e->row, line, sizeof line);
            text_in(app, r, r.x + cw / 2, y, line, ink);
            x = r.x + cw / 2 + 10 * cw;
            was = gui_text_cols(snx_tr("was"));
            if (x + 4 * cw <= r.x + r.w) swatch(app, gui_rect(x, y, 4 * cw, cell_h(app)), *c);
            if (x + (10 + was) * cw <= r.x + r.w) {
                text_in(app, r, x + 5 * cw, y, snx_tr("was"), band ? ink : rgb(t->soft));
                swatch(app, gui_rect(x + (6 + was) * cw, y, 4 * cw, cell_h(app)), e->was);
            }
            continue;
        }
        text_in(app, r, r.x + cw / 2, y, names[i], ink);
        snprintf(line, sizeof line, "%3d", *channel(c, i));
        text_in(app, r, p.bars[i].x + p.bars[i].w + cw, y, line, ink);
        { /* the colour the channel's every value would give, framed, and a mark where it stands */
            GuiRect b = p.bars[i];
            int w = b.w - 2 * s, at;
            if (w <= s) continue;
            fill_rect(app, b, t->soft);
            shade = *c;
            for (x = 0; x < w; x += s) {
                *channel(&shade, i) = (uint8_t)strip_value(app, b, x);
                fill_rect(app, gui_rect(b.x + s + x, b.y + s, s, b.h - 2 * s), shade);
            }
            at = b.x + s + *channel(c, i) * (w - s) / 255 / s * s + s / 2;
            fill_rect(app, gui_rect(at - s, b.y - 2 * s, 3 * s, b.h + 4 * s), band ? t->page : t->ink);
            fill_rect(app, gui_rect(at, b.y - s, s, b.h + 2 * s), band ? t->focus : t->page);
        }
    }
}
static void picker_set(App* app, const GuiPicker* p, int i, float x) {
    gui_editor_pick(&app->editor, i, strip_value(app, p->bars[i], (int)SDL_floorf(x) - (p->bars[i].x + app->scale)));
}
/* the pointer on the picker, at pixel x, y. A press on a bar's row sets its channel there and holds it, the pointer's
   moves dragging it until the press lifts; on #RRGGBB it types it; off the box it keeps the colour */
static void picker_pointer(App* app, float x, float y, bool press) {
    GuiEditor* e = &app->editor;
    GuiPicker p;
    int i;
    picker_place(app, &p);
    if (!press) {
        if (app->pick_press > 0 && app->pick_press <= 3) picker_set(app, &p, app->pick_press - 1, x);
        return;
    }
    for (i = 0; i < GUI_PICK_ROWS; ++i)
        if (x >= (float)p.box.x && x < (float)(p.box.x + p.box.w) && y >= (float)p.rows[i].y &&
            y < (float)(p.rows[i].y + p.rows[i].h))
            break;
    app->pick_press = i < 3 ? i + 1 : GUI_PICK_ROWS + 1;
    if (e->typing && i != GUI_PICK_HEX) editor_key(app, GUI_CANCEL);
    if (i < 3)
        picker_set(app, &p, i, x);
    else if (i == GUI_PICK_HEX && !e->typing) {
        e->channel = GUI_PICK_HEX;
        editor_key(app, GUI_SELECT);
    } else if (i == GUI_PICK_ROWS && (x < (float)p.box.x || x >= (float)(p.box.x + p.box.w) || y < (float)p.box.y ||
                                      y >= (float)(p.box.y + p.box.h)))
        gui_editor_pick_end(e, true);
}
/* a row of a form under the pointer: focused, or on the focused one Select, as a key would */
static void form_row_pointer(App* app, int r) {
    if (r == app->editor.row && !app->editor.typing)
        editor_key(app, GUI_SELECT);
    else if (r != app->editor.row) {
        if (app->editor.typing) editor_key(app, GUI_CANCEL);
        app->editor.row = r;
    }
}
/* FormList's row under the pointer. The theme editor's keys and the groups take the focus as it goes over them, a
   click on the focused one selecting it; on a group's screen, whose keys move in its rows, a click opens another group
   from its row, as Enter there */
void ui_form_list_rows_row_action_cb(App* app, size_t row, uint32_t hit) {
    int r = (int)row;
    if (app->form == GUI_FORM_THEME || app->form == GUI_FORM_SETTINGS) {
        if (hit & UI_HIT_CLICK)
            form_row_pointer(app, r);
        else if (!app->editor.typing)
            app->editor.row = r;
        return;
    }
    if (!(hit & UI_HIT_CLICK)) return;
    if (app->editor.typing) editor_key(app, GUI_CANCEL);
    keep_settings(app);
    leave_editor(app);
    settings_top(app, r);
    open_group(app, r);
}
/* FormSide's row under the pointer: where the keys move in its rows, the focus on it as it goes over it and a click on
   the focused one selecting it; on the groups a click opens the focused group, on that row. Folders' groups' titles
   take nothing */
void ui_form_side_rows_row_action_cb(App* app, size_t row, uint32_t hit) {
    int r = (int)row, group;
    if (app->form == GUI_FORM_FOLDERS && (r = folder_entry_row(app, r, &group)) < 0) return;
    if (!(hit & UI_HIT_CLICK)) {
        if (app->form != GUI_FORM_SETTINGS && !app->editor.typing) app->editor.row = r;
        return;
    }
    if (app->form == GUI_FORM_SETTINGS) {
        open_group(app, app->editor.row);
        app->editor.row = SDL_min(r, gui_editor_rows(&app->editor) - 1);
        return;
    }
    form_row_pointer(app, r);
}
/* the pointer at pixel x, y on a form's rows, either list's, clicked (event UI_HIT_CLICK) or not (0) */
static void form_rows_pointer(App* app, const GuiFormBoxes* f, float x, float y, uint32_t event) {
    int item;
    size_t row;
    uint32_t hit = ui_form_list_hit(app, f->left, app->scale, f->left_lists, (int)x, (int)y, event, &item, &row);
    if (hit) {
        ui_form_list_act(app, item, row, hit);
        return;
    }
    hit = ui_form_side_hit(app, f->right, app->scale, f->right_lists, (int)x, (int)y, event, &item, &row);
    if (hit) ui_form_side_act(app, item, row, hit);
}
/* the pointer over a form, not pressed: a row of the list the keys move in takes the focus, as on the menu's screens
   and the setup's pages; the other list, a tab and Back wait for a click */
static void form_hover(App* app, float x, float y) {
    GuiFormBoxes f;
    form_boxes(app, &f);
    form_rows_pointer(app, &f, x, y, 0);
}
static void go_tab(App* app, int tab);
/* a click on a form: a tab; a row of the list or of the right box; the theme editor's preview switches */
static void form_pointer(App* app, float x, float y) {
    GuiFormBoxes f;
    int i;
    if ((i = tab_hit(app, x, y)) >= 0) {
        go_tab(app, i);
        return;
    }
    if (user_hit(app, x, y)) {
        users_open(app);
        return;
    }
    form_boxes(app, &f);
    if (app->form == GUI_FORM_THEME && x >= (float)f.place.side.x && x < (float)(f.place.side.x + f.place.side.w) &&
        y >= (float)f.place.side.y && y < (float)(f.place.side.y + f.place.side.h)) {
        app->editor.preview = app->editor.preview == GUI_SCREEN_MENU ? GUI_SCREEN_COMPILING : GUI_SCREEN_MENU;
        app->preview_since = 0;
        return;
    }
    form_rows_pointer(app, &f, x, y, UI_HIT_CLICK);
}
/* ---- the setup a first start goes through (common/utilities/setup.h), in place of the menu: a page at a time, the
   notices of what older versions did, then each step setup.ini has left. Its boxes say what the page is about, its
   question and choices come as the theme's options-dialog places them, its tip in the foot. What a step does is done
   as it is taken; setup.ini is written once the last one is ---- */
#ifdef SDL_PLATFORM_ANDROID
#define SAY_SETUP        SNX_TR_KEYS("Tap: choose. Back: go back.")
#define SAY_SETUP_FIRST  SAY_SETUP
#define SAY_SETUP_TYPING SNX_TR_KEYS("Type the nickname. Back: go back.")
#define SAY_SETUP_NOTICE SAY_SETUP
#else
#define SAY_SETUP        SNX_TR_KEYS("Up/Down: choose. Enter: next. ESC: back.")
#define SAY_SETUP_FIRST  SNX_TR_KEYS("Up/Down: choose. Enter: next. ESC: exit.")
#define SAY_SETUP_TYPING SNX_TR_KEYS("Type the nickname. Enter: done. ESC: back.")
#define SAY_SETUP_NOTICE SNX_TR_KEYS("Left/Right: choose. Enter: take it. ESC: exit.")
#endif
#ifdef SDL_PLATFORM_ANDROID
#define SAY_SETUP_LOOK       SAY_SETUP
#define SAY_SETUP_LOOK_FIRST SAY_SETUP
#define SAY_SETUP_SIZE       SAY_SETUP
#define SAY_SETUP_SIZE_FIRST SAY_SETUP
#define SAY_SETUP_ON         SAY_SETUP
#define SAY_SETUP_ON_FIRST   SAY_SETUP
#else /* the Appearance page's, as Settings > App's rows: Enter changes Language and Theme, opens UI scale's box */
#define SAY_SETUP_LOOK       SNX_TR_KEYS("Up/Down: row. Left/Right: change. Enter: change. ESC: back.")
#define SAY_SETUP_LOOK_FIRST SNX_TR_KEYS("Up/Down: row. Left/Right: change. Enter: change. ESC: exit.")
#define SAY_SETUP_SIZE       SNX_TR_KEYS("Up/Down: row. Enter: open. ESC: back.")
#define SAY_SETUP_SIZE_FIRST SNX_TR_KEYS("Up/Down: row. Enter: open. ESC: exit.")
#define SAY_SETUP_ON         SNX_TR_KEYS("Up/Down: row. Enter: next. ESC: back.")
#define SAY_SETUP_ON_FIRST   SNX_TR_KEYS("Up/Down: row. Enter: next. ESC: exit.")
#endif
#define GUI_SETUP_CHOICES  3 /* a page's, at most: Folders' and Keys' */
static void open_files(App* app);
static void left_behind(App* app);
static void ask_key(App* app, GuiKey key);
static bool menu_key_of(const SDL_KeyboardEvent* k, GuiKey* key);
#define GUI_CLOSING_MS 500 /* the screen of what the menu keeps, as it closes */
/* the menu closing: on a computer the short screen of what it keeps first, then out; on a phone at once */
static void closing_start(App* app) {
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
    app->menu.quit = true;
#else
    if (app->closing_ms) app->menu.quit = true; /* closed again meanwhile: out now */
    app->closing_ms = SDL_max(now_ms(app), 1);
#endif
}
static int setup_page_of(const App* app) { return app->setup_pages[app->setup_page]; }
static int setup_steps(const App* app) { return app->setup_count - (app->setup_pages[0] == SETUP_NOTICE); }
/* a portable copy's folder beside the program (snx_portable_dir), and whether it is there: owned */
static char* setup_portable(const App* app, bool* there) {
    char* portable = snx_portable_dir(app->drivers);
    *there = portable && snx_is_dir(portable);
    return portable;
}
/* users.ini as the user's page finds it: there or not, its users or why it is no list of them */
static void setup_users_read(App* app) {
    char* path = app->users_dir ? snx_path_join(app->users_dir, "users.ini") : NULL;
    app->setup_written = path && snx_is_file(path);
    app->setup_users = 0;
    app->setup_why[0] = 0;
    if (app->setup_written)
        app->setup_users = snx_users_load(app->users_dir, 1, app->setup_why, sizeof app->setup_why) ? -1 : g_user_count;
    users_close(app); /* their pictures drawn again for the page */
    free(path);
}
/* the nickname of the new first user typed, its uid the fixed one while games have saves of it, else random */
static void setup_type(App* app) {
    app->setup_saves = snx_setup_first_uid(app->games_dir, app->setup_uid);
    if (app->setup_saves < 0) {
        app->setup_failed = true;
        snprintf(app->message, sizeof app->message, "%s",
                 snx_tr("The system's random number generator failed: no uid for a new user"));
        say_failure(app, app->message);
        return;
    }
    app->setup_typing = true;
    snprintf(app->setup_name, sizeof app->setup_name, "Player");
    app->setup_choice.count = 1;
    app->setup_choice.focus = 0;
    SDL_StartTextInput(app->window);
}
/* the notices' choices: Next (OK on the last), Back before it past the first; the focus on Back while back says so and
   there is one, else on Next */
static void notice_choices(App* app, bool back) {
    app->setup_choice.count = app->setup_top > 0 ? 2 : 1;
    app->setup_choice.focus = back && app->setup_top > 0 ? 0 : app->setup_choice.count - 1;
}
/* the page shown from its start: its choices, the one in focus the one in use */
static void setup_enter(App* app) {
    int page = setup_page_of(app);
    bool there;
    char *portable, *prod;
    app->setup_top = 0;
    app->setup_typing = false;
    app->setup_keys = false;
    app->setup_choice.focus = 0;
    SDL_StopTextInput(app->window);
    switch (page) {
        case SNX_SETUP_LANGUAGE: /* the Appearance's rows, the first in focus, and Next */
            app->setup_choice.count = 1;
            app->setup_row = GUI_APPEARANCE_LANGUAGE;
            break;
        case SNX_SETUP_INSTALL:
            app->setup_choice.count = 2;
            portable = setup_portable(app, &there);
            app->setup_choice.focus = there;
            free(portable);
            break;
        case SNX_SETUP_FOLDERS: app->setup_choice.count = 3; break;
        case SNX_SETUP_KEYS:
            prod = snx_keys_found(app->data_dir, "prod.keys");
            app->setup_choice.count = prod ? 3 : 2;
            app->setup_choice.focus = prod ? 0 : 1; /* these, or later */
            free(prod);
            break;
        case SNX_SETUP_USER:
            setup_users_read(app);
            app->setup_saves = snx_setup_first_saves(app->games_dir);
            app->setup_choice.count = 2; /* Keep, Overwrite */
            if (!app->setup_written) setup_type(app);
            break;
        default: notice_choices(app, false); break; /* the notices' Next or OK, Back past the first */
    }
}
/* the setup at the start, unless --debug or --skip twice leave it out: the pages setup.ini has left, or none */
static void setup_start(App* app) {
    char why[512], *file = snx_setup_file(app->home_dir ? app->home_dir : app->data_dir);
    int i;
    app->setup_on = false;
    app->setup_count = app->setup_page = 0;
    if (snx_setup_skip || !file) {
        free(file);
        return;
    }
    if (snx_setup_read(file, &app->setup, why, sizeof why))
        snprintf(app->message, sizeof app->message, snx_tr("%s: the setup asks again"), why);
    free(file);
    for (i = 0; i < SNX_NOTICES && !snx_setup_shows(&app->setup, i); ++i) {}
    if (i < SNX_NOTICES) app->setup_pages[app->setup_count++] = SETUP_NOTICE;
    for (i = 0; i < SNX_SETUP_STEPS; ++i)
        if (snx_setup_asks(&app->setup, i) && snx_setup_here(i)) app->setup_pages[app->setup_count++] = i;
    app->setup_on = app->setup_count > 0;
    app->setup_save = app->setup_moved = false;
    app->setup_language = app->language;
    if (app->setup_on) setup_enter(app);
}
/* the last page done: setup.ini says so, the language picked kept in app.ini, the menu with the user its head shows */
static void setup_finish(App* app) {
    char why[512], *file = snx_setup_file(app->home_dir ? app->home_dir : app->data_dir);
    app->setup_on = false;
    app->setup_typing = false;
    SDL_StopTextInput(app->window);
    users_close(app);
    app->message[0] = 0;
    users_read(app);
    if (app->setup_save) app_save(app);
    if (!file || snx_setup_done(file, &app->setup, why, sizeof why)) {
        snprintf(app->message, sizeof app->message, snx_tr("The setup was not kept, so it asks again: %s"),
                 file ? why : snx_tr("no folder for setup.ini"));
        say_failure(app, app->message);
    }
    app->setup_failed = false;
    if (app->setup_moved) { /* in the folders it has now */
        left_behind(app);
        app->left_shown = app->lefts > 0;
    }
    free(file);
    layout(app);
}
static void setup_next(App* app) {
    app->message[0] = 0;
    app->setup_failed = false;
    if (++app->setup_page >= app->setup_count)
        setup_finish(app);
    else
        setup_enter(app);
}
/* a failure on the page, which stays */
static void setup_fail(App* app, const char* text) {
    snprintf(app->message, sizeof app->message, "%s", text);
    app->setup_failed = true;
}
/* the data folder snx_data_dir names now, a portable copy's the setup just made: the menu's own files are its */
static void data_moved(App* app) {
    char* data = snx_data_dir(app->drivers);
    int i;
    if (!data) return;
    free(app->home_dir);
    app->home_dir = data;
    for (i = 0; i < GUI_PLACES; ++i) { /* the folders the other app.ini picked */
        free(app->place[i]);
        app->place[i] = NULL;
    }
    places_apply(app);
    app->setup_moved = true;
    for (i = 0; i < app->folder_count; ++i) free(app->folders[i]);
    app->folder_count = 0;
    free(app->settings_file);
    free(app->folders_file);
    free(app->app_file);
    free(app->run_log);
    free(app->users_dir);
    app->settings_file = app->folders_file = app->app_file = app->run_log = app->users_dir = NULL;
    gui_settings_free(&app->settings);
    open_files(app);
    space_start(app);
}
/* the new first user written, as it was typed */
static void setup_user_done(App* app) {
    char why[512], note[640];
    if (!snx_users_nickname_valid(app->setup_name)) {
        snprintf(note, sizeof note, snx_tr("%s is not a nickname: %s"), app->setup_name, snx_tr(SAY_SETUP_NICKNAME));
        setup_fail(app, note);
    } else if (snx_users_start(app->users_dir, app->setup_uid, app->setup_name, why, sizeof why)) {
        snprintf(note, sizeof note, snx_tr("No user written: %s"), why);
        setup_fail(app, note);
        say_failure(app, note);
    } else
        setup_next(app);
}
/* the folder of keys the picker gave (NULL: none, error why it could not open), installed where the menu keeps them */
static void setup_keys_from(App* app, const char* folder, const char* error) {
    char *dir = app->keys_path ? snx_dirname(app->keys_path) : NULL, why[1024], note[1200], shown[1024];
    int titles, ok = 0, failure = 1; /* none picked is no failure */
    app->setup_keys = false;
    if (!folder) {
        snprintf(note, sizeof note, "%s", error ? error : snx_tr("No folder was picked."));
        failure = error != NULL;
    } else if (!dir) /* snx_keys_dir's: the variables it reads are unset */
#ifdef _WIN32
        snprintf(note, sizeof note, "%s", snx_tr("Cannot install prod.keys: neither USERPROFILE nor HOME is set"));
#else
        snprintf(note, sizeof note, "%s", snx_tr("Cannot install prod.keys: HOME is not set"));
#endif
    else if (snx_keys_install(folder, dir, &titles, why, sizeof why))
        snprintf(note, sizeof note, snx_tr("Not installed: %s"), why);
    else {
        snx_home_short(dir, shown, sizeof shown);
        snprintf(note, sizeof note,
                 titles ? snx_tr("Installed prod.keys and title.keys in %s") : snx_tr("Installed prod.keys in %s"),
                 shown);
        ok = 1;
    }
    setup_enter(app); /* found now: Use these; else Choose folder again */
    if (ok) {
        snprintf(app->message, sizeof app->message, "%s", note);
        app->setup_failed = false;
    } else {
        app->setup_choice.focus = app->setup_choice.count == 3;
        setup_fail(app, note);
        if (failure) say_failure(app, note);
    }
    free(dir);
}
/* the focused choice of the page taken */
static int setup_notices(const App* app, int* list);
static void setup_take(App* app) {
    int page = setup_page_of(app), choice = app->setup_choice.focus;
    char note[1100], *portable, *prod, shown[1024];
    bool there;
    switch (page) {
        case SNX_SETUP_LANGUAGE: setup_next(app); return; /* its Next: the rows did what they do as they changed */
        case SNX_SETUP_INSTALL:
            portable = setup_portable(app, &there);
            snx_home_short(portable ? portable : "", shown, sizeof shown);
            if (!choice && there) {
                snprintf(note, sizeof note, snx_tr("This copy is portable: to install it, first move away %s"), shown);
                setup_fail(app, note);
            } else if (choice && !there && (!portable || snx_mkdirs(portable))) {
                snprintf(note, sizeof note, snx_tr("Could not make %s"), portable ? shown : "(?)");
                setup_fail(app, note);
                say_failure(app, note);
            } else {
                if (choice && !there) data_moved(app);
                setup_next(app);
            }
            free(portable);
            return;
        case SNX_SETUP_FOLDERS:
            if (!choice) {
                setup_next(app);
                return;
            }
            app->place_row = choice == 1 ? GUI_PLACE_GAMES : -1; /* the games' folder, or one more of dumps */
            open_folder_picker(app);
            return;
        case SNX_SETUP_KEYS:
            prod = snx_keys_found(app->data_dir, "prod.keys");
            if (choice == (prod ? 1 : 0)) {
                app->setup_keys = true;
                open_folder_picker(app);
            } else
                setup_next(app);
            free(prod);
            return;
        case SNX_SETUP_USER:
            if (app->setup_typing)
                setup_user_done(app);
            else if (choice)
                setup_type(app);
            else
                setup_next(app);
            return;
        case SETUP_NOTICE: { /* each notice read before the setup goes on, as terms are; Back the one before */
            int list[SNX_NOTICES];
            bool back = app->setup_choice.count == 2 && !app->setup_choice.focus;
            if (back)
                --app->setup_top;
            else if (app->setup_top < setup_notices(app, list) - 1)
                ++app->setup_top;
            else {
                setup_next(app);
                return;
            }
            notice_choices(app, back);
            return;
        }
        default: setup_next(app); return;
    }
}
/* back a page, out of the nickname to Keep and Overwrite, or on the first page what ESC does on the menu's */
static void setup_back(App* app) {
    app->message[0] = 0;
    app->setup_failed = false;
    if (app->setup_typing && app->setup_written) {
        app->setup_typing = false;
        SDL_StopTextInput(app->window);
        app->setup_choice.count = 2;
        app->setup_choice.focus = 1;
    } else if (app->setup_page > 0) {
        --app->setup_page;
        setup_enter(app);
    } else {
#ifdef SDL_PLATFORM_ANDROID
        gui_android_background(app->message, sizeof(app->message));
#else
        gui_confirm_open(&app->confirm, GUI_ASK_EXIT, false);
#endif
    }
}
/* the notices left, and how many */
static int setup_notices(const App* app, int* list) {
    int i, n = 0;
    for (i = 0; i < SNX_NOTICES; ++i)
        if (snx_setup_shows(&app->setup, i)) list[n++] = i;
    return n;
}
/* the Appearance's row changed a step either way, at once, as Settings > App's: the language among the menu's (Auto
   again on the system's, where the setup found Auto), the theme among the themes */
static void appearance_change(App* app, int row, int step) {
    if (row == GUI_APPEARANCE_LANGUAGE) {
        app->language = (snx_tr_language() + step + snx_gui_language_count) % snx_gui_language_count + 1;
        if (!app->setup_language && app->language - 1 == language_auto()) app->language = 0;
        language_apply(app);
    } else
        next_theme(app, step);
    app->setup_save = true;
}
static void setup_key(App* app, GuiKey key) {
    int step;
    if (setup_page_of(app) == SNX_SETUP_LANGUAGE) {
        switch (gui_appearance_key(&app->setup_row, key, &step)) {
            case GUI_APPEARANCE_CHANGE: appearance_change(app, app->setup_row, step); break;
            case GUI_APPEARANCE_SIZES: sizes_open(app); break;
            case GUI_APPEARANCE_ON: setup_next(app); break;
            case GUI_APPEARANCE_BACK: setup_back(app); break;
            default: break;
        }
        return;
    }
    switch (gui_choice_key(&app->setup_choice, key)) {
        case GUI_CHOICE_TAKE: setup_take(app); break;
        case GUI_CHOICE_BACK: setup_back(app); break;
        default: break;
    }
}
/* a character typed into the nickname, or the last one taken out */
static void setup_type_text(App* app, const char* text) {
    size_t n = strlen(app->setup_name);
    if (n + strlen(text) <= SNX_USERS_NICKNAME) memcpy(app->setup_name + n, text, strlen(text) + 1);
}
static void setup_type_back(App* app) {
    size_t n = strlen(app->setup_name);
    while (n && ((unsigned char)app->setup_name[n - 1] & 0xC0) == 0x80) --n; /* a UTF-8 letter's last bytes */
    if (n) app->setup_name[n - 1] = 0;
}
/* the keys and the input the setup takes while it shows: true for what it took */
static bool setup_event(App* app, const SDL_Event* e) {
    GuiKey key;
    if (e->type == SDL_EVENT_TEXT_INPUT) {
        if (app->setup_typing && !app->confirm.shown) setup_type_text(app, e->text.text);
        return true;
    }
    if (e->type != SDL_EVENT_KEY_DOWN) return false;
    if (app->confirm.shown) {
        if (!e->key.repeat && menu_key_of(&e->key, &key)) ask_key(app, key);
        return true;
    }
    if (app->sizes_shown) { /* UI scale's box, over the Appearance page */
        if (!e->key.repeat && menu_key_of(&e->key, &key)) sizes_key(app, key);
        return true;
    }
    if (app->setup_typing && e->key.key == SDLK_BACKSPACE) {
        setup_type_back(app);
        return true;
    }
    if (app->setup_typing && (e->key.key == SDLK_SPACE || (e->key.key >= SDLK_A && e->key.key <= SDLK_Z)))
        return true; /* letters come as text */
    if (menu_key_of(&e->key, &key)) setup_key(app, key);
    return true;
}
/* ---- its drawing ---- */
/* the page's choices into labels, own the room for those it makes; how many */
static int setup_labels(const App* app, const char** labels) {
    static const char* const install[] = {SNX_TR_NOOP("Installed"), SNX_TR_NOOP("Portable")};
    static const char* const folders[] = {SNX_TR_NOOP("Keep"), SNX_TR_NOOP("Change recompiled folder"),
                                          SNX_TR_NOOP("Add a folder")};
    static const char* const keys[] = {SNX_TR_NOOP("Use these"), SNX_TR_NOOP("Choose folder"), SNX_TR_NOOP("Later")};
    static const char* const user[] = {SNX_TR_NOOP("Keep"), SNX_TR_NOOP("Overwrite")};
    int i, page = setup_page_of(app), count = app->setup_choice.count, list[SNX_NOTICES];
    for (i = 0; i < count; ++i)
        labels[i] = page == SNX_SETUP_LANGUAGE                     ? snx_tr("Next")
                    : page == SNX_SETUP_INSTALL                    ? snx_tr(install[i])
                    : page == SNX_SETUP_FOLDERS                    ? snx_tr(folders[i])
                    : page == SNX_SETUP_KEYS                       ? snx_tr(keys[i + 3 - count])
                    : page == SNX_SETUP_USER && !app->setup_typing ? snx_tr(user[i])
                    : page == SETUP_NOTICE && i < count - 1        ? snx_tr("Back")
                    : page == SETUP_NOTICE && app->setup_top < setup_notices(app, list) - 1 ? snx_tr("Next")
                                                                                            : snx_tr("OK");
    return count;
}
static const char* setup_question(const App* app, char* out, size_t size) {
    switch (setup_page_of(app)) {
        case SETUP_NOTICE:
            return app->setup.version ? snx_tr("Changed since the last version") : snx_tr("Changed in older versions");
        case SNX_SETUP_LANGUAGE: return snx_tr("Appearance");
        case SNX_SETUP_INSTALL: return snx_tr("Install type");
        case SNX_SETUP_FOLDERS: return snx_tr("Folders");
        case SNX_SETUP_KEYS: return snx_tr("Keys");
        default:
            if (app->setup_typing)
                snprintf(out, size, snx_tr("Nickname: %s_"), app->setup_name);
            else if (app->setup_users < 0)
                snprintf(out, size, "%s", snx_tr("users.ini is no list of users"));
            else
                snprintf(out, size,
                         app->setup_users == 1 ? snx_tr("users.ini has %d user") : snx_tr("users.ini has %d users"),
                         app->setup_users);
            return out;
    }
}
static const char* setup_tip(const App* app) {
    switch (setup_page_of(app)) {
        case SETUP_NOTICE: return snx_tr("Tip: --debug skips the setup");
        case SNX_SETUP_LANGUAGE: return snx_tr("Tip: games keep their own language: Settings > System > language");
        case SNX_SETUP_INSTALL: return snx_tr("Tip: a portable copy is a folder you can copy");
        case SNX_SETUP_FOLDERS: return snx_tr("Tip: Settings > App > Library and Folders change them");
        case SNX_SETUP_KEYS: return snx_tr("Tip: the menu's Setup device keys changes them");
        default: return snx_tr("Tip: nickname 4 to 20 of A-Z a-z 0-9 space . _ -");
    }
}
static const char* setup_keys_said(const App* app) {
    if (setup_page_of(app) == SNX_SETUP_LANGUAGE && app->setup_row == GUI_APPEARANCE_TEXT)
        return app->setup_page ? SAY_SETUP_SIZE : SAY_SETUP_SIZE_FIRST;
    if (setup_page_of(app) == SNX_SETUP_LANGUAGE && app->setup_row == GUI_APPEARANCE_NEXT)
        return app->setup_page ? SAY_SETUP_ON : SAY_SETUP_ON_FIRST;
    if (setup_page_of(app) == SNX_SETUP_LANGUAGE) return app->setup_page ? SAY_SETUP_LOOK : SAY_SETUP_LOOK_FIRST;
    return app->setup_typing                    ? SAY_SETUP_TYPING
           : setup_page_of(app) == SETUP_NOTICE ? SAY_SETUP_NOTICE
           : app->setup_page                    ? SAY_SETUP
                                                : SAY_SETUP_FIRST;
}
/* the page's choice drawn in focus: the Appearance's Next only while the focus is on it, not on a row; -1 for none */
static int setup_focus(const App* app) {
    if (setup_page_of(app) == SNX_SETUP_LANGUAGE) return app->setup_row == GUI_APPEARANCE_NEXT ? 0 : -1;
    return app->setup_choice.focus;
}
/* ---- the Appearance page's box: at the top of r, as high as its rows; its rows' list in it ---- */
static const char* const appearance_names[GUI_APPEARANCE_NEXT] = {SNX_TR_NOOP("Language"), SNX_TR_NOOP("Theme"),
                                                                  SNX_TR_NOOP("UI scale")};
static GuiRect appearance_box(const App* app, GuiRect r, GuiList* rows) {
    int s = app->scale, top = head_h(app) + 4 * s, h = GUI_APPEARANCE_NEXT * (item_h(app) + 6 * s);
    GuiRect box = gui_rect(r.x, r.y, r.w, SDL_min(top + h + 2 * s, r.h));
    list_place(app, rows, gui_rect(box.x + cell_w(app), box.y + top, box.w - 2 * cell_w(app), h), GUI_APPEARANCE_NEXT,
               SDL_min(app->setup_row, GUI_APPEARANCE_NEXT - 1));
    return box;
}
/* the column a row's value starts at, in the row's text: past the marker and the longest name */
static int appearance_column(const App* app) {
    int i, cols = 0;
    for (i = 0; i < GUI_APPEARANCE_NEXT; ++i) cols = SDL_max(cols, gui_text_cols(snx_tr(appearance_names[i])));
    return gui_text_cols(app->theme.marker) + cols + 2;
}
/* row i as it reads: its name, then from appearance_column its value, between the arrows Left and Right go by, or for
   UI scale followed by the dots of the box it opens; Auto's with the scale it has now */
static void appearance_row(const App* app, int i, char* out, size_t size) {
    char value[128];
    int pad = appearance_column(app) - gui_text_cols(app->theme.marker) - gui_text_cols(snx_tr(appearance_names[i]));
    if (i == GUI_APPEARANCE_LANGUAGE)
        snprintf(value, sizeof value, "< %s >", snx_gui_languages[snx_tr_language()].name);
    else if (i == GUI_APPEARANCE_THEME)
        snprintf(value, sizeof value, "< %s >", app->theme.name);
    else {
        char shown[96];
        if (app->menu.text_size)
            size_name(app, app->menu.text_size, shown, sizeof shown);
        else
            snprintf(shown, sizeof shown, snx_tr("Auto (%dx)"), app->auto_scale);
        snprintf(value, sizeof value, "  %s  ...", shown);
    }
    snprintf(out, size, "%s%s%*s%s", app->theme.marker, snx_tr(appearance_names[i]), pad, "", value);
}
typedef struct {
    GuiPlace place;
    GuiRect content; /* the page's boxes */
    GuiChoices choices;
    const char* labels[GUI_SETUP_CHOICES];
    int count;
    char question[96], tab[64];
} GuiSetupBoxes;
static void setup_boxes(App* app, GuiSetupBoxes* b) {
    int n = setup_number(app, app->setup_page);
    char question[sizeof b->question];
    GuiRect area;
    place_screen(app, &b->place, snx_tr(setup_keys_said(app)), 0, GUI_SHOWS_TABS);
    b->count = setup_labels(app, b->labels);
    snprintf(b->question, sizeof b->question, "%s", setup_question(app, question, sizeof question));
    if (app->theme.dialog == GUI_DIALOG_YORHA && n)
        snprintf(b->tab, sizeof b->tab, "ScarletNX  %d/%d", n, setup_steps(app));
    else if (app->theme.dialog == GUI_DIALOG_YORHA)
        snprintf(b->tab, sizeof b->tab, "ScarletNX");
    else if (n)
        snprintf(b->tab, sizeof b->tab, "%d/%d", n, setup_steps(app));
    else
        b->tab[0] = 0;
    area = b->place.list;
    /* the Appearance's question in the middle of what is under its rows, YoRHa's at the foot as any */
    if (setup_page_of(app) == SNX_SETUP_LANGUAGE && app->theme.dialog != GUI_DIALOG_YORHA) {
        GuiList rows;
        GuiRect box = appearance_box(app, area, &rows);
        int under = box.y + box.h + cell_h(app) - area.y;
        area = gui_rect(area.x, area.y + under, area.w, SDL_max(0, area.h - under));
    }
    choices_place(app, area, b->question, b->tab, b->labels, b->count, SDL_max(setup_focus(app), 0), &b->choices);
    b->content = b->place.list;
    if (app->theme.dialog == GUI_DIALOG_YORHA) /* above the choices */
        b->content.h = SDL_max(0, b->choices.list.at.y - cell_h(app) / 2 - b->content.y);
}
/* a box of lines under its label, each wrapped from a row of its own (at its slashes, as paths); the rows they took */
static int setup_box(App* app, GuiRect r, const char* label, const char* const* lines, int n) {
    GuiRect text = inner(app, r);
    int y = pane(app, r, label), i, rows = 0;
    text = gui_rect(text.x, y, text.w, r.y + r.h - y);
    for (i = 0; i < n; ++i)
        rows += wrap_in(app, text, text.x, y + rows * cell_h(app), lines[i], 1, rgb(app->theme.ink));
    return rows;
}
/* two boxes side by side, the first of fifths of r: their rectangles */
static void setup_split(const App* app, GuiRect r, int fifths, GuiRect* a, GuiRect* b) {
    int w = (r.w - cell_w(app)) * fifths / 5;
    *a = gui_rect(r.x, r.y, w, r.h);
    *b = gui_rect(r.x + w + cell_w(app), r.y, r.w - w - cell_w(app), r.h);
}
/* the setup's rows' labels padded to the columns of the widest of a group, translated, and more: they line up whatever
   letters a language's take (gui_text_cols) */
static int setup_cols(const char* const* labels, int n, int more) {
    int i, cols = 0;
    for (i = 0; i < n; ++i) cols = SDL_max(cols, gui_text_cols(snx_tr(labels[i])));
    return cols + more;
}
static const char* setup_pad(char* out, size_t size, const char* label, int cols) {
    snprintf(out, size, "%s%*s", snx_tr(label), SDL_max(0, cols - gui_text_cols(snx_tr(label))), "");
    return out;
}
static void setup_notices_box(App* app, GuiRect r) {
    static const char* const said[] = {SNX_TR_NOOP("before"), SNX_TR_NOOP("now")};
    char text[4][320], head[64], pad[64];
    const char* lines[4];
    int list[SNX_NOTICES], n = setup_notices(app, list), k = 0, cols = setup_cols(said, 2, 2);
    if (n) { /* in the launcher's language: setup.c marks them for scripts/build-languages.py */
        const SnxNotice* notice = &snx_notices[list[SDL_min(app->setup_top, n - 1)]];
        snprintf(text[k], sizeof text[k], "%s%s", app->theme.marker, snx_tr(notice->what));
        lines[k] = text[k];
        ++k;
        snprintf(text[k], sizeof text[k], "    %s%s", setup_pad(pad, sizeof pad, said[0], cols),
                 snx_tr(notice->before));
        lines[k] = text[k];
        ++k;
        snprintf(text[k], sizeof text[k], "    %s%s", setup_pad(pad, sizeof pad, said[1], cols), snx_tr(notice->now));
        lines[k] = text[k];
        ++k;
    }
    snprintf(text[k], sizeof text[k], "  %s", snx_tr("Not moved. See docs/DEPRECATIONS.md"));
    lines[k] = text[k];
    ++k;
    snprintf(head, sizeof head, snx_tr("Changed %d/%d"), SDL_min(app->setup_top, n - 1) + 1, n);
    setup_box(app, r, head, lines, k);
}
/* users.ini's users, a row each: the picture, the nickname, the uid */
static void setup_users_box(App* app, GuiRect r) {
    GuiRect text = inner(app, r);
    int y = pane(app, r, "users.ini") + 2 * app->scale, i, side = cell_h(app), longest = 0;
    char uid[33];
    for (i = 0; i < g_user_count; ++i) longest = SDL_max(longest, gui_text_cols(g_users[i].nickname));
    if (app->setup_users < 0) {
        wrap_in(app, gui_rect(text.x, y, text.w, r.y + r.h - y), text.x, y, app->setup_why, 1, rgb(app->theme.failure));
        return;
    }
    for (i = 0; i < g_user_count && y + side <= r.y + r.h; ++i, y += side + 4 * app->scale) {
        if (!app->users_pictures[i] || app->users_side != side) {
            uint32_t size;
            const unsigned char* jpeg = snx_users_picture(&g_users[i], &size);
            bool failed;
            SDL_DestroyTexture(app->users_pictures[i]);
            app->users_pictures[i] = picture_texture(app, jpeg, size, side, &failed);
        }
        if (app->users_pictures[i]) {
            SDL_FRect at = {(float)text.x, (float)y, (float)side, (float)side};
            SDL_RenderTexture(app->renderer, app->users_pictures[i], NULL, &at);
        }
        text_in(app, text, text.x + side + cell_w(app), y, g_users[i].nickname, rgb(app->theme.ink));
        snx_users_uid_text(g_users[i].uid, uid);
        text_in(app, text, text.x + side + cell_w(app) * (longest + 3), y, uid, rgb(app->theme.soft));
    }
    app->users_side = side;
}
/* what the new first user's uid is: random, or the fixed one for its saves */
static void setup_uid_line(const App* app, char* out, size_t size) {
    char uid[33];
    if (app->setup_saves > 0 && app->setup_typing) {
        snx_users_uid_text(app->setup_uid, uid);
        snprintf(out, size,
                 app->setup_saves == 1 ? snx_tr("uid %s, of the saves of %d game")
                                       : snx_tr("uid %s, of the saves of %d games"),
                 uid, app->setup_saves);
    } else if (app->setup_typing) {
        snx_users_uid_text(app->setup_uid, uid);
        snprintf(out, size, snx_tr("uid %s, random"), uid);
    } else if (app->setup_saves > 0)
        snprintf(out, size,
                 app->setup_saves == 1 ? snx_tr("one new user, the uid of the saves of %d game")
                                       : snx_tr("one new user, the uid of the saves of %d games"),
                 app->setup_saves);
    else
        snprintf(out, size, "%s", snx_tr("one new user, random uid"));
}
/* the Install page's line: the copy portable while its folder beside the program is there, its files in it, else
   installed, its files in the user's data folder */
static void setup_install_now(const App* app, char* out, size_t size) {
    char shown[1024];
    bool there;
    char *portable = setup_portable(app, &there), *data = there ? NULL : snx_user_dir(SNX_USER_DATA);
    snx_home_short(there ? portable : data ? data : snx_tr("(no home folder)"), shown, sizeof shown);
    snprintf(out, size, there ? snx_tr("Now: portable, its files in %s") : snx_tr("Now: installed, its files in %s"),
             shown);
    free(data);
    free(portable);
}
/* that line in a box of its own over the page's two: what r has left under it */
static GuiRect setup_now_box(App* app, GuiRect r) {
    char line[1100];
    GuiRect now;
    int rows;
    setup_install_now(app, line, sizeof line);
    rows = wrap_rows(app, line, inner(app, r).w / cell_w(app), 1);
    now = gui_rect(r.x, r.y, r.w, SDL_min(rows * cell_h(app) + 8 * app->scale, r.h));
    pane(app, now, NULL);
    wrap_in(app, inner(app, now), now.x + cell_w(app), now.y + 4 * app->scale, line, 1, rgb(app->theme.ink));
    return gui_rect(r.x, now.y + now.h + cell_h(app) / 2, r.w, SDL_max(0, r.h - now.h - cell_h(app) / 2));
}
static void setup_panes(App* app, GuiRect r) {
    static const int kinds[] = {SNX_USER_CONFIG, SNX_USER_DATA, SNX_USER_STATE, SNX_USER_CACHE};
    static const char* const kind_names[] = {SNX_TR_NOOP("settings"), SNX_TR_NOOP("data"), SNX_TR_NOOP("logs"),
                                             SNX_TR_NOOP("caches")};
    static const char* const folder_names[] = {SNX_TR_NOOP("Recompiled"), SNX_TR_NOOP("Dumps")};
    static const char* const found[] = {SNX_TR_NOOP("found"), SNX_TR_NOOP("missing")};
    char text[GUI_FOLDERS_MAX + 4][1100], shown[1024], pad[96];
    const char* lines[GUI_FOLDERS_MAX + 4];
    GuiRect a, b;
    int i, n = 0;
    char *path, *other;
    bool there;
    const char* m = app->theme.marker;
    if (!r.h) return;
#define SETUP_LINE(...) (snprintf(text[n], sizeof text[n], __VA_ARGS__), lines[n] = text[n], ++n)
    switch (setup_page_of(app)) {
        case SETUP_NOTICE: setup_notices_box(app, r); break;
        case SNX_SETUP_LANGUAGE: {
            GuiList rows;
            GuiRect box = appearance_box(app, r, &rows);
            pane(app, box, snx_tr("Appearance"));
            for (i = 0; i < GUI_APPEARANCE_NEXT; ++i) {
                appearance_row(app, i, text[0], sizeof text[0]);
                list_item(app, &rows, i, text[0], app->setup_row == i, false);
            }
            break;
        }
        case SNX_SETUP_INSTALL:
            setup_split(app, setup_now_box(app, r), 3, &a, &b);
            for (i = 0; i < 4; ++i) {
                path = snx_user_dir(kinds[i]);
                snx_home_short(path ? path : snx_tr("(no home folder)"), shown, sizeof shown);
                SETUP_LINE("%s%s", setup_pad(pad, sizeof pad, kind_names[i], setup_cols(kind_names, 4, 1)), shown);
                free(path);
            }
            setup_box(app, a, snx_tr("Installed"), lines, n);
            n = 0;
            path = setup_portable(app, &there);
            snx_home_short(path ? path : "", shown, sizeof shown);
            SETUP_LINE("%s %s", snx_tr("everything"), shown);
            SETUP_LINE("%s", there ? snx_tr("there: this copy is portable") : snx_tr("beside the program, made then"));
            setup_box(app, b, snx_tr("Portable"), lines, n);
            free(path);
            break;
        case SNX_SETUP_FOLDERS:
            snx_home_short(app->games_dir ? app->games_dir : "recompiled/", shown, sizeof shown);
            SETUP_LINE("%s%s%s", m, setup_pad(pad, sizeof pad, folder_names[0], setup_cols(folder_names, 2, 2)), shown);
            /* the data folder's dumps/, always listed, then the player's folders of games or dumps */
            path = app->data_dir ? snx_path_join(app->data_dir, "dumps") : NULL;
            snx_home_short(path ? path : "dumps/", shown, sizeof shown);
            free(path);
            SETUP_LINE("%s%s%s%s", m, setup_pad(pad, sizeof pad, folder_names[1], setup_cols(folder_names, 2, 2)),
                       shown, snx_tr(" + subfolders"));
            for (i = 0; i < app->folder_count; ++i) { /* each under its library's name */
                snx_home_short(app->folders[i], shown, sizeof shown);
                SETUP_LINE(
                    "%s%s%s%s", m,
                    setup_pad(pad, sizeof pad, folder_names[app->folders_library[i]], setup_cols(folder_names, 2, 2)),
                    shown, app->folders_deep[i] ? snx_tr(" + subfolders") : "");
            }
            setup_box(app, r, snx_tr("Folders"), lines, n);
            break;
        case SNX_SETUP_KEYS:
            path = snx_keys_found(app->data_dir, "prod.keys");
            other = snx_keys_found(app->data_dir, "title.keys");
            snx_home_short(path ? path : "", shown, sizeof shown);
            SETUP_LINE("%s%-12s%s%s", m, "prod.keys", setup_pad(pad, sizeof pad, found[!path], setup_cols(found, 2, 2)),
                       path ? shown : snx_tr("games are extracted with it"));
            snx_home_short(other ? other : "", shown, sizeof shown);
            SETUP_LINE("%s%-12s%s%s", m, "title.keys",
                       setup_pad(pad, sizeof pad, found[!other], setup_cols(found, 2, 2)),
                       other ? shown : snx_tr("only some .nsp need it"));
            setup_box(app, r, snx_tr("Keys"), lines, n);
            free(path);
            free(other);
            break;
        default:
            if (app->setup_typing) {
                SETUP_LINE("%s%s  %s", m, snx_tr("nickname"), app->setup_name);
                setup_uid_line(app, shown, sizeof shown);
                SETUP_LINE("%s%s", m, shown);
                setup_box(app, r, snx_tr("First user"), lines, n);
                break;
            }
            setup_split(app, r, 3, &a, &b);
            setup_users_box(app, a);
            setup_uid_line(app, shown, sizeof shown);
            SETUP_LINE("%s%s", m, shown);
            SETUP_LINE("%s%s", m, snx_tr("old file: users.ini.old"));
            setup_box(app, b, snx_tr("Overwrite"), lines, n);
            break;
    }
#undef SETUP_LINE
}
static void draw_setup(App* app) {
    GuiSetupBoxes b;
    const GuiTheme* t = &app->theme;
    setup_boxes(app, &b);
    chrome(app, &b.place);
    setup_panes(app, b.content);
    choices_draw(app, &b.choices, b.question, b.tab, b.labels, b.count, setup_focus(app));
    foot(app, &b.place, app->message[0] ? app->message : setup_tip(app),
         rgb(app->setup_failed ? t->failure
             : app->message[0] ? t->focus
                               : t->ink),
         snx_tr(setup_keys_said(app)));
    if (app->sizes_shown) render_sizes(app); /* UI scale's box, over the Appearance page */
    if (app->confirm.shown) render_ask(app);
}
/* the pointer on the page: a choice under it takes the focus, a click takes it. On the Appearance's rows, a click
   changes the value a step on, or back on its left arrow, and on UI scale opens its box */
static void setup_pointer(App* app, float x, float y, bool click) {
    GuiSetupBoxes b;
    GuiList rows;
    bool on;
    int i;
    setup_boxes(app, &b);
    if ((i = choices_hit(&b.choices, x, y, &on)) >= 0) {
        app->setup_choice.focus = i;
        app->setup_row = GUI_APPEARANCE_NEXT;
        if (click) setup_take(app);
        return;
    }
    if (setup_page_of(app) != SNX_SETUP_LANGUAGE) return;
    appearance_box(app, b.content, &rows);
    if ((i = list_hit(&rows, x, y, false)) < 0) return;
    app->setup_row = i;
    if (click && i == GUI_APPEARANCE_TEXT)
        sizes_open(app);
    else if (click) {
        int arrow = list_box(&rows, i).x + cell_w(app) / 2 + appearance_column(app) * cell_w(app);
        appearance_change(app, i, x >= (float)arrow && x < (float)(arrow + cell_w(app)) ? -1 : 1);
    }
}
/* what the menu keeps for the next start, as it closes: each of its files there and its folder, in a box */
static void draw_closing(App* app) {
    const char* files[4] = {app->settings_file, app->app_file, app->folders_file, app->recent_file};
    static const char* const what[4] = {SNX_TR_NOOP("Settings"), SNX_TR_NOOP("Picks of Settings > App"),
                                        SNX_TR_NOOP("The libraries' folders"), SNX_TR_NOOP("Recent games")};
    char line[4400];
    int cols, rows = 1, i, y;
    GuiRect r, text;
    SDL_SetRenderDrawColor(app->renderer, app->theme.page.r, app->theme.page.g, app->theme.page.b, 255);
    SDL_RenderClear(app->renderer);
    backdrop(app);
    cols = app->cols - 6;
    for (i = 0; i < 4; ++i)
        if (files[i] && snx_is_file(files[i])) {
            snprintf(line, sizeof line, "%s: %s", snx_tr(what[i]), files[i]);
            rows += wrap_rows(app, line, cols - 2, 1);
        }
    r = over_rect(app, cols, head_h(app) + rows * cell_h(app) + 8 * app->scale);
    y = pane(app, r, snx_tr("Closing ScarletNX"));
    text = inner(app, r);
    y += wrap_in(app, text, text.x, y,
                 rows > 1 ? snx_tr("Kept for the next start:") : snx_tr("Nothing to keep: no file of the menu's"), 0,
                 rgb(app->theme.soft)) *
         cell_h(app);
    for (i = 0; i < 4; ++i)
        if (files[i] && snx_is_file(files[i])) {
            snprintf(line, sizeof line, "%s: %s", snx_tr(what[i]), files[i]);
            y += wrap_in(app, text, text.x, y, line, 1, rgb(app->theme.ink)) * cell_h(app);
        }
}
static void render(App* app) {
    layout(app);
    if (splash_shown(app)) return;
    if (app->closing_ms) {
        draw_closing(app);
        SDL_RenderPresent(app->renderer);
        return;
    }
    fades_mark(app);
    if (app->setup_on && !app->editing) {
        draw_setup(app);
        overlay(app);
    } else if (app->editing && app->form == GUI_FORM_PAD)
        render_pad(app);
    else if (app->editing) {
        draw_form(app);
        if (app->confirm.shown)
            render_ask(app); /* a folder picked on the folders' screen, a UI scale picked */
        else if (app->sizes_shown)
            render_sizes(app);
        else if (app->users_shown)
            render_users(app); /* opened from the form's head, as from the menu's */
    } else
        draw_screen(app);
    if (app->cleaned_shown) render_files(app, 1); /* over the menu, or the App's rows it was started from */
    fades_draw(app);
    SDL_RenderPresent(app->renderer);
}
/* the game's name as its run gave it, from the step that names it, for its recent game */
static void recent_name(App* app, const char* label) {
    const GuiStep* named = gui_steps_last(&app->steps, label);
    if (!app->recent_file || app->recent_named || !app->file || !named || !named->detail[0]) return;
    app->recent_named = 1;
    if (gui_recent_rename(&app->recent, app->file, named->detail)) recent_changed(app);
}
static void list_games(App* app);
static uint64_t games_stamp(const App* app);
/* back to the menu from a finished run, which fades in */
static void leave_run(App* app) {
    app->finished = false;
    app->menu.view = GUI_LOAD_NONE;
    space_start(app); /* a run takes disk */
    app->message[0] = 0;
    app->aot_checked = 0; /* the cache may have grown: look again when Games shows */
    recent_note(app);
    app->leaving_ms = 0;
    app->fade_ms = app->theme.fade_in_ms ? SDL_max(now_ms(app), 1) : 0;
    layout(app);
}
#ifdef SDL_PLATFORM_ANDROID
/* A game runs in the app's game activity, a process of its own: no child of this one gets a window */
static int play(App* app, const char* game) {
    GuiSettings* s = settings_of(app);
    char* option = snx_format("--game=%s", game);
    const char** argv = (const char**)snx_alloc((size_t)(s->count + 3) * sizeof(*argv));
    int ok;
    argv[0] = option;
    argv[1 + gui_settings_options(s, 0, argv + 1)] = NULL;
    ok = gui_android_play(argv, app->message, sizeof(app->message));
    free(argv);
    free(option);
    return ok;
}
/* the game a run of the driver made ready: the one it ran, when it was a game already, else in its folder of games/,
   where the driver names it, as it cannot open it; whether it opened */
static int ready(App* app) {
    const GuiStep* made = gui_steps_last(&app->steps, "READY");
    char* folder = made && !app->native ? snx_path_join(app->games_dir, made->detail) : NULL;
    char* game = !made         ? NULL
                 : app->native ? snx_strdup(app->file)
                 : folder      ? snx_path_join(folder, made->detail)
                               : NULL;
    int ok = game && play(app, game);
    free(game);
    free(folder);
    return ok;
}
/* the run in its notification: the game, the step on the screen and the percent, as they change, a second apart at
   most (Android drops notifications that come faster) */
static void tell(App* app) {
    char name[96], runs[3][112], line[sizeof app->told];
    size_t lengths[3];
    int percent = gui_steps_percent(&app->steps);
    Uint64 now = SDL_max(SDL_GetTicks(), 1);
    line[0] = 0;
    if (app->steps.count) {
        step_runs(&app->steps.step[app->steps.count - 1], app->theme.progress, now_ms(app), runs, lengths);
        snprintf(line, sizeof line, "%s%s%s", runs[0], runs[1], runs[2]);
    }
    if (app->told_ms && (now - app->told_ms < 1000 || (percent == app->told_percent && !strcmp(line, app->told))))
        return;
    run_name(app, name, sizeof name);
    gui_android_running(name, line, percent);
    snprintf(app->told, sizeof app->told, "%s", line);
    app->told_percent = percent;
    app->told_ms = now;
}
#endif
#ifdef SDL_PLATFORM_IOS
/* A game runs in this process once the menu has closed (gui_apple_play, src/nx/runtime/entry.c): an iOS app has no
   other */
static int play(App* app, const char* game) {
    GuiSettings* s = settings_of(app);
    char* option = snx_format("--game=%s", game);
    const char** argv = (const char**)snx_alloc((size_t)(s->count + 4) * sizeof(*argv));
    argv[0] = "scarletnx";
    argv[1] = option;
    argv[2 + gui_settings_options(s, 0, argv + 2)] = NULL;
    gui_apple_play(argv);
    app->menu.quit = true;
    free(argv);
    free(option);
    return 1;
}
#endif
/* a line more while what an older version left at old (a folder, or a game's files) is there; this one keeps it at now */
static void left_line(App* app, const char* old, const char* now, bool there) {
    char *line, *grown;
    size_t n = app->left ? strlen(app->left) : 0;
    if (!there || !strcmp(old, now)) return;
    line = snx_format("%s" SAY_LEFT_BY "%s\n", old, now);
    grown = line ? (char*)realloc(app->left, n + strlen(line) + 1) : NULL;
    if (grown) {
        strcpy(grown + n, line);
        app->left = grown;
        app->lefts++;
    }
    free(line);
}
/* the games of folder an older version laid out beside each other (<name>, its code, <name>_data/): each one's folder
   now, recompiled/<title ID>/ or recompiled/<name>/ for one without an ID of its own (main.npdm in its data) */
static void left_games(App* app, const char* folder) {
    char** names = NULL;
    size_t count = 0, i, n;
    if (!folder || snx_list_dir(folder, &names, &count)) return;
    for (i = 0; i < count; ++i) {
        char *data, *npdm, *name, *old, *now, id[17];
        unsigned char* bytes;
        size_t size = 0;
        n = strlen(names[i]);
        if (n <= 5 || strcmp(names[i] + n - 5, "_data")) continue;
        data = snx_path_join(folder, names[i]);
        name = snx_format("%.*s", (int)(n - 5), names[i]);
        npdm = data ? snx_path_join(data, "exefs/main.npdm") : NULL;
        bytes = npdm && snx_is_file(npdm) ? (unsigned char*)snx_read_file(npdm, &size) : NULL;
        old = name ? snx_path_join(folder, name) : NULL;
        now = name ? snx_format("%s/%s/", folder, snx_title_text(snx_npdm_title(bytes, size), id) ? id : name) : NULL;
        if (old && now && data && snx_is_dir(data)) left_line(app, old, now, true);
        free(bytes);
        free(npdm);
        free(now);
        free(old);
        free(name);
        free(data);
    }
    snx_free_strings(names, count);
}
/* a folder with anything in it */
static bool has_files(const char* folder) {
    char** names = NULL;
    size_t count = 0;
    bool any = folder && !snx_list_dir(folder, &names, &count) && count;
    snx_free_strings(names, count);
    return any;
}
/* the data folder's games/, where an older version made its games, while it is not the folder games go to: its games
   are still listed until the player moves them to recompiled/. NULL when there is none */
static char* games_old(const App* app) {
    char* old = app->data_dir ? snx_path_join(app->data_dir, "games") : NULL;
    if (old && snx_is_dir(old) && (!app->games_dir || !snx_same_file(old, app->games_dir))) return old;
    free(old);
    return NULL;
}
/* whether that games/ holds the game at path */
static bool game_was_old(const App* app, const char* path) {
    char* old = games_old(app);
    bool was = old && path && snx_path_within(path, old);
    free(old);
    return was;
}
#ifdef SDL_PLATFORM_ANDROID
/* what older versions kept elsewhere on Android: /sdcard/ScarletNX's games and keys (a shared storage the app no longer
   uses), and the games/ in the app's own files from before it had All files access */
static void left_android(App* app) {
    static const char* const shared[] = {"games", "prod.keys", "title.keys"};
    const char* files = SDL_GetAndroidInternalStoragePath();
    char *old, *now;
    size_t i;
    for (i = 0; app->data_dir && i < sizeof shared / sizeof shared[0]; ++i) {
        old = snx_path_join(SNX_ANDROID_DIR, shared[i]);
        now = i ? snx_path_join(app->data_dir, shared[i]) : snx_strdup(app->games_dir);
        if (old && now) left_line(app, old, now, i ? snx_is_file(old) : has_files(old));
        free(old);
        free(now);
    }
    old = files ? snx_path_join(files, "games") : NULL;
    if (old && app->games_dir) left_line(app, old, app->games_dir, has_files(old));
    free(old);
}
#endif
/* the space each folder app->left names takes, counted again only when they change */
static void left_count(App* app) {
    char** olds = (char**)calloc((size_t)app->lefts + 1, sizeof *olds);
    const char* at = app->left;
    int i, same = app->left_space && app->left_space->count == app->lefts;
    for (i = 0; olds && i < app->lefts; ++i, at = strchr(at, '\n') + 1) {
        olds[i] = snx_format("%.*s", (int)(strstr(at, SAY_LEFT_BY) - at), at);
        same = same && olds[i] && !strcmp(olds[i], app->left_space->dirs[i]);
    }
    if (!olds || !app->lefts)
        space_end(&app->left_space, &app->left_counter);
    else if (!same)
        space_count(&app->left_space, &app->left_counter, olds, app->lefts, true);
    for (i = 0; olds && i < app->lefts; ++i) free(olds[i]);
    free(olds);
}
/* what older versions left where this one no longer looks, a folder at a time, for the menu to say while it is there:
   the menu's own files' folder (gui/), work/, each game laid out beside the others, a portable copy's games/ beside
   the program (its data in .scarletnx/ now), a build tree's games/, work/ and keys in its checkout. Nothing is moved:
   the player does that (docs/DEPRECATIONS.md) */
static void left_behind(App* app) {
    char *config = menu_folder(app, SNX_USER_CONFIG, false), *state = menu_folder(app, SNX_USER_STATE, false);
    char *data = snx_user_data_dir(), *gui = data ? snx_path_join(data, "gui") : NULL, *kept = NULL;
    char* work = app->data_dir ? snx_path_join(app->data_dir, "work") : NULL;
    char *moved = work ? snx_user_path(work, SNX_USER_CACHE) : NULL, *old = NULL, *now = NULL;
    int i;
    free(app->left);
    app->left = NULL;
    app->lefts = 0;
    app->left_at = SDL_GetTicks();
#ifndef SDL_PLATFORM_ANDROID /* there an older version's were in the app's own files: menu_old, below */
    /* gui/: the menu's own, now in the config and state folders, each where it goes */
    for (i = 0; gui && i < 2; ++i) {
        const char* to = i ? state : config;
        int k, there = 0;
        for (k = 0; k < 4 && menu_files[i][k][0]; ++k) {
            char* file = snx_path_join(gui, menu_files[i][k]);
            there |= file && snx_is_file(file);
            free(file);
        }
        if (to) left_line(app, gui, to, there);
    }
#ifdef SNX_XDG /* and the state folder's gui/ of the version just before */
    kept = snx_user_dir(SNX_USER_STATE);
    old = kept ? snx_path_join(kept, "gui") : NULL;
    if (old && state) left_line(app, old, state, snx_is_dir(old));
    free(old);
    old = NULL;
#endif
#endif
    /* the menu's own files in the app's own files on Android, a file at a time: that folder holds the data folder too.
       Out of the player's reach, clearing the app's storage takes them while the app is in testing (left_clear) */
    app->left_clear = false;
    for (i = 0; app->menu_old && i < 2; ++i) {
        const char* to = i ? state : config;
        int k;
        for (k = 0; to && k < 4 && menu_files[i][k][0]; ++k) {
            char* file = snx_path_join(app->menu_old, menu_files[i][k]);
            if (snx_is_file(file)) {
                left_line(app, file, to, true);
                app->left_clear = true;
            }
            free(file);
        }
    }
    if (work && moved) left_line(app, work, moved, snx_path_exists(work));
#ifdef SDL_PLATFORM_ANDROID
    left_android(app);
#endif
    left_games(app, app->games_dir);
    for (i = 0; i < app->folder_count; ++i) left_games(app, app->folders[i]);
    /* the data folder's games/, recompiled/ now */
    old = games_old(app);
    if (old && app->games_dir) left_line(app, old, app->games_dir, has_files(old));
    free(old);
    old = NULL;
    /* a portable copy's games/ beside the program, which a .scarletnx/ there holds now */
    if (app->drivers && app->data_dir) {
        old = snx_path_join(app->drivers, "games");
        now = snx_format("%s/" SNX_PORTABLE_DIR "/recompiled", app->drivers);
        if (old && now && app->games_dir && strcmp(old, app->games_dir)) left_line(app, old, now, snx_is_dir(old));
    }
    free(old);
    free(now);
    /* the checkout a build tree's menu used as its data folder */
    for (i = 0; app->checkout && app->data_dir && i < SNX_CHECKOUT_FILES; ++i) {
        old = snx_path_join(app->checkout, snx_checkout_files[i]);
        now = snx_checkout_now(app->data_dir, i);
        if (old && now) left_line(app, old, now, snx_path_exists(old));
        free(old);
        free(now);
    }
    left_count(app);
    free(kept);
    free(moved);
    free(work);
    free(gui);
    free(data);
    free(state);
    free(config);
}
/* ---- Clean old settings (clean.c): [Deprecated] out of each game's .ini the menu reaches, and the keys the menu's own
   files hold that nothing reads ---- */
/* the folders a game's .ini can be in: the config folder's games/ (an installed build's), games/, the player's; each
   once. How many, each in out to free */
static int ini_folders(const App* app, char** out) {
    int n = 0, i, j, k;
    char* config = app->games_dir ? snx_user_path(app->games_dir, SNX_USER_CONFIG) : NULL;
    const char* listed[GUI_FOLDERS_MAX + 2];
    listed[n++] = config;
    listed[n++] = app->games_dir;
    for (i = 0; i < app->folder_count; ++i) listed[n++] = app->folders[i];
    for (i = 0, k = 0; i < n; ++i) {
        int seen = !listed[i];
        for (j = 0; j < k && !seen; ++j) seen = !strcmp(out[j], listed[i]);
        if (!seen) out[k++] = snx_strdup(listed[i]);
    }
    free(config);
    return k;
}
/* settings.ini's keys no row takes, while the runtime has listed its settings: without them every key of the
   runtime's would look old */
static int settings_unread(App* app, char* out, size_t size) {
    GuiSettings* s;
    if (size) out[0] = 0;
    if (!app->settings_file || !snx_is_file(app->settings_file)) return 0;
    s = settings_of(app);
    return s->count > app->settings.own ? gui_settings_unread(s, app->settings_file, out, size) : 0;
}
/* the .ini files with a [Deprecated] section; with done, each written again without it */
static int clean_inis(App* app, GuiCleaned* done) {
    char* folders[GUI_FOLDERS_MAX + 2];
    int n = ini_folders(app, folders), i, found = 0;
    for (i = 0; i < n; ++i) {
        found += gui_clean_inis(done, folders[i]);
        free(folders[i]);
    }
    return found;
}
/* a file of the menu's own without the unread keys, written again by save (0, else nonzero), an entry */
static void clean_keys(App* app, const char* file, const char* unread, int failed) {
    char what[1100];
    snprintf(what, sizeof what, failed ? snx_tr("Not written without %s") : snx_tr("Took out %s"), unread);
    gui_cleaned_add(&app->cleaned, !failed, file, what);
}
/* it done, each file an entry of app->cleaned, which shows over the screen */
static void clean_old(App* app) {
    char unread[1024];
    gui_cleaned_free(&app->cleaned);
    app->report_move = false;
    clean_inis(app, &app->cleaned);
    if (settings_unread(app, unread, sizeof unread))
        clean_keys(app, app->settings_file, unread, gui_settings_save(&app->settings, app->settings_file));
    if (app_read(app, false, unread, sizeof unread, NULL, 0)) /* the picks as the menu has them, nothing else */
        clean_keys(app, app->app_file, unread, app_save(app));
    app->cleaned_shown = app->cleaned.count > 0;
    app->files_top = 0;
}
/* the moves of Move old files, made with apply, else what they would be, an entry each in done: each folder app->left
   names, entry by entry, into the one this version keeps it in; the menu's gui/ split by its files between the config
   and state folders; with the data folder's games/, its games' settings and caches in the config, state and cache
   folders. A game laid out beside others is not moved: its run from its dump makes its folder (docs/DEPRECATIONS.md).
   How many entries move */
static const char* last_part(const char* path) {
    const char* end = path + strlen(path);
    while (end > path && end[-1] != '/' && end[-1] != '\\') --end;
    return end;
}
static int move_old(App* app, GuiCleaned* done, bool apply) {
    char old[4400], now[4400], what[4500];
    char *config = menu_folder(app, SNX_USER_CONFIG, false), *state = menu_folder(app, SNX_USER_STATE, false);
    char* games = games_old(app);
    const char* only[2][5];
    const char* at = app->left;
    int i, k, moves = 0;
    for (i = 0; i < 2; ++i) { /* menu_files, as gui_move_entries takes them */
        for (k = 0; k < 4 && menu_files[i][k][0]; ++k) only[i][k] = menu_files[i][k];
        only[i][k] = NULL;
    }
    for (i = 0; at && *at && i < app->lefts; ++i, at = strchr(at, '\n') ? strchr(at, '\n') + 1 : NULL) {
        const char* const* files = NULL;
        char* data;
        left_paths(at, old, now, sizeof old);
        data = snx_format("%s_data", old);
        if (!snx_is_dir(old) && data && snx_is_dir(data)) { /* a game laid out beside others */
            snprintf(what, sizeof what,
                     snx_tr("Not moved: run it from its dump once, which makes %s, then move its saves "
                            "(docs/DEPRECATIONS.md)"),
                     now);
            gui_cleaned_add(done, false, old, what);
            free(data);
            continue;
        }
        free(data);
        if (!strcmp(last_part(old), "gui") && snx_is_dir(old))
            files = config && !strcmp(now, config) ? only[0] : state && !strcmp(now, state) ? only[1] : NULL;
        moves += gui_move_entries(done, old, now, files, apply);
        for (k = SNX_USER_CONFIG; games && !strcmp(old, games) && k <= SNX_USER_CACHE; ++k) {
            char *from = snx_user_path(old, k), *to = snx_user_path(now, k);
            if (from && to && strcmp(from, old) && strcmp(to, now))
                moves += gui_move_entries(done, from, to, NULL, apply);
            else if (from && strcmp(from, old) && snx_path_exists(from)) { /* recompiled/ is not in the data folder */
                snprintf(what, sizeof what,
                         snx_tr("Not moved: %s is not in the data folder, which these settings go by"), now);
                gui_cleaned_add(done, false, from, what);
            }
            free(from);
            free(to);
        }
    }
    free(games);
    free(config);
    free(state);
    return moves;
}
/* App's Move old files: what it would move, in a box (GUI_ASK_MOVE); nothing left, said; nothing that can move, the
   box of entries saying why */
static void move_row(App* app) {
    GuiCleaned plan = {0};
    int moves;
    left_behind(app);
    app_values(app);
    moves = move_old(app, &plan, false);
    if (!plan.count) {
        snprintf(app->editor.note, sizeof app->editor.note, "%s", snx_tr("Nothing left by an older version"));
        return;
    }
    if (!moves) {
        gui_cleaned_free(&app->cleaned);
        app->cleaned = plan;
        app->report_move = true;
        app->cleaned_shown = true;
        app->files_top = 0;
        snprintf(app->editor.note, sizeof app->editor.note, snx_tr("None of the %d can move: the box says why"),
                 plan.count);
        return;
    }
    snprintf(app->ask_line, sizeof app->ask_line,
             plan.count > moves ? snx_tr("Move old files? %d move, %d stay where they are")
                                : snx_tr("Move old files? %d move"),
             moves, plan.count - moves);
    gui_cleaned_free(&plan);
    gui_confirm_open(&app->confirm, GUI_ASK_MOVE, false);
}
/* App's row: what it would take out, in a box (GUI_ASK_CLEAN), else that there is nothing */
static void clean_row(App* app) {
    char unread[16], *line = app->ask_line;
    size_t size = sizeof app->ask_line;
    int inis = clean_inis(app, NULL);
    int keys = settings_unread(app, unread, sizeof unread) + app_read(app, false, unread, sizeof unread, NULL, 0);
    if (!inis && !keys) {
        snprintf(app->editor.note, sizeof app->editor.note, "%s",
                 snx_tr("Nothing to clean: no [Deprecated] in a game's .ini, no old keys"));
        return;
    }
    if (inis && keys)
        snprintf(line, size,
                 keys > 1 ? snx_tr("Clean old settings? [Deprecated] in %d .ini, %d old keys")
                          : snx_tr("Clean old settings? [Deprecated] in %d .ini, %d old key"),
                 inis, keys);
    else if (inis)
        snprintf(line, size, snx_tr("Clean old settings? [Deprecated] in %d .ini"), inis);
    else
        snprintf(line, size,
                 keys > 1 ? snx_tr("Clean old settings? %d old keys") : snx_tr("Clean old settings? %d old key"), keys);
    gui_confirm_open(&app->confirm, GUI_ASK_CLEAN, false);
}
/* Settings > App's Reset all: how many settings it changes, in a box (GUI_ASK_RESET), else that none */
static void settings_reset_ask(App* app) {
    int changed = gui_settings_changed(&app->settings);
    if (!changed) {
        snprintf(app->editor.note, sizeof app->editor.note, "%s", snx_tr("Every setting is at its default"));
        return;
    }
    snprintf(app->ask_line, sizeof app->ask_line,
             changed > 1 ? snx_tr("Reset all to the defaults? %d settings change")
                         : snx_tr("Reset all to the defaults? %d setting changes"),
             changed);
    gui_confirm_open(&app->confirm, GUI_ASK_RESET, false);
}
/* a change asked about (GUI_ASK_CLEAN to GUI_ASK_FOLDER) taken: done, its report or note shown, the screen as it left
   things; Back changes nothing */
static void change_answer(App* app, GuiAsk ask, bool taken) {
    if (!taken) return;
    if (ask == GUI_ASK_CLEAN) {
        clean_old(app);
        snprintf(app->editor.note, sizeof app->editor.note, snx_tr("Cleaned: %d done, %d not"), app->cleaned.done,
                 app->cleaned.count - app->cleaned.done);
    } else if (ask == GUI_ASK_RESET) {
        int changed = gui_settings_changed(&app->settings);
        gui_settings_reset(&app->settings, -1);
        snprintf(app->editor.note, sizeof app->editor.note,
                 changed > 1 ? snx_tr("%d settings back to their defaults") : snx_tr("%d setting back to its default"),
                 changed);
        write_settings(app);          /* a save that fails says so over the count */
    } else if (ask == GUI_ASK_MOVE) { /* done: what it did over the screen, and the menu as the files are now */
        int moved;
        gui_cleaned_free(&app->cleaned);
        moved = move_old(app, &app->cleaned, true);
        app->report_move = true;
        app->cleaned_shown = app->cleaned.count > 0;
        app->files_top = 0;
        snprintf(app->editor.note, sizeof app->editor.note, snx_tr("%d moved, %d not"), moved,
                 app->cleaned.count - app->cleaned.done);
        left_behind(app);
        app->games_stamp = 0; /* Games lists them again where they are */
        app_values(app);
    } else if (ask == GUI_ASK_PLACES)
        places_reset(app);
    else if (ask == GUI_ASK_PLACE)
        place_use(app, app->place_off, NULL);
    else if (ask == GUI_ASK_FOLDER)
        folder_remove(app, app->folder_off);
}
/* the functions an AOT pack used, from the driver's line, -1 when it printed none */
static int aot_built(const App* app) {
    int i, n;
    for (i = app->steps.notes - 1; i >= 0; --i)
        if (sscanf(app->steps.note[i].text, "AOT pack: %d", &n) == 1) return n;
    return -1;
}
static void pump(App* app) {
    keys_left(app);
    text_left(app);
    if (app->menu.running) {
        char buffer[4096];
        /* Bounded work per frame keeps keyboard/window events responsive even
           when a compiler floods the inherited pipe. There is no log backlog. */
        for (int reads = 0; reads < 16; ++reads) {
            int n = gui_process_read(app->process, buffer, sizeof(buffer));
            if (n <= 0) break;
            if (app->run_out) fwrite(buffer, 1, (size_t)n, app->run_out);
            gui_console_feed(&app->console, buffer, (size_t)n);
            gui_steps_feed(&app->steps, buffer, (size_t)n, now_ms(app));
            app->has_output = true;
        }
        recent_name(app, "LAUNCH");
#ifdef SDL_PLATFORM_ANDROID
        tell(app);
#endif
        int status;
        if (gui_process_finished(app->process, &status)) {
            app->menu.running = false;
            /* Normal exits wait for acknowledgement. ESC already acknowledged
               an abort, so return straight to the menu after nonblocking reaping. */
            app->finished = !app->aborting;
            app->abnormal_exit = app->aborting || status != 0;
            if (!app->aborting) app->log_due = status != 0; /* an ESC is no crash, and keeps what was due */
            users_read(app); /* users.ini may have changed while it ran */
            gui_steps_finish(&app->steps, !app->abnormal_exit);
            if (app->run_out) {
                fprintf(app->run_out, "\n[%s, status %d]\n", app->aborting ? "aborted" : "ended", status);
                fclose(app->run_out);
                app->run_out = NULL;
            }
            recent_name(app, "LAUNCH");
            recent_name(app, "RECOMPILE"); /* a run that ended before its game loaded */
            recent_as_games(app);          /* a dump's run made its game */
            app->menu.view = GUI_LOAD_NONE;
            if (app->aot_pack && app->aborting)
                snprintf(app->message, sizeof(app->message), "%s", snx_tr("AOT pack build aborted."));
            else if (app->aot_pack && app->abnormal_exit)
                snprintf(app->message, sizeof(app->message), snx_tr("AOT pack build failed (status %d)."), status);
            else if (app->aot_pack) {
                int built = aot_built(app);
                if (built > 0)
                    snprintf(app->message, sizeof(app->message), snx_tr("AOT pack built from %d cached function%s."),
                             built, built == 1 ? "" : "s");
                else if (built == 0)
                    snprintf(app->message, sizeof(app->message), "%s",
                             snx_tr("AOT pack built, but no cached function matched the game."));
                else
                    snprintf(app->message, sizeof(app->message), "%s", snx_tr("AOT pack built."));
            } else if (app->aborting)
                snprintf(app->message, sizeof(app->message), "%s",
                         snx_tr("Program ended abnormally (force-terminated by ESC)."));
            else if (app->abnormal_exit)
                snprintf(app->message, sizeof(app->message), snx_tr("Program ended abnormally (status %d)."), status);
            else
                snprintf(app->message, sizeof(app->message), snx_tr("Driver exited with status %d."), status);
            if (app->aot_pack) {
                app->aot_pack = false;
                free(app->aot_cache);
                app->aot_cache = NULL;
            }
            recent_note(app);
            app->aborting = false;
            app->confirm.shown = false; /* a run that ended while asking has nothing left to abort */
            layout(app);
#ifdef SDL_PLATFORM_ANDROID
            /* the game opens in its own activity, over this one: coming back from it is coming back to the menu.
               Android lets no app behind others open one, so then it waits for the menu to show */
            if (!app->abnormal_exit && app->hidden)
                app->play_later = true;
            else if (!app->abnormal_exit && ready(app))
                leave_run(app);
            /* seen here, the end needs no word; behind other apps one stays until the menu shows */
            gui_android_ran(!app->hidden ? NULL : app->abnormal_exit ? snx_tr("Failed") : snx_tr("Ready"));
#endif
        }
    }
#ifdef SDL_PLATFORM_ANDROID
    if (app->play_later && !app->hidden) {
        app->play_later = false;
        if (ready(app)) leave_run(app);
    }
#endif
    Uint64 now = SDL_GetTicks();
    if (!app->sampled || now - app->sampled >= 500) {
        app->usage = gui_process_usage(app->process);
        app->sampled = now;
    }
    if (app->left_shown && (!app->left_at || now - app->left_at >= 5000)) { /* the player may have moved them */
        left_behind(app);
        app->left_shown = app->lefts > 0;
    }
    if (app->leaving_ms && now_ms(app) - app->leaving_ms >= app->theme.fade_out_ms) leave_run(app);
    if (app->closing_ms && now_ms(app) - app->closing_ms >= GUI_CLOSING_MS) app->menu.quit = true;
    if (app->dump_icons.lock) { /* a dump's icon the driver answered: its row looks again */
        unsigned answered;
        SDL_LockMutex(app->dump_icons.lock);
        answered = app->dump_icons.answered;
        SDL_UnlockMutex(app->dump_icons.lock);
        if (answered != app->dump_icons_seen) {
            app->dump_icons_seen = answered;
            icons_forget_none(app);
        }
    }
    /* games put in or taken out of the folders while the menu is open, looked for once a second: Games and its list
       follow, the list on the file it had */
    if (!app->menu.running && !app->finished && now_ms(app) - app->games_checked >= 1000) {
        app->games_checked = now_ms(app);
        if (games_stamp(app) != app->games_stamp) {
            app->games_stamp = games_stamp(app);
            recent_as_games(app);
            if (app->menu.view == GUI_LOAD_GAMES) {
                GuiBrowser was = app->browser;
                list_games(app);
                app->browser.page = was.page;
                app->browser.selected = was.selected;
                gui_browser_resize(&app->browser, app->browser.page_size);
            }
        }
    }
}
/* ---- Recompile: the recompiled library's folders a dump's game can go in, its Main first, then each Extra that can
   take one (snx_folder_unfit); asked when there are more than one, the game going in the one taken ---- */
static int dest_dirs(const App* app, const char** dirs) {
    int i, n = 0;
    if (app->games_dir) dirs[n++] = app->games_dir;
    for (i = 0; i < app->folder_count; ++i)
        if (app->folders_library[i] == GUI_LIBRARY_RECOMPILED && !snx_folder_unfit(app->folders[i], true))
            dirs[n++] = app->folders[i];
    return n;
}
static void dests_open(App* app, char* file, bool prepared) {
    const char* dirs[GUI_FOLDERS_MAX + 1];
    free(app->dest_file);
    app->dest_file = file;
    app->dest_item = app->menu.selected;
    app->dest_prepared = prepared;
    app->dest_recompile = app->recompile;
    app->dests.count = dest_dirs(app, dirs);
    app->dests.focus = 0;
    app->dests_shown = true;
}
/* the box's question, and each choice: the folder from home, cut in its middle to fit, what it is and its free room */
static void dests_text(App* app, char* question, size_t size, char (*text)[512], const char** labels) {
    const char* dirs[GUI_FOLDERS_MAX + 1];
    char *slash = app->dest_file ? strrchr(app->dest_file, '/') : NULL, home[512], left[32];
    int i, n = dest_dirs(app, dirs), room = SDL_max(app->cols - 40, 16);
    snprintf(question, size, snx_tr("%s: into which folder of the recompiled library?"),
             slash            ? slash + 1
             : app->dest_file ? app->dest_file
                              : "");
    for (i = 0; i < n && i < app->dests.count; ++i) {
        snx_home_short(dirs[i], home, sizeof home);
        gui_path_fit(home, room, text[i], 300);
        gui_size_text(left, sizeof left, free_space(dirs[i]));
        snprintf(text[i] + strlen(text[i]), 512 - strlen(text[i]),
                 i                             ? snx_tr("  %s free")
                 : app->place[GUI_PLACE_GAMES] ? snx_tr("  picked, %s free")
                                               : snx_tr("  default, %s free"),
                 left);
        labels[i] = text[i];
    }
}
static void dests_place(App* app, GuiChoices* c, char* question, size_t size, char (*text)[512], const char** labels) {
    int width, height;
    area(app, &width, &height);
    dests_text(app, question, size, text, labels);
    choices_place(app,
                  app->theme.dialog == GUI_DIALOG_YORHA
                      ? gui_rect(cell_w(app), cell_h(app) / 2, width - 2 * cell_w(app), height - cell_h(app))
                      : gui_rect(0, 0, width, height),
                  question, snx_tr("Recompile"), labels, app->dests.count, app->dests.focus, c);
}
static void render_dests(App* app) {
    GuiChoices c;
    char question[640], text[GUI_FOLDERS_MAX + 1][512];
    const char* labels[GUI_FOLDERS_MAX + 1];
    dests_place(app, &c, question, sizeof question, text, labels);
    box_shade(app);
    choices_draw(app, &c, question, snx_tr("Recompile"), labels, app->dests.count, app->dests.focus);
}
static void launch(App* app, char* absolute, bool prepared);
/* choice i taken (the run goes, its game in that folder), or none (-1: Back, nothing runs) */
static void dest_take(App* app, int i) {
    const char* dirs[GUI_FOLDERS_MAX + 1];
    char* file = app->dest_file;
    int item = app->menu.selected, n = dest_dirs(app, dirs);
    app->dests_shown = false;
    app->dest_file = NULL;
    if (i < 0 || i >= n) {
        free(file);
        return;
    }
    free(app->dest_out);
    app->dest_out = snx_strdup(dirs[i]);
    app->recompile = app->dest_recompile;
    app->menu.selected = app->dest_item;
    launch(app, file, app->dest_prepared);
    app->menu.selected = item;
    app->recompile = false;
}
static void dests_key(App* app, GuiKey key) {
    switch (gui_choice_key(&app->dests, key)) {
        case GUI_CHOICE_TAKE: dest_take(app, app->dests.focus); break;
        case GUI_CHOICE_BACK: dest_take(app, -1); break;
        default: break;
    }
}
/* a folder under the pointer takes the focus, a click takes it; a click off the box is Back */
static void dests_pointer(App* app, float x, float y, bool click) {
    GuiChoices c;
    char question[640], text[GUI_FOLDERS_MAX + 1][512];
    const char* labels[GUI_FOLDERS_MAX + 1];
    bool on;
    int i;
    dests_place(app, &c, question, sizeof question, text, labels);
    if ((i = choices_hit(&c, x, y, &on)) >= 0) {
        app->dests.focus = i;
        if (click) dest_take(app, i);
    } else if (click && !on)
        dest_take(app, -1);
}
static void launch(App* app, char* absolute, bool prepared) {
    if (!absolute || !snx_is_file(absolute)) {
        snprintf(app->message, sizeof(app->message), "%s",
                 snx_tr("That path is not a readable file. Try again or press ESC."));
        free(absolute);
        return;
    }
    /* The 3DS browser lists CIA inputs, not prepared host executables. */
    if (app->menu.selected == GUI_3DS) prepared = false;
    bool native = prepared && app->menu.selected == GUI_SWITCH;
    /* a game in a folder of its own goes through the driver, which recompiles it first when another build made it; one
       an older version laid out beside the others runs as it is, as every game on iOS, which runs no other program */
#ifdef SDL_PLATFORM_IOS
    bool direct = native && !app->recompile;
#else
    bool direct = native && !app->recompile && !gui_game_folder(absolute);
#endif
    char* driver = direct ? NULL : find_driver(app, app->menu.selected);
    if (!direct && !driver) {
        snprintf(app->message, sizeof(app->message), snx_tr("Cannot find %s beside the GUI or on PATH."),
                 driver_name(app->menu.selected) ? driver_name(app->menu.selected) : snx_tr("the selected driver"));
        say_failure(app, app->message);
        free(absolute);
        return;
    }
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
    if (direct) {
        if (play(app, absolute) && app->recent_file && !gui_recent_add(&app->recent, absolute, GUI_SWITCH, true))
            recent_changed(app);
        free(absolute);
        return;
    }
#endif
    /* the folders picked in Settings > App > Folders, which a Switch or 3DS run works in: refused when one cannot be */
    bool folders = (app->menu.selected == GUI_SWITCH && !native) || app->menu.selected == GUI_3DS;
    if (folders && !places_fit(app, app->message, sizeof app->message)) {
        free(driver);
        free(absolute);
        return;
    }
    /* a dump made into the folder the player picks, when the recompiled library has more than one that can take it */
    if (app->menu.selected == GUI_SWITCH && !prepared && !app->dest_out) {
        const char* dirs[GUI_FOLDERS_MAX + 1];
        if (dest_dirs(app, dirs) > 1) {
            dests_open(app, absolute, prepared);
            free(driver);
            return;
        }
    }
    /* a dump not extracted yet may need more room than its disks have: asked first, Run anyway pressed twice */
    if (folders && !app->room_ok && !room_fits(app, absolute)) {
        free(app->room_file);
        app->room_file = absolute;
        app->room_item = app->menu.selected;
        app->room_prepared = prepared;
        app->room_recompile = app->recompile;
        gui_confirm_open(&app->confirm, GUI_ASK_ROOM, true);
        free(driver);
        return;
    }
    /* the driver, the file, the settings, --repo and its folder, the picked games and extraction folders, NULL */
    GuiSettings* s = settings_of(app);
    const char** argv = (const char**)snx_alloc((size_t)(s->count + 16) * sizeof(*argv));
    char* out = NULL; /* a dump's extraction in the picked extractions folder */
    size_t n = 1;
    argv[0] = direct ? absolute : driver;
    if (app->recompile) argv[n++] = "-r";
    if (!direct) argv[n++] = absolute;
    /* Switch options only, never guest argv for the CPU-only Linux driver or the 3DS one; R's recompile takes no
       option of a run. The drivers work in the menu's data folder, so they list the same games */
    if (app->recompile && app->menu.selected == GUI_SWITCH && gui_settings_tmpfs(s)) argv[n++] = gui_settings_tmpfs(s);
    if (!direct && app->menu.selected == GUI_SWITCH) n += gui_settings_recompile(s, argv + n);
    /* B's pack: the cache the profile run left, and the level and hook its key was made with. -O after the
       settings' --opt= so the matched level is the one the build uses */
    if (app->aot_pack && app->aot_cache && app->menu.selected == GUI_SWITCH) {
        argv[n++] = "--jit-cache";
        argv[n++] = app->aot_cache;
        argv[n++] = "-O";
        argv[n++] = app->aot_opt;
        if (app->aot_hook) argv[n++] = "--hook-direct-calls";
    }
    if (app->menu.selected == GUI_SWITCH && !app->recompile) n += gui_settings_options(s, 1, argv + n);
    /* Extract game container, which both drivers take, for a run and for R's recompile: off, nothing is extracted */
    if ((app->menu.selected == GUI_3DS || (app->menu.selected == GUI_SWITCH && app->recompile)) &&
        gui_settings_extract(s))
        argv[n++] = gui_settings_extract(s);
    if (folders || (native && !direct)) {
        argv[n++] = "--repo";
        argv[n++] = app->data_dir;
    }
    /* a CIA's game is a folder of the recompiled library as a Switch game is, named after its title ID, its saves in
       it */
    if (folders && (app->place[GUI_PLACE_GAMES] || app->dest_out)) {
        argv[n++] = "--outdir";
        argv[n++] = app->dest_out ? app->dest_out : app->games_dir;
    }
    /* the picked extractions folder, which Extract to tmpfs leaves for the temporary one, and a Switch dump read in
       place (Extract game container off) has no use for */
    if (folders && app->place[GUI_PLACE_WORK] && app->menu.selected == GUI_SWITCH && !nro_file(absolute) &&
        !gui_settings_tmpfs(s) && gui_settings_extract(s)) {
        out = dump_out(app, absolute, NULL); /* an NRO has no extraction */
        argv[n++] = "--out";
        argv[n++] = out;
    }
    argv[n] = NULL;
#ifndef SDL_PLATFORM_ANDROID /* there the data folder's config/, which every game reads (gamepad.c) */
    pad_mappings_share(app);
#endif
    bool started = gui_process_start(app->process, argv, app->message, sizeof(app->message));
    if (started && app->run_log) {
        size_t k;
        if (app->run_out) fclose(app->run_out);
        app->run_out = snx_fopen(app->run_log, "wb");
        for (k = 0; app->run_out && k < n; ++k) fprintf(app->run_out, "%s%s", k ? " " : "$ ", argv[k]);
        if (app->run_out) fputc('\n', app->run_out);
    }
    free(argv);
    free(out);
    free(app->dest_out); /* the run's, which argv held until here: a next one asks again */
    app->dest_out = NULL;
    gui_steps_reset(&app->steps);
    app->native = native;
    /* the menu fades out, then the run fades in */
    Uint64 now = SDL_max(now_ms(app), 1);
    app->entering_ms = started && app->theme.fade_out_ms ? now : 0;
    app->fade_ms = started ? now + (app->entering_ms ? app->theme.fade_out_ms : 0) : 0;
    app->leaving_ms = 0;
    free(driver);
    if (!started) {
        free(absolute);
        return;
    }
    free(app->file);
    app->file = absolute;
    if (app->recent_file && !gui_recent_add(&app->recent, absolute, app->menu.selected, prepared)) recent_changed(app);
    app->recent_named = 0;
    app->menu.view = GUI_LOAD_NONE;
    app->menu.running = true;
    app->finished = false;
    app->abnormal_exit = false;
    app->message[0] = 0;
    gui_console_init(&app->console, app->cols - 2, app->rows - 8);
    app->has_output = true;
    layout(app);
#ifdef SDL_PLATFORM_ANDROID
    /* now, while the menu shows: Android 12 starts no notification's service for an app behind others */
    app->told_ms = 0;
    tell(app);
#endif
}
/* the answer to a run's room: Run anyway, pressed twice, runs it as it was asked; Back runs nothing */
static void room_answer(App* app, bool taken) {
    char* file = app->room_file;
    int item = app->menu.selected;
    app->room_file = NULL;
    if (!taken) {
        free(file);
        free(app->dest_out); /* not run: a next one asks where again */
        app->dest_out = NULL;
        return;
    }
    app->room_ok = true;
    app->recompile = app->room_recompile;
    app->menu.selected = app->room_item;
    launch(app, file, app->room_prepared);
    app->menu.selected = item;
    app->room_ok = false;
    app->recompile = false;
}
/* a recent game, run as the menu would: its driver, the options the menu has now */
static void open_recent(App* app, int index) {
    GuiRecentGame game = app->recent.game[index];
    if (!snx_is_file(game.path)) {
        snprintf(app->message, sizeof(app->message), snx_tr("%s is gone (%s): it left the recent games."), game.name,
                 game.path);
        gui_recent_remove(&app->recent, index);
        recent_changed(app);
        return;
    }
    int item = app->menu.selected;
    app->menu.selected = game.driver;
    launch(app, snx_strdup(game.path), game.prepared);
    app->menu.selected = item;
}
/* a game of Games' page, at index of the list: a CIA is the 3DS driver's, anything else a Switch game's, ready to run
   or a dump to prepare */
static void open_game(App* app, size_t index) {
    int item = app->menu.selected;
    if (gui_games_heading(&app->games, index)) return; /* a group's heading runs nothing */
    if (app->games.addons[index]) { /* its updates and DLCs: which one runs is asked first */
        addons_open(app, app->games.paths[index], app->games.shown[index]);
        if (app->addons_shown) return;
    }
    app->menu.selected = gui_is_cia(app->games.names[index]) ? GUI_3DS : GUI_SWITCH;
    launch(app, snx_absolute(app->games.paths[index]), !gui_is_dump(app->games.names[index]));
    if (!app->menu.running) app->menu.selected = item;
}
/* R on Games: the focused Switch game recompiled where it is, or its dump prepared again, playing nothing; else why
   not */
static void recompile_focus(App* app) {
    size_t row = app->browser.selected, head = app->browser.head;
    size_t index = app->browser.page * app->browser.page_size + row - head;
    bool recent = row && row < head && (int)row - 1 < recent_shown(app),
         game = row >= head && index < app->games.count && !gui_games_heading(&app->games, index);
    if (!recent && !game)
        snprintf(app->message, sizeof app->message, "%s", snx_tr("No game in focus to recompile."));
    else if (recent ? app->recent.game[row - 1].driver != GUI_SWITCH : gui_is_cia(app->games.names[index]))
        snprintf(app->message, sizeof app->message, "%s", snx_tr("R recompiles Switch games only."));
    else {
        app->recompile = true;
        if (recent)
            open_recent(app, (int)row - 1);
        else
            open_game(app, index);
        app->recompile = false;
    }
}
/* the focused prepared Switch game: its program path, and the folder and name the runtime's cache is under */
typedef struct {
    const char* path;
    char* folder;
    char* name;
} AotFocus;
static char* aot_leaf(const char* folder) {
    const char* leaf = folder + strlen(folder);
    while (leaf > folder && leaf[-1] != '/' && leaf[-1] != '\\') --leaf;
    return snx_strdup(leaf);
}
static int aot_focus_game(App* app, AotFocus* f) {
    size_t row = app->browser.selected, head = app->browser.head;
    size_t index = app->browser.page * app->browser.page_size + row - head;
    bool recent = row && row < head && (int)row - 1 < recent_shown(app), game = row >= head && index < app->games.count;
    const char* path = NULL;
    f->path = NULL;
    f->folder = f->name = NULL;
    if (recent) {
        if (app->recent.game[row - 1].driver != GUI_SWITCH) return 0;
        path = app->recent.game[row - 1].path;
    } else if (game) {
        if (gui_games_heading(&app->games, index) || gui_is_cia(app->games.names[index])) return 0; /* no path */
        path = app->games.paths[index];
    } else {
        return 0;
    }
    if (!gui_game_folder(path)) return 0;
    f->folder = snx_dirname(path);
    f->name = f->folder ? aot_leaf(f->folder) : NULL;
    {
        /* the base program, not an add-on's: one pack for the game, the add-ons run from it */
        char* file = aot_leaf(path);
        int base = f->folder && f->name && file && !strcmp(file, f->name);
        free(file);
        if (!base) {
            free(f->folder);
            free(f->name);
            f->folder = f->name = NULL;
            return 0;
        }
    }
    f->path = path;
    return 1;
}
/* the launcher's AOT optimization: the row's pick, else its .ini default, 2 */
static const char* aot_build_opt(App* app) {
    GuiSettings* s = settings_of(app);
    const char* value;
    int i;
    if (!s) return "2";
    value = gui_settings_value(s, "opt");
    if (value && value[0]) return value;
    for (i = 0; i < s->count; ++i)
        if (!s->values[i].kind && s->values[i].key && !strcmp(s->values[i].key, "opt"))
            return s->values[i].def && s->values[i].def[0] ? s->values[i].def : "2";
    return "2";
}
static void aot_clear_focus(App* app) {
    free(app->aot_focus);
    app->aot_focus = NULL;
    app->aot_state = GUI_AOT_NONE;
    app->aot_checked = 0;
    app->aot_match = 0;
    memset(&app->aot_info, 0, sizeof app->aot_info);
}
/* the focused game's cache, for the side panel and the B key: looked at again when the focus moves or a second passes */
static void aot_status_update(App* app) {
    AotFocus f = {0};
    GuiAotInfo info = {0};
    char* cache = NULL;
    const char* opt;
    int state;
    if (!aot_focus_game(app, &f)) {
        aot_clear_focus(app);
        return;
    }
    if (app->aot_focus && !strcmp(app->aot_focus, f.path) && now_ms(app) - app->aot_checked < 1000) {
        free(f.folder);
        free(f.name);
        return;
    }
    free(app->aot_focus);
    app->aot_focus = snx_strdup(f.path);
    app->aot_checked = now_ms(app);
    state = GUI_AOT_NO_CACHE;
    cache = gui_aot_cache_path(f.folder, f.name);
    if (cache && snx_is_file(cache)) state = gui_aot_cache_info(cache, &info) ? GUI_AOT_CACHE : GUI_AOT_BAD;
    free(cache);
    app->aot_info = info;
    app->aot_state = state;
    app->aot_match = 0;
    if (state == GUI_AOT_CACHE) {
        opt = aot_build_opt(app);
        app->aot_build_opt = opt && opt[0] ? opt[0] : '2';
        app->aot_match = gui_aot_levels_match(opt, info.opt);
    }
    free(f.folder);
    free(f.name);
}
/* B on Games: build the focused game's AOT pack from its JIT cache, with the same optimization and hook the cache
   was made with; the game's AOT level must be that level too, or the two kinds of code would not be coherent */
static void aot_focus(App* app) {
    AotFocus f = {0};
    GuiAotInfo info = {0};
    char* cache = NULL;
    const char* opt;
    char build;
    if (!aot_focus_game(app, &f)) {
        snprintf(app->message, sizeof app->message, "%s",
                 snx_tr("B builds an AOT pack for a prepared Switch game in focus."));
        goto done;
    }
    cache = gui_aot_cache_path(f.folder, f.name);
    if (!cache || !snx_is_file(cache)) {
        snprintf(app->message, sizeof app->message, "%s",
                 snx_tr("No JIT cache for this game yet. Play it once to collect one."));
        goto done;
    }
    if (!gui_aot_cache_info(cache, &info)) {
        snprintf(app->message, sizeof app->message, "%s",
                 snx_tr("The JIT cache is unreadable. Play it once to rebuild it."));
        goto done;
    }
    opt = aot_build_opt(app);
    build = opt && opt[0] ? opt[0] : '2';
    if (!gui_aot_levels_match(opt, info.opt)) {
        snprintf(app->message, sizeof app->message,
                 snx_tr("The JIT cache is O%c, but the game's AOT optimization is O%c. They must match: set "
                        "Optimization level to O%c before building, or set JIT optimization to O%c and play once to "
                        "make a matching cache."),
                 info.opt, build, info.opt, build);
        goto done;
    }
    app->aot_pack = true;
    free(app->aot_cache);
    app->aot_cache = cache;
    cache = NULL;
    app->aot_opt[0] = build;
    app->aot_opt[1] = 0;
    app->aot_hook = info.hook;
    app->recompile = true;
    {
        int item = app->menu.selected;
        app->menu.selected = GUI_SWITCH;
        launch(app, snx_absolute(f.path), true);
        app->menu.selected = item;
    }
    app->recompile = false;
    if (!app->menu.running) {
        app->aot_pack = false;
        free(app->aot_cache);
        app->aot_cache = NULL;
    }
done:
    free(cache);
    free(f.folder);
    free(f.name);
}
/* M on Games: the focused game's mods folder opened, made first when it isn't there; else why not */
static void mods_focus(App* app) {
    size_t row = app->browser.selected, head = app->browser.head;
    size_t index = app->browser.page * app->browser.page_size + row - head;
    bool recent = row && row < head && (int)row - 1 < recent_shown(app), game = row >= head && index < app->games.count;
    char* folder = NULL;
    if (recent && app->recent.game[row - 1].driver == GUI_SWITCH)
        gui_game_mods(app->recent.game[row - 1].path, &folder, NULL);
    else if (game)
        gui_game_mods(app->games.paths[index], &folder, NULL);
    if (folder)
        show_folder(app, folder);
    else
        snprintf(app->message, sizeof app->message, "%s",
                 snx_tr("M opens the mods folder of a recompiled Switch game in focus."));
    free(folder);
}
#define SNX_GUI_KEYS_MAX (1 << 20) /* a prod.keys is a few kilobytes: past this it is not one */
/* the bytes of file (a path, or on Android a document's URI, which SDL opens through its app), up to SNX_GUI_KEYS_MAX;
   NULL when it cannot be read or is bigger */
static char* keys_read(const char* file, size_t* size) {
    SDL_IOStream* io = SDL_IOFromFile(file, "rb");
    char* data = io ? (char*)malloc(SNX_GUI_KEYS_MAX + 1) : NULL;
    size_t n = 0, got = 1;
    while (data && n <= SNX_GUI_KEYS_MAX && got) n += got = SDL_ReadIO(io, data + n, SNX_GUI_KEYS_MAX + 1 - n);
    if (data && (n > SNX_GUI_KEYS_MAX || SDL_GetIOStatus(io) == SDL_IO_STATUS_ERROR)) {
        free(data);
        data = NULL;
    }
    if (io) SDL_CloseIO(io);
    *size = n;
    return data;
}
/* the keys read into keys_path */
/* what installing keys did, on their screen in app->message until it goes back to the menu by itself */
static void keys_result(App* app, bool ok) {
    if (app->menu.view != GUI_LOAD_KEYS) return;
    app->keys_ok = ok;
    app->keys_done = now_ms(app) ? now_ms(app) : 1;
}
static void keys_write(App* app, const char* data, size_t size) {
    char why[256];
    if (!app->keys_path) { /* snx_keys_dir's: the variables it reads are unset */
#ifdef _WIN32
        snprintf(app->message, sizeof(app->message), "%s",
                 snx_tr("Cannot install prod.keys: neither USERPROFILE nor HOME is set"));
#else
        snprintf(app->message, sizeof(app->message), "%s", snx_tr("Cannot install prod.keys: HOME is not set"));
#endif
        keys_result(app, false); /* the keys' screen says it whole in its box, Not installed over it */
        return;
    }
    /* staged beside them first: a failed copy (or dropping the installed file itself) must not destroy existing keys */
    bool ok = !snx_keys_put(app->keys_path, data, size, why, sizeof why);
    if (ok)
        snprintf(app->message, sizeof(app->message), "%s", snx_tr("prod.keys installed: games can be extracted now."));
    else
        snprintf(app->message, sizeof(app->message), snx_tr("Cannot install %s: %s"), app->keys_path, why);
    keys_result(app, ok); /* the keys' screen says it whole in its box */
}
static void install_keys(App* app, const char* source) {
    const char* name = source;
    char* data = NULL;
    size_t size = 0;
    if (source)
        for (const char* p = source; *p; ++p)
            if (*p == '/' || *p == '\\') name = p + 1;
    if (source && !strcmp(name, "prod.keys") && snx_is_file(source)) data = keys_read(source, &size);
    if (!data) {
        snprintf(app->message, sizeof(app->message), "%s", snx_tr("Please drop a readable file named prod.keys."));
        keys_result(app, false);
    } else
        keys_write(app, data, size);
    free(data);
}
/* what a picked file that cannot be used says, for the platform's picker: Android's documents, a desktop's files */
#ifdef SDL_PLATFORM_ANDROID
#define SAY_KEYS_UNREAD                                                                                                \
    SNX_TR_NOOP("This file could not be read. Pick prod.keys again, from the device's storage (%s).")
#define SAY_KEYS_NOT SNX_TR_NOOP("This file is not a prod.keys. Pick the prod.keys file (%s).")
#define SAY_PICK_UNREAD                                                                                                \
    SNX_TR_NOOP("Cannot read this file by its path. Check that ScarletNX may access the device's files in Android's "  \
                "settings, then pick it from the device's storage (%s).")
#else
#define SAY_KEYS_UNREAD SNX_TR_NOOP("The picked file could not be read as prod.keys (%s).")
#define SAY_KEYS_NOT    SNX_TR_NOOP("The picked file is not a prod.keys (%s).")
#define SAY_PICK_UNREAD SNX_TR_NOOP("Pick the file from this computer's storage (%s).")
#endif
/* keys the picker gave as a document of an app's (Android's), read through it: a path to it may be one this app cannot
   read, or there is none (Downloads by number, #280). With no name to go by, its text has to be a prod.keys' */
static void install_keys_document(App* app, const char* uri) {
    size_t size = 0;
    char* data = keys_read(uri, &size);
    if (!data)
        snprintf(app->message, sizeof(app->message), snx_tr(SAY_KEYS_UNREAD), uri);
    else if (!gui_keys_text(data, size))
        snprintf(app->message, sizeof(app->message), snx_tr(SAY_KEYS_NOT), uri);
    else
        keys_write(app, data, size);
    if (!data || !gui_keys_text(data, size)) keys_result(app, false);
    free(data);
}
static void drop(App* app, const char* value, bool text_drop) {
    if ((app->menu.view != GUI_LOAD_DROP && app->menu.view != GUI_LOAD_KEYS) || app->menu.running || app->finished)
        return;
    char* path = gui_drop_path(value, text_drop);
    if (!path) {
        snprintf(app->message, sizeof(app->message), "%s",
                 snx_tr("Drop one local file path, not a list or remote URL."));
        keys_result(app, false);
        return;
    }
    char* absolute = snx_absolute(path);
    free(path);
    if (app->menu.view == GUI_LOAD_KEYS) {
        install_keys(app, absolute);
        free(absolute);
    } else {
        /* Hovering another main item must not change the driver for this drop. */
        app->menu.selected = app->menu.active;
        launch(app, absolute, false);
    }
}
/* the system's file picker answers on its own thread: the choice comes back to event() as an app->picked event */
static void SDLCALL picker_done(void* userdata, const char* const* files, int filter) {
    SDL_Event e;
    (void)filter;
    SDL_zero(e);
    e.type = *(const Uint32*)userdata;
    e.user.data1 = files && files[0] ? SDL_strdup(files[0]) : NULL;
    SDL_PushEvent(&e);
}
static void picked(App* app, const char* uri);
static void open_picker(App* app) {
    app->keys_done = 0; /* another try: the last one's outcome no longer stands */
    if (!app->picked) app->picked = SDL_RegisterEvents(1);
#ifdef SDL_PLATFORM_IOS /* SDL has no file dialog there: the document picker (dist/apple) */
    if (app->picked) gui_apple_pick(app->window, 0, picker_done, &app->picked, app->message, sizeof(app->message));
#else
    if (app->picked) SDL_ShowOpenFileDialog(picker_done, &app->picked, app->window, NULL, 0, NULL, false);
#endif
}
/* the last max bytes of a file, as text with only ASCII (what Java takes as a string from any log), or "". A CR before
   an LF goes (a log on Windows has CRLF lines), so a line ends where its text does */
static char* log_tail(const char* path, size_t max) {
    size_t size = 0, i, n = 0;
    unsigned char* data = path ? snx_read_file(path, &size) : NULL;
    char* text = (char*)snx_alloc(SDL_min(size, max) + 1);
    for (i = 0; i < SDL_min(size, max); ++i) {
        size_t at = size - SDL_min(size, max) + i;
        unsigned char c = data[at];
        if (c == '\r' && at + 1 < size && data[at + 1] == '\n') continue;
        text[n++] = c == '\n' || c == '\t' || (c >= 32 && c < 127) ? (char)c : '?';
    }
    text[n] = 0;
    free(data);
    return text;
}
/* the game log written last, games/<game>/<game>.log, or an older version's games/<game>.log: beside the games, or an
   installed build's in its state folder */
static char* newest_log(const App* app) {
    char *folder = snx_user_path(app->games_dir, SNX_USER_STATE), *newest = NULL, **names = NULL;
    size_t count = 0, i, n;
    SDL_Time time = 0;
    if (folder) snx_list_dir(folder, &names, &count);
    for (i = 0; i < count; ++i) {
        SDL_PathInfo info;
        char *path = snx_path_join(folder, names[i]), *log = NULL;
        n = strlen(names[i]);
        if (path && snx_is_dir(path)) { /* a game's folder: its own log */
            char* own = snx_format("%s.log", names[i]);
            log = snx_path_join(path, own);
            free(own);
        } else if (n > 4 && !strcmp(names[i] + n - 4, ".log")) {
            log = path;
            path = NULL;
        }
        if (log && SDL_GetPathInfo(log, &info) && info.type == SDL_PATHTYPE_FILE &&
            (!newest || info.modify_time > time)) {
            free(newest);
            newest = log;
            log = NULL;
            time = info.modify_time;
        }
        free(log);
        free(path);
    }
    snx_free_strings(names, count);
    free(folder);
    return newest;
}
/* the game log's summary (common/utilities/logscan.h) under "== summary ==", for the top of the shared file: how
   the run ended and where, without the log's last lines, which follow. The file itself, streamed whole: the shared
   copy holds only its end, its header gone from a long one. "" when it can't be made */
static char* log_summary(const char* path) {
    SnxLogScan scan;
    char *markdown, *out;
    FILE* f = snx_fopen(path, "rb");
    int failed = !f || snx_logscan_file(&scan, f);
    if (f) fclose(f);
    if (failed) {
        if (f) snx_logscan_free(&scan);
        return snx_strdup("");
    }
    markdown = snx_logscan_markdown(&scan, "the game log", "###", 1, snx_demangle);
    snx_logscan_free(&scan);
    out = markdown ? snx_format("== summary ==\n%s\n", markdown) : snx_strdup("");
    free(markdown);
    return out;
}
/* the message on the screen, the summary of the newest game log, the last run's output and that log, as scarletnx.log in the data folder's state
   folder (the working folder without one: started from the desktop, that is the home folder), then on Android handed
   to the share sheet, elsewhere its path copied */
static void share_log(App* app) {
    char *game, *run, *played, *summary, *text, *saved, *state = NULL;
    const char* folder;
    bool written;
    folder = app->cwd;
    if (app->data_dir && snx_app_dir(app->data_dir)) state = snx_app_kind_dir(app->data_dir, SNX_USER_STATE);
    if (state && snx_mkdirs(state)) {
        free(state);
        state = NULL;
    }
    if (state) folder = state;
    left_behind(app);
    game = newest_log(app);
    run = log_tail(app->run_log, 1 << 20);
    played = log_tail(game, 4 << 20);
    summary = game ? log_summary(game) : snx_strdup("");
    text = snx_format("%s\nMessage: %s\n%s%s%s%s\n%s== the last run: %s ==\n%s\n\n== %s ==\n%s\n", snx_build_info(),
                      app->message, app->left ? app->left : "", app->left_clear ? SAY_LEFT_CLEAR "\n" : "",
                      app->cleaned.count ? "Clean old settings:\n" : "", app->cleaned.text ? app->cleaned.text : "",
                      summary, app->run_log ? app->run_log : "", run, game ? game : "no game log", played);
    saved = folder ? snx_path_join(folder, "scarletnx.log") : NULL;
    written = saved && !snx_write_file(saved, text, strlen(text));
    if (written) app->log_due = false; /* the crash's log is out */
    if (!saved) {
        snprintf(app->message, sizeof(app->message), "%s", snx_tr("The log could not be saved: no working folder"));
        say_failure(app, app->message);
    } else if (!written) {
        char why[256];
        snprintf(app->message, sizeof(app->message), snx_tr("The log could not be saved to %s: %s"), saved,
                 snx_os_error(why, sizeof why));
        say_failure(app, app->message);
    }
#ifdef SDL_PLATFORM_ANDROID
    else if (gui_android_share(saved, app->message, sizeof(app->message)))
        snprintf(app->message, sizeof(app->message), snx_tr("Log saved to %s"), saved);
#elif defined(SDL_PLATFORM_APPLE)
    else if (gui_apple_share(app->window, saved, app->message, sizeof(app->message)))
        snprintf(app->message, sizeof(app->message), snx_tr("Log saved to %s"), saved);
#else
    else if (SDL_SetClipboardText(saved))
        snprintf(app->message, sizeof(app->message), snx_tr("Log saved to %s (path copied)"), saved);
    else
        snprintf(app->message, sizeof(app->message), snx_tr("Log saved to %s"), saved);
#endif
    free(saved);
    free(text);
    free(summary);
    free(played);
    free(run);
    free(game);
    free(state);
}
/* gui_games_group's made: a dump a game of recompiled/ was made from already, of made_read's */
static bool made_dump(const char* path, const void* made) { return made_program((const MadeGames*)made, path) != NULL; }
/* the Games list: recompiled/, dumps/ (with its subfolders) and the Library's folders, every kind, in two groups: the
   games ready to run, then the dumps not made already. The first folder that cannot be read is named, the others still
   listed */
static void games_scan(App* app, GuiGames* games, char* message, size_t size) {
    int kinds = GUI_GAMES_RUN | GUI_GAMES_CIA | GUI_GAMES_DUMP, i; /* the old games/'s: it held both */
    char error[sizeof app->message];
    MadeGames made;
    char *dumps = app->data_dir ? snx_path_join(app->data_dir, "dumps") : NULL, *old;
    gui_games_free(games);
    message[0] = 0;
    if (!gui_games_add(games, app->games_dir, GUI_GAMES_RUN | GUI_GAMES_CIA, error, sizeof error))
        snprintf(message, size, "%s", error);
    old = games_old(app);
    if (old && !gui_games_add(games, old, kinds, error, sizeof error) && !message[0])
        snprintf(message, size, "%s", error);
    free(old);
    if (dumps && snx_is_dir(dumps) && /* none: places_apply said it could not be made */
        !gui_games_add_deep(games, dumps, GUI_GAMES_DUMP | GUI_GAMES_CIA, GUI_FOLDER_DEPTH, error, sizeof error) &&
        !message[0])
        snprintf(message, size, "%s", error);
    for (i = 0; i < app->folder_count; ++i) /* a library's folder, its library's games */
        if (!gui_games_add_deep(games, app->folders[i],
                                (app->folders_library[i] == GUI_LIBRARY_RECOMPILED ? GUI_GAMES_RUN : GUI_GAMES_DUMP) |
                                    GUI_GAMES_CIA,
                                app->folders_deep[i] ? GUI_FOLDER_DEPTH : 0, error, sizeof error) &&
            !message[0])
            snprintf(message, size, snx_tr("%s It is in Settings > App > Folders."), error);
    made_read(app, &made);
    if (!gui_games_group(games, snx_tr("Recompiled"), snx_tr("Dumps"), made_dump, &made) && !message[0])
        snprintf(message, size, "%s", snx_tr("Not enough memory to list the games in their groups."));
    made_free(&made);
    free(dumps);
}
/* gui_browser_key_skip's skip on Games: a group's heading */
static bool games_heading_at(size_t item, const void* games) { return gui_games_heading((const GuiGames*)games, item); }
/* the games Games lists, its groups' headings left out */
static size_t games_listed(const App* app) {
    size_t i, n = 0;
    for (i = 0; i < app->games.count; ++i) n += !gui_games_heading(&app->games, i);
    return n;
}
/* the names dir holds into h, and with depth those of its folders, down that many levels, hidden ones left alone */
static uint64_t stamp_dir(uint64_t h, const char* dir, int depth) {
    char** names = NULL;
    size_t count = 0, k;
    const char* c;
    if (!dir || snx_list_dir(dir, &names, &count)) count = (size_t)-1; /* none: not the same as an empty one */
    for (k = 0; k < count && names; ++k) {
        for (c = names[k];; ++c) {
            h = (h ^ (unsigned char)*c) * 1099511628211ull;
            if (!*c) break;
        }
        if (depth > 0 && names[k][0] != '.') {
            char* sub = snx_path_join(dir, names[k]);
            if (sub && snx_is_dir(sub)) h = stamp_dir(h, sub, depth - 1);
            free(sub);
        }
    }
    h = (h ^ (uint64_t)count) * 1099511628211ull;
    if (names) snx_free_strings(names, count);
    return h;
}
/* the names recompiled/ and the player's folders hold (a folder kept with its subfolders, theirs too), hashed: a game
   put in, taken out or renamed changes it, and so does a folder that comes or goes */
static uint64_t games_stamp(const App* app) {
    uint64_t h = 1469598103934665603ull; /* FNV-1a */
    int i;
    char* dumps = app->data_dir ? snx_path_join(app->data_dir, "dumps") : NULL;
    for (i = -1; i < app->folder_count; ++i)
        h = stamp_dir(h, i < 0 ? app->games_dir : app->folders[i],
                      i >= 0 && app->folders_deep[i] ? GUI_FOLDER_DEPTH : 0);
    h = stamp_dir(h, dumps, GUI_FOLDER_DEPTH);
    free(dumps);
    dumps = games_old(app);
    if (dumps) h = stamp_dir(h, dumps, 0);
    free(dumps);
    return h;
}
static void list_games(App* app) {
    icons_free(app); /* a game made since has one now */
    games_scan(app, &app->games, app->message, sizeof app->message);
    app->games_stamp = games_stamp(app);
    memset(&app->browser, 0, sizeof(app->browser));
    app->browser.count = app->games.count;
    app->browser.head = games_head(app);
    layout(app);
}
/* the screen a tab opens: the menu, Games or the Settings. A theme edited and not saved keeps its screen, the note
   saying why */
static void go_tab(App* app, int tab) {
    if (app->editing) {
        if (app->editor.typing || tab == GUI_TAB_SETTINGS) return;
        if (app->form == GUI_FORM_THEME && app->editor.dirty) {
            snprintf(app->editor.note, sizeof app->editor.note, "%s",
                     snx_tr("Not saved: Ctrl+S saves it, ESC twice leaves without it"));
            return;
        }
        keep_settings(app);
        if (app->form == GUI_FORM_PAD) pad_close(app);
        device_stop(app);
        leave_editor(app);
        app->theme_from_app = false;
    }
    if (tab == tab_shown(app)) return;
    app->message[0] = 0;
    app->menu.view = tab == GUI_TAB_GAMES ? GUI_LOAD_GAMES : GUI_LOAD_NONE;
    if (tab == GUI_TAB_GAMES)
        list_games(app);
    else if (tab == GUI_TAB_SETTINGS)
        open_settings(app);
}
/* the tab step tabs from the one shown, round the three: Tab and Shift+Tab, a pad's shoulders */
static void screen_tab(App* app, int step) {
    if (app->menu.running || app->finished || app->confirm.shown || app->splash) return;
    go_tab(app, ((tab_shown(app) + step) % GUI_TABS + GUI_TABS) % GUI_TABS);
}
static void menu_key(App* app, GuiKey key) {
    GuiLoadView before = app->menu.view; /* the screen this key was pressed on */
    if (app->finished) {
        if (key == GUI_CANCEL && app->theme.fade_out_ms && !app->leaving_ms)
            app->leaving_ms = SDL_max(now_ms(app), 1); /* the run fades out first; pump() comes back */
        else if (key == GUI_CANCEL)
            leave_run(app);
        return;
    }
    if (app->menu.view == GUI_LOAD_GAMES) {
        /* Resizes and key events can arrive in the same SDL poll batch. */
        layout(app);
        GuiBrowserAction action = gui_browser_key_skip(&app->browser, key, games_heading_at, &app->games);
        size_t row = app->browser.selected;
        if (action == GUI_BROWSER_BACK) {
            app->menu.view = GUI_LOAD_NONE;
            app->message[0] = 0;
        } else if (action == GUI_BROWSER_HEAD && !row)
            open_folder_picker(app);
        else if (action == GUI_BROWSER_HEAD && (int)row - 1 < recent_shown(app))
            open_recent(app, (int)row - 1);
        else if (action ==
                 GUI_BROWSER_HEAD) { /* ...show more: every recent game, the focus on the first that was hidden */
            app->recent_all = true;
            app->browser.head = games_head(app);
            layout(app);
        } else if (action == GUI_BROWSER_OPEN) {
            size_t index = app->browser.page * app->browser.page_size + row - app->browser.head;
            if (index < app->games.count) open_game(app, index);
        }
        return;
    }
    if (pick_screen(app) && key != GUI_SELECT && key != GUI_CANCEL) return; /* nothing there to move to */
    switch (gui_menu_key(&app->menu, key)) {
        case GUI_MENU_SETTINGS: open_settings(app); break;
        case GUI_MENU_LOG: share_log(app); break;
#ifdef SDL_PLATFORM_ANDROID
        case GUI_MENU_DATA: gui_android_open_data(app->message, sizeof app->message); break;
#endif
        default: break;
    }
    if (key == GUI_CANCEL || (key == GUI_SELECT && pick_screen(app))) app->message[0] = 0;
    /* the screen opens first, so what happens shows there; a select on it (a tap in its box) opens the picker, whose
       answer may come at once */
    if (key == GUI_SELECT && app->menu.view == before && (before == GUI_LOAD_DROP || before == GUI_LOAD_KEYS))
        open_picker(app);
    if (app->menu.view == GUI_LOAD_GAMES) list_games(app);
    /* Back or ESC with nothing open to close: on Android the app goes behind, as an app's first screen does; on a
       computer the menu asks before it closes, as rEden does */
    if (key == GUI_CANCEL && before == GUI_LOAD_NONE && app->menu.view == GUI_LOAD_NONE && !app->menu.running) {
#ifdef SDL_PLATFORM_ANDROID
        gui_android_background(app->message, sizeof(app->message));
#else
        gui_confirm_open(&app->confirm, GUI_ASK_EXIT, false);
#endif
    }
}
static void abort_run(App* app) {
    if (app->aborting) return;
    /* Do not ask a hung program to shut down or wait on it in the event
       handler. Kill its tree now and let pump reap it without blocking. */
    gui_process_terminate(app->process);
    app->aborting = true;
}
/* a key on the question shown (gui_confirm_key); answered, what it asked about: the menu closed, a run aborted or
   started, a folder added or used as the choice says */
static void ask_key(App* app, GuiKey key) {
    GuiAsk ask = app->confirm.ask;
    bool second;
    if (ask == GUI_ASK_ERROR) { /* its one choice: OK or Back close it, the focus never leaves it */
        if (key == GUI_SELECT || key == GUI_CANCEL) app->confirm.shown = false;
        return;
    }
    second = gui_confirm_key(&app->confirm, key);
    if (app->confirm.shown) return;
    if (ask == GUI_ASK_DEEP)
        folder_answer(app, key == GUI_SELECT, second);
    else if (ask == GUI_ASK_OUTSIDE)
        outside_answer(app, second);
    else if (ask == GUI_ASK_ROOM)
        room_answer(app, second);
    else if (ask == GUI_ASK_TEXT)
        size_answer(app, second);
    else if (ask == GUI_ASK_CLEAN || ask == GUI_ASK_RESET || ask == GUI_ASK_PLACES || ask == GUI_ASK_PLACE ||
             ask == GUI_ASK_FOLDER || ask == GUI_ASK_MOVE)
        change_answer(app, ask, second);
    else if (ask == GUI_ASK_CLOSE && second) { /* the run ends with the menu */
        abort_run(app);
        closing_start(app);
    } else if (ask == GUI_ASK_EXIT && second)
        closing_start(app);
    else if (ask == GUI_ASK_ABORT && second)
        abort_run(app);
}
/* the pointer on the question, at pixel x, y: a choice under it takes the focus, a click presses it; a click off the
   box is Back */
static void ask_pointer(App* app, float x, float y, bool click) {
    GuiChoices c;
    const char* labels[2];
    char second[96];
    bool on;
    int i;
    ask_place(app, &c, labels, second, sizeof second);
    if ((i = choices_hit(&c, x, y, &on)) >= 0) {
        gui_confirm_focus(&app->confirm, i);
        if (click) ask_key(app, GUI_SELECT);
    } else if (click && !on)
        ask_key(app, GUI_CANCEL);
}
/* whether pixel x, y is on the head's user */
static bool user_hit(App* app, float x, float y) {
    GuiPlace p;
    place_screen(app, &p, "", 0, GUI_SHOWS_TABS);
    return p.badge.h && x >= (float)p.badge.x && x < (float)(p.badge.x + p.badge.w) && y >= (float)p.badge.y &&
           y < (float)(p.badge.y + p.badge.h);
}
/* the tab at pixel x, y, -1 for none */
static int tab_hit(App* app, float x, float y) {
    GuiPlace p;
    int i;
    place_screen(app, &p, "", 0, GUI_SHOWS_TABS);
    for (i = 0; i < GUI_TABS; ++i) {
        GuiRect r = tab_rect(app, p.tabs, i);
        if (x >= (float)r.x && x < (float)(r.x + r.w) && y >= (float)r.y && y < (float)(r.y + r.h)) return i;
    }
    return -1;
}
/* the pointer on a screen, at pixel x, y: the box under it takes the focus, and a click selects it; on the
   screen a file is picked on, a click on the box the waiting line is in opens the picker */
static void screen_pointer(App* app, float x, float y, bool click, bool touch) {
    GuiMenuBoxes m;
    int i;
    (void)touch; /* a finger takes a row anywhere on it, as a click does: the scenes' rows are whole */
    if ((i = tab_hit(app, x, y)) >= 0) {
        if (click) go_tab(app, i);
        return;
    }
    if (user_hit(app, x, y)) {
        if (click) users_open(app);
        return;
    }
    if (app->menu.view == GUI_LOAD_GAMES) {
        GuiGamesBoxes g;
        size_t row;
        uint32_t hit;
        games_boxes(app, &g);
        hit = ui_games_hit(app, g.rects, app->scale, g.lists, (int)x, (int)y, click ? UI_HIT_CLICK : 0, &i, &row);
        if (hit) ui_games_act(app, i, row, hit);
        return;
    }
    if (pick_screen(app)) {
        GuiPickBoxes k;
        pick_boxes(app, &k);
        if (click && y >= (float)k.place.list.y && y < (float)(k.place.list.y + k.place.list.h) &&
            x >= (float)k.place.list.x && x < (float)(k.place.list.x + k.place.list.w))
            menu_key(app, GUI_SELECT); /* the box the waiting line is in */
        return;
    }
    menu_boxes(app, &m);
    {
        size_t row;
        uint32_t hit =
            ui_menu_hit(app, m.rects, app->scale, m.lists, (int)x, (int)y, click ? UI_HIT_CLICK : 0, &i, &row);
        if (hit) ui_menu_act(app, i, row, hit);
    }
}
/* an entry of the menu hit: the focus on it, and a click activates it */
static void menu_entry(App* app, int item, uint32_t hit) {
    app->menu.selected = item;
    if (hit & UI_HIT_CLICK) menu_key(app, GUI_SELECT);
}
void ui_menu_entries_load_switch_nsp_xci_nro_action_cb(App* app, uint32_t hit) { menu_entry(app, GUI_SWITCH, hit); }
void ui_menu_entries_load_linux_arm_elf_action_cb(App* app, uint32_t hit) { menu_entry(app, GUI_LINUX, hit); }
void ui_menu_entries_load_3ds_cia_action_cb(App* app, uint32_t hit) { menu_entry(app, GUI_3DS, hit); }
void ui_menu_entries_setup_device_keys_action_cb(App* app, uint32_t hit) { menu_entry(app, GUI_KEYS, hit); }
void ui_menu_entries_settings_action_cb(App* app, uint32_t hit) { menu_entry(app, GUI_SETTINGS, hit); }
void ui_menu_entries_share_log_action_cb(App* app, uint32_t hit) { menu_entry(app, GUI_LOG, hit); }
#ifdef SDL_PLATFORM_ANDROID
void ui_menu_entries_open_data_folder_action_cb(App* app, uint32_t hit) { menu_entry(app, GUI_DATA, hit); }
#endif
void ui_menu_entries_exit_action_cb(App* app, uint32_t hit) { menu_entry(app, GUI_EXIT, hit); }
/* the keys the foot names, a word each ("Up/Down" is two): what a click or a tap on the word presses. Space selects
   where the foot names it, as Enter; Add a folder's opens the folder picker, as Enter on its row. A word is found by
   its English or its translation, which the foot's sentences keep (scripts/build-languages.py checks them) */
static const struct {
    const char* word;
    SDL_Keycode key;
    SDL_Keymod mod;
} hint_keys[] = {{SNX_TR_KEY("ESC"), SDLK_ESCAPE, 0},
                 {SNX_TR_KEY("Back"), SDLK_AC_BACK, 0},
                 {SNX_TR_KEY("Tab"), SDLK_TAB, 0},
                 {SNX_TR_KEY("Enter"), SDLK_RETURN, 0},
                 {SNX_TR_KEY("Space"), SDLK_RETURN, 0},
                 {SNX_TR_KEY("click"), SDLK_RETURN, 0},
                 {SNX_TR_KEY("Tap"), SDLK_RETURN, 0},
                 {SNX_TR_KEY("A"), SDLK_RETURN, 0},
                 {SNX_TR_KEY("Up"), SDLK_UP, 0},
                 {SNX_TR_KEY("Down"), SDLK_DOWN, 0},
                 {SNX_TR_KEY("Left"), SDLK_LEFT, 0},
                 {SNX_TR_KEY("Right"), SDLK_RIGHT, 0},
                 {SNX_TR_KEY("PgUp"), SDLK_PAGEUP, 0},
                 {SNX_TR_KEY("PgDn"), SDLK_PAGEDOWN, 0},
                 {SNX_TR_KEY("PgDown"), SDLK_PAGEDOWN, 0},
                 {SNX_TR_KEY("Swipe"), SDLK_PAGEDOWN, 0},
                 {SNX_TR_KEY("Del"), SDLK_DELETE, 0},
                 {SNX_TR_KEY("R"), SDLK_R, 0},
                 {SNX_TR_KEY("Recompile"), SDLK_R, 0},
                 {SNX_TR_KEY("B"), SDLK_B, 0},
                 {SNX_TR_KEY("AOT pack"), SDLK_B, 0},
                 {SNX_TR_KEY("M"), SDLK_M, 0},
                 {SNX_TR_KEY("Ctrl+S"), SDLK_S, SDL_KMOD_CTRL},
                 {SNX_TR_KEY("Ctrl+V"), SDLK_V, SDL_KMOD_CTRL},
                 {SNX_TR_KEY("F5"), SDLK_F5, 0},
                 {SNX_TR_KEY("F2"), SDLK_F2, 0},
                 {SNX_TR_KEY("Refresh"), SDLK_F5, 0},
                 {SNX_TR_KEY("Add a folder"), SDLK_UNKNOWN, 0}};
/* the hint_keys entry of the n bytes at word, -1 for none */
static int hint_word(const char* word, size_t n) {
    int i;
    for (i = 0; i < (int)SDL_arraysize(hint_keys); ++i) {
        const char* shown = snx_tr(hint_keys[i].word);
        if ((strlen(hint_keys[i].word) == n && !strncmp(hint_keys[i].word, word, n)) ||
            (strlen(shown) == n && !strncmp(shown, word, n)))
            return i;
    }
    return -1;
}
/* the columns the first n bytes of s take */
static int text_cols(const char* s, size_t n) {
    const char* end = s + n;
    int cols = 0;
    while (s < end && *s) cols += gui_glyph_cols(gui_menu_glyph(gui_utf8_step(&s)));
    return cols;
}
/* the hint_keys entry a click or a tap col columns into the foot's sentence of key (hints()) presses, -1 for none: the
   word of key it is on, the slash after a word its own and the margin before the first's; the sentence's text, and the
   margin after it, what every word does when they all do one thing */
static int hint_pick(const char* key, int col) {
    const char *word = key, *end;
    int k = gui_text_cols(key), pick = -1, first = 1, w;
    for (;; word = end + 1) {
        end = strchr(word, '/');
        if (!end) end = word + strlen(word);
        w = hint_word(word, (size_t)(end - word));
        if (col < k && col <= text_cols(key, (size_t)(end - key))) {
            pick = w;
            break;
        }
        if (col >= k && first)
            pick = w;
        else if (col >= k && (w < 0 || pick < 0 || hint_keys[w].key != hint_keys[pick].key ||
                              hint_keys[w].mod != hint_keys[pick].mod))
            pick = -1;
        first = 0;
        if (!*end) break;
    }
    return pick;
}
/* the key it picks pressed, as on the keyboard; whether there was one */
static bool hint_press(App* app, const char* key, int col) {
    int pick = hint_pick(key, col);
    SDL_Event e;
    if (pick < 0) return false;
    if (hint_keys[pick].key == SDLK_UNKNOWN) { /* Add a folder: on Folders to the focused row's library */
        if (app->editing && app->form == GUI_FORM_FOLDERS) app->adding = folder_row_library(app, app->editor.row);
        open_folder_picker(app);
        return true;
    }
    SDL_zero(e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = hint_keys[pick].key;
    e.key.mod = hint_keys[pick].mod;
    e.key.down = true;
    event(app, &e);
    return true;
}
/* the boxes of the screen shown, and the keys its foot says */
static const char* shown_place(App* app, GuiPlace* p, char* keys, size_t size) {
    if (app->setup_on && !app->editing) {
        GuiSetupBoxes b;
        setup_boxes(app, &b);
        *p = b.place;
        snprintf(keys, size, "%s", snx_tr(setup_keys_said(app)));
        return keys;
    }
    if (app->editing && app->form == GUI_FORM_PAD)
        place_screen(app, p, shown_keys(app, keys, size), 0, GUI_SHOWS_TABS); /* render_pad's */
    else if (app->editing) {
        GuiFormBoxes f;
        form_boxes(app, &f);
        *p = f.place;
    } else if (app->menu.view == GUI_LOAD_GAMES)
        games_place(app, p);
    else if (pick_screen(app)) {
        GuiPickBoxes k;
        pick_boxes(app, &k);
        *p = k.place;
    } else {
        GuiMenuBoxes m;
        menu_boxes(app, &m);
        *p = m.place;
    }
    return shown_keys(app, keys, size);
}
/* a click or a tap at pixel x, y on a key of the foot presses it (hint_press), the keys' box's whole height their rows';
   whether it did */
static bool foot_press(App* app, float x, float y) {
    GuiPlace p;
    GuiRect text;
    GuiHintHit hit;
    char keys[256];
    const char* say = shown_place(app, &p, keys, sizeof keys);
    int s = app->scale, rows;
    if (x < (float)p.keys.x || x >= (float)(p.keys.x + p.keys.w) || y < (float)p.keys.y ||
        y >= (float)(p.keys.y + p.keys.h))
        return false;
    app->scale = foot_scale(app);
    text = foot_text(app, p.keys, app->theme.footer_box);
    rows = hints(app, say, text.w / cell_w(app), text, text.y, 0, 0, NULL);
    hit.x = x;
    hit.y = SDL_clamp(y, (float)text.y, (float)(text.y + rows * cell_h(app) - 1));
    hit.key[0] = 0;
    hints(app, say, text.w / cell_w(app), text, text.y, 0, 0, &hit);
    app->scale = s;
    return hit.key[0] && hint_press(app, hit.key, hit.col);
}
static void pointer(App* app, float window_x, float window_y, bool click, bool touch) {
    float x, y;
    bool picking = app->editing && app->editor.picking;
    if ((app->menu.running && !app->confirm.shown) || app->finished) {
        app->hovering = false; /* a run's foot presses nothing */
        return;
    }
    layout(app);
    if (!SDL_RenderCoordinatesFromWindow(app->renderer, window_x, window_y, &x, &y)) return;
    app->hovering = !click && !touch; /* a finger hovers nowhere, and a click has done what it lit */
    app->hover_x = x;
    app->hover_y = y;
    /* under a question, the folder's too, a click is the question's */
    if (click && !app->confirm.shown && foot_press(app, x, y)) {
        if (touch && picking) app->pick_press = GUI_PICK_ROWS + 1; /* the picker's finger, pressed as it went down */
        return;
    }
    if (app->confirm.shown) /* over a run, the menu, Games or a form */
        ask_pointer(app, x, y, click);
    else if (app->sizes_shown)
        sizes_pointer(app, x, y, click);
    else if (app->dests_shown)
        dests_pointer(app, x, y, click);
    else if (app->users_shown)
        users_pointer(app, x, y, click, touch);
    else if (app->addons_shown)
        addons_pointer(app, x, y, click, touch);
    else if (app->editing && app->form == GUI_FORM_PAD) {
        if (click) pad_did(app, snx_pad_screen_tap(&app->pad, snx_pad_screen_hit(&app->pad_drawing, x, y)));
    } else if (app->editing && app->editor.picking)
        picker_pointer(app, x, y, click);
    else if (app->editing) {
        if (click)
            form_pointer(app, x, y);
        else if (!touch)
            form_hover(app, x, y);
    } else if (app->setup_on)
        setup_pointer(app, x, y, click);
    else
        screen_pointer(app, x, y, click, touch);
}
/* the file the system's picker chose, as a dropped one; none (cancelled) stays on the screen and says so */
static void picked(App* app, const char* uri) {
    char* path;
    if (uri && app->menu.view == GUI_LOAD_KEYS && strstr(uri, "://") && strncmp(uri, "file://", 7)) {
        install_keys_document(app, uri);
        return;
    }
    path = uri ? gui_document_path(uri) : NULL;
#ifdef SDL_PLATFORM_ANDROID
    if (uri && !path) path = gui_android_document_file(uri);
#endif
    if (!uri)
        snprintf(app->message, sizeof(app->message), "%s", snx_tr("No file was picked."));
    else if (!path) {
        snprintf(app->message, sizeof(app->message), snx_tr(SAY_PICK_UNREAD), uri);
        keys_result(app, false);
    } else
        drop(app, path, false);
    free(path);
}
/* ESC: a run is aborted, or asked about first when ask_abort says so; anything else goes back */
static void cancel(App* app) {
    if (!app->menu.running)
        menu_key(app, GUI_CANCEL);
    else if (app->ask_abort && !app->aborting)
        gui_confirm_open(&app->confirm, GUI_ASK_ABORT, false);
    else
        abort_run(app);
}
/* a finger's id, the pen's apart: SDL gives a pen's touch the id 1 on every platform, the id Android and Wayland
   give a first finger and any finger may have, and no real finger's id sets bit 63 */
static Uint64 finger_id(const SDL_TouchFingerEvent* f) {
    return (Uint64)f->fingerID | (f->touchID == SDL_PEN_TOUCHID ? 1ull << 63 : 0);
}
/* a row up or down for a finger's drag and the wheel, which stop at the first and last rows where the keys go round */
static void scroll(App* app, GuiKey key) {
    int row;
    if (app->editing && app->form == GUI_FORM_PAD) {
        pad_did(app, snx_pad_screen_key(&app->pad, key == GUI_UP ? SNX_PAD_KEY_UP : SNX_PAD_KEY_DOWN));
        return;
    }
    if (app->editing) {
        editor_key(app, key); /* stops at its ends */
        return;
    }
    if (app->menu.view == GUI_LOAD_GAMES) { /* over its pages as the keys go, from the head's first to the last game */
        bool last = app->browser.page + 1 >= gui_browser_pages(&app->browser) &&
                    app->browser.selected + 1 >= app->browser.head + gui_browser_items(&app->browser);
        if (key == GUI_UP ? app->browser.selected > 0 : !last) menu_key(app, key);
        return;
    }
    row = app->menu.selected;
    menu_key(app, key);
    if (key == GUI_UP ? app->menu.selected > row : app->menu.selected < row)
        menu_key(app, key == GUI_UP ? GUI_DOWN : GUI_UP); /* it went round: back */
}
/* a drag up or down from where the finger went down: on the menu, whether or not its rows all fit (the focus follows
   it), and on a form */
static bool drags(App* app, float x, float y) {
    float dx = x - app->finger_x, dy = y - app->finger_y;
    return SDL_fabsf(dy) > 0.05f && SDL_fabsf(dy) > 2 * SDL_fabsf(dx) && app->menu.view != GUI_LOAD_GAMES &&
           !app->menu.running && !app->finished && !app->setup_on &&
           (app->editing ? app->form != GUI_FORM_PAD : !pick_screen(app));
}
/* the share of the window's height the finger goes for a row: one of the menu's, or of the form's list the keys move
   in; a row of text where it has none */
static float drag_row(App* app) {
    GuiRect first, second;
    int width, height;
    layout(app);
    area(app, &width, &height);
    if (!app->editing) {
        GuiMenuBoxes m;
        menu_boxes(app, &m);
        first = ui_menu_row(m.rects, app->scale, UI_MENU_ENTRIES, 0);
        second = ui_menu_row(m.rects, app->scale, UI_MENU_ENTRIES, 1);
    } else {
        GuiFormBoxes f;
        form_boxes(app, &f);
        if (app->form == GUI_FORM_THEME || app->form == GUI_FORM_SETTINGS) {
            first = ui_form_list_row(f.left, app->scale, UI_FORM_LIST_ROWS, 0);
            second = ui_form_list_row(f.left, app->scale, UI_FORM_LIST_ROWS, 1);
        } else {
            first = ui_form_side_row(f.right, app->scale, UI_FORM_SIDE_ROWS, 0);
            second = ui_form_side_row(f.right, app->scale, UI_FORM_SIDE_ROWS, 1);
        }
    }
    if (height > 0 && second.y > first.y) return (float)(second.y - first.y) / (float)height;
    return 1.0f / (float)SDL_max(1, app->rows);
}
/* the focus follows the finger, up as it goes up, a row each time it passes a row's height, so the highlight is seen
   going */
static void drag(App* app, float y) {
    float row = drag_row(app);
    while (y <= app->finger_mark - row) {
        scroll(app, GUI_UP);
        app->finger_mark -= row;
    }
    while (y >= app->finger_mark + row) {
        scroll(app, GUI_DOWN);
        app->finger_mark += row;
    }
}
/* a finger lifted at x, y (0 to 1 across the window): a swipe turns the games' pages, left or up to the next as a
   phone's lists scroll; a tap clicks there, is ESC on a finished run and on a running one's ESC line */
static void touch(App* app, float x, float y) {
    float dx = x - app->finger_x, dy = y - app->finger_y, rx, ry;
    int w, h;
    bool games = app->menu.view == GUI_LOAD_GAMES && !app->menu.running && !app->finished;
    if (SDL_fabsf(dx) > 0.1f && SDL_fabsf(dx) > 2 * SDL_fabsf(dy)) {
        if (games) menu_key(app, dx < 0 ? GUI_PAGE_DOWN : GUI_PAGE_UP);
        return;
    }
    if (games && SDL_fabsf(dy) > 0.1f && SDL_fabsf(dy) > 2 * SDL_fabsf(dx)) { /* Games has no drag: its pages */
        menu_key(app, dy < 0 ? GUI_PAGE_DOWN : GUI_PAGE_UP);
        return;
    }
    if (drags(app, x, y)) {
        drag(app, y); /* what its last moves did not reach */
        return;
    }
    if (SDL_fabsf(dx) > 0.03f || SDL_fabsf(dy) > 0.03f) return; /* a drag, no tap */
    if (app->finished) {
        menu_key(app, GUI_CANCEL);
        return;
    }
    SDL_GetWindowSize(app->window, &w, &h);
    if (app->confirm.shown) {
        pointer(app, x * (float)w, y * (float)h, true, true);
        return;
    }
    if (app->menu.running) {
        GuiPlace p;
        layout(app);
        if (!SDL_RenderCoordinatesFromWindow(app->renderer, x * (float)w, y * (float)h, &rx, &ry)) return;
        if (app->theme.run != GUI_RUN_STEPS) { /* the foot the ESC line is in */
            run_place(app, &p);
            if (ry >= (float)p.keys.y && ry < (float)(p.keys.y + p.keys.h)) cancel(app);
        } else if ((int)(ry / (float)(16 * app->scale)) == app->rows - 1) /* the loading screen's last row */
            cancel(app);
        return;
    }
    pointer(app, x * (float)w, y * (float)h, true, true);
}
#ifdef SDL_PLATFORM_ANDROID
/* SDL hands the app's lifecycle to watchers only, inside SDL_PollEvent on this thread */
static bool SDLCALL lifecycle(void* data, SDL_Event* e) {
    App* app = (App*)data;
    if (e->type == SDL_EVENT_DID_ENTER_BACKGROUND) app->hidden = true;
    if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND) app->hidden = false;
    return true;
}
#endif
/* the menu's key for a key press; false for none and for Enter or Space held down */
static bool menu_key_of(const SDL_KeyboardEvent* k, GuiKey* key) {
    switch (k->key) {
        case SDLK_UP: *key = GUI_UP; return true;
        case SDLK_DOWN: *key = GUI_DOWN; return true;
        case SDLK_LEFT: *key = GUI_LEFT; return true;
        case SDLK_RIGHT: *key = GUI_RIGHT; return true;
        case SDLK_PAGEUP: *key = GUI_PAGE_UP; return true;
        case SDLK_PAGEDOWN: *key = GUI_PAGE_DOWN; return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE: *key = GUI_SELECT; return !k->repeat;
        case SDLK_ESCAPE:
        case SDLK_AC_BACK: *key = GUI_CANCEL; return true;
        default: return false;
    }
}
/* the box of files over the screen step files on (back for a negative one), down only while some are under them */
static void files_step(App* app, int step) {
    int count = app->cleaned_shown ? app->cleaned.count : app->lefts;
    if (step > 0 && app->files_top + app->files_shown >= count) return; /* the last one shows already */
    app->files_top = SDL_max(0, app->files_top + step);
}
/* a key that goes over its files, Up/Down a file and PgUp/PgDn as many as show: true for those, another closing it */
/* F5, or the foot's Refresh: what the screen shows of the folders and the disks looked at again now, not at pump's
   next look; the Library and Folders say how many of their folders are there, Games how many games it lists */
static void refresh(App* app) {
    if (app->left_shown) {
        left_behind(app);
        app->left_shown = app->lefts > 0;
    } else if (app->editing && app->form == GUI_FORM_FOLDERS) {
        /* the places, a default made when a run needs it and a picked one not; then the libraries' folders */
        char gone[sizeof app->editor.note];
        size_t n = 0;
        char* dumps = app->data_dir ? snx_path_join(app->data_dir, "dumps") : NULL;
        int missing = !(dumps && snx_is_dir(dumps));
        for (int i = 0; i < app->folder_count; ++i) missing += !snx_is_dir(app->folders[i]);
        free(dumps);
        space_start(app);
        open_folders(app, app->editor.row);
        for (int i = 0; i < GUI_PLACES; ++i) {
            const char* path = place_path(app, i);
            if ((!path || !snx_is_dir(path)) && n < sizeof gone)
                n += (size_t)snprintf(gone + n, sizeof gone - n,
                                      app->place[i] ? snx_tr("%s%s %s is not there")
                                                    : snx_tr("%s%s %s, made when needed"),
                                      n ? ", " : "", snx_tr(place_labels[i]), path ? path : snx_tr("(none)"));
        }
        if (!n) snprintf(gone, sizeof gone, "%s", snx_tr("Data, Recompiled and Extractions are there"));
        snprintf(app->editor.note, sizeof app->editor.note,
                 snx_tr("Refreshed: %s; dumps/ and %d Extra(s), %d not found"), gone, app->folder_count, missing);
    } else if (!app->editing && app->menu.view == GUI_LOAD_GAMES) {
        GuiBrowser was = app->browser;
        size_t games = 0;
        list_games(app);
        app->browser.page = was.page;
        app->browser.selected = was.selected;
        gui_browser_resize(&app->browser, app->browser.page_size);
        for (size_t i = 0; i < app->games.count; ++i) games += !gui_games_heading(&app->games, i);
        if (!app->message[0]) snprintf(app->message, sizeof app->message, snx_tr("Refreshed: %zu games"), games);
    } else if (!app->editing &&
               app->menu.view == GUI_LOAD_NONE) { /* the status: the usage and the disk counted again */
        app->usage = gui_process_usage(app->process);
        app->sampled = SDL_GetTicks();
        space_start(app);
    }
}
static const char* shown_place(App* app, GuiPlace* p, char* keys, size_t size);
/* F2: the side box of the screen shown off where it shows, on where it does not; the App's row says it */
static void side_toggle(App* app) {
    GuiPlace p;
    char keys[256];
    shown_place(app, &p, keys, sizeof keys); /* a form without it has its rows where the side box was, the list empty */
    app->side_box = p.side.w && p.side.h && p.list.w && p.list.h ? GUI_SIDE_OFF : GUI_SIDE_ON;
    layout(app);
    app_save(app);
    if (app->editing && app->form == GUI_FORM_APP) app_values(app);
}
static bool files_key(App* app, SDL_Keycode code) {
    int shown = SDL_max(1, app->files_shown);
    switch (code) {
        case SDLK_UP: files_step(app, -1); return true;
        case SDLK_DOWN: files_step(app, 1); return true;
        case SDLK_PAGEUP: files_step(app, -shown); return true;
        case SDLK_PAGEDOWN: files_step(app, shown); return true;
        default: return false;
    }
}
static void event(App* app, const SDL_Event* e) {
    GuiKey key;
    /* the window closed: asked first while a run goes, unless it asks already; else the screen of what is kept */
    if (e->type == SDL_EVENT_QUIT || e->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        if ((app->menu.running || app->finished) && !app->aborting && !app->closing_ms &&
            !(app->confirm.shown && app->confirm.ask == GUI_ASK_CLOSE)) {
            if (app->finished) {
                closing_start(app);
                return;
            }
            gui_confirm_open(&app->confirm, GUI_ASK_CLOSE, false);
            return;
        }
        closing_start(app);
        return;
    }
    if (app->closing_ms) return; /* closing: nothing else is taken */
    /* the setup, while it shows, takes the keys and the typing; the pointer goes to it through pointer() */
    if (app->setup_on && !app->splash && setup_event(app, e)) return;
    /* what Clean old settings did, and the box of an older version's files: Up/Down and the wheel go over their files,
       the first other key, button or finger closes it */
    if (!app->setup_on && (app->cleaned_shown || (app->left_shown && !app->splash)) &&
        ((e->type == SDL_EVENT_KEY_DOWN && files_key(app, e->key.key)) || e->type == SDL_EVENT_MOUSE_WHEEL)) {
        if (e->type == SDL_EVENT_MOUSE_WHEEL) files_step(app, -e->wheel.integer_y); /* a notch a file, as a row */
        return;
    }
    if (app->left_shown && !app->splash && !app->setup_on && e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_F5) {
        if (!e->key.repeat) refresh(app);
        return;
    }
    if (app->cleaned_shown &&
        (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_FINGER_DOWN)) {
        app->cleaned_shown = false;
        return;
    }
    if (app->left_shown && !app->splash && !app->setup_on &&
        (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_FINGER_DOWN)) {
        app->left_shown = false;
        return;
    }
    /* the splash takes the input it gets: a key, a button or a finger ends it, and does nothing else */
    if (app->splash && (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                        e->type == SDL_EVENT_FINGER_DOWN || e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)) {
        app->splash_until = 0;
        return;
    }
    /* the box of sizes and the box of users take every key while they show, as a question does */
    if (app->sizes_shown && !app->confirm.shown && (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_TEXT_INPUT)) {
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && menu_key_of(&e->key, &key)) sizes_key(app, key);
        return;
    }
    if (app->dests_shown && !app->confirm.shown && (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_TEXT_INPUT)) {
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && menu_key_of(&e->key, &key)) dests_key(app, key);
        return;
    }
    if (app->users_shown && !app->confirm.shown && (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_TEXT_INPUT)) {
        if (e->type == SDL_EVENT_KEY_DOWN && menu_key_of(&e->key, &key)) users_key(app, key);
        return;
    }
    /* so does the box of a game's updates and DLCs */
    if (app->addons_shown && !app->confirm.shown && (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_TEXT_INPUT)) {
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && menu_key_of(&e->key, &key)) addons_key(app, key);
        return;
    }
    /* a question takes every key while it shows, over the menu, Games or a form (a run's is the run's, below); a held
       key answers nothing */
    if (app->confirm.shown && !app->menu.running &&
        (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_TEXT_INPUT)) {
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && menu_key_of(&e->key, &key)) ask_key(app, key);
        return;
    }
    /* F2, or the foot's: the side box hidden where it shows, shown where it does not, kept in app.ini */
    if (e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_F2 && !app->menu.running && !app->finished &&
        !(app->editing && app->editor.typing)) {
        if (!e->key.repeat) side_toggle(app);
        return;
    }
    if (e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_F5 && !app->menu.running && !app->finished &&
        !(app->editing && app->editor.typing)) {
        if (!e->key.repeat) refresh(app);
        return;
    }
    if (app->editing && app->form == GUI_FORM_PAD && pad_event(app, e)) return;
    if (app->editing && device_event(app, e)) return;
    if (app->editing && (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_TEXT_INPUT)) {
        editor_input(app, e);
        return;
    }
    if ((e->type == SDL_EVENT_DROP_FILE || e->type == SDL_EVENT_DROP_TEXT) && app->editing &&
        app->form == GUI_FORM_FOLDERS) {
        char* path = gui_drop_path(e->drop.data, e->type == SDL_EVENT_DROP_TEXT);
        if (path)
            folders_take(app, path);
        else
            snprintf(app->editor.note, sizeof app->editor.note, "%s", snx_tr("Drop one local folder"));
        free(path);
        return;
    }
    if (e->type == SDL_EVENT_DROP_FILE || e->type == SDL_EVENT_DROP_TEXT) {
        drop(app, e->drop.data, e->type == SDL_EVENT_DROP_TEXT);
        return; /* SDL3 owns the event's data, unlike SDL2. */
    }
    /* the folder picker's answer: a folder, none, or why it could not open */
    if (app->picked_folder && e->type == app->picked_folder) {
        char* error = (char*)e->user.data2;
        int place = app->place_row;
        app->place_row = -1;
        if (app->setup_keys) /* the setup's keys */
            setup_keys_from(app, (const char*)e->user.data1, e->user.code ? error : NULL);
        else if (e->user.data1 && place >= 0)
            place_set(app, place, (const char*)e->user.data1);
        else if (e->user.data1)
            folder_ask(app, (const char*)e->user.data1); /* to the library Add a folder was for */
        else {
            char note[sizeof app->editor.note];
            if (e->user.code) {
                snprintf(note, sizeof note, snx_tr("The folder picker could not open: %s"),
                         error ? error : snx_tr("no reason given"));
                say_failure(app, note);
            } else
                snprintf(note, sizeof note, "%s", snx_tr("No folder was picked."));
            if (app->editing && app->form == GUI_FORM_FOLDERS)
                snprintf(app->editor.note, sizeof app->editor.note, "%s", note);
            else
                snprintf(app->message, sizeof app->message, "%s: %s", snx_tr("Folders"), note);
        }
        app->adding = -1; /* a folder from Games' Add a folder goes where its games say */
        SDL_free(e->user.data1);
        SDL_free(error);
        return;
    }
    if (app->picked && e->type == app->picked) {
        picked(app, e->user.data1);
        SDL_free(e->user.data1);
        return;
    }
    if (e->type == SDL_EVENT_FINGER_DOWN && !app->finger) {
        int w, h;
        app->finger = finger_id(&e->tfinger);
        app->finger_x = e->tfinger.x;
        app->finger_y = app->finger_mark = e->tfinger.y;
        SDL_GetWindowSize(app->window, &w, &h);
        app->pick_press = 0;                     /* the finger's own, not a mouse's whose lift was missed */
        if (app->editing && app->editor.picking) /* the picker's bars take a finger as it goes down, to drag */
            pointer(app, e->tfinger.x * (float)w, e->tfinger.y * (float)h, true, true);
        return;
    }
    if (e->type == SDL_EVENT_FINGER_MOTION && finger_id(&e->tfinger) == app->finger) {
        int w, h;
        SDL_GetWindowSize(app->window, &w, &h);
        if (app->pick_press)
            pointer(app, e->tfinger.x * (float)w, e->tfinger.y * (float)h, false, true);
        else if (drags(app, e->tfinger.x, e->tfinger.y))
            drag(app, e->tfinger.y);
        return;
    }
    if ((e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED) &&
        finger_id(&e->tfinger) == app->finger && app->pick_press) { /* the picker had it: no tap */
        app->finger = 0;
        app->pick_press = 0;
        return;
    }
    if ((e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED) &&
        finger_id(&e->tfinger) == app->finger) {
        app->finger = 0;
        if (e->tfinger.touchID == SDL_PEN_TOUCHID) {
            app->pen_held = true;
            app->pen_x = e->tfinger.x;
            app->pen_y = e->tfinger.y;
        }
        if (e->type == SDL_EVENT_FINGER_UP) touch(app, e->tfinger.x, e->tfinger.y);
        return;
    }
    if (e->type == SDL_EVENT_MOUSE_MOTION) {
        pointer(app, e->motion.x, e->motion.y, false, false);
        return;
    }
    if (e->type == SDL_EVENT_WINDOW_MOUSE_LEAVE) {
        app->hovering = false;
        return;
    }
    /* a notch of the wheel is a row, as a finger's drag, the way the system turns it; not under a box */
    if (e->type == SDL_EVENT_MOUSE_WHEEL) {
        int rows = e->wheel.integer_y;
        for (; app->setup_on && !app->splash && !app->confirm.shown && rows; rows += rows > 0 ? -1 : 1)
            setup_key(app, rows > 0 ? GUI_UP : GUI_DOWN);
        if (app->splash || app->menu.running || app->finished || app->confirm.shown || app->left_shown ||
            app->users_shown || app->addons_shown || (app->editing && app->editor.picking))
            return;
        for (; rows > 0; --rows) scroll(app, GUI_UP);
        for (; rows < 0; ++rows) scroll(app, GUI_DOWN);
        return;
    }
    /* a hovering pen highlights as the mouse does (it sends no mouse motion). Touching, it is a finger, and once it
       lifts its hover waits until it has moved away as far as a tap may: the focus a drag moved would otherwise jump
       to the row under the hover at the lift, which SDL sends right after it */
    if (e->type == SDL_EVENT_PEN_PROXIMITY_OUT) {
        app->pen_held = false;
        return;
    }
    if (e->type == SDL_EVENT_PEN_MOTION) {
        int w, h;
        if (e->pmotion.pen_state & SDL_PEN_INPUT_DOWN) return;
        SDL_GetWindowSize(app->window, &w, &h);
        if (app->pen_held && w > 0 && h > 0 && SDL_fabsf(e->pmotion.x / (float)w - app->pen_x) <= 0.03f &&
            SDL_fabsf(e->pmotion.y / (float)h - app->pen_y) <= 0.03f)
            return;
        app->pen_held = false;
        pointer(app, e->pmotion.x, e->pmotion.y, false, false);
        return;
    }
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && e->button.button == SDL_BUTTON_LEFT) {
        pointer(app, e->button.x, e->button.y, true, false);
        return;
    }
    if (e->type == SDL_EVENT_MOUSE_BUTTON_UP && e->button.button == SDL_BUTTON_LEFT) {
        app->pick_press = 0; /* a drag on the picker's bar ends */
        return;
    }
    if (e->type != SDL_EVENT_KEY_DOWN) return;
    /* a running run takes ESC, and the question, while it shows, every key; a held key asks and answers nothing */
    if (app->menu.running) {
        if (e->key.repeat || !menu_key_of(&e->key, &key)) return;
        if (app->confirm.shown)
            ask_key(app, key);
        else if (key == GUI_CANCEL)
            cancel(app);
        return;
    }
    if (e->key.key == SDLK_V && (e->key.mod & SDL_KMOD_CTRL) &&
        (app->menu.view == GUI_LOAD_DROP || app->menu.view == GUI_LOAD_KEYS) && !e->key.repeat) {
        char* clipboard = SDL_GetClipboardText();
        drop(app, clipboard, true);
        SDL_free(clipboard);
        return;
    }
    if (e->key.key == SDLK_TAB && !e->key.repeat) {
        screen_tab(app, e->key.mod & SDL_KMOD_SHIFT ? -1 : 1);
        return;
    }
    if (e->key.key == SDLK_R && !e->key.repeat && !(e->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) &&
        app->menu.view == GUI_LOAD_GAMES && !app->finished) {
        layout(app); /* the page a resize in the same poll gave */
        recompile_focus(app);
        return;
    }
    if (e->key.key == SDLK_B && !e->key.repeat && !(e->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) &&
        app->menu.view == GUI_LOAD_GAMES && !app->finished) {
        layout(app);
        aot_focus(app);
        return;
    }
    if (e->key.key == SDLK_M && !e->key.repeat && !(e->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) &&
        app->menu.view == GUI_LOAD_GAMES && !app->finished) {
        layout(app);
        mods_focus(app);
        return;
    }
    if (menu_key_of(&e->key, &key)) menu_key(app, key);
}
/* the themes that come with the GUI: beside it in a build tree, else where an install puts data, share/scarletnx/themes
   beside its bin/ (/usr/local, the Windows zip) */
static char* shipped_themes(const char* program_dir) {
    char* beside = snx_path_join(program_dir, "themes");
    if (snx_is_dir(beside)) return beside;
    free(beside);
    return snx_path_join(program_dir, "../share/scarletnx/themes");
}
#ifndef SDL_PLATFORM_ANDROID
/* where the menu runs from and keeps its files, which need no window: the drivers' folder (self's, else this
   program's), the data folder, a build tree's checkout, the folders by them and the shipped themes */
static bool paths_start(App* app, const char* self) {
    app->cwd = SDL_GetCurrentDirectory();
    app->drivers = self ? snx_dirname(self) : SDL_GetBasePath() ? snx_strdup(SDL_GetBasePath()) : NULL;
    if (!app->drivers) { /* SDL_GetBasePath's failure */
        char why[256];
        SDL_strlcpy(why, SDL_GetError(), sizeof why);
        return SDL_SetError(snx_tr("Cannot find the folder ScarletNX runs from: %s"), why);
    }
    app->data_dir = snx_data_dir(app->drivers);
    if (!app->data_dir) app->data_dir = snx_strdup(app->cwd ? app->cwd : ".");
    app->checkout = snx_checkout(GUI_CHECKOUT, GUI_BUILD, app->drivers);
    app->home_dir = snx_strdup(app->data_dir);
    app->place_row = -1;
    app->adding = -1;
    places_apply(app); /* recompiled/, themes/, work/ and the keys, by the data folder */
    app->themes_dirs[1] = shipped_themes(app->drivers);
    return true;
}
#endif
static bool initialize(App* app, const char* self) {
    SDL_WindowFlags flags;
    memset(app, 0, sizeof(*app));
    app->menu.selected = GUI_SWITCH; /* the first item until Games shows */
    if (self) app->self = snx_strdup(self);
    app->theme = *gui_theme_builtin("yorha"); /* every platform's look until Settings > App picks another */
#ifdef SDL_PLATFORM_ANDROID
    app->ask_abort = true;
    gui_android_start();
    /* back is ESC here: without this it would close the app */
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    /* a run goes on behind other apps: its output is read, or it stops once the pipe is full */
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
#endif
    /* touch() takes the fingers, and a finger and the mouse each arrive once: SDL would also send each as the other,
       and a pen as both (it stays a finger), so one tap would select twice */
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_PEN_MOUSE_EVENTS, "0");
#ifdef SDL_PLATFORM_IOS
    gui_apple_start();
#endif
    if (!SDL_Init(SDL_INIT_VIDEO)) return false;
    snx_tr_languages(snx_gui_languages, snx_gui_language_count);
    language_apply(app);
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
    /* the whole screen, the system's bars out of the way, as the Settings' fullscreen says: a phone held wide is short */
    flags = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#else
    flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif
    if (!SDL_CreateWindowAndRenderer(snx_build_info(), 1280, 800, flags, &app->window, &app->renderer)) return false;
    snx_window_icon(app->window);
    SDL_SetRenderVSync(app->renderer, 1);
    SDL_SetEventEnabled(SDL_EVENT_DROP_FILE, true);
    SDL_SetEventEnabled(SDL_EVENT_DROP_TEXT, true);
    app->font = create_font(app->renderer);
    if (!app->font) {
        char why[256];
        SDL_strlcpy(why, SDL_GetError(), sizeof why); /* SDL_SetError writes over the text it formats */
        return SDL_SetError(snx_tr("Cannot make the menu's font: %s"), why);
    }
    app->process = gui_process_create();
    if (!app->process) return SDL_SetError("%s", snx_tr("Cannot start the menu: out of memory"));
#ifdef SDL_PLATFORM_ANDROID
    app->cwd = SDL_GetCurrentDirectory();
    app->drivers = gui_android_libraries() ? snx_strdup(gui_android_libraries()) : NULL;
    if (!app->drivers) return SDL_SetError("%s", snx_tr("Cannot find the folder of the app's libraries"));
    /* the app's own storage: no FUSE between it and the drivers, as the shared storage has */
    if (gui_android_own_folder()) {
        SDL_free(app->cwd);
        app->cwd = SDL_GetCurrentDirectory();
    }
    app->data_dir = snx_strdup(app->cwd ? app->cwd : ".");
    app->own_dir = SDL_GetAndroidInternalStoragePath() ? snx_strdup(SDL_GetAndroidInternalStoragePath()) : NULL;
    {
        char* sdl = SDL_GetPrefPath("scarletnx", "gui"); /* the app's own files, whatever its names */
        app->menu_old = sdl ? snx_strdup(sdl) : NULL;
        SDL_free(sdl);
    }
    app->home_dir = snx_strdup(app->data_dir);
    app->place_row = -1;
    app->adding = -1;
    places_apply(app); /* recompiled/, themes/, work/ and the keys, by the data folder */
    app->themes_dirs[1] = shipped_themes(app->drivers);
#else
    if (!paths_start(app, self)) return false;
#endif
    gui_console_init(&app->console, 78, 17);
#ifdef SDL_PLATFORM_ANDROID
    SDL_AddEventWatch(lifecycle, app);
#endif
    layout(app);
    return true;
}
/* what the menu holds, let go; SDL itself left as it is (cleanup's) */
static void forget(App* app) {
#ifdef SDL_PLATFORM_ANDROID
    SDL_RemoveEventWatch(lifecycle, app);
    gui_android_ran(NULL); /* the run goes with the menu */
#endif
    gui_process_destroy(app->process);
    dump_icons_stop(app);
    space_stop(app);
    space_end(&app->left_space, &app->left_counter);
    settings_forget(app);
    free(app->settings_file);
    for (int i = 0; i < app->folder_count; ++i) free(app->folders[i]);
    free(app->folder_asked);
    free(app->own_dir);
    free(app->menu_old);
    free(app->outside_path);
    free(app->room_file);
    free(app->dest_file);
    free(app->dest_out);
    free(app->folders_file);
    free(app->app_file);
    gui_cleaned_free(&app->cleaned);
    free(app->left);
    free(app->self);
    free(app->drivers);
    free(app->file);
    free(app->data_dir);
    free(app->home_dir);
    for (int i = 0; i < GUI_PLACES; ++i) free(app->place[i]);
    free(app->work_dir);
    free(app->checkout);
    free(app->games_dir);
    free(app->library_dumps);
    free(app->aot_focus);
    free(app->aot_cache);
    free(app->themes_dirs[0]);
    free(app->themes_dirs[1]);
    gui_themes_free(&app->themes);
    free(app->keys_path);
    free(app->recent_file);
    if (app->run_out) fclose(app->run_out);
    free(app->run_log);
    gui_games_free(&app->games);
    addons_close(app);
    SDL_free(app->cwd);
    if (app->editing) gui_editor_stop(&app->editor);
    SDL_DestroyTexture(app->preview);
    SDL_DestroyTexture(app->splash);
    snx_splash_close(app->splash_drawing);
    SDL_DestroyTexture(app->user_picture);
    icons_free(app);
    users_close(app);
    free(app->user_jpeg);
    free(app->users_dir);
    SDL_DestroyTexture(app->font);
    SDL_DestroyRenderer(app->renderer);
    SDL_DestroyWindow(app->window);
}
static void cleanup(App* app) {
    forget(app);
    SDL_Quit();
}
/* the settings, its picks and its game folders in a file each */
static char* settings_path(const char* config) { return config ? snx_path_join(config, "settings.ini") : NULL; }
/* the menu's own files, in its folders of them (menu_folder) alone: an older version's elsewhere are listed by
   left_behind, never read, and with none here the menu starts from its defaults */
static void open_files(App* app) {
    char *prefs = menu_folder(app, SNX_USER_STATE, !app->look_only),
         *config = menu_folder(app, SNX_USER_CONFIG, !app->look_only);
    if (prefs) recent_open(app, snx_path_join(prefs, "recent.txt"));
    if (prefs) app->run_log = snx_path_join(prefs, "last-run.log");
    app->settings_file = settings_path(config);
    if (app->settings_file) {
        app->folders_file = snx_path_join(config, "folders.txt");
        app->app_file = snx_path_join(config, "app.ini");
        folders_load(app);
        app_load(app);
    }
    if (prefs) recent_as_games(app); /* by the recompiled/ app.ini picked */
    app->users_dir = snx_users_dir(app->home_dir ? app->home_dir : app->data_dir);
    if (!app->users_dir) {
        snprintf(app->message, sizeof app->message, "%s", snx_tr("No user shown: out of memory"));
        say_failure(app, app->message);
    }
    users_read(app);
    free(config);
    free(prefs);
}
const char* snx_menu_language(int i, const char** name) {
    if (i < 0 || i >= snx_gui_language_count) return NULL;
    *name = snx_gui_languages[i].name;
    return snx_gui_languages[i].code;
}
#if !defined(SDL_PLATFORM_ANDROID) && !defined(SDL_PLATFORM_IOS)
/* Move old files without the menu, to out: each entry and what it does or would, then how many; with apply, done.
   0, or 1 when one could not be moved; 2 when the folders cannot be found */
static int migrate(const char* self, bool apply, FILE* out) {
    App app;
    char file[4400], what[4400];
    const char* at;
    bool ok;
    int moves, rc;
    memset(&app, 0, sizeof app);
    snx_tr_languages(snx_gui_languages, snx_gui_language_count);
    app.look_only = !apply;
    if (!paths_start(&app, self)) {
        fprintf(stderr, "scarletnx: %s\n", SDL_GetError());
        forget(&app);
        return 2;
    }
    open_files(&app); /* the folders the menu picked, which tell where things go */
    left_behind(&app);
    moves = move_old(&app, &app.cleaned, apply);
    for (at = app.cleaned.text; at;) {
        at = gui_cleaned_entry(at, &ok, file, what, sizeof file);
        fprintf(out, "%s: %s\n", file, what);
    }
    if (!app.cleaned.count)
        snprintf(what, sizeof what, "%s", snx_tr("Nothing left by an older version"));
    else if (!apply)
        snprintf(what, sizeof what, snx_tr("%d to move, %d staying: --migrate=yes moves them"), moves,
                 app.cleaned.count - moves);
    else
        snprintf(what, sizeof what, snx_tr("%d moved, %d not"), moves, app.cleaned.count - app.cleaned.done);
    fprintf(out, "%s\n", what);
    rc = apply && app.cleaned.done < app.cleaned.count;
    forget(&app);
    return rc;
}
int snx_gui_migrate(const char* self, const char* option) {
    if (!strcmp(option, "--migrate") || !strcmp(option, "--migrate=yes"))
        return migrate(self, option[9] == '=', stdout);
    fprintf(stderr, "scarletnx: %s is not --migrate (it says what would move) or --migrate=yes (it moves it)\n",
            option);
    return 2;
}
#endif
int snx_gui_main(const char* self) {
    App app;
    if (!initialize(&app, self)) {
        fprintf(stderr, "scarletnx: %s\n", SDL_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "ScarletNX", SDL_GetError(), app.window);
        cleanup(&app);
        return 1;
    }
    open_files(&app); /* here, not in initialize(), so the tests keep their own */
    setup_start(&app);
    app.games_stamp = games_stamp(&app);
    app.dump_icons.live = true; /* the dumps' icons asked on a thread of their own */
    space_start(&app);          /* the footer's disk */
    left_behind(&app);
    app.left_shown = app.lefts > 0;
    splash_start(&app);
    while (!app.menu.quit) {
        SDL_Event e;
#ifdef SDL_PLATFORM_ANDROID
        /* behind other apps with no run to read, the menu waits to show again */
        if (app.hidden && !app.menu.running && SDL_WaitEvent(&e)) event(&app, &e);
#endif
        while (SDL_PollEvent(&e)) event(&app, &e);
        pump(&app);
#ifdef SDL_PLATFORM_ANDROID
        if (!app.hidden)
#endif
            render(&app);
        SDL_Delay(16);
    }
    cleanup(&app);
    return 0;
}
#ifndef SNX_GUI_NO_MAIN /* the menu alone: Android's activity runs it, and a build without the Switch runtime */
int main(int argc, char** argv) {
    char why[160];
    if (snx_setup_options(argc, argv, why, sizeof why) < 0) { /* --debug and --skip --skip leave the setup out */
        fprintf(stderr, "scarletnx-gui: %s\n", why);
        return 2;
    }
    return snx_gui_main(NULL);
}
#endif
