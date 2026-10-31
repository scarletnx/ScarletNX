/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include <SDL3/SDL.h>
#include "common/utilities/ini_lines.h"
#ifndef SNX_GUI_NO_MAIN
#include <SDL3/SDL_main.h>
#endif
#include "common/utilities/common.h"
#include "common/utilities/game_ini.h"
#include "common/utilities/cheat_list.h"
#include "common/utilities/mods.h"
#include "common/cpu/aarch64/cpu.h" /* SNX_ABI, which the games this build's driver makes are stamped with */
#include "common/utilities/state_header.h"
#include "common/utilities/demangle.h"
#include "common/utilities/logscan.h"
#include "gui/aot.h"
#include "gui/browser.h"
#include "gui/editor.h"
#include "common/utilities/font.h"
#include "gui/gui.h"
#include "gui/i18n.h"
#include "gui/icons.h"
#include "gui/latin.h"
#include "gui/log.h"
#include "gui/logzip.h"
#include "gui/wide.h"
#include "gui/model.h"
#include "gui/place.h"
#include "gui/queue.h"
#include "gui/ui.h" /* generated from ui.txt by tools/uigen */
#include "gui/gui_process.h"
#include "gui/steps.h"
#include "gui/theme.h"
#include "common/utilities/window_icon.h"
#include "common/utilities/build_info.h"
#include "gui/recent.h"
#include "gui/pad_menu.h"
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
/* a screen of fingers: the texts and the layout Android's phones take, and iOS's, whose own texts leave out the system's
   Back button (it has none) */
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
#define SNX_TOUCH_UI 1
#endif
/* a finger's row in pixels at 1x, as ui.txt's android.row_height has the lists' rows: the tabs' and the foot's keys'
   on a screen of fingers */
#define GUI_FINGER_ROW 48
#ifdef SDL_PLATFORM_APPLE
#include "gui/apple.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
enum { GUI_PLACE_DATA, GUI_PLACE_GAMES, GUI_PLACE_WORK, GUI_PLACE_STATE, GUI_PLACE_CACHE, GUI_PLACES };
/* Games' options, the rows of the View box its V opens, in the order of their keys in app.ini's [app] */
enum { GUI_OPTION_RECENT, GUI_OPTION_DUMPS, GUI_OPTION_SORT, GUI_OPTION_VIEW, GUI_OPTION_ICONS, GUI_OPTIONS };
/* whose choices the scene Choices shows (app->choices_of while it is drawn or hit) */
typedef enum {
    GUI_CHOICES_SETUP,
    GUI_CHOICES_ASK,
    GUI_CHOICES_SIZES,
    GUI_CHOICES_DESTS,
    GUI_CHOICES_TYPED,
    GUI_CHOICES_PICKS
} GuiChoicesOf;
/* whose few choices the picks box shows (picks_open) */
typedef enum { GUI_PICKS_SHARE, GUI_PICKS_FOLDER } GuiPicksOf;
/* the lists whose first row App's firsts keeps between frames: Choices' a GuiChoicesOf each, then the others */
enum {
    GUI_LIST_USERS = GUI_CHOICES_PICKS + 1,
    GUI_LIST_ADDONS,
    GUI_LIST_VIEW,
    GUI_LIST_MENU,
    GUI_LIST_GAMES,
    GUI_LIST_GRID,
    GUI_LIST_FORM_LIST,
    GUI_LIST_FORM_SIDE,
    GUI_LIST_APPEARANCE,
    GUI_LIST_QUEUE,
    GUI_LIST_STATES,
    GUI_LISTS
};
/* a scene list drawn this frame (ui_bar): its rows' area, a finger's to scroll, and its scroll bar's track and thumb
   (empty while its rows all show), the rows it has and shows from first, kept in App's firsts[list] */
typedef struct {
    GuiRect area, track, thumb;
    size_t count, shown;
    int list;
} GuiBar;
#define GUI_BARS         16 /* the scene lists one frame shows at most, a box's over a screen's */
#define GUI_GROUPS_MAX   32
#define GUI_FOLDERS_MAX  16    /* the player's game folders, past recompiled/ and dumps/ */
#define GUI_FOLDER_DEPTH 4     /* how deep the games of a folder kept with its subfolders are looked for */
#define GUI_FOLDER_DEEP  "/**" /* after such a folder's path, in the file of folders */
/* the library a folder of the player's is in, the word that starts its line in folders.txt: Games lists the games
   ready to run of the recompiled ones, the dumps of the dumps ones, and the CIAs (run as they are) of both */
enum { GUI_LIBRARY_RECOMPILED, GUI_LIBRARY_DUMPS, GUI_LIBRARIES };
static const char* const library_words[GUI_LIBRARIES] = {"recompiled", "dumps"};
/* What a row of Folders is: a place (Data, Logs, Caches, Extractions, the recompiled library's Main), dumps/ (the dumps
   library's Main), a folder of the player's (an Extra), a library's Add a folder, or Settings, shown only: app.ini in it
   names the others; by group, a box each */
typedef enum { GUI_ROW_PLACE, GUI_ROW_DUMPS, GUI_ROW_FOLDER, GUI_ROW_ADD, GUI_ROW_CONFIG } GuiFolderRow;
enum { GUI_GROUP_APP, GUI_GROUP_DUMPS, GUI_GROUP_RECOMPILED, GUI_FOLDER_GROUPS }; /* as the screen has them */
#define GUI_VIEW_ROWS    24    /* a group's rows shown as a view of them */
#define SNX_GUI_PLAYERS  8     /* Controllers' players, as the runtime's gamepad_p<N>_ keys */
/* Settings > App > Folders' rows, its last aside: the app's places and Settings, then each library's Main, its Extras
   and Add a folder, the dumps' library first */
#define GUI_FOLDER_ROWS  (GUI_PLACES + 2 + GUI_FOLDERS_MAX + 2)
/* the game folders watched on a thread of their own: listing them, a library's with its subfolders, can keep a busy
   disk a while, the window frozen if the menu's thread waited for it. Once a second the thread hashes what they hold
   (stamp_dirs); the menu hands it the folders (watch_set) and takes the last hash of those (watch_get) */
#define GUI_WATCH_DIRS  (GUI_FOLDERS_MAX + 3) /* recompiled/, the player's folders, dumps/, an older version's games/ */
typedef struct {
    SDL_Mutex* lock;
    SDL_Thread* thread;
    char* dirs[GUI_WATCH_DIRS]; /* as the menu last handed them, NULL none; each one's depth beside it */
    int depths[GUI_WATCH_DIRS];
    int count, stop;
    unsigned set;     /* how many times the folders were handed: a hash of older ones is not taken */
    unsigned stamped; /* the set the hash is of, 0 none */
    uint64_t stamp;
} GuiWatch;
/* the focused game's runtime JIT cache: none to show, no cache yet, one read, one unreadable */
enum { GUI_AOT_NONE, GUI_AOT_NO_CACHE, GUI_AOT_CACHE, GUI_AOT_BAD };
#define GUI_ICONS     96  /* the games' icons kept drawn: a page's rows and the recent ones, with room */
#define GUI_ICON_SIDE 128 /* the pixels square they are drawn at, scaled to their rows */
/* a game's icon as its rows show it: its program's path, and its texture; NULL for a game without one */
typedef struct {
    char* path;
    SDL_Texture* texture;
    bool loading; /* a dump's, which the driver has not answered for yet: the row shows the loading one */
} GuiIcon;
/* the dumps whose icons the driver is asked for (-x --icon), through the menu's own processes (gui_process.h), on a
   thread of its own: reading a dump's Control NCA takes the keys and the disk, which the window's thread waits for
   neither. iOS runs the driver on a thread of the app, one at a time, so there the menu's loop asks them as the queue's
   recompiles go (icons_pump). Games puts a dump in (dump_icon_ask); each is asked a dump at a time into its folder of
   the cache (dump_icon_dir), and the answers counted, which the menu takes to look at those rows again. A dump the
   driver gave no icon for is kept with why for the session, in memory alone: its row says why, the log has what the
   driver said, and the next start asks again */
#define GUI_DUMP_ASKS 128   /* the dumps waiting at most: a page's and more */
#define GUI_ICON_WAIT 30000 /* the milliseconds the driver has to answer, then it is stopped */
typedef struct {
    char name[24]; /* the dump's folder in the cache, dump_icon_dir's last part */
    char why[160]; /* what the driver's answer came to, its last line */
} GuiIconFailure;
/* a dump being asked: the driver's process, what it printed (its first said bytes), since when */
typedef struct {
    GuiProcess* process;
    char *dump, *dir;
    char said[16384];
    size_t size;
    Uint64 since;
} GuiIconAsk;
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
    int wait_ms;       /* GUI_ICON_WAIT, a test's own less */
    GuiIconFailure* failed; /* failed_count of them, room for failed_room */
    int failed_count, failed_room;
    GuiIconAsk ask; /* iOS: the one icons_pump asks now (process set) */
} GuiDumpIcons;
/* the App screen's values, as its rows show them, and Folders' (what each row's folder is, past its path) */
typedef struct {
    char theme[64], text[64], language[64], animations[16], side[16], background[16], places[48], moves[32];
    char folder[GUI_FOLDER_ROWS][64];
} GuiAppForm;
/* the head's measures: the row of tabs (its height and the tabs' width), the title's height and width (0: none), the
   user's box (0: none), a cell of the text */
typedef struct {
    int tabs, tabs_w, title, title_w, badge, badge_w, cw, ch;
} GuiHead;
/* the screen place_screen lays out, for the frame's callbacks (ui.txt's Screen and Wide): its head, its keys, whether
   it has the side box and the status box, and its tips in the foot beside the keys rather than on a row of their own */
typedef struct {
    GuiHead head;
    const char* keys;
    bool side, status, tips_foot;
} GuiFrame;
/* the folders of a recompiled game its box's Open Folder lists, in its order (the mods' and the cheats' have rows of
   their own, beside Add Mod and Add Cheats) */
enum {
    GUI_FOLDER_DUMP,
    GUI_FOLDER_GAME,
    GUI_FOLDER_SAVES,
    GUI_FOLDER_SETTINGS,
    GUI_FOLDER_LOGS,
    GUI_FOLDER_CACHES,
    GUI_GAME_FOLDERS
};
typedef struct App {
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* font;
    GuiProcess* process;
    GuiMenu menu;
    GuiBrowser browser;
    GuiGames games;
    int* games_drivers; /* the driver that plays each of games' rows (row_driver), read as list_games lists them */
    /* each of games' rows and of the recent ones: its .ini has a Debug setting off its default, read as list_games
       lists them; and the picks or the GDB stub put one on every game, as Games is drawn (game_debug) */
    bool *games_debug, recent_debug[GUI_RECENT_MAX], debug_picks;
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
    int clean_found;   /* what Clean Old Settings would take out, as App last looked (clean_left); -1 before a look */
    /* UI scale's slider, in its dialog: its bar as last drawn, empty when none shows; a drag on it going on */
    GuiRect slider_bar;
    bool slider_drag;
    bool left_shown;   /* in a box as the menu opens, until a key, a button or a finger closes it */
    bool left_clear;   /* the menu's own files of menu_old among them: the box says how they go (SAY_LEFT_CLEAR) */
    char* menu_old;    /* Android: SDL's folder for the app, its own files, where an older version kept the menu's own
                          files (menu_files); NULL elsewhere */
    Uint64 left_at;    /* when they were looked for */
    char* recent_file; /* where the recent games are kept; NULL keeps none */
    char* run_log;     /* where a run's output goes as it comes, the last run's; NULL keeps none */
    intptr_t run_lock; /* run_log held (snx_lock_file) while run_locked, so another launcher takes a pair of its own */
    bool run_locked;
    FILE* run_out;
    int recent_named;           /* the run's own name went to its recent game */
    bool recent_unsaved;        /* the list could not be written the last time it changed: the message says so */
    Uint64 clock;               /* fixed time for tests; 0 is the real clock */
    Uint64 fade_ms, leaving_ms; /* a screen fading in from black since; the run fading out since */
    Uint64 entering_ms;         /* the menu fading out since, before a run fades in */
    char* self;    /* the program that runs Switch inputs: the runtime the menu is part of, NULL to look for one */
    char* drivers; /* the folder the other drivers are looked for in first */
    /* the menu's loads whose driver is not there (loads_off), a bit an item, as looked for at loads_at with drivers,
       self and PATH hashing to loads_key */
    unsigned loads_off;
    uint64_t loads_key;
    Uint64 loads_at;
    char *cwd, *file, *games_dir, *keys_path;
    char*
        data_dir; /* recompiled/, dumps/, work/ and themes/: the driver's too, which gets it as --repo (snx_data_dir) */
    char* checkout;       /* a build tree's menu's checkout (snx_checkout), where an older version kept games/ */
    char* themes_dirs[2]; /* themes/ in the data folder, then beside the GUI */
    char message[512];
    Uint64 keys_done; /* the keys screen's outcome shown since, 0 for none: then the menu by itself */
    bool keys_ok;
    int cols, rows, scale;
    int auto_scale; /* the UI scale a start takes with none in app.ini, chosen as the window opens (pick_auto_scale) */
    Uint64 sampled;
    bool has_output, finished, aborting, abnormal_exit;
    bool log_due; /* the last run ended abnormally, not by ESC, and its log is not saved since: Share Log blinks */
    char row_text[700]; /* the text of the row a list of ui.txt asked for last, until it is drawn (get_row_info) */
    char pick_text[96]; /* a picks box's choice with its mark, "(*) Subfolders" (pick_label) */
    GuiConfirm confirm; /* the question before a run is aborted, when ask_abort says to ask it */
    int choices_of;     /* whose choices ui.txt's scene Choices draws or hits now (GuiChoicesOf) */
    bool ask_abort;     /* ESC asks first: on a phone it is Back, an edge swipe easily made by mistake */
    int native;
    bool recompile; /* R on Games: the run recompiles its game (-r) and plays nothing */
    /* B on Games: the focused game's JIT cache (aot.c), and the pack build it starts */
    int aot_state;       /* GUI_AOT_* */
    GuiAotInfo aot_info; /* the cache's header */
    char* aot_focus;     /* the focused game's path the state is for, owned */
    char aot_build_opt;  /* the launcher's opt setting, one char */
    int aot_match;         /* the cache's level is the build's */
    Uint64 aot_checked;    /* when aot_focus was looked at */
    SnxGameMade made;      /* how the focused game's code was made, its stamp's made line (unknown: opt 0) */
    bool made_here;        /* this build made it with the mods it plays: false, Play compiles it first */
    /* the Debug settings a run of the focused row takes off their default (game_debug), for its path, owned, as it
       was looked at */
    char debug_focus[1024], *debug_path;
    Uint64 debug_checked;
    bool aot_pack;   /* the run builds a pack from the cache */
    char* aot_cache; /* its .jitcache, owned */
    char aot_opt[2]; /* the -O the build uses */
    int aot_hook;    /* --hook-direct-calls */
    /* R's and B's recompiles, which play nothing: a queue (queue.h) run one at a time behind the menu's screens, the
       one running on a process of its own with its output, its steps and its log; the last one that ended kept as it
       was, for its row on the Compile tab */
    GuiQueue queue;
    GuiProcess* job_process;
    GuiConsole job_console, ended_console;
    GuiSteps job_steps, ended_steps;
    uint32_t ended_serial; /* the job ended_console and ended_steps are of, 0 for none */
    FILE* job_out;
    char* job_log;      /* where the running one's output goes, beside run_log; NULL keeps none */
    GuiUsage job_usage; /* its tree alone */
    bool job_aborting;
    bool job_low;      /* its priority lowered: Background recompile Gentle, or a game playing */
    bool job_kept_low; /* the system refused to raise it again: low until it ends, job_said says why */
    char job_said[160];
    int job_threads;        /* the threads it was started with: 0 for the driver's own count (the AOT threads') */
    int queue_focus;        /* the Compile tab's row, a job of the queue */
    bool background_gentle; /* app.ini's recompile_background = gentle: every recompile of the queue is */
    /* the taskbar's progress as SDL was last given it, the value in percent: none until a recompile runs. What SDL
       said when it could not be shown, "" when it was */
    int taskbar_state, taskbar_value;
    char taskbar_error[256];
    Uint64 finger;            /* the finger on the screen (finger_id), 0 for none */
    float finger_x, finger_y; /* where it went down, 0 to 1 across the window */
    bool pen_held;            /* a pen lifted: its hover leaves the focus where the lift put it until it moves away */
    float pen_x, pen_y;       /* where it lifted, 0 to 1 across the window */
    bool hovering;            /* a mouse or a pen over the window, at hover_x, hover_y of the render: a key of the foot
                                 under it that a click would press is drawn lit */
    float hover_x, hover_y;
    /* the focus last put by the pointer (a mouse, a pen, a tap), not a key, a pad, the wheel or a drag: a list keeps
       its first row as last shown (firsts, a GUI_LIST_* each) while the focus shows; a key's goes in its middle */
    bool pointed;
    size_t firsts[GUI_LISTS];
    /* the scene lists drawn this frame (ui_bar), with their scroll bars, under a box none but its own; the list whose
       thumb a press holds, plus one (bar_hold, 0: none), and for each list whose rows the thumb or a finger put, its
       focus plus one then (SIZE_MAX: not seen since): they stay there until the focus moves, or the pointer puts it on
       a row they do not show (0: they follow it) */
    GuiBar bars[GUI_BARS];
    int nbars, bar_hold;
    size_t bar_focus[GUI_LISTS];
    /* where the thumb or a finger put each list: its top list_y pixels down its rows, list_max at most, flung at list_v
       pixels a second; a row's step and the rows shown as last laid out. The list a finger's drag scrolls, plus one
       (list_held, 0: none), the list's y and the finger's where the drag took it (list_hold_*), its last move */
    float list_y[GUI_LISTS], list_v[GUI_LISTS], list_max[GUI_LISTS], list_hold_y, list_hold_at, list_hold_x,
        list_last_y;
    int list_step[GUI_LISTS], list_held;
    size_t list_shown[GUI_LISTS];
    Uint64 list_last_ms, lists_tick;
    /* Games' list (View List), one page that scrolls: its top games_y pixels down its rows, going to games_to (the
       focus a key moved, the wheel) or flung at games_v pixels a second once a finger let go; a finger holding it
       (games_held: the list's y where it went down and the finger's then, games_hold_*, its last move for the fling),
       the scroll bar's thumb held (bar_held), and the focus the list last followed (games_seen) */
    float games_y, games_to, games_v, games_hold_y, games_hold_at, games_last_y;
    bool clip_had;    /* a clip set when a list's shifted rows took theirs (ui_clip), put back after them */
    bool rows_barred; /* the rows being drawn have a scroll bar beside them: no arrows of their own */
    SDL_Rect clip_was;
    bool games_held, bar_held;
    size_t games_seen;
    Uint64 games_tick, games_last_ms;
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
    SDL_Texture* icon_loading; /* what a dump's row shows while the driver is asked for its icon */
    SDL_Texture* icon_none;    /* what a game's row shows with no icon of its own: Wimsy in grey */
    SDL_Texture* icon_more;    /* Show More's, as a game's cover: a grey + on white */
    GuiDumpIcons dump_icons;   /* the dumps' icons asked of the driver */
    unsigned dump_icons_seen;  /* the answers the rows were drawn after */
    size_t icons_next;
    int user_side, user_shape;
    /* the box a click on the user opens (users_open): each user of users.ini, the focused one picked as Settings' user */
    bool users_shown;
    int users_focus;
    /* Settings > App's UI scale: its dialog over the rows, the size its slider picks (size_pick) and OK and Cancel; OK
       uses it in place of size_was until size_until, when it goes back unless kept, the dialog asking meanwhile in
       size_line with Keep and Back; size_until 0 while it picks */
    bool sizes_shown;
    GuiChoice sizes;
    int size_pick, size_was;
    Uint64 size_until;
    char size_line[128];
    /* a resolution past 4x or under 0.25x: its row flashes in the warning colour and the foot says why, until
       warn_until; warn_key is its .ini key, empty for none */
    char warn_key[64];
    Uint64 warn_until;
    SDL_Texture* users_pictures[SNX_USERS_MAX]; /* theirs, users_side pixels square, made as the box is drawn */
    int users_side;
    /* the box opening a game with updates, DLCs or mods beside it shows (addons_open): a box to check each of them
       by, the game's addons= and mods_off= when it opened, then Play, which runs as recompile was when it opened */
    bool addons_shown, addons_recompile;
    int addons_focus;
    /* Open Folder's page of the box over its rows, a folder of the game a row (GUI_FOLDER_*): each one's path, owned,
       NULL for one not known; addons_back the box's focus to go back to */
    bool addons_folders_shown;
    char* addons_folders[GUI_GAME_FOLDERS];
    int addons_back;
    char *addons_game, *addons_title; /* the game's path and what it is shown as */
    bool addons_debug;                /* it runs with a Debug setting off its default: its Play says so */
    GuiGames addons;
    bool* addons_checked;      /* each of addons' */
    char *mods_folder, **mods; /* the game's mods folder and its mods (gui_game_mods), mods_count of them */
    size_t mods_count;
    bool* mods_on;                 /* each of mods': played, not in mods_off= */
    char *cheats_folder, **cheats; /* the game's cheats folder and its cheats' names (gui_game_cheats), cheats_count */
    size_t cheats_count;
    bool* cheats_on;     /* each of cheats': run, not in cheats_off= */
    Uint32 addon_picked; /* the event the system's picker answers Add Mod's folder or Add Cheats' file with */
    int addon_adding;    /* the row it was opened for, ROW_ADD_MOD or ROW_ADD_CHEATS; 0 while none waits */
    char* adding_game;   /* and the game whose box it was, which the answer goes to only while that box shows */
    SDL_DialogFileFilter cheats_filter; /* Add Cheats' .txt, which the file picker reads until it answers */
    /* the box of a game's save states (states_open), the newest first, and the focused one's picture as a texture */
    bool states_shown;
    int states_focus, states_texture_of; /* the state the texture is of, -1 for none */
    char* states_title;
    struct GuiState* states;
    size_t states_count;
    SDL_Texture* states_texture;
    char*
        state_load; /* the --state_load= option launch adds to a Switch game's run, while Play from a state starts it */
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
    bool folders_from_games;   /* Folders goes back to Games, whose P opened it */
    bool folders_from_menu;    /* and to the menu, whose Folders opened it */
    int half;                  /* the letters in halves of a cell while text_half has them so, else 0: the scale's */
    bool help_shown;           /* F1's box over the screen: every key it has (screen_said) */
    SnxPadScreen pad;          /* the button map's, on the row gamepad_map, A and B as gamepad_swap_ab says */
    SnxPadDrawing pad_drawing; /* as last drawn: what a click or a tap lands on */
    SDL_Gamepad* gamepad;      /* the pad it shows, the first one plugged in, while it is open */
    bool pads_read;            /* SDL's gamepads started for it */
    unsigned pad_pushed;       /* the triggers and the left stick's directions past half way, a bit each */
    /* the menu's own pads, whose presses are its keys (pad_menu.h), open until a run's game loads; pads_failed: SDL
       could not start them, for pads_error, not asked again. pad_driven: a pad pressed last, not the keyboard, the
       mouse or a finger, so the cursor is hidden and a mouse moving less than pointer_travel's GUI_PAD_TRAVEL moves
       nothing */
    GuiPads pads;
    bool pads_failed, pad_driven;
    char pads_error[256];
    float pointer_travel;
    /* the Settings' groups: App, then a group per section of the settings, its first row and rows; a row each */
    int groups, group_first[GUI_GROUPS_MAX], group_rows[GUI_GROUPS_MAX];
    GuiThemeKey group_keys[GUI_GROUPS_MAX];
    char group_name[GUI_GROUPS_MAX][64]; /* each one's key: a copy, the list it is from being freed apart */
    char group_about[GUI_GROUPS_MAX][128];
    GuiAppForm app_form;
    GuiFrame frame; /* what place_screen measured of the screen it lays out */
    GuiUsersForm users_form;
    GuiThemeKey users_keys[SNX_USERS_MAX + 1]; /* Current user, then a row a user */
    char users_label[SNX_USERS_MAX][32];
    int language;        /* Settings > App's Language: 0 Auto (the system's), else snx_gui_languages' index + 1 */
    char* games_key;     /* [folders] games = as app.ini had it, recompiled = before it was named so: written again */
    char* text_size_key; /* [app] text_size = as app.ini had it, ui_scale = before it was named so: written again */
    char* scale_gone;    /* [app] ui_scale = or text_size = Auto or a part of it, "ui_scale = auto": written again */
    bool scale_new;      /* no UI scale in app.ini as the start read it: the start's, written by setup_start */
    GuiCleaned cleaned;  /* what it did, in a box over the screen while cleaned_shown, until a key closes it */
    bool cleaned_shown;
    bool report_move; /* the box shows what Move Old Files did, not Clean Old Settings */
    /* app.ini's animations: a screen comes in from the page's colour and a box's shade comes and goes, GUI_FADE_MS
       each; with it off (0) each shows at once */
    bool animations_off;
    bool
        fingers; /* a screen of fingers (SNX_TOUCH_UI): the tabs and the foot's keys a finger's row high, its letters */
    int side_box;        /* app.ini's side_box (GuiSide): auto, on or off; F2 shows or hides it */
    int options[GUI_OPTIONS]; /* Games' options, each its word's index in option_keys (0 its first word) */
    bool options_shown;       /* the View box over Games, a row an option, options_focus's in focus */
    int options_focus;
    unsigned screen_was; /* the screen drawn last (screen_mark), for when another comes */
    bool form_beside;    /* the form laid out last showed the groups and the side panel's rows beside them */
    bool boxed_was;      /* a box over it then */
    Uint64 switch_ms, box_ms;
    Uint64 closing_ms; /* the short screen of what the menu keeps, shown as it closes, from then; 0 for none */
    bool look_only; /* scarletnx --migrate, =yes left out: nothing is written, app.ini's rewrite of an older key too */
    int files_top, files_shown;     /* the box of files over the screen (render_files): its first file, and how many */
    char* folders[GUI_FOLDERS_MAX]; /* the player's game folders, listed after recompiled/ and dumps/ */
    bool folders_deep[GUI_FOLDERS_MAX];   /* each with its subfolders' games too */
    bool dumps_shallow; /* dumps/ of the data folder without its subfolders' games: app.ini's dumps_subfolders = off */
    int folders_library[GUI_FOLDERS_MAX]; /* each one's GUI_LIBRARY_* */
    int adding;                           /* the library Add a folder adds to, -1: the one its games say */
    int asked_holds;                      /* folder_asked's libraries, 1 << GUI_LIBRARY_* */
    int folder_count;
    char* folder_asked; /* a folder picked with folders in it, asked whether their games are listed too */
    char* folders_file; /* where they are kept; NULL keeps none */
    GuiThemeKey folder_keys[GUI_FOLDER_ROWS];
    char* library_dumps;  /* dumps/ of the data folder, as Folders' row names it while it is open */
    int folder_off;       /* the game folder GUI_ASK_FOLDER asks about, its index in folders */
    uint64_t games_stamp; /* what the game folders held when last listed, as the watcher hashes them */
    bool games_fresh;     /* listed just now: the watcher's next hash is what the list holds */
    GuiWatch watch;
    bool recent_all;        /* every recent game shows, not GUI_RECENT_SHOWN and a row for the rest */
    Uint32 picked_folder;   /* the event a folder from the system's picker comes back as */
    SDL_Texture* preview;   /* the editor's: the menu or a run drawn in its theme, at 1x */
    GuiSteps preview_steps; /* the run it shows, over again every GUI_PREVIEW_MS */
    GuiConsole preview_console;

    /* Settings > App > Folders */
    char* home_dir;          /* the data folder snx_data_dir picks: the menu's own files go by it, [folders] or not */
    char* place[GUI_PLACES]; /* app.ini's [folders]: data, games, work, state and cache picked in Folders; NULL the
                                default */
    char* work_dir;          /* the extractions: [folders] work, else the data folder's work/ in its cache */
    char *state_dir, *cache_dir; /* the logs and the caches: [folders] state and cache, else the menu's own */
    char* config_dir;            /* the settings, app.ini's folder: shown only */
    char place_about[GUI_PLACES][384];
    char place_line[GUI_PLACES][4200]; /* the folder in use, whole, under the rows */
    char places_note[3 * 4200];        /* the three, under App's rows */
    int place_row;                     /* the row the folder picker was opened for, -1 none */
    char* own_dir;      /* Android: the app's own storage; a folder outside it is asked about (GUI_ASK_OUTSIDE) */
    char* outside_path; /* the folder asked about, for the row outside_place */
    int outside_place;
    char* made_file; /* a dump whose game is made, asked about (GUI_ASK_MADE): played or recompiled, by made_item */
    int made_item;
    bool made_ok;    /* asked and answered Play: the dump runs, its driver playing the game made */
    bool compiled;   /* the game a compile just made plays (job_play): its stamp is not looked at again */
    char* room_file; /* a run asked about (GUI_ASK_ROOM): its file, its load and whether it was prepared */
    int room_item;
    bool room_prepared, room_recompile;
    bool room_ok;    /* asked and answered: the run goes */
    GuiChoice dests; /* Recompile: which of the recompiled library's folders a dump's game goes in */
    bool dests_shown;
    bool picks_shown; /* a box of a few choices over the screen (picks_open): Share Log's, a Folders row's */
    GuiPicksOf picks_of;
    GuiChoice picks;
    int pick_row;                      /* the Folders row the box is of */
    bool (*open_url)(const char* url); /* SDL_OpenURL, which a test takes the place of (show_folder) */
    char* dest_file; /* the run asked about, as room_file, and its load */
    int dest_item;
    bool dest_prepared, dest_recompile;
    char* dest_out;           /* the folder picked there: the run's --outdir, until it starts or is not run */
    char room_line[96];       /* the question: what it takes, what is free */
    char ask_line[4400];      /* GUI_ASK_CLEAN's to GUI_ASK_FOLDER's question, with what the change takes or where */
    char error_line[4400];    /* GUI_ASK_ERROR's: the failure, whole */
    uint32_t errors_said[16]; /* the failures it said in the session, by their text's FNV-1a: each said once */
    uint32_t errors_logged[64]; /* and those it wrote in its log (gui_log), shown or not: each written once */
    int logged_count;
    int errors_count;
    int place_off;      /* the folder GUI_ASK_PLACE puts back to its default */
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
    bool telling;            /* the notification shows a run, the menu's or a recompile of the queue */
    const char* told_result; /* how the last of them ended, English (snx_tr's), for the one left once none runs */
    bool playing; /* a game started here plays in the game activity, the menu behind it until it shows again */
#endif
} App;

/* the menu's item i: a row of ui.txt's Entries, in model.h's order (a negative size if not); its text shown
   translated */
#define GUI_ENTRY(item) (UI_MENU_ENTRIES_LOAD_SWITCH_NSP_XCI_NRO + (item))
typedef char gui_entries_in_order[GUI_ENTRY(GUI_LINUX) == UI_MENU_ENTRIES_LOAD_LINUX_ARM_ELF &&
                                          GUI_ENTRY(GUI_3DS) == UI_MENU_ENTRIES_LOAD_3DS_GAME &&
                                          GUI_ENTRY(GUI_VITA) == UI_MENU_ENTRIES_LOAD_VITA_VPK &&
                                          GUI_ENTRY(GUI_GBA) == UI_MENU_ENTRIES_LOAD_GBA_ROM &&
                                          GUI_ENTRY(GUI_KEYS) == UI_MENU_ENTRIES_INSTALL_KEYS &&
                                          GUI_ENTRY(GUI_SETTINGS) == UI_MENU_ENTRIES_SETTINGS &&
                                          GUI_ENTRY(GUI_LOG) == UI_MENU_ENTRIES_SHARE_LOG &&
                                          GUI_ENTRY(GUI_FOLDERS) == UI_MENU_ENTRIES_FOLDERS &&
                                          GUI_ENTRY(GUI_EXIT) == UI_MENU_ENTRIES_EXIT &&
                                          GUI_ENTRY(GUI_ITEM_COUNT) == UI_MENU_ITEMS
                                      ? 1
                                      : -1];
static const char* entry_text(int item) { return ui_menu_texts + ui_menu_items[GUI_ENTRY(item)].text; }
#define GUI_RECENT_SHOWN 3 /* the recent games Games shows before a row that shows the rest */
/* what the screens say to press: a phone has a finger, the back button and the system's file picker, nothing to drop;
   a computer has the picker too (on Linux the desktop's portal, else zenity), on a screen of its own as the keys' */
#ifdef SDL_PLATFORM_ANDROID
#ifdef SNX_TOUCH_UI
#define SAY_MENU SNX_TR_KEYS("Tap: select. Refresh. Back.")
#else
#define SAY_MENU SNX_TR_KEYS("Tap: select. Refresh.")
#endif
#ifdef SDL_PLATFORM_ANDROID
#define SAY_GAMES SNX_TR_KEYS("Tap: open. Drag: scroll. Refresh. Back.")
#else
#define SAY_GAMES SNX_TR_KEYS("Tap: open. Drag: scroll. Refresh.")
#endif
#define SAY_GAMES_GRID SAY_GAMES /* a finger's are the list's */
/* Games' focused row, each when it can be done there (games_said) */
#define SAY_GAMES_R      SNX_TR_KEYS("Recompile.")
#define SAY_GAMES_M      SNX_TR_KEYS("Mods.")
#define SAY_GAMES_AOT    SNX_TR_KEYS("AOT pack.")
#define SAY_GAMES_STATES SNX_TR_KEYS("Save states.")
#ifdef SDL_PLATFORM_ANDROID
#define SAY_KEYS_HOW SNX_TR_KEYS("Back.")
#else
#define SAY_KEYS_HOW SNX_TR_KEYS("Pick the prod.keys file.")
#endif
#define SAY_KEYS_WAIT SNX_TR_NOOP("Pick prod.keys")
#define SAY_PICK_BOX  SNX_TR_NOOP("tap the box to open the file picker")
#define SAY_FILE_WAIT SNX_TR_NOOP("Pick a file")
#define SAY_DONE      SNX_TR_NOOP("Tap to return to menu")
#define SAY_ABORT     SNX_TR_NOOP("Tap here to abort and return to menu")
#define SAY_NO_GAMES  SNX_TR_NOOP("No games in recompiled/ or dumps/: put a game's .nro, .xci or .nsp in dumps/.")
#ifdef SDL_PLATFORM_ANDROID
#define SAY_EDITOR SNX_TR_KEYS("Tap: change. Back.")
#else
#define SAY_EDITOR SNX_TR_KEYS("Tap: change.")
#endif
#ifdef SDL_PLATFORM_ANDROID
#define SAY_PICKER SNX_TR_KEYS("Drag: change. Tap #RRGGBB: type. Tap outside: OK. Back: cancel.")
#else
#define SAY_PICKER SNX_TR_KEYS("Drag: change. Tap #RRGGBB: type. Tap outside: OK.")
#endif
#define SAY_SETTINGS         SAY_EDITOR
#define SAY_SETTINGS_DEFAULT NULL /* a finger has no Del */
#ifdef SDL_PLATFORM_ANDROID
#define SAY_GROUPS SNX_TR_KEYS("Tap: open. Back.")
#else
#define SAY_GROUPS SNX_TR_KEYS("Tap: open.")
#endif
#ifdef SDL_PLATFORM_ANDROID
#define SAY_APP SNX_TR_KEYS("Tap: change. Back.")
#else
#define SAY_APP SNX_TR_KEYS("Tap: change.")
#endif
#ifdef SDL_PLATFORM_ANDROID
#define SAY_FOLDERS SNX_TR_KEYS("Refresh. Back.")
#else
#define SAY_FOLDERS SNX_TR_KEYS("Refresh.")
#endif
/* Folders' focused row, each when it can be done there: a finger has no Del, the default is Reset to Defaults' */
#define SAY_CHANGE  SNX_TR_KEYS("Tap: open or change.")
#define SAY_DEFAULT NULL
#define SAY_DELETE  NULL /* a finger has no Del: its Tap's box removes it */
#define SAY_PICK    SNX_TR_KEYS("Tap: open or remove.")
#define SAY_OPEN    SNX_TR_KEYS("Tap: open.")
#define SAY_ADD     SNX_TR_KEYS("Tap: add.")
#define SAY_RESET   SNX_TR_KEYS("Tap: reset.")
#ifdef SDL_PLATFORM_ANDROID
#define SAY_PAD SNX_TR_KEYS("Tap: select. Tap again: press. Back.")
#else
#define SAY_PAD SNX_TR_KEYS("Tap: select. Tap again: press.")
#endif
#ifdef SDL_PLATFORM_ANDROID
#define SAY_USERS SNX_TR_KEYS("Tap: rename. Back.")
#else
#define SAY_USERS SNX_TR_KEYS("Tap: rename.")
#endif
/* the Compile tab's, each when it can be done there (queue_said); then its panel's line once the one in focus ended */
#define SAY_QUEUE_MOVE   SNX_TR_KEYS("Tap: select.")
#define SAY_QUEUE_TAB    SNX_TR_KEYS("Games.")
#define SAY_QUEUE_DEL    SNX_TR_KEYS("Take out.")
#define SAY_QUEUE_ABORT  SNX_TR_KEYS("Back: abort.")
#define SAY_QUEUE_BACK   SNX_TR_KEYS("Back.")
#define SAY_QUEUE_PLAY   SNX_TR_KEYS("Tap again: play.")
#define SAY_QUEUE_DONE   SNX_TR_NOOP("Back: to Games. Tap its row again: play")
#define SAY_QUEUE_FAILED SNX_TR_NOOP("Back: to Games")
#else
#define SAY_MENU         SNX_TR_KEYS("Arrows/mouse: move. Space/Enter/click: select.")
#define SAY_GAMES        SNX_TR_KEYS("Up/Down: select. Enter/Space: open. Left/Right/PgUp/PgDown: scroll. F5: refresh.")
#define SAY_GAMES_GRID   SNX_TR_KEYS("Arrows: select. Enter/Space: open. PgUp/PgDown: scroll. F5: refresh.")
/* Games' focused row, each when it can be done there (games_said) */
#define SAY_GAMES_R      SNX_TR_KEYS("R: recompile.")
#define SAY_GAMES_M      SNX_TR_KEYS("M: mods.")
#define SAY_GAMES_AOT    SNX_TR_KEYS("B: AOT pack.")
#define SAY_GAMES_STATES SNX_TR_KEYS("S: save states.")
#define SAY_PICK_BOX     SNX_TR_NOOP("click the box (or Enter) to open the file picker")
#define SAY_KEYS_HOW     SNX_TR_KEYS("Ctrl+V: paste. ESC: back.")
#define SAY_KEYS_WAIT    SNX_TR_NOOP("Pick or drop prod.keys")
#define SAY_FILE_WAIT    SNX_TR_NOOP("Pick or drop a file")
#define SAY_DONE         SNX_TR_NOOP("Press [ESC] to return to menu")
#define SAY_ABORT        SNX_TR_NOOP("Press [ESC] to abort and return to menu")
#define SAY_NO_GAMES     SNX_TR_NOOP("No games in recompiled/, dumps/ or the libraries of Settings > App > Folders.")
#define SAY_EDITOR       SNX_TR_KEYS("Up/Down: key. Left/Right: change. Enter: edit. Ctrl+S: save. ESC: back.")
#define SAY_PICKER       SNX_TR_KEYS("Up/Down: row. Left/Right: step 1. PgUp/PgDn: step 16. Enter: OK. ESC: cancel.")
#define SAY_SETTINGS     SNX_TR_KEYS("Up/Down: setting. Left/Right: change. Enter: type. ESC: back.")
#define SAY_SETTINGS_DEFAULT SNX_TR_KEYS("Del: default.") /* a setting not at its default */
#define SAY_GROUPS           SNX_TR_KEYS("Up/Down: group. Enter: open. ESC: back.")
#define SAY_APP              SNX_TR_KEYS("Up/Down: row. Left/Right: change. Enter: change or open. ESC: back.")
#define SAY_FOLDERS          SNX_TR_KEYS("Up/Down: folder. F5: refresh. ESC: back.")
/* Folders' focused row, each when it can be done there */
#define SAY_CHANGE           SNX_TR_KEYS("Enter: open or change.")
#define SAY_DEFAULT          SNX_TR_KEYS("Del: default.")
#define SAY_DELETE           SNX_TR_KEYS("Del: delete.")
#define SAY_PICK             SNX_TR_KEYS("Enter: open or remove.")
#define SAY_OPEN             SNX_TR_KEYS("Enter: open.")
#define SAY_ADD              SNX_TR_KEYS("Enter: add.")
#define SAY_RESET            SNX_TR_KEYS("Enter: reset.")
#define SAY_PAD              SNX_TR_KEYS("Arrows/pad: move, change. Enter/A: select. Del: default. ESC: back.")
#define SAY_USERS            SNX_TR_KEYS("Up/Down: user. Left/Right: current user. Enter: rename. ESC: back.")
/* the Compile tab's, each when it can be done there (queue_said); then its panel's line once the one in focus ended */
#define SAY_QUEUE_MOVE       SNX_TR_KEYS("Up/Down: queue.")
#define SAY_QUEUE_TAB        SNX_TR_KEYS("Tab: games.")
#define SAY_QUEUE_DEL        SNX_TR_KEYS("Del: take out.")
#define SAY_QUEUE_ABORT      SNX_TR_KEYS("ESC: abort.")
#define SAY_QUEUE_BACK       SNX_TR_KEYS("ESC: games.")
#define SAY_QUEUE_PLAY       SNX_TR_KEYS("Enter: play.")
#define SAY_QUEUE_DONE       SNX_TR_NOOP("Press [ESC] to return to Games, Enter to play")
#define SAY_QUEUE_FAILED     SNX_TR_NOOP("Press [ESC] to return to Games")
#endif
/* and what they say while a pad drives the menu (pad_menu.h's keys), on every system: its face's letters as an Xbox
   pad's and a Switch one's name them, a PlayStation pad's shapes in their place (gui_pad_keys_named) */
#define SAY_PAD_MENU             SNX_TR_KEYS("D-pad: move. A: select. Start: refresh. B: back.")
#define SAY_PAD_GAMES            SNX_TR_KEYS("D-pad: select. A: open. LT/RT: scroll. Start: refresh. B: back.")
#define SAY_PAD_GAMES_R          SNX_TR_KEYS("Y: recompile.")
#define SAY_PAD_GAMES_M          SNX_TR_KEYS("L3: mods.")
#define SAY_PAD_GAMES_AOT        SNX_TR_KEYS("R3: AOT pack.")
#define SAY_PAD_GAMES_STATES     SNX_TR_KEYS("X: save states.")
#define SAY_PAD_KEYS_HOW         SNX_TR_KEYS("A: pick a file. B: back.")
#define SAY_PAD_EDITOR           SNX_TR_KEYS("D-pad: key, change. A: edit. B: back.")
#define SAY_PAD_PICKER           SNX_TR_KEYS("D-pad: row, step 1. LT/RT: step 16. A: OK. B: cancel.")
#define SAY_PAD_SETTINGS         SNX_TR_KEYS("D-pad: setting, change. A: type. B: back.")
#define SAY_PAD_SETTINGS_DEFAULT SNX_TR_KEYS("X: default.")
#define SAY_PAD_GROUPS           SNX_TR_KEYS("D-pad: group. A: open. B: back.")
#define SAY_PAD_APP              SNX_TR_KEYS("D-pad: row, change. A: change or open. B: back.")
#define SAY_PAD_FOLDERS          SNX_TR_KEYS("D-pad: folder. Start: refresh. B: back.")
#define SAY_PAD_CHANGE           SNX_TR_KEYS("A: open or change.")
#define SAY_PAD_PICK             SNX_TR_KEYS("A: open or remove.")
#define SAY_PAD_OPEN             SNX_TR_KEYS("A: open.")
#define SAY_PAD_DEFAULT          SNX_TR_KEYS("X: default.")
#define SAY_PAD_DELETE           SNX_TR_KEYS("X: delete.")
#define SAY_PAD_ADD              SNX_TR_KEYS("A: add.")
#define SAY_PAD_RESET            SNX_TR_KEYS("A: reset.")
#define SAY_PAD_USERS            SNX_TR_KEYS("D-pad: user, current user. A: rename. B: back.")
#define SAY_PAD_TAB              SNX_TR_KEYS("LB/RB: screen.")
#define SAY_PAD_QUEUE_MOVE       SNX_TR_KEYS("D-pad: queue.")
#define SAY_PAD_QUEUE_TAB        SNX_TR_KEYS("RB: games.")
#define SAY_PAD_QUEUE_DEL        SNX_TR_KEYS("X: take out.")
#define SAY_PAD_QUEUE_ABORT      SNX_TR_KEYS("B: abort.")
#define SAY_PAD_QUEUE_BACK       SNX_TR_KEYS("B: games.")
#define SAY_PAD_QUEUE_PLAY       SNX_TR_KEYS("A: play.")
/* a failure of the system's (a file not written, a picker that did not open): in a box over the screen, whole, until
   OK, once in the session; the tips or the note keep it after. Over a run or another question, those alone. In the
   launcher's log once in the session either way */
static void say_failure(App* app, const char* text) {
    uint32_t hash = 2166136261u;
    const char* c;
    int i;
    for (c = text; *c; ++c) hash = (hash ^ (unsigned char)*c) * 16777619u;
    for (i = 0; i < app->logged_count && i < (int)SDL_arraysize(app->errors_logged) && app->errors_logged[i] != hash;
         ++i) {}
    if (i == SDL_min(app->logged_count, (int)SDL_arraysize(app->errors_logged))) {
        gui_log("%s", text);
        app->errors_logged[app->logged_count++ % (int)SDL_arraysize(app->errors_logged)] = hash;
    }
    if (app->menu.running || app->finished || app->confirm.shown) return;
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
/* the glyphs a row of the font's texture: 512 pixels wide, so 8192 of them (CP437's, the Latin letters, the icons and
   3780 wide letters) fit 2048 high, where 16 a row would take 8192 */
#define GUI_FONT_ROW 64
/* the font's glyphs: CP437's 256, then the Latin letters past it, then each wide letter's two halves, then each
   icon's (gui_menu_glyph) */
static SDL_Texture* create_font(SDL_Renderer* renderer) {
    int wide = 256 + gui_latin_count + 2 * gui_wide_count, glyphs = wide + 2 * GUI_ICON_COUNT;
    SDL_Surface* surface =
        SDL_CreateSurface(GUI_FONT_ROW * 8, (glyphs + GUI_FONT_ROW - 1) / GUI_FONT_ROW * 16, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return NULL;
    for (int glyph = 0; glyph < glyphs; ++glyph) {
        const uint8_t* rows = glyph < 256                     ? gui_vga_font + glyph * 16
                              : glyph < 256 + gui_latin_count ? gui_latin_font + (glyph - 256) * 16
                              : glyph < wide                  ? gui_wide_font + (glyph - 256 - gui_latin_count) * 16
                                                              : gui_icon_font + (glyph - wide) * 16;
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
    int half = app->half ? app->half : 2 * app->scale;
    glyph_sized(app, x, y, c, 4.0f * (float)half, 8.0f * (float)half);
}
/* text from a pixel position, cols columns of it at most (a wide letter that would cross them left out), alpha its
   opacity, cut at the last column; where it ends */
static float text_px(App* app, float x, float y, const char* value, size_t cols, SDL_Color color, float alpha) {
    size_t used = 0;
    float w = 4.0f * (float)(app->half ? app->half : 2 * app->scale), limit = (float)(app->cols - 1) * w;
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
#ifdef SDL_PLATFORM_IOS
/* the app's container is no business of the player: what the Files app shows as ScarletNX is Documents, so a path in
   it is told from ScarletNX on, and one in Library from there. Every container path in s is rewritten, into out */
static const char* tidy_paths(const char* s, char* out, size_t size) {
    static const char* const marks[] = {"/private/var/mobile/Containers/Data/Application/",
                                        "/var/mobile/Containers/Data/Application/"};
    size_t n = 0;
    bool changed = false;
    while (*s && n + 1 < size) {
        const char* rest = NULL;
        for (size_t i = 0; i < 2 && !rest; ++i)
            if (!strncmp(s, marks[i], strlen(marks[i])) && strlen(s + strlen(marks[i])) > 36 &&
                s[strlen(marks[i]) + 36] == '/')
                rest = s + strlen(marks[i]) + 37;
        if (rest) {
            changed = true;
            if (!strncmp(rest, "Documents", 9) && (rest[9] == '/' || !rest[9])) {
                n += (size_t)snprintf(out + n, size - n, "ScarletNX");
                s = rest + 9;
            } else
                s = rest;
            if (n >= size) n = size - 1;
        } else
            out[n++] = *s++;
    }
    out[n < size ? n : size - 1] = 0;
    return changed ? out : NULL;
}
#endif
static void text(App* app, int x, int y, const char* value, SDL_Color color) {
#ifdef SDL_PLATFORM_IOS
    char tidy[1100];
    const char* shorter = tidy_paths(value, tidy, sizeof tidy);
    if (shorter) value = shorter;
#endif
    int skip = gui_slide(gui_text_cols(value) - (app->cols - 1 - x), now_ms(app), app->theme.scroll_ms);
    int w = 4 * (app->half ? app->half : 2 * app->scale); /* a cell of the letters in use, as cell_w and cell_h */
    text_px(app, (float)(x * w), (float)(y * 2 * w), gui_cols_skip(value, skip), SIZE_MAX, color, 1);
}
static int cell_h(const App* app) { return app->half ? 8 * app->half : 16 * app->scale; }
static int cell_w(const App* app) { return app->half ? 4 * app->half : 8 * app->scale; }
/* the letters in use as a scale, the text's, or a half step's (1.5 by a 2x text): what a run's ring and bar follow */
static float letters(const App* app) { return (float)(app->half ? app->half : 2 * app->scale) / 2.0f; }
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
                                             {"recent.txt", "last-run.log", "last-recompile.log", ""}};
/* the folder of kind for the menu's own files (menu_files), made: its data folder's (snx_app_dir: the user's, a
   portable copy's, Android's in the app's own storage), else the user's. Made with make; NULL when none can be */
static char* menu_folder(const App* app, int kind, bool make) {
    const char* picked = kind == SNX_USER_STATE   ? app->place[GUI_PLACE_STATE]
                         : kind == SNX_USER_CACHE ? app->place[GUI_PLACE_CACHE]
                                                  : NULL;
    char* folder = picked ? snx_strdup(picked) : snx_menu_dir(app->home_dir ? app->home_dir : app->data_dir, kind);
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
    text(app, 1, row + 1, snx_tr("Working folder:"), rgb(bar ? page : soft));
    text(app, 2 + gui_text_cols(snx_tr("Working folder:")), row + 1, app->cwd ? app->cwd : snx_tr("(unavailable)"),
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
static const char* const place_names[GUI_PLACES] = {"data", "recompiled", "work", "state", "cache"};
static const char* const place_labels[GUI_PLACES] = {SNX_TR_NOOP("Data"), SNX_TR_NOOP("Recompiled"),
                                                     SNX_TR_NOOP("Extractions"), SNX_TR_NOOP("Logs"),
                                                     SNX_TR_NOOP("Caches")};
/* side_box's words in app.ini, GuiSide's order, and as Settings > App shows them */
static const char* const side_words[] = {"auto", "on", "off"};
static const char* const side_names[] = {SNX_TR_NOOP("Auto"), SNX_TR_NOOP("On"), SNX_TR_NOOP("Off")};
/* recompile_background's, full its default: the queue's recompiles at full speed, or gentle (job_gentle) */
static const char* const background_words[] = {"full", "gentle"};
static const char* const background_names[] = {SNX_TR_NOOP("Full"), SNX_TR_NOOP("Gentle")};
/* the UI scales older versions had besides 1 to 4, as their app.ini named them, Auto and its parts: read as the
   number a start takes on that screen, said once (docs/DEPRECATIONS.md) */
static const char* const scale_words_gone[] = {"auto", "0.25", "0.5", "0.75"};
/* Games' options (GUI_OPTION_*): each one's key in app.ini, its row in the View box, and its two words or three in
   app.ini and as the row shows them, the first its default (games_defaults says where it is not) */
static const struct {
    const char *key, *name, *words[3], *shown[3];
} option_keys[GUI_OPTIONS] = {
    {"recent_games", SNX_TR_NOOP("Recent Games"), {"on", "off"}, {SNX_TR_NOOP("On"), SNX_TR_NOOP("Off")}},
    {"games_dumps", SNX_TR_NOOP("Dumps"), {"on", "off"}, {SNX_TR_NOOP("On"), SNX_TR_NOOP("Off")}},
    {"games_sort", SNX_TR_NOOP("Sort"), {"title", "last_opened"}, {SNX_TR_NOOP("Title"), SNX_TR_NOOP("Last opened")}},
    {"games_view", SNX_TR_NOOP("View"), {"list", "grid"}, {SNX_TR_NOOP("List"), SNX_TR_NOOP("Grid")}},
    {"games_icons",
     SNX_TR_NOOP("Icon size"),
     {"small", "medium", "large"},
     {SNX_TR_NOOP("Small"), SNX_TR_NOOP("Medium"), SNX_TR_NOOP("Large")}}};
/* option i's words: two or three */
static int option_words(int i) { return option_keys[i].words[2] ? 3 : 2; }
/* Icon size's tiles in pixels at 1x, a cell 8 by 16: Small 8 cells by 3 rows and 8px, Medium ui.txt's GamesGrid
   (0: 12 by 4 and 8px), Large 16 by 6 and 8px */
static const struct {
    int w, h;
} icon_tiles[3] = {{8 * 8, 3 * 16 + 8}, {0, 0}, {16 * 8, 6 * 16 + 8}};
/* the option a key of app.ini's [app] sets, -1 for none; the index of value among its words, -1 for none */
static int option_of(const char* key) {
    int i;
    for (i = 0; i < GUI_OPTIONS && strcmp(key, option_keys[i].key); ++i) {}
    return i < GUI_OPTIONS ? i : -1;
}
static int option_word(int option, const char* value) {
    int w;
    for (w = 0; w < option_words(option) && strcmp(value, option_keys[option].words[w]); ++w) {}
    return w < option_words(option) ? w : -1;
}
/* the options as Games reads them: the recent games over the list, the dumps in it, sorted by the last opened, a
   grid of icons */
static bool recent_on(const App* app) { return app->options[GUI_OPTION_RECENT] == 0; }
static bool dumps_on(const App* app) { return app->options[GUI_OPTION_DUMPS] == 0; }
static bool by_last_opened(const App* app) { return app->options[GUI_OPTION_SORT] == 1; }
static bool games_grid(const App* app) { return app->options[GUI_OPTION_VIEW] == 1; }
/* the options before app.ini names them: each its first word, Icon size Medium, but on iOS (ios) Recent Games and
   Dumps off, as a phone keeps the games made in the app's recompiled/, which Games lists whole (a second list of them
   only repeats it), and prepares a dump from the Menu */
static void games_defaults(App* app, bool ios) {
    memset(app->options, 0, sizeof app->options);
    app->options[GUI_OPTION_ICONS] = 1; /* Medium */
    if (ios) app->options[GUI_OPTION_RECENT] = app->options[GUI_OPTION_DUMPS] = 1;
}
/* Settings > App's picks and Games' options written to app_file: 0, else nonzero, said by the caller */
static int app_write(App* app) {
    char *text, *dir, *staged;
    size_t size = 256 + 32 * GUI_OPTIONS;
    int n, failed, i;
    if (!app->app_file || app->look_only) return 0;
    for (i = 0; i < GUI_PLACES; ++i) size += 16 + (app->place[i] ? strlen(app->place[i]) : 0);
    text = (char*)snx_alloc(size);
    n = snprintf(text, size, "; Settings > App and Games' View in the launcher, as they were left\n[app]\ntheme = %s\n",
                 app->theme.id);
    if (app->menu.text_size) n += snprintf(text + n, size - (size_t)n, "ui_scale = %d\n", app->menu.text_size);
    n += snprintf(text + n, size - (size_t)n, "language = %s\n",
                  app->language ? snx_gui_languages[app->language - 1].code : "auto");
    n += snprintf(text + n, size - (size_t)n, "animations = %d\n", !app->animations_off);
    n += snprintf(text + n, size - (size_t)n, "side_box = %s\n", side_words[app->side_box]);
    n += snprintf(text + n, size - (size_t)n, "recompile_background = %s\n", background_words[app->background_gentle]);
    n += snprintf(text + n, size - (size_t)n, "dumps_subfolders = %s\n", app->dumps_shallow ? "off" : "on");
    for (i = 0; i < GUI_OPTIONS; ++i)
        n += snprintf(text + n, size - (size_t)n, "%s = %s\n", option_keys[i].key,
                      option_keys[i].words[app->options[i]]);
    /* Folders' section once one is picked; an empty key is the default */
    for (i = 0; i < GUI_PLACES && !app->place[i]; ++i) {}
    if (i < GUI_PLACES) {
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
    free(dir);
    free(staged);
    free(text);
    return failed;
}
/* Settings > App's picks, kept at once in app_file for the next start. The App's note, or the menu's message, says when
   they could not be; 0, else nonzero */
static int app_save(App* app) {
    int failed = app_write(app);
    if (failed && app->editing) {
        snprintf(app->editor.note, sizeof app->editor.note,
                 snx_tr("Not kept for the next start: %s could not be written"), app->app_file);
        say_failure(app, app->editor.note);
    } else if (failed) {
        snprintf(app->message, sizeof app->message, snx_tr("Not kept for the next start: %s could not be written"),
                 app->app_file);
        say_failure(app, app->message);
    }
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
   theme no longer there, leaves what the menu has, named in bad as key = value (an option's with the word it keeps,
   the file's last it knows or the menu's); how many. A language named in another form of its tag (pt_BR, which builds
   before the tags wrote) is that language, the form read kept in renamed */
static int app_read(App* app, bool apply, char* bad, size_t size, char* renamed, size_t renamed_size) {
    size_t bytes = 0, i;
    char *text = app->app_file ? (char*)snx_read_file(app->app_file, &bytes) : NULL, *key, *value, *copy;
    char* places[GUI_PLACES] = {NULL};
    const char* const* dirs = (const char* const*)app->themes_dirs;
    int scale = app->menu.text_size, language = app->language, n = 0, count = 0, moved = 0;
    int options[GUI_OPTIONS];
    bool still = app->animations_off, gentle = app->background_gentle, kept, scale_read = false;
    bool shallow = app->dumps_shallow;
    int side = app->side_box;
    SnxIni ini;
    if (size) bad[0] = 0;
    if (renamed_size) renamed[0] = 0;
    if (!text) return 0;
    /* the options' words first, on a copy (a read cuts the text): a word it does not know is then said with the one
       the file leaves in use */
    memcpy(options, app->options, sizeof options);
    copy = snx_strdup(text);
    snx_ini_start(&ini, copy);
    while (snx_ini_next(&ini, &key, &value)) {
        int option = strcmp(ini.section, "app") ? -1 : option_of(key);
        if (option >= 0 && option_word(option, value) >= 0) options[option] = option_word(option, value);
        if (!strcmp(ini.section, "app") && !strcmp(key, "recompile_background") &&
            (!strcmp(value, "full") || !strcmp(value, "gentle")))
            gentle = !strcmp(value, "gentle");
    }
    free(copy);
    kept = gentle; /* what a word it does not know leaves in use */
    snx_ini_start(&ini, text);
    while (snx_ini_next(&ini, &key, &value)) {
        char* end = NULL;
        long number = strtol(value, &end, 10);
        int place = GUI_PLACES, option = strcmp(ini.section, "app") ? -1 : option_of(key);
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
            for (q = 0; q < 4 && strcmp(value, scale_words_gone[q]); ++q) {}
            if (q < 4)
                scale = SDL_max(1, app->auto_scale);
            else if (*value && !*end && number >= 1 && number <= GUI_TEXT_MAX)
                scale = (int)number;
            else
                taken = 0;
            scale_read = scale_read || taken;
            if (taken && apply && q < 4) { /* Auto or a part of it, steps no longer: the number a start takes here */
                free(app->scale_gone);
                app->scale_gone = snx_format("%s = %s", key, value);
            } else if (taken && apply && !strcmp(key, "text_size")) { /* ui_scale = as an older version named it */
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
        } else if (!strcmp(ini.section, "app") && !strcmp(key, "recompile_background")) {
            if (!strcmp(value, "full") || !strcmp(value, "gentle")) continue; /* taken in the first read */
        } else if (!strcmp(ini.section, "app") && !strcmp(key, "animations")) {
            if (!strcmp(value, "0") || !strcmp(value, "1")) {
                still = !strcmp(value, "0");
                continue;
            }
        } else if (!strcmp(ini.section, "app") && !strcmp(key, "dumps_subfolders")) {
            if (!strcmp(value, "on") || !strcmp(value, "off")) {
                shallow = !strcmp(value, "off");
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
        } else if (option >= 0 && option_word(option, value) >= 0)
            continue; /* taken in the first read */
        ++count;
        if (n) n += snprintf(bad + n, size - (size_t)n, ", ");
        if (n < (int)size && strcmp(ini.section, "app")) /* a key outside [app] says where it is */
            n += snprintf(bad + n, size - (size_t)n, "[%s] ", ini.section);
        if (n < (int)size) n += snprintf(bad + n, size - (size_t)n, "%s = %s", key, value);
        if (n < (int)size && option >= 0)
            n += snprintf(bad + n, size - (size_t)n, snx_tr(" (it keeps %s)"),
                          option_keys[option].words[options[option]]);
        else if (n < (int)size && !strcmp(ini.section, "app") && !strcmp(key, "recompile_background"))
            n += snprintf(bad + n, size - (size_t)n, snx_tr(" (it keeps %s)"), background_words[kept]);
        else if (n < (int)size && !strcmp(ini.section, "app") && !strcmp(key, "dumps_subfolders"))
            n += snprintf(bad + n, size - (size_t)n, snx_tr(" (it keeps %s)"), shallow ? "off" : "on");
        if (n >= (int)size) n = (int)size - 1;
    }
    free(text);
    if (apply) app->scale_new = !scale_read;
    if (apply) memcpy(app->options, options, sizeof options);
    if (apply && scale != app->menu.text_size) {
        app->menu.text_size = scale;
        layout(app);
    }
    if (apply && language != app->language) {
        app->language = language;
        language_apply(app);
    }
    if (apply) app->animations_off = still;
    if (apply) app->dumps_shallow = shallow;
    if (apply) app->background_gentle = gentle;
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
    /* [folders] games = and [app] text_size =, the names an older version wrote, and Auto or a part of it as UI scale:
       written again as recompiled =, ui_scale = and the number, each said once */
    if (app->games_key || app->text_size_key || app->scale_gone) {
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
        if (!failed && app->scale_gone) {
            size_t m = strlen(said);
            snprintf(said + m, sizeof said - m, "%s", m ? " " : "");
            m = strlen(said);
            snprintf(said + m, sizeof said - m, snx_tr("%s: %s is no longer a UI scale: ui_scale = %d now"),
                     app->app_file, app->scale_gone, app->menu.text_size);
        }
        if (!failed) snprintf(app->message, sizeof app->message, "%s", said);
        free(app->games_key);
        app->games_key = NULL;
        free(app->text_size_key);
        app->text_size_key = NULL;
        free(app->scale_gone);
        app->scale_gone = NULL;
    }
}
/* a list that could not be kept: the message says so, after what it says already, until it is */
static void recent_note(App* app) {
    char note[sizeof app->message], *at;
    size_t n = strlen(app->message);
    if (!app->recent_file) return;
    snprintf(note, sizeof note, snx_tr("Recent Games not kept: %s could not be written."), app->recent_file);
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
/* a recent game Games shows: a game made, or a Linux program, which runs as it is. A dump not made yet stays in the
   list, so that its game takes its place once made (recent_as_games), but is not shown: opening it would recompile it */
static bool recent_game(const GuiRecentGame* r) { return r->prepared || r->driver == GUI_LINUX; }
static int recent_games(const App* app) {
    int n = 0;
    for (int i = 0; i < app->recent.count; ++i) n += recent_game(&app->recent.game[i]);
    return n;
}
/* the list's index of the k-th recent game shown (k under recent_games) */
static int recent_index(const App* app, int k) {
    for (int i = 0; i < app->recent.count; ++i)
        if (recent_game(&app->recent.game[i]) && !k--) return i;
    return 0;
}
/* the recent games shown, none with Recent Games off, and whether a row under them shows the rest */
static int recent_shown(const App* app) {
    if (!recent_on(app)) return 0;
    return app->recent_all ? recent_games(app) : SDL_min(recent_games(app), GUI_RECENT_SHOWN);
}
static int recent_more(const App* app) { return recent_on(app) && recent_shown(app) < recent_games(app); }
/* the rows over Games' games: the recent games and the row that shows the rest (P, the foot's, adds a folder) */
static size_t games_head(const App* app) { return (size_t)(recent_shown(app) + recent_more(app)); }
/* the list as the menu and the file have it */
static int game_debug(App* app, const char* path, int picks, char* out, size_t size);
static void recent_debug_read(App* app);
static void recent_changed(App* app) {
    recent_debug_read(app); /* its rows moved */
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
/* still the menu, fading out, while a run starts */
static int entering(const App* app) {
    return app->entering_ms && now_ms(app) < app->entering_ms + app->theme.fade_out_ms;
}
/* the rows a UI scale keeps at the least on any screen: one that leaves fewer shows the largest under it that does */
#define GUI_LEAST_ROWS 16
/* the pixels high rows rows of letters take at scale */
static int rows_height(int rows, int scale) { return rows * 16 * scale; }
/* the largest scale from scale down, 1x at the least, whose rows rows fit a window height pixels high */
static int scale_fits(int scale, int height, int rows) {
    while (scale > 1 && height < rows_height(rows, scale)) --scale;
    return scale;
}
/* the rows a start's UI scale keeps at the least: a phone held wide is short, and its menu needs room for its items,
   a finger's row each, and the setup's boxes for their lines: 2x on the phones of 1080 and 1156 rows of pixels */
#define GUI_AUTO_ROWS_PHONE 30
#ifdef SNX_TOUCH_UI
#define GUI_AUTO_ROWS GUI_AUTO_ROWS_PHONE
#else
#define GUI_AUTO_ROWS 16
#endif
/* the UI scale a start takes with none in app.ini: twice the display's own (Android's dpi / 160), so a computer at
   100% gets 2x and a phone's letters come out as big to the eye, as long as rows rows fit in a window height pixels
   high */
static int pick_auto_scale(float display_scale, int height, int rows) {
    return scale_fits(SDL_max(1, (int)(2.0f * display_scale)), height, rows);
}
/* what a screen has besides its title and foot: the status box (unless the footer's style is none), the tabs (a run
   has none), a list with the side box beside it (one box without), a wide list (a run's output, the theme editor's
   keys) laid out as ui.txt's Wide, not its Screen, and the tips in the foot beside the keys, as the setup's are */
enum { GUI_SHOWS_STATUS = 1, GUI_SHOWS_TABS = 2, GUI_SHOWS_WIDE = 4, GUI_SHOWS_SIDE = 8, GUI_SHOWS_FOOT_TIPS = 16 };
static void place_screen(App* app, GuiPlace* p, const char* keys, int shows);
static const char* shown_keys(const App* app, char* out, size_t size);
static void aot_status_update(App* app);
static void debug_focus_update(App* app);
/* a screen's keys, Tab's after them; the key back of a screen whose own keys do not say it; F1's, last in a foot */
#define SAY_TAB SNX_TR_KEYS("Tab: screen. F2: side box.")
#ifdef SNX_TOUCH_UI
#define SAY_GAMES_FOLDERS SNX_TR_KEYS("Folders.")
#define SAY_GAMES_VIEW    SNX_TR_KEYS("View.")
#else
#define SAY_GAMES_FOLDERS SNX_TR_KEYS("P: folders.")
#define SAY_GAMES_VIEW    SNX_TR_KEYS("V: view.")
#endif
#define SAY_BACK     SNX_TR_KEYS("ESC: back.")
#define SAY_HELP     SNX_TR_KEYS("F1: help.")
#define SAY_PAD_HELP SNX_TR_KEYS("Select: help.")
/* Games' foot says the AOT pack's key: there is a cache to build it from, or the pack built is bad */
/* B (R3) said where it builds a pack: a JIT cache read whole, at the build's level */
static bool aot_said(const App* app) {
    return app->menu.view == GUI_LOAD_GAMES && app->aot_state == GUI_AOT_CACHE && app->aot_match;
}
/* the grid is laid out as a wide list, three fifths of the screen, for its tiles across */
static void games_place(App* app, GuiPlace* p) {
    char keys[256];
    place_screen(app, p, shown_keys(app, keys, sizeof keys),
                 GUI_SHOWS_STATUS | GUI_SHOWS_TABS | GUI_SHOWS_SIDE | (games_grid(app) ? GUI_SHOWS_WIDE : 0));
}
/* the list's entries before the page's games: "Recent Games" and the recent rows, none without recent games */
static int games_lead(const App* app) { return app->browser.head ? (int)app->browser.head + 1 : 0; }
/* one page of all the games, the list's and the grid's, which they scroll: the list by pixels (games_y), the grid by
   its lines */
static size_t games_page(App* app) { return app->games.count ? app->games.count : 1; }
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
    place_screen(app, p, snx_tr(SAY_ABORT), GUI_SHOWS_STATUS | GUI_SHOWS_WIDE | GUI_SHOWS_SIDE);
}
static void job_console_size(App* app, int* cols, int* rows);
static void size_shown(const App* app, char* out, size_t n);
static void layout(App* app) {
    int run;
    int width, height, whole_width, whole_height;
    set_area(app);
    area(app, &width, &height);
    /* a start's letters are the opening window's, measured whole: a phone's system bars may still show over it then,
       and the safe area be short. The window then keeps room for 16 rows of them */
    if (!app->auto_scale && SDL_GetWindowSizeInPixels(app->window, &whole_width, &whole_height) && whole_height > 0) {
        app->auto_scale = pick_auto_scale(SDL_GetWindowDisplayScale(app->window), whole_height, GUI_AUTO_ROWS);
        SDL_SetWindowMinimumSize(app->window, 800,
                                 SDL_max(480, (int)SDL_ceilf((float)rows_height(GUI_LEAST_ROWS, app->auto_scale) /
                                                             SDL_GetWindowPixelDensity(app->window))));
    }
    if (!app->menu.text_size) app->menu.text_size = app->auto_scale; /* a start with none in app.ini: a number then */
    /* as long as GUI_LEAST_ROWS still fit: a start's always do on a computer, a phone's not while Fullscreen is off */
    app->scale = scale_fits(SDL_max(app->menu.text_size, 1), height, GUI_LEAST_ROWS);
    size_shown(app, app->app_form.text, sizeof app->app_form.text); /* App's row, with the one in use if lower */
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
    if (gui_queue_running(&app->queue) >= 0 && app->theme.run != GUI_RUN_STEPS) { /* the Compile tab's */
        int cols, rows;
        job_console_size(app, &cols, &rows);
        gui_console_resize(&app->job_console, cols, rows);
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
/* what a run's screen draws: the run the menu started and shows (run_of), or a recompile of the queue (job_view) */
typedef struct {
    const GuiSteps* steps;
    const GuiConsole* console;
    const char* file; /* its input, "" for none */
    const char* said; /* how it ended, once it did */
    bool native;      /* a game made already, which its recompile's line names */
    bool finished, aborting, failed;
    Uint64 since; /* a line that came while the menu faded out comes in with the run, from here */
} GuiRunView;
static GuiRunView run_of(const App* app) {
    GuiRunView v = {&app->steps,   &app->console, app->file ? app->file : "", app->message, app->native != 0,
                    app->finished, app->aborting, app->abnormal_exit,         app->fade_ms};
    return v;
}
/* ---- R's and B's recompiles, which play nothing: a queue run one at a time behind the screens (queue.h), the
   Compile tab showing it ---- */
static bool jobs_live(const App* app) {
    return gui_queue_running(&app->queue) >= 0 || gui_queue_next(&app->queue) >= 0;
}
/* job i as its panel draws it: its output while it runs, or the last one's to end once kept (ended_*); none else
   (steps NULL): one waiting, or one whose output a later one's replaced */
static GuiRunView job_view(const App* app, int i) {
    const GuiJob* job = &app->queue.job[i];
    bool running = job->state == GUI_JOB_RUNNING, kept = !running && app->ended_serial == job->serial;
    GuiRunView v = {running ? &app->job_steps
                    : kept  ? &app->ended_steps
                            : NULL,
                    running ? &app->job_console
                    : kept  ? &app->ended_console
                            : NULL,
                    gui_queue_file(&app->queue, i),
                    job->said,
                    job->native,
                    job->state >= GUI_JOB_OK,
                    running && app->job_aborting,
                    job->state == GUI_JOB_FAILED,
                    job->started_ms};
    return v;
}
/* where job i is, as its row on the Compile tab says it (games: as Games' row, in the queue's words): how far the
   one running is, the place of one waiting, how one ended */
static void job_where(const App* app, int i, bool games, char* out, size_t size) {
    const GuiJob* job = &app->queue.job[i];
    char place[32];
    if (job->state == GUI_JOB_RUNNING && job->percent < 0)
        snprintf(out, size, "%s", "--%");
    else if (job->state == GUI_JOB_RUNNING)
        snprintf(out, size, "%d%%", job->percent);
    else if (job->state == GUI_JOB_WAITING) {
        int at = gui_queue_place(&app->queue, i);
        gui_queue_place_text(at, place, sizeof place);
        if (games && at > 2)
            snprintf(out, size, snx_tr("%s in queue"), place);
        else
            snprintf(out, size, "%s", place);
    } else
        snprintf(out, size, "%s", job->state == GUI_JOB_OK ? snx_tr("OK") : snx_tr("FAILED"));
}
/* the row of a game of Games or the recent ones at path: two columns after its name, where it is in the queue while it
   waits or runs; nothing else */
static void job_suffix(const App* app, const char* path, char* out, size_t size) {
    int i = path ? gui_queue_find(&app->queue, path) : -1;
    out[0] = 0;
    if (i < 0) return;
    memcpy(out, "  ", 3);
    job_where(app, i, true, out + 2, size - 2);
}
/* the ring that turns while a run goes: a thin outer ring, a thicker inner one and a bar going round it */
static void ring(App* app, float cx, float cy, bool still) {
    float s = letters(app), r = 7.0f * s, inner = r * 0.62f, thick = SDL_max(1.0f, 1.5f * s);
    float angle = still ? 0 : (float)((now_ms(app) / 4) % 360);
    arc(app, cx, cy, r, 0, 360, SDL_max(1.0f, 0.5f * s), app->theme.run_colors.dim);
    arc(app, cx, cy, inner, 0, 360, thick, app->theme.run_colors.soft);
    arc(app, cx, cy, inner, angle, 70, thick, app->theme.run_colors.ink);
}
/* "", ".", "..", "..." round and round; "..." once the run is over */
static const char* dots(const App* app, bool finished) {
    static const char* const turn[] = {"", ".", "..", "..."};
    return finished ? "..." : turn[(now_ms(app) / 400) % 4];
}
static void launch(App* app, char* absolute, bool prepared);
static void render_dests(App* app);
static void render_picks(App* app);
/* the game's name as the run knows it: the runner's banner, else the file's own, in capitals */
static void run_name_of(const GuiSteps* steps, bool native, const char* file, char* out, size_t size) {
    const char* q;
    size_t n;
    const char* dot;
    const GuiStep *launch = gui_steps_last(steps, "LAUNCH"), *made = gui_steps_last(steps, "RECOMPILE");
    /* a game made already is named by its title ID: its recompile's line says what it is shown as */
    const char* name = launch ? launch->detail : native && made ? made->detail : file ? file : "";
    for (q = name; *q; ++q)
        if (*q == '/' || *q == '\\') name = q + 1;
    n = 0;
    dot = launch || (native && made) ? NULL : strrchr(name, '.');
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
static int hint_pick(const char* key, int col, bool pad);
/* a run as a loading screen: the title, the name and the ring; the steps typed down the page, their status last;
   the progress bar and its percentage; the footer and the ESC line. As a panel (a recompile of the queue on its tab,
   in its letters and the cells of the viewport it is drawn in): a band of a row at its foot in their place, its
   usage while it goes (band, "" for none), the line to go on once it ended */
static void steps_draw(App* app, const GuiRunView* v, bool panel, const char* band, const char* ended) {
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
    Uint64 now = now_ms(app), since = v->since;
    const GuiSteps* steps = v->steps;
    float cw = (float)cell_w(app), ch = (float)cell_h(app), s = letters(app);
    int width, height, name_col, col, y, i, r, rows = 0, last_step = -1, typing, top = 3, bottom, fit, first, cursor;
    int foot = !panel && theme->footer != GUI_STYLE_NONE ? 2 : 0; /* the footer's rows, over the ESC line */
    const char* title;
    char name[96], head[128];
    SDL_Color ink, soft, failure;
    area(app, &width, &height);
    if (theme->texture)
        for (y = 3; y < height; y += 4) fill(app, 0, (float)y, (float)width, 1, white, 14);
    ink = rgb(colors->ink);
    soft = rgb(colors->soft);
    failure = rgb(colors->failure);

    run_name_of(steps, v->native, v->file, name, sizeof name);
    col = theme->ring == GUI_RING_LEFT ? 4 : 1;
    title = theme->title[panel ? GUI_SCREEN_COMPILING : screen(app)];
    name_col = col + gui_text_cols(title) + (title[0] ? 1 : 0);
    if (theme->ring == GUI_RING_LEFT) ring(app, cw * 2, ch * 1.5f, v->finished);
    if (theme->ring == GUI_RING_RIGHT) ring(app, (float)width - cw * 2, ch * 1.5f, v->finished);
    if (theme->ring == GUI_RING_NEAR_TEXT) /* after the name and the dots' whole width, so it stays put */
        ring(app, cw * (float)(name_col + gui_text_cols(name) + 4) + 7.0f * s, ch * 1.5f, v->finished);
    text(app, col, 1, title, ink);
    snprintf(head, sizeof head, "%s%s", name, dots(app, v->finished));
    text(app, name_col, 1, head, soft);
    if (v->finished) text(app, 1, 2, v->said, v->failed ? failure : soft);
    for (i = 0; i < steps->count; ++i) {
        int n;
        const GuiNote* notes[GUI_NOTES_MAX];
        int k;
        const GuiStep* step = &steps->step[i];
        owner[rows] = step;
        is_note[rows] = 0;
        shown[rows] = SDL_max(step->shown_ms, since);
        step_runs(step, theme->progress, now, runs[rows], lengths[rows]);
        clip_row(runs[rows], lengths[rows], 0, app->cols - 6);
        last_step = rows++;
        if (!theme->detailed || (theme->collapse && step->state == GUI_STEP_DONE)) continue;

        n = gui_step_notes(steps, step, notes, GUI_NOTES_MAX);
        for (k = 0; k < n; ++k, ++rows) {
            owner[rows] = step;
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
        const GuiStep* step = owner[r];
        int last = step == &steps->step[steps->count - 1];
        int failed = step->state == GUI_STEP_FAILED, active = step->state == GUI_STEP_ACTIVE;
        size_t total = lengths[r][0] + lengths[r][1] + lengths[r][2];
        Uint64 age = now > shown[r] ? now - shown[r] : 0;
        int moving = theme->line_in == GUI_LINE_SLIDE || theme->line_in == GUI_LINE_FADE;
        float p = moving && theme->line_in_ms ? SDL_min(1.0f, (float)age / (float)theme->line_in_ms) : 1.0f;
        float x = cw * (is_note[r] ? 3 : 1) - (theme->line_in == GUI_LINE_SLIDE ? 2 * cw * (1 - p) * (1 - p) : 0);
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
        if (!is_note[r] && typed[r] == total && active) x = text_px(app, x, y, dots(app, v->finished), 3, ink, p);
        blink = !theme->blink_ms || (now / theme->blink_ms) % 2 == 0;
        working = typing == r || (typing < 0 && active);
        if (r == cursor && working && blink) text_px(app, x, y, "_", 1, ink, p);
    }
    if (theme->bar) {
        int k, percent = gui_steps_percent(steps);
        int filled = percent < 0 ? 0 : (percent * SEGMENTS + 50) / 100;
        float span = (float)(app->cols - 8) * cw, gap = 2.0f * s;
        float seg = (span - gap * (SEGMENTS - 1)) / SEGMENTS, sy = ch * (float)(app->rows - foot - 3) + ch * 0.3f;
        char number[16];
        fill(app, cw, ch * (float)(app->rows - foot - 4) + ch / 2, (float)(app->cols - 2) * cw, s, colors->dim, 255);
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
    if (panel && !v->finished && !v->aborting && band[0]) { /* its usage on the footer's band, or over its rule */
        bool bar = theme->footer == GUI_STYLE_BAR;
        if (bar) fill(app, 0, ch * (float)(app->rows - 1), (float)width, ch, colors->ink, 255);
        if (theme->footer_rule)
            fill(app, cw, ch * (float)(app->rows - 1) - s, (float)(app->cols - 2) * cw, s, colors->soft, 255);
        text(app, 1, app->rows - 1, band, rgb(bar ? colors->page : colors->soft));
    } else if (v->finished)
        prompt(app, v->failed ? GUI_PROMPT_FAILED : GUI_PROMPT_DONE, 1, app->rows - 1, panel ? ended : snx_tr(SAY_DONE),
               soft);
    else if (v->aborting)
        prompt(app, GUI_PROMPT_ABORTING, 1, app->rows - 1, snx_tr("Aborting..."), rgb(colors->warning));
    else if (!panel)
        text(app, 1, app->rows - 1, snx_tr(SAY_ABORT), soft);
}
static void render_steps(App* app) {
    GuiRunView v = run_of(app);
    steps_draw(app, &v, false, "", "");
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
static void box_shade(App* app) {
    app->nbars = 0; /* the screen's under it, which no press reaches now */
    shade(app, (Uint8)(128 * faded(app, app->box_ms)));
}
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
   never 0, which screen_was holds before the first frame. The Settings' groups with the side panel's rows beside them
   are one screen, whichever has the focus and whichever group's rows show */
static unsigned screen_mark(const App* app) {
    int form = app->form, group = app->group, page = app->page;
    if (app->editing && app->form_beside &&
        (form == GUI_FORM_SETTINGS || form == GUI_FORM_GROUP || form == GUI_FORM_APP || form == GUI_FORM_USERS)) {
        form = GUI_FORM_SETTINGS;
        group = 0;
        page = -1;
    }
    return (unsigned)app->editing | (unsigned)form << 1 | (unsigned)app->menu.view << 5 | (unsigned)app->setup_on << 9 |
           (unsigned)app->setup_page << 10 | (unsigned)group << 16 | (unsigned)(page + 1) << 22 | 1u << 31;
}
/* what changed since the last frame: a screen come, whose fade starts now, a box opened or closed, whose shade does */
static void fades_mark(App* app) {
    unsigned mark = screen_mark(app);
    bool boxed = app->confirm.shown || app->sizes_shown || (app->editing && app->editor.typing) || app->users_shown ||
                 app->addons_shown || app->options_shown || app->states_shown || app->cleaned_shown ||
                 app->left_shown || app->dests_shown || app->picks_shown || app->help_shown;
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
static bool jobs_live(const App* app);
/* leaving the menu while the queue's recompile runs, its line in ask_line (no other question of a menu that is
   leaving uses it): which runs and how far, how many wait */
static const char* job_leaving(App* app) {
    int run = gui_queue_running(&app->queue), waiting = gui_queue_waiting(&app->queue);
    if (run < 0) run = gui_queue_next(&app->queue);
    if (run < 0) return snx_tr("Exit ScarletNX?"); /* it ended while asked */
    if (waiting > (app->queue.job[run].state == GUI_JOB_WAITING))
        snprintf(app->ask_line, sizeof app->ask_line,
                 snx_tr("%s is compiling (%d%%), %d more in the queue: exiting stops them. Exit?"),
                 gui_queue_name(&app->queue, run), SDL_max(app->queue.job[run].percent, 0),
                 waiting - (app->queue.job[run].state == GUI_JOB_WAITING));
    else
        snprintf(app->ask_line, sizeof app->ask_line, snx_tr("%s is compiling (%d%%): exiting stops it. Exit?"),
                 gui_queue_name(&app->queue, run), SDL_max(app->queue.job[run].percent, 0));
    return app->ask_line;
}
static const char* ask_question(App* app) {
    switch (app->confirm.ask) {
        case GUI_ASK_DEEP: return snx_tr("List the games in its subfolders too?");
        case GUI_ASK_EXIT: return jobs_live(app) ? job_leaving(app) : snx_tr("Exit ScarletNX?");
        case GUI_ASK_OUTSIDE: return snx_tr("Outside the app, Android checks every file access: games may run slower.");
        case GUI_ASK_ROOM: return app->room_line;
        case GUI_ASK_CLEAN:
        case GUI_ASK_RESET:
        case GUI_ASK_PLACES:
        case GUI_ASK_PLACE:
        case GUI_ASK_FOLDER:
        case GUI_ASK_MOVE:
        case GUI_ASK_MADE:
        case GUI_ASK_STATE:
        case GUI_ASK_CACHE: return app->ask_line;
        case GUI_ASK_ERROR: return app->error_line;
        case GUI_ASK_CLOSE:
            if (!app->menu.running && !app->finished) return job_leaving(app);
            return screen(app) == GUI_SCREEN_COMPILING
                       ? snx_tr("A compile goes: exiting ScarletNX stops it, its work lost.")
                       : snx_tr("A game runs: exiting ScarletNX ends it.");
        default: /* the run's, or on the Compile tab the queue's one running */
            return !app->menu.running || screen(app) == GUI_SCREEN_COMPILING
                       ? snx_tr("Abort the compile? Its work is lost.")
                       : snx_tr("Abort the run?");
    }
}
/* app->left's lines, Share Log's: English whatever the menu shows */
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
#ifdef SDL_PLATFORM_IOS
    char tidy[1100];
    const char* shorter = tidy_paths(s, tidy, sizeof tidy);
    if (shorter) s = shorter;
#endif
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
   what Clean Old Settings did (1), each file and what was done to it or why not. Up/Down go over them, another key
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
static int rows_h(void (*layout)(GuiRect, int, GuiRect*), GuiRect (*row)(const GuiRect*, int, int, size_t),
                  GuiRect* rects, int item, int scale, int count);
static void render_files(App* app, int report) {
    char a[4400], b[4400], label[96];
    const char *first = report ? app->cleaned.text : app->left, *at = first;
    int width, height, cols, room, used = 0, shown, i, y, path, s = app->scale, ok_h, hint;
    int count = report ? app->cleaned.count : app->lefts, done = app->cleaned.done, top;
    SDL_Color ca, cb;
    GuiRect r, text;
    GuiRect ok[UI_FILES_ITEMS];
    UiList oks[UI_FILES_ITEMS] = {{0}}; /* OK in focus */
    area(app, &width, &height);
    cols = width / cell_w(app) - 6;
    hint = !report && app->left_clear ? wrap_rows(app, snx_tr(SAY_LEFT_CLEAR), cols, 0) : 0; /* under them, before OK */
    /* the first shown, kept among them when there are fewer since */
    top = app->files_top = SDL_clamp(app->files_top, 0, SDL_max(count - 1, 0));
    for (i = 0; at && i < top; ++i) at = files_entry(app, report, i, at, a, b, sizeof a, &ca, &cb, &path);
    first = at;
    /* the files' rows, and one for how many more */
    ok_h = rows_h(ui_files_layout, ui_files_row, ok, UI_FILES_OK, s, 1); /* ui.txt's scene Files: OK's row */
    room = (height - head_h(app) - ok_h - 24 * s) / cell_h(app) - 2 - hint;
    for (shown = 0; at && top + shown < count; ++shown) {
        int need;
        at = files_entry(app, report, top + shown, at, a, b, sizeof a, &ca, &cb, &path);
        need = wrap_rows(app, a, cols - 2, 1) + wrap_rows(app, b, cols - 2, path);
        if (shown && used + need > room) break;
        used += need;
    }
    app->files_shown = shown;
    r = over_rect(app, cols, head_h(app) + (used + (top + shown < count) + hint) * cell_h(app) + ok_h + 16 * s);
    if (!report)
        snprintf(label, sizeof label, "%s", snx_tr(SAY_LEFT_TITLE));
    else if (app->report_move)
        snprintf(label, sizeof label, snx_tr("Move Old Files: %d done, %d not"), done, count - done);
    else if (count > done)
        snprintf(label, sizeof label, snx_tr("Clean Old Settings: %d done, %d not"), done, count - done);
    else
        snprintf(label, sizeof label, snx_tr("Clean Old Settings: %d done"), done);
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
        snprintf(label, sizeof label, snx_tr("and %d more, which Share Log lists"), count - top - shown);
        text_in(app, text, text.x, y, label, rgb(app->theme.soft));
        y += cell_h(app);
    }
    if (hint) wrap_in(app, text, text.x, y, snx_tr(SAY_LEFT_CLEAR), 0, rgb(app->theme.ink));
    ui_files_layout(gui_rect(text.x, r.y + r.h - ok_h - 6 * s, SDL_min(text.w, 26 * cell_w(app)), ok_h), s, ok);
    ui_files_draw(app, ok, s, oks);
}
/* ---- a question and its choices, as the theme's options-dialog places them: in a box in the middle of their area, the
   question over the choices; or the question in a box at the area's foot, a tab with its label on the box's top edge,
   and the choices at its right above it. The choices are ui.txt's scene Choices, a row each. The questions over a
   screen, UI scale's dialog, the folders a recompile's game may go in and the setup's steps ---- */
typedef struct {
    GuiRect box; /* the question's: the whole dialog in the middle, or the box at the foot */
    GuiRect tab; /* at the foot: the label on the box's top edge; no size without a label */
    GuiRect at;  /* the choices' rows, the scene's area */
    GuiRect rects[UI_CHOICES_ITEMS];
    UiList lists[UI_CHOICES_ITEMS]; /* the rows' at UI_CHOICES_ROWS: row i is choice i */
    int of, count;                  /* whose (GuiChoicesOf), and how many */
} GuiChoices;
/* choice i of whose choices (GuiChoicesOf) as its row reads, in out when it is made there; NULL past the last */
static const char* choice_text(App* app, int of, int i, char* out, size_t size);
/* the height count rows of a scene's list take, from the first one's top to the last one's foot, as the scene lays
   them out: its layout and its row functions, rects room for its items, item the list's */
static int rows_h(void (*layout)(GuiRect, int, GuiRect*), GuiRect (*row)(const GuiRect*, int, int, size_t),
                  GuiRect* rects, int item, int scale, int count) {
    GuiRect first, last;
    if (count <= 0) return 0;
    layout(gui_rect(0, 0, 1 << 14, 1 << 20), scale, rects);
    first = row(rects, scale, item, 0);
    last = row(rects, scale, item, (size_t)count - 1);
    return last.y + last.h - first.y;
}
/* list, a scene's item, from its first row as last shown (app's firsts[which]) to where follow (the scene's
   ui_*_follow) puts it for its focus: in its middle, or nowhere else for a focus the pointer put on a row that shows */
static void list_follow(App* app, int which, void (*follow)(const GuiRect*, int, int, size_t, UiList*),
                        GuiRect (*row)(const GuiRect*, int, int, size_t), const GuiRect* rects, int item, size_t count,
                        UiList* list) {
    int step = row(rects, app->scale, item, 1).y - row(rects, app->scale, item, 0).y;
    float y = SDL_clamp(app->list_y[which], 0.0f, app->list_max[which]);
    size_t at = step > 0 ? (size_t)(y / (float)step) : 0;
    app->list_step[which] = step;
    list->pointed = app->pointed;
    list->count = count;
    list->store = &app->firsts[which];
    if (app->bar_focus[which] == SIZE_MAX) app->bar_focus[which] = list->focus + 1;
    /* where the thumb or a finger put it, until the focus moves: to a row the pointer put it on that shows, still */
    list->free = app->bar_focus[which] && step > 0 &&
                 (app->bar_focus[which] == list->focus + 1 ||
                  (app->pointed && (float)list->focus * (float)step >= y &&
                   (float)(list->focus + 1) * (float)step <= y + (float)(app->list_shown[which] * (size_t)step)));
    app->bar_focus[which] = list->free ? list->focus + 1 : 0;
    list->first = list->free ? at : app->firsts[which];
    follow(rects, app->scale, item, count, list);
    if (list->free && list->first == at)
        list->shift = (int)y - (int)at * step; /* between two rows */
    else {
        list->shift = 0;
        app->list_y[which] = (float)list->first * (float)SDL_max(step, 0);
        app->list_v[which] = 0;
    }
    app->firsts[which] = list->first;
}
/* a frame of the lists a finger flung: on at their speed, slowing, until they stop or meet an end */
static void lists_glide(App* app) {
    Uint64 now = now_ms(app);
    float dt = app->lists_tick && now > app->lists_tick ? SDL_min((float)(now - app->lists_tick) / 1000.0f, 0.1f) : 0;
    int i;
    app->lists_tick = now;
    for (i = 0; i < GUI_LISTS; ++i) {
        if (app->list_v[i] == 0 || app->list_held == i + 1) continue;
        app->list_y[i] += app->list_v[i] * dt;
        app->list_v[i] *= SDL_expf(-3.0f * dt);
        if (SDL_fabsf(app->list_v[i]) < 30.0f || app->list_y[i] <= 0 || app->list_y[i] >= app->list_max[i])
            app->list_v[i] = 0;
        app->list_y[i] = SDL_clamp(app->list_y[i], 0.0f, app->list_max[i]);
    }
}
/* the choices' rows laid out in c->at, the one in focus (-1 for none) followed as the scene scrolls its list */
static void choices_rows(App* app, GuiChoices* c, int focus) {
    ui_choices_layout(c->at, app->scale, c->rects);
    c->lists[UI_CHOICES_ROWS].focus = (size_t)SDL_max(focus, 0);
    list_follow(app, c->of, ui_choices_follow, ui_choices_row, c->rects, UI_CHOICES_ROWS, (size_t)c->count,
                &c->lists[UI_CHOICES_ROWS]);
    if (focus < 0) c->lists[UI_CHOICES_ROWS].focus = SIZE_MAX;
}
/* the dialog of whose choices in area, the one in focus (-1 for none) among the rows shown. Rows that do not fit
   between the area's top and the question are scrolled, as the scene scrolls its list */
static void choices_place(App* app, int of, GuiRect area, const char* question, const char* tab, int focus,
                          GuiChoices* c) {
    int s = app->scale, cw = cell_w(app), longest = 16, rows, list_h;
    int marker = gui_text_cols(app->theme.marker), tab_w = tab && tab[0] ? gui_text_cols(tab) + 2 : 0;
    const char* label;
    memset(c, 0, sizeof *c);
    c->of = of;
    for (; (label = choice_text(app, of, c->count, app->row_text, sizeof app->row_text)); ++c->count)
        longest = SDL_max(longest, gui_text_cols(label) + marker + 2);
    list_h = rows_h(ui_choices_layout, ui_choices_row, c->rects, UI_CHOICES_ROWS, s, c->count);
    if (app->theme.dialog == GUI_DIALOG_YORHA) {
        int margin = app->theme.cursor[0] ? gui_text_cols(app->theme.cursor) * cw : 0;
        int w = SDL_min(SDL_max(longest, 24) * cw + margin, area.w), top;
        rows = wrap_rows(app, question, (area.w - 4 * cw) / cw, 0);
        c->box = gui_rect(area.x, area.y + area.h - (rows + 2) * cell_h(app), area.w, (rows + 2) * cell_h(app));
        if (tab_w) c->tab = gui_rect(c->box.x + 2 * cw, c->box.y - cell_h(app), tab_w * cw, cell_h(app) + 4 * s);
        top = (tab_w ? c->tab.y : c->box.y) - cell_h(app) / 2 - list_h;
        if (top < area.y) { /* the rows from the area's top */
            list_h = SDL_max(0, list_h - (area.y - top));
            top = area.y;
        }
        c->at = gui_rect(area.x + area.w - w - 2 * cw, top, w, list_h);
    } else { /* in the middle, as wide as the question or the longest choice */
        int cols = SDL_max(gui_text_cols(question) + tab_w, longest), w, h;
        w = SDL_min((cols + 2) * cw, area.w - 2 * cw);
        rows = wrap_rows(app, question, (w - 2 * cw - tab_w * cw) / cw, 0);
        list_h = SDL_min(list_h, SDL_max(0, area.h - rows * cell_h(app) - 12 * s)); /* the box kept in the area */
        h = rows * cell_h(app) + list_h + 12 * s;
        c->box = gui_rect(area.x + (area.w - w) / 2, area.y + SDL_max(0, (area.h - h) / 2), w, h);
        c->at = gui_rect(c->box.x + cw, c->box.y + rows * cell_h(app) + 10 * s, c->box.w - 2 * cw, list_h);
    }
    choices_rows(app, c, focus);
}
/* the dialog drawn where choices_place put it: the question and the tab, then the scene's rows */
static void choices_draw(App* app, const GuiChoices* c, const char* question, const char* tab) {
    const GuiTheme* t = &app->theme;
    int s = app->scale, filled = t->panels == GUI_PANELS_FILL;
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
    app->choices_of = c->of;
    ui_choices_draw(app, c->rects, s, c->lists);
}
/* the pointer at pixel x, y on the dialog placed, pressed (click) or over it: the row under it hit, its choice's
   row_action_cb called, true; *on whether it is on the dialog at all */
static bool choices_pointer(App* app, const GuiChoices* c, float x, float y, bool click, bool* on) {
    const GuiRect* r[3];
    int i, item;
    size_t row;
    uint32_t hit;
    r[0] = &c->box;
    r[1] = &c->tab;
    r[2] = &c->at;
    *on = false;
    for (i = 0; i < 3; ++i)
        *on = *on || (x >= (float)r[i]->x && x < (float)(r[i]->x + r[i]->w) && y >= (float)r[i]->y &&
                      y < (float)(r[i]->y + r[i]->h));
    app->choices_of = c->of;
    hit = ui_choices_hit(app, c->rects, app->scale, c->lists, (int)x, (int)y, click ? UI_HIT_CLICK : 0, &item, &row);
    if (hit) ui_choices_act(app, item, row, hit);
    return hit != 0;
}
/* the area a box over the screen has: the whole of it in the middle, or above its foot */
static GuiRect choices_over(const App* app) {
    int width, height;
    area(app, &width, &height);
    return app->theme.dialog == GUI_DIALOG_YORHA
               ? gui_rect(cell_w(app), cell_h(app) / 2, width - 2 * cell_w(app), height - cell_h(app))
               : gui_rect(0, 0, width, height);
}
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
#define SAY_AGAIN SNX_TR_NOOP("tap again")
#else
#define SAY_AGAIN SNX_TR_NOOP("press again")
#endif
/* the questions over a run, the menu or a form (GuiAsk's order): the first choice keeps things as they are */
static const char* const ask_choices[][2] = {{SNX_TR_NOOP("Continue"), SNX_TR_NOOP("Abort")},
                                             {SNX_TR_NOOP("Cancel"), SNX_TR_NOOP("Exit")},
                                             {SNX_TR_NOOP("Only This Folder"), SNX_TR_NOOP("With Subfolders")},
                                             {SNX_TR_NOOP("Cancel"), SNX_TR_NOOP("Use Anyway")},
                                             {SNX_TR_NOOP("Cancel"), SNX_TR_NOOP("Run Anyway")},
                                             {SNX_TR_NOOP("Cancel"), SNX_TR_NOOP("Clean")},
                                             {SNX_TR_NOOP("Cancel"), SNX_TR_NOOP("Reset")},
                                             {SNX_TR_NOOP("Cancel"), SNX_TR_NOOP("Reset")},
                                             {SNX_TR_NOOP("Cancel"), SNX_TR_NOOP("Reset")},
                                             {SNX_TR_NOOP("Cancel"), SNX_TR_NOOP("Remove")},
                                             {SNX_TR_NOOP("Cancel"), SNX_TR_NOOP("Move")},
                                             {SNX_TR_NOOP("Cancel"), SNX_TR_NOOP("Delete")},
                                             {SNX_TR_NOOP("Cancel"), SNX_TR_NOOP("Delete")},
                                             {SNX_TR_NOOP("Cancel"), SNX_TR_NOOP("Exit")},
                                             {SNX_TR_NOOP("Play"), SNX_TR_NOOP("Recompile")},
                                             {SNX_TR_NOOP("OK"), NULL}};
/* a question added to GuiAsk takes its choices here, in its place */
typedef char snx_gui_ask_choices[SDL_arraysize(ask_choices) == GUI_ASK_ERROR + 1 ? 1 : -1];
/* the question shown, over the whole screen in the middle or at its foot, a choice asked twice saying so once pressed:
   how many choices it has, one for a failure's OK */
static int ask_place(App* app, GuiChoices* c) {
    choices_place(app, GUI_CHOICES_ASK, choices_over(app), ask_question(app), NULL, app->confirm.selected, c);
    return c->count;
}
static void render_ask(App* app) {
    GuiChoices c;
    ask_place(app, &c);
    box_shade(app); /* over another screen */
    choices_draw(app, &c, ask_question(app), NULL);
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
        gui_log("the splash's %s could not be drawn %d pixels square, so the icon shows", app->splash_svg->name, side);
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
    if (!app->splash_drawing) gui_log("the splash's %s could not be read, so the icon shows", svg->name);
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
#ifdef SNX_TOUCH_UI
#define SAY_MENU_KEYS SAY_MENU
#else
#define SAY_MENU_KEYS SNX_TR_KEYS("Arrows/mouse: move. Space/Enter/click: select. F5: refresh. ESC: exit.")
#endif
/* the keys the foot of the screen shown says, Tab's after them: what it draws, and what a click on one presses */
#define GUI_SAID_PARTS 9 /* the most sentences of keys one screen's foot puts together (screen_said) */
/* part after the screen's others while parts has room, so one more than GUI_SAID_PARTS holds is left out of the foot
   (the test of Games' every key shows it), never written past them */
static void said_add(const char** parts, int* n, const char* part) {
    if (*n < GUI_SAID_PARTS) parts[(*n)++] = part;
}
static void folders_said(const App* app, bool pad, const char** parts, int* n);
static void games_said(const App* app, bool pad, const char** parts, int* n);
static int setting_of(const App* app, int row);
static void settings_said(const App* app, bool pad, const char** parts, int* n);
static int hint_next(const char** s, char* key, char* what, size_t size);
/* a key every screen has, which F1's box says and a foot leaves out: moving and picking, paging, the tabs, the side
   box, a pad's D-pad, A, LT/RT and LB/RB. key: an English sentence's, its words between slashes */
static bool key_generic(const char* key) {
    static const char* const words[] = {"Up",    "Down",  "Left",  "Right", "Arrows", "mouse",  "pad",
                                        "Space", "Enter", "click", "PgUp",  "PgDn",   "PgDown", "Tab",
                                        "F2",    "D-pad", "A",     "LT",    "RT",     "LB",     "RB"};
    const char* at = key;
    if (!*key) return false;
    while (*at) {
        size_t n = strcspn(at, "/"), i;
        bool known = false;
        for (i = 0; i < SDL_arraysize(words) && !known; ++i) known = strlen(words[i]) == n && !strncmp(words[i], at, n);
        if (!known) return false;
        at += n + (at[n] == '/');
    }
    return true;
}
/* a sentence of a key every screen has that does a screen's own thing there, and so stays in its foot: the Recompile
   tab's Tab back to Games and Enter that plays */
static bool key_own(const char* part) {
    return !strcmp(part, SAY_QUEUE_TAB) || !strcmp(part, SAY_QUEUE_PLAY) || !strcmp(part, SAY_PAD_QUEUE_TAB) ||
           !strcmp(part, SAY_PAD_QUEUE_PLAY);
}
/* parts' sentences as a translation says them, for a foot (help false): the key back first and a \x1f after it,
   which hints() puts alone at the left, then the sentences of the screen's own keys, F1's last; for F1's box (help)
   every one. A translation has the sentences with keys of its English in their order (scripts/build-languages.py),
   so each is known by its English's */
static const char* keys_said(const char* const* parts, int n, bool pad, bool help, char* out, size_t size) {
    char back[192] = "", kept[1024] = "";
    size_t used = 0;
    int i;
    for (i = 0; i < n; ++i) {
        const char *english = parts[i], *said = snx_tr(parts[i]);
        char key[128], what[256], ekey[128], ewhat[256];
        for (;;) {
            const char* from = said;
            bool found = false;
            while (*from == ' ') ++from;
            if (!hint_next(&said, key, what, sizeof key)) break;
            while (key[0] && !found && hint_next(&english, ekey, ewhat, sizeof ekey)) found = ekey[0] != 0;
            if (!help && found && !strcmp(ekey, pad ? "B" : "ESC")) {
                snprintf(back, sizeof back, "%.*s", (int)(said - from), from);
                continue;
            }
            if (!help && found && key_generic(ekey) && !key_own(parts[i])) continue;
            used += (size_t)snprintf(kept + used, sizeof kept - SDL_min(used, sizeof kept), "%.*s ", (int)(said - from),
                                     from);
            if (used >= sizeof kept) used = sizeof kept - 1;
        }
    }
    if (help)
        snprintf(out, size, "%s", kept);
    else
        snprintf(out, size, "%s\x1f%s%s", back, kept, snx_tr(pad ? SAY_PAD_HELP : SAY_HELP));
    return out;
}
/* the keys of the screen shown: what its foot says (help false: keys_said), or F1's box every one (help) */
static void queue_said(const App* app, bool pad, const char** parts, int* n);
#define SAY_SIDE SNX_TR_KEYS("F2: side box.")
static const char* screen_said(const App* app, bool help, char* out, size_t size) {
    const char *keys = SAY_MENU_KEYS, *parts[GUI_SAID_PARTS];
    int n = 0;
    bool pad = app->pad_driven && !(app->editing && app->form == GUI_FORM_PAD);    /* the button map's speaks of pads */
    if (!app->editing && !app->setup_on && app->menu.view == GUI_LOAD_RECOMPILE) { /* its Tab goes to Games */
        queue_said(app, pad, parts, &n);
#ifdef SNX_TOUCH_UI
        if (!pad) { /* a finger's: the words to tap are the foot, whole */
            size_t used = 0;
            for (int i = 0; i < n && used < size; ++i)
                used += (size_t)snprintf(out + used, size - used, "%s%s", i ? " " : "", snx_tr(parts[i]));
            return out;
        }
#else
        if (!pad) said_add(parts, &n, SAY_SIDE);
#endif
    } else if (pad) {
        if (app->editing)
            keys = app->editor.picking              ? SAY_PAD_PICKER
                   : app->form == GUI_FORM_THEME    ? SAY_PAD_EDITOR
                   : app->form == GUI_FORM_SETTINGS ? SAY_PAD_GROUPS
                   : app->form == GUI_FORM_APP      ? SAY_PAD_APP
                   : app->form == GUI_FORM_FOLDERS  ? SAY_PAD_FOLDERS
                   : app->form == GUI_FORM_USERS    ? SAY_PAD_USERS
                                                    : SAY_PAD_SETTINGS;
        else
            keys = app->menu.view == GUI_LOAD_GAMES ? SAY_PAD_GAMES
                   : pick_screen(app)               ? SAY_PAD_KEYS_HOW
                                                    : SAY_PAD_MENU;
        if (app->editing && app->form == GUI_FORM_FOLDERS) folders_said(app, true, parts, &n);
        if (!app->editing && app->menu.view == GUI_LOAD_GAMES) games_said(app, true, parts, &n);
        said_add(parts, &n, keys);
        if (app->editing) settings_said(app, true, parts, &n);
        if (aot_said(app)) said_add(parts, &n, SAY_PAD_GAMES_AOT);
        said_add(parts, &n, SAY_PAD_TAB);
    } else {
        if (app->editing)
            keys = app->form == GUI_FORM_PAD        ? SAY_PAD
                   : app->editor.picking            ? SAY_PICKER
                   : app->form == GUI_FORM_THEME    ? SAY_EDITOR
                   : app->form == GUI_FORM_SETTINGS ? SAY_GROUPS
                   : app->form == GUI_FORM_APP      ? SAY_APP
                   : app->form == GUI_FORM_FOLDERS  ? SAY_FOLDERS
                   : app->form == GUI_FORM_USERS    ? SAY_USERS
                                                    : SAY_SETTINGS;
        else if (app->menu.view == GUI_LOAD_GAMES)
            keys = games_grid(app) ? SAY_GAMES_GRID : SAY_GAMES;
        else if (pick_screen(app))
            keys = SAY_KEYS_HOW;
        if (app->editing && app->form == GUI_FORM_FOLDERS) folders_said(app, false, parts, &n);
        if (!app->editing && app->menu.view == GUI_LOAD_GAMES) games_said(app, false, parts, &n);
        said_add(parts, &n, keys);
        if (app->editing) settings_said(app, false, parts, &n);
        if (!app->editing && app->menu.view == GUI_LOAD_GAMES) {
            said_add(parts, &n, SAY_GAMES_FOLDERS); /* to add a folder */
            said_add(parts, &n, SAY_GAMES_VIEW);    /* its options' box */
        }
        if (aot_said(app)) said_add(parts, &n, SAY_GAMES_AOT);
#ifdef SNX_TOUCH_UI
        { /* a finger's: the words to tap are the foot, whole */
            size_t used = 0;
            int i;
            for (i = 0; i < n && used < size; ++i)
                used += (size_t)snprintf(out + used, size - used, "%s%s", i ? " " : "", snx_tr(parts[i]));
            return out;
        }
#else
        if (!app->editing && app->menu.view == GUI_LOAD_GAMES) said_add(parts, &n, SAY_BACK); /* to the menu */
        said_add(parts, &n, SAY_TAB);
#endif
    }
    if (!pad) return keys_said(parts, n, false, help, out, size);
    {
        char said[1024];
        keys_said(parts, n, true, help, said, sizeof said);
        gui_pad_keys_named(said, app->pads.labels, out, size);
        return out;
    }
}
static const char* shown_keys(const App* app, char* out, size_t size) { return screen_said(app, false, out, size); }
/* the colour picker's box, its rows and the bars of the first three, in pixels */
typedef struct {
    GuiRect box, rows[GUI_PICK_ROWS], bars[3];
} GuiPicker;
static int item_text_y(const App* app, GuiRect b) { return b.y + app->scale; }
static void touch(App* app, float x, float y);
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
/* a row of a scene of ui.txt, a box of its own on the dim colour (fill) or framed in it (line), the focused one inverted
   on the focus colour or framed and written in it: the theme's cursor and the arrows of hidden rows inside it, the box
   between them, its icon (icons.h) after the marker and a column before the text; the failure colour fills it (every
   other blink_ms with UI_ATTR_BLINK, steady with 0), the active colour writes the one activated last, and an open one
   (UI_ATTR_OPEN) has the focus's look without the cursor; a title (UI_ATTR_TITLE) is on a band of ink in a theme of
   filled boxes, else over a rule. One with nothing to do now (UI_ATTR_OFF) is greyed, still there: written in the soft
   colour, or inverted on it where the focus inverts */
/* a row's text's top: as item_text_y has it in a row a cell and a pixel high, in the middle of a taller one */
static int ui_text_y(const App* app, GuiRect b) { return b.y + (b.h - cell_h(app) + app->scale) / 2; }
static int user_side(const App* app, int h);
/* the arrows of the rows hidden above and below (UI_ATTR_MORE_UP, UI_ATTR_MORE_DOWN) at the right of b, from pixel
   row y: up beside down when one row shows */
static void row_arrows(App* app, GuiRect b, int y, unsigned attr, SDL_Color color) {
    if (app->rows_barred) return; /* a scroll bar beside the rows says it */
    if (attr & UI_ATTR_MORE_UP)
        text_in(app, b, b.x + b.w - ((attr & UI_ATTR_MORE_DOWN) ? 2 : 1) * cell_w(app), y, "\x1e", color);
    if (attr & UI_ATTR_MORE_DOWN) text_in(app, b, b.x + b.w - cell_w(app), y, "\x1f", color);
}
/* the drawing kept inside r while a list draws its rows shifted (UiList's shift), within a clip set before; NULL puts
   that one back */
void ui_clip(App* app, const GuiRect* r) {
    if (r) {
        SDL_Rect clip = {r->x, r->y, r->w, r->h};
        app->clip_had = SDL_RenderClipEnabled(app->renderer) && SDL_GetRenderClipRect(app->renderer, &app->clip_was);
        if (app->clip_had && !SDL_GetRectIntersection(&clip, &app->clip_was, &clip)) clip = (SDL_Rect){0, 0, 0, 0};
        SDL_SetRenderClipRect(app->renderer, &clip);
    } else
        SDL_SetRenderClipRect(app->renderer, app->clip_had ? &app->clip_was : NULL);
}
/* the room a scroll bar takes at a list's right: a letter, and a pixel between it and the rows */
static int bar_room(const App* app) { return cell_w(app) + app->scale; }
/* a scroll bar a letter wide at the right of area, for view of total (rows or pixels) shown from at: its track, as
   tall as area, and its thumb, as tall as the share shown (twice as tall as wide at least) and where that share is
   among them; false while all show */
static bool bar_place(const App* app, GuiRect area, long long total, long long view, long long at, GuiRect* track,
                      GuiRect* thumb) {
    int w = cell_w(app), h;
    if (view <= 0 || total <= view) return false;
    *track = gui_rect(area.x + area.w - w, area.y, w, area.h);
    h = SDL_min(area.h, SDL_max(2 * w, (int)((long long)area.h * view / total)));
    at = SDL_clamp(at, 0, total - view);
    *thumb = gui_rect(track->x, track->y + (int)((long long)(area.h - h) * at / (total - view)), w, h);
    return true;
}
/* a scroll bar: its track faint, its thumb in the soft colour, lit while held */
static void bar_draw(App* app, GuiRect track, GuiRect thumb, bool held) {
    fill(app, (float)track.x, (float)track.y, (float)track.w, (float)track.h, app->theme.soft, 72);
    fill_rect(app, thumb, held ? app->theme.focus : app->theme.soft);
}
/* the bar drawn, a press there taking its thumb (bar_at), unless a frame already has all it holds */
static void bar_keep(App* app, GuiRect area, GuiRect track, GuiRect thumb, size_t count, size_t shown, int list) {
    if (app->nbars < GUI_BARS) app->bars[app->nbars++] = (GuiBar){area, track, thumb, count, shown, list};
}
int ui_bar_width(App* app, size_t count, size_t shown) { return shown && count > shown ? bar_room(app) : 0; }
int ui_bar(App* app, GuiRect r, const UiList* list, size_t count, size_t shown) {
    GuiRect track = {0}, thumb = {0};
    int which = list->store ? (int)(list->store - app->firsts) : -1; /* list_follow's, in firsts */
    long long step = which >= 0 && app->list_step[which] > 0 ? app->list_step[which] : 1; /* by the pixel, or row */
    bool barred =
        ui_bar_width(app, count, shown) && bar_place(app, r, (long long)count * step, (long long)shown * step,
                                                     (long long)list->first * step + list->shift, &track, &thumb);
    if (barred) bar_draw(app, track, thumb, which >= 0 && which + 1 == app->bar_hold);
    if (which >= 0) { /* a finger's or a thumb's to scroll, as far as its last rows */
        app->list_shown[which] = shown;
        app->list_max[which] = count > shown && step > 1 ? (float)((long long)(count - shown) * step) : 0;
        bar_keep(app, r, track, thumb, count, shown, which);
    }
    return barred ? bar_room(app) : 0;
}
/* row i shown of a scene's list (its row and shown, rects, item) as drawn: up by its shift, scrolled between two
   rows, and beside its scroll bar while its rows do not all show */
static GuiRect row_drawn(App* app, GuiRect (*row)(const GuiRect*, int, int, size_t),
                         size_t (*shown)(const GuiRect*, int, int), const GuiRect* rects, int item, const UiList* list,
                         size_t i) {
    GuiRect r = row(rects, app->scale, item, i);
    r.w -= ui_bar_width(app, list->count, shown(rects, app->scale, item));
    r.y -= list->shift;
    return r;
}
void ui_draw_row(App* app, GuiRect r, const char* text, unsigned icon, const void* image, unsigned color, unsigned attr,
                 bool focused) {
    const GuiTheme* t = &app->theme;
    char glyph[4];
    int look = focused || (attr & UI_ATTR_OPEN), filled = t->panels == GUI_PANELS_FILL, inverted = look && t->focus_bar;
    int failing =
        (attr & UI_ATTR_FAILURE) && !((attr & UI_ATTR_BLINK) && t->blink_ms && (now_ms(app) / t->blink_ms) % 2);
    int off = (attr & UI_ATTR_OFF) != 0;
    GuiRect b = ui_row_box(app, r);
    char label[400];
    if (attr & UI_ATTR_LABEL) { /* between rows, as "Recent Games": no box, no marker */
        text_in(app, b, b.x, ui_text_y(app, b), text, rgb(t->soft));
        row_arrows(app, b, ui_text_y(app, b), attr, rgb(t->soft)); /* a list's first or last row too */
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
    snprintf(label, sizeof label, "%s%s%s%s", t->marker, gui_icon_utf8(icon, glyph), icon ? " " : "", text);
    if (failing || inverted)
        fill_rect(app, b, failing ? t->failure : off ? t->soft : t->focus);
    else if (filled)
        fill_rect(app, b, t->dim);
    if (!failing && !inverted && (!filled || look)) outline(app, b, look ? t->focus : t->dim);
    {
        SDL_Color ink = rgb(failing || inverted      ? t->page
                            : off                    ? t->soft
                            : attr & UI_ATTR_WARNING ? t->warning
                            : attr & UI_ATTR_ACTIVE  ? t->active
                            : look                   ? t->focus
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
    row_arrows(app, b, ui_text_y(app, b), attr, rgb(failing || inverted ? t->page : t->soft));
}
/* a label of a scene of ui.txt, in the soft colour unless it names one */
void ui_draw_label(App* app, GuiRect r, const char* text, unsigned color) {
    text_in(app, r, r.x, item_text_y(app, r), text, rgb(ui_colour(app, color, app->theme.soft)));
}
/* a tile of a tile list (Games' grid), framed as a row's box is in its look: the image a square in the middle of its
   top, the text under it in the middle (from its left, sliding, where it is longer than the tile), the arrows of the
   rows hidden above and below at its top right; greyed as a row (UI_ATTR_OFF) */
void ui_draw_tile(App* app, GuiRect r, const char* text, const void* image, unsigned color, unsigned attr,
                  bool focused) {
    const GuiTheme* t = &app->theme;
    int look = focused || (attr & UI_ATTR_OPEN), filled = t->panels == GUI_PANELS_FILL, inverted = look && t->focus_bar;
    int failing =
        (attr & UI_ATTR_FAILURE) && !((attr & UI_ATTR_BLINK) && t->blink_ms && (now_ms(app) / t->blink_ms) % 2);
    int off = (attr & UI_ATTR_OFF) != 0;
    int s = app->scale, cw = cell_w(app), ch = cell_h(app), side = SDL_min(r.w - 4 * s, r.h - ch - 6 * s);
    int cols = gui_text_cols(text), room = (r.w - cw) / cw;
    SDL_Color ink = rgb(failing || inverted      ? t->page
                        : off                    ? t->soft
                        : attr & UI_ATTR_WARNING ? t->warning
                        : attr & UI_ATTR_ACTIVE  ? t->active
                        : look                   ? t->focus
                                                 : ui_colour(app, color, t->ink));
    SDL_Color soft = rgb(failing || inverted ? t->page : t->soft);
    GuiRect in = gui_rect(r.x + cw / 2, r.y, r.w - cw, r.h); /* half a cell either side */
    if (failing || inverted)
        fill_rect(app, r, failing ? t->failure : off ? t->soft : t->focus);
    else if (filled)
        fill_rect(app, r, t->dim);
    if (!failing && !inverted && (!filled || look)) outline(app, r, look ? t->focus : t->dim);
    if (image && side > 0) {
        SDL_FRect at = {(float)(r.x + (r.w - side) / 2), (float)(r.y + 2 * s), (float)side, (float)side};
        SDL_RenderTexture(app->renderer, (SDL_Texture*)image, NULL, &at);
    }
    text_in(app, in, cols < room ? r.x + (r.w - cols * cw) / 2 : in.x, r.y + r.h - ch - 2 * s, text, ink);
    row_arrows(app, r, r.y + 2 * s, attr, soft);
}
const char* ui_tr(const char* english) { return snx_tr(english); }
/* The keys of a screen: "Keys: what they do." sentences, each drawn as the keys, inverted (the theme's focus bar) or in
   the focus colour, and what they do; a row holds as many as fit, against its right edge */
static int hint_word(const char* word, size_t n, bool pad);
static bool in_rect(GuiRect r, float x, float y);
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
    if (!colon && hint_word(p, (size_t)(end - p), true) >= 0) { /* a key's word alone ("Refresh."): the key it is */
        snprintf(key, size, "%.*s", (int)(end - p), p);
        what[0] = 0;
        *s = *end ? end + 1 : end;
        return 1;
    }
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
/* a button's measures in columns of its letters, the tabs' and the foot's keys' alike: a column inside either side of
   its text, and one between two */
#define GUI_BUTTON_PAD 1
#define GUI_BUTTON_GAP 1
/* a sentence of the foot's keys as a button: its key, a space and what it does, a column inside either side */
static int hint_width(const char* key, const char* what) {
    return gui_text_cols(key) + (key[0] ? 1 : 0) + gui_text_cols(what) + 2 * GUI_BUTTON_PAD;
}
/* the columns the key back takes alone at the left of a foot (keys_said: what is before its \x1f), 0 for none */
static int hints_cols(const char* keys) {
    const char* apart = strchr(keys, '\x1f');
    char back[192], key[128], what[128];
    const char* at = back;
    int cols = 0;
    if (!apart) return 0;
    snprintf(back, sizeof back, "%.*s", (int)(apart - keys), keys);
    while (hint_next(&at, key, what, sizeof key)) cols += hint_width(key, what) + (cols ? GUI_BUTTON_GAP : 0);
    return cols;
}
/* a click or a tap, for hints() to say which sentence with keys it is on: their keys, and how many columns into the
   sentence (-1: the margin before it); key empty for none */
typedef struct {
    float x, y;
    char key[128];
    int col;
} GuiHintHit;
/* the rows keys take in width columns, each sentence a button as a tab is (a box button_h high, two pixels times the
   scale over and under its row, its text a column inside, a column between two); with draw, drawn from pixel y in r,
   on the foot's band of ink when band; with hit, the sentence whose button its pixel is in */
/* h pixels, or on a screen of fingers a finger's row at the least (GUI_FINGER_ROW times the scale, as ui.txt's rows) */
static int finger_h(const App* app, int h) { return app->fingers ? SDL_max(h, GUI_FINGER_ROW * app->scale) : h; }
/* a button's height, a tab's and a row of the foot's keys' alike: a cell of its letters and two pixels times the scale
   above and below, a finger's row on a phone */
static int button_h(const App* app) { return finger_h(app, cell_h(app) + 4 * app->scale); }
/* a row of the foot's keys, in the foot's letters: a button's without the two pixels above and below, which the foot's
   margins give it */
static int hint_pitch(const App* app) { return button_h(app) - 4 * app->scale; }
static int hints(App* app, const char* keys, int width, GuiRect r, int y, int draw, int band, GuiHintHit* hit) {
    const GuiTheme* t = &app->theme;
    char key[128], what[128];
    const char *row = keys, *apart = strchr(keys, '\x1f');
    int rows = 0, s = app->scale, lit, off, filled = t->panels == GUI_PANELS_FILL;
    if (apart) { /* the key back alone at the left of the first row (keys_said), three columns before the others */
        char back[192];
        int cols = hints_cols(keys);
        snprintf(back, sizeof back, "%.*s", (int)(apart - keys), keys);
        if (cols) hints(app, back, cols, gui_rect(r.x, r.y, cols * cell_w(app), r.h), y, draw, band, hit);
        return hints(app, apart + 1, width - (cols ? cols + 3 * GUI_BUTTON_GAP : 0), r, y, draw, band, hit);
    }
    for (;;) {
        const char *at = row, *end = row, *u = row;
        int used = 0, x;
        while (hint_next(&at, key, what, sizeof key)) {
            int w = hint_width(key, what) + (used ? GUI_BUTTON_GAP : 0);
            if (used && used + w > width) break;
            used += w;
            end = at;
        }
        if (end == row) break;
        for (x = r.x + r.w - used * cell_w(app);
             (draw || hit) && u != end && (off = hint_off(u), hint_next(&u, key, what, sizeof key));) {
            int cy = y + rows * (hint_pitch(app) + 4 * s), k = gui_text_cols(key),
                w = hint_width(key, what) * cell_w(app);
            int ty = cy + (hint_pitch(app) - cell_h(app)) / 2; /* the text in the middle of its row */
            GuiRect button = gui_rect(x, cy - 2 * s, w, button_h(app));
            x += GUI_BUTTON_PAD * cell_w(app); /* its text */
            if (hit && k && !off && in_rect(button, hit->x, hit->y)) {
                snprintf(hit->key, sizeof hit->key, "%s", key);
                hit->col = (int)SDL_floorf((hit->x - (float)x) / (float)cell_w(app));
            }
            lit = draw && k && !off && app->hovering && in_rect(button, app->hover_x, app->hover_y) &&
                  hint_pick(key, (int)SDL_floorf((app->hover_x - (float)x) / (float)cell_w(app)), app->pad_driven) >= 0;
            if (lit) /* the button under the pointer: inverted */
                fill_rect(app, button, band ? t->page : t->focus);
            else if (draw && band) /* on the band of ink: framed in the soft colour */
                outline(app, button, t->soft);
            else if (draw && filled)
                fill_rect(app, button, t->dim);
            else if (draw)
                outline(app, button, t->dim);
            if (draw && k && !off && !lit && t->focus_bar)
                fill_rect(app, gui_rect(x - s, ty, k * cell_w(app) + 2 * s, cell_h(app)), band ? t->soft : t->focus);
            if (draw && k)
                text_in(app, r, x, ty, key,
                        rgb(off                    ? t->soft
                            : lit                  ? (band ? t->ink : t->page)
                            : t->focus_bar || band ? t->page
                                                   : t->focus));
            if (draw)
                text_in(app, r, x + (k + (k ? 1 : 0)) * cell_w(app), ty, what,
                        rgb(off    ? t->soft
                            : lit  ? (band ? t->ink : t->page)
                            : band ? t->page
                                   : t->ink));
            x += w - GUI_BUTTON_PAD * cell_w(app) + GUI_BUTTON_GAP * cell_w(app);
        }
        ++rows;
        row = end;
    }
    return SDL_max(rows, 1);
}
/* the pixels rows rows of the foot's keys take, their buttons' four pixels times the scale between two */
static int hints_h(const App* app, int rows) { return rows * hint_pitch(app) + (rows - 1) * 4 * app->scale; }
/* ---- the foot and the tips: the keys' box, the tips' beside it or on a row under the head ---- */
/* the foot's letters, a size under the rest's but 1 at the least (a finger's buttons, the text's): what draws or measures
   the keys, and the tips when they are in the foot, does it with app->scale set to them, then puts the menus' back */
static int foot_scale(const App* app) { return app->fingers ? app->scale : SDL_max(1, app->scale - 1); }
/* the tips under the head, a Settings box's note and the Status box: their letters a half step under the text's,
   between its size and the foot's (1.5x by a 2x text); app->half as it was, to put back */
static int text_half(App* app) {
    int was = app->half;
    app->half = app->scale + foot_scale(app);
    return was;
}
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
    int small = foot_scale(app), half = app->half;
    GuiRect foot = below ? gui_rect(p->tips.x, p->keys.y, p->keys.x + p->keys.w - p->tips.x, p->keys.h) : p->keys;
    GuiRect text;
    if (!p->keys.h) return;
    if (band) fill_rect(app, foot, t->ink);
    if (t->footer_rule) fill_rect(app, gui_rect(foot.x, foot.y - cell_h(app) / 4 - s / 2, foot.w, s), t->soft);
    if (below && band && !(color.r == t->failure.r && color.g == t->failure.g && color.b == t->failure.b))
        color = rgb(t->page);      /* on the band but a failure */
    if (below) app->scale = small; /* beside the keys, in the foot's letters */
    text = foot_box(app, p->tips, t->tips_box, below && band);
    if (!below) half = text_half(app); /* under the head, a half step under the text's */
    if (below && t->tips_wrap)
        wrap_in(app, text, text.x, text.y, tips, 0, color);
    else if (p->tips.h) /* one row: a long one slides */
        text_in(app, text, text.x, text.y, tips, color);
    app->half = half;
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
/* dir's last part, its name among the failed ones */
static const char* icon_name(const char* dir) {
    const char* leaf = dir + strlen(dir);
    while (leaf > dir && leaf[-1] != '/' && leaf[-1] != '\\') --leaf;
    return leaf;
}
/* a dump the driver gave no icon for, kept for the session with why: said once in the log, with all the driver said */
static void icon_failed_keep(GuiDumpIcons* d, const char* dump, const char* dir, const char* why, const char* said) {
    GuiIconFailure* grown;
    SDL_LockMutex(d->lock);
    if (d->failed_count == d->failed_room &&
        (grown = (GuiIconFailure*)realloc(d->failed, (size_t)(d->failed_room * 2 + 16) * sizeof *grown)) != NULL) {
        d->failed = grown;
        d->failed_room = d->failed_room * 2 + 16;
    }
    if (d->failed_count < d->failed_room) {
        GuiIconFailure* f = &d->failed[d->failed_count++];
        snprintf(f->name, sizeof f->name, "%s", icon_name(dir));
        snprintf(f->why, sizeof f->why, "%s", why);
    }
    SDL_UnlockMutex(d->lock);
    size_t n = said ? strlen(said) : 0;
    while (n && (said[n - 1] == '\n' || said[n - 1] == '\r')) --n;
    gui_log("no icon for %s: %s%s%.*s", dump, why, n ? "\nthe driver said:\n" : "", (int)n, said ? said : "");
}
/* why the driver gave no icon for the dump whose folder in the cache is dir, in this session: true and why in out */
static bool icon_failed(const GuiDumpIcons* d, const char* dir, char* out, size_t size) {
    bool found = false;
    if (!d->lock) return false;
    SDL_LockMutex(d->lock);
    for (int i = 0; i < d->failed_count && !found; ++i)
        if (!strcmp(d->failed[i].name, icon_name(dir))) {
            found = true;
            snprintf(out, size, "%s", d->failed[i].why);
        }
    SDL_UnlockMutex(d->lock);
    return found;
}
/* the last line of text that is not blank, at most size - 1 bytes of it, into out */
static void last_line(const char* text, char* out, size_t size) {
    const char *end = text + strlen(text), *start;
    while (end > text && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) --end;
    for (start = end; start > text && start[-1] != '\n';) --start;
    snprintf(out, size, "%.*s", (int)(end - start), start);
}
/* a dump's ask started: the driver's process, its output read from then on; false when it could not start, kept as
   failed then */
static bool icon_ask_start(GuiDumpIcons* d, GuiIconAsk* a, char* dump, char* dir) {
    const char* args[] = {d->driver, "--data", d->repo, "-x", "--icon", "--out", dir, "--", dump, NULL};
    char why[256];
    *a = (GuiIconAsk){0};
    a->dump = dump;
    a->dir = dir;
    a->since = SDL_GetTicks();
    if (snx_mkdirs(dir)) {
        char error[160];
        snprintf(why, sizeof why, snx_tr("%s cannot be made: %s"), dir, snx_os_error(error, sizeof error));
    } else if (!(a->process = gui_process_create()))
        snprintf(why, sizeof why, "%s", snx_tr("Cannot start the run: out of memory"));
    else if (gui_process_start(a->process, args, why, sizeof why))
        return true;
    icon_failed_keep(d, dump, dir, why, NULL);
    gui_process_destroy(a->process);
    a->process = NULL;
    return false;
}
/* the ask looked at: what the driver printed since, and once it ended (or was stopped past wait_ms) its answer, the
   icon in its folder or why not kept. True once it ended, its process gone */
static bool icon_ask_poll(GuiDumpIcons* d, GuiIconAsk* a) {
    char buffer[4096], why[256], line[200];
    int n, status = 1;
    bool late = SDL_GetTicks() - a->since > (Uint64)d->wait_ms;
    while ((n = gui_process_read(a->process, buffer, sizeof buffer)) > 0) {
        size_t take = SDL_min((size_t)n, sizeof a->said - 1 - a->size);
        memcpy(a->said + a->size, buffer, take);
        a->said[a->size += take] = 0;
    }
    if (late) gui_process_terminate(a->process);
    if (!gui_process_finished(a->process, &status)) return false;
    {
        char* icon = snx_path_join(a->dir, "icon.jpg");
        if (late || status || !snx_is_file(icon)) {
            last_line(a->said, line, sizeof line);
            if (late)
                snprintf(why, sizeof why, snx_tr("the driver gave no answer in %d s"), (d->wait_ms + 999) / 1000);
            else if (line[0])
                snprintf(why, sizeof why, "%s", line);
            else if (status)
                snprintf(why, sizeof why, snx_tr("the driver ended with status %d"), status);
            else
                snprintf(why, sizeof why, "%s", snx_tr("the driver ended without one"));
            /* all the driver said, past the line that is why */
            icon_failed_keep(d, a->dump, a->dir, why,
                             strcmp(why, line) || strstr(a->said, line) != a->said ? a->said : NULL);
        }
        free(icon);
    }
    gui_process_destroy(a->process);
    a->process = NULL;
    return true;
}
/* one dump asked of the driver, waiting for its answer: its icon.jpg in dir, else why kept */
static void dump_icon_answer(GuiDumpIcons* d, char* dump, char* dir) {
    GuiIconAsk* a = (GuiIconAsk*)malloc(sizeof *a); /* its said is too big for a thread's stack */
    if (!a) return;
    if (icon_ask_start(d, a, dump, dir))
        while (!icon_ask_poll(d, a)) SDL_Delay(5);
    free(a);
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
static const char* driver_name(int item);
static unsigned loads_off(App* app);
/* a dump's icon to ask the driver for, once: false when it cannot be asked (the queue full, or no driver, kept as why
   it has none) */
static bool dump_icon_ask(App* app, const char* dump, const char* dir) {
    GuiDumpIcons* d = &app->dump_icons;
    bool asked = false;
    int i;
    if (!app->data_dir) return false;
    if (!d->lock && (!(d->lock = SDL_CreateMutex()) || !(d->wake = SDL_CreateCondition()))) {
        if (d->lock) SDL_DestroyMutex(d->lock);
        d->lock = NULL;
        return false;
    }
    if (!d->wait_ms) d->wait_ms = GUI_ICON_WAIT;
    if (!d->driver) {
        d->driver = find_driver(app, GUI_SWITCH);
#ifdef SDL_PLATFORM_IOS /* the app's own, which gui_process_start runs on a thread: its name is only argv[0] */
        if (!d->driver) d->driver = snx_strdup("scarletnx");
#endif
        if (!d->driver) {
            char why[256];
            snprintf(why, sizeof why, snx_tr("Cannot find %s beside ScarletNX or on PATH."), driver_name(GUI_SWITCH));
            icon_failed_keep(d, dump, dir, why, NULL);
            return false;
        }
        free(d->repo);
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
/* the asker's thread, answering from now on: the program's alone (tests answer with dump_icons_pass); none on iOS,
   whose loop asks them (icons_pump) */
static void dump_icons_start(App* app) {
#ifndef SDL_PLATFORM_IOS
    GuiDumpIcons* d = &app->dump_icons;
    if (d->lock && !d->thread && !(d->thread = SDL_CreateThread(dump_icons_run, "dump icons", d))) {
        snprintf(app->message, sizeof app->message, snx_tr("The dumps' icons are not asked for (%s)"), SDL_GetError());
        say_failure(app, app->message);
    }
#else
    (void)app;
#endif
}
#ifdef SDL_PLATFORM_IOS
/* iOS: the driver runs on a thread of the app, one at a time (process_posix.c), so a dump's icon is asked by the menu's
   loop while neither a run nor the queue's recompile goes, as the queue's are: the next once one ended. job_start waits
   for it, and launch until it ended */
static void icons_pump(App* app) {
    GuiDumpIcons* d = &app->dump_icons;
    char *dump, *dir;
    if (!d->lock) return;
    if (d->ask.process) {
        if (icon_ask_poll(d, &d->ask)) {
            free(d->ask.dump);
            free(d->ask.dir);
            d->ask.dump = d->ask.dir = NULL;
            SDL_LockMutex(d->lock);
            ++d->answered;
            SDL_UnlockMutex(d->lock);
        }
        return;
    }
    if (app->menu.running || gui_queue_running(&app->queue) >= 0) return;
    SDL_LockMutex(d->lock);
    if (!d->count) {
        SDL_UnlockMutex(d->lock);
        return;
    }
    dump = d->dumps[0];
    dir = d->dirs[0];
    memmove(d->dumps, d->dumps + 1, (size_t)--d->count * sizeof *d->dumps);
    memmove(d->dirs, d->dirs + 1, (size_t)d->count * sizeof *d->dirs);
    SDL_UnlockMutex(d->lock);
    if (!icon_ask_start(d, &d->ask, dump, dir)) {
        free(dump);
        free(dir);
        SDL_LockMutex(d->lock);
        ++d->answered;
        SDL_UnlockMutex(d->lock);
    }
}
/* iOS: the icon being asked, answered first, as a run or a recompile takes the one driver thread */
static void icons_wait(App* app) {
    while (app->dump_icons.ask.process) {
        icons_pump(app);
        if (app->dump_icons.ask.process) SDL_Delay(5);
    }
}
#endif
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
    if (d->ask.process) { /* iOS's: asked to stop, then reaped */
        gui_process_terminate(d->ask.process);
        while (!icon_ask_poll(d, &d->ask)) SDL_Delay(5);
    }
    free(d->ask.dump);
    free(d->ask.dir);
    for (i = 0; i < d->count; ++i) {
        free(d->dumps[i]);
        free(d->dirs[i]);
    }
    free(d->failed);
    free(d->driver);
    free(d->repo);
    if (d->wake) SDL_DestroyCondition(d->wake);
    if (d->lock) SDL_DestroyMutex(d->lock);
    *d = (GuiDumpIcons){0};
}
/* the icon file of the game or dump at path, owned: a game's own icon.jpg (gui_game_icon); a dump's in its folder of
   the cache, asked of the driver when it has none and the driver gave none in this session, *loading then saying so.
   NULL while there is none */
static char* icon_file(App* app, const char* path, bool* loading) {
    char *dir, *file, why[8];
    const char* leaf = path + strlen(path);
    *loading = false;
    while (leaf > path && leaf[-1] != '/' && leaf[-1] != '\\') --leaf;
    if (!gui_is_dump(leaf)) return gui_game_icon(path);
    if (!(dir = dump_icon_dir(app, path))) return NULL;
    file = snx_path_join(dir, "icon.jpg");
    if (!snx_is_file(file)) {
        if (!icon_failed(&app->dump_icons, dir, why, sizeof why) && (*loading = dump_icon_ask(app, path, dir)) &&
            app->dump_icons.live)
            dump_icons_start(app);
        free(file);
        file = NULL;
    }
    free(dir);
    return file;
}
/* what a dump's row shows while its icon is asked for: three dots, grey on either theme's page */
static SDL_Texture* icon_loading(App* app) {
    static const int dots[3] = {34, 64, 94};
    Uint32* pixels;
    int x, y, i;
    if (app->icon_loading) return app->icon_loading;
    if (!(pixels = (Uint32*)calloc((size_t)GUI_ICON_SIDE * GUI_ICON_SIDE, sizeof *pixels))) return NULL;
    for (i = 0; i < 3; ++i)
        for (y = 54; y < 74; ++y)
            for (x = dots[i] - 10; x < dots[i] + 10; ++x)
                if ((x - dots[i]) * (x - dots[i]) + (y - 64) * (y - 64) <= 100)
                    pixels[y * GUI_ICON_SIDE + x] = 0xff8a8a8au;
    app->icon_loading = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
                                          GUI_ICON_SIDE, GUI_ICON_SIDE);
    if (app->icon_loading && (!SDL_UpdateTexture(app->icon_loading, NULL, pixels, GUI_ICON_SIDE * 4) ||
                              !SDL_SetTextureBlendMode(app->icon_loading, SDL_BLENDMODE_BLEND_PREMULTIPLIED))) {
        SDL_DestroyTexture(app->icon_loading);
        app->icon_loading = NULL;
    }
    free(pixels);
    return app->icon_loading;
}
/* a + on a white square into side by side ARGB pixels, as Wimsy's cover is drawn: two bars an eighth of the side
   thick and three fifths of it long, in a grey of Wimsy's */
static void plus_pixels(Uint32* pixels, int side) {
    const int c = side / 2, half = side * 3 / 10, thick = side / 16;
    for (int i = 0; i < side * side; ++i) pixels[i] = 0xffffffffu;
    for (int y = c - half; y < c + half; ++y)
        for (int x = c - half; x < c + half; ++x)
            if ((x >= c - thick && x < c + thick) || (y >= c - thick && y < c + thick))
                pixels[y * side + x] = 0xff626262u;
}
/* Show More's cover, as big as a game's: a + (plus_pixels) */
static SDL_Texture* icon_more(App* app) {
    Uint32* pixels;
    if (app->icon_more) return app->icon_more;
    if (!(pixels = (Uint32*)malloc((size_t)GUI_ICON_SIDE * GUI_ICON_SIDE * sizeof *pixels))) return NULL;
    plus_pixels(pixels, GUI_ICON_SIDE);
    app->icon_more = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, GUI_ICON_SIDE,
                                       GUI_ICON_SIDE);
    if (app->icon_more && (!SDL_UpdateTexture(app->icon_more, NULL, pixels, GUI_ICON_SIDE * 4) ||
                           !SDL_SetTextureBlendMode(app->icon_more, SDL_BLENDMODE_BLEND_PREMULTIPLIED))) {
        SDL_DestroyTexture(app->icon_more);
        app->icon_more = NULL;
    }
    free(pixels);
    return app->icon_more;
}
/* count ARGB pixels, premultiplied, made grey: each colour its light (BT.601), under the same alpha */
static void icon_grey(Uint32* pixels, int count) {
    for (int i = 0; i < count; ++i) {
        Uint32 p = pixels[i], r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
        Uint32 y = (r * 77 + g * 150 + b * 29) >> 8;
        pixels[i] = (p & 0xff000000u) | y << 16 | y << 8 | y;
    }
}
/* Wimsy in grey, as a game with no icon of its own shows: the splash's drawing */
static SDL_Texture* icon_none(App* app) {
    Uint32* pixels;
    if (app->icon_none) return app->icon_none;
    if (!(pixels = (Uint32*)malloc((size_t)GUI_ICON_SIDE * GUI_ICON_SIDE * 4))) return NULL;
    snx_splash_picture(NULL, 0, GUI_ICON_SIDE, 0, pixels, GUI_ICON_SIDE * 4);
    icon_grey(pixels, GUI_ICON_SIDE * GUI_ICON_SIDE);
    app->icon_none = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, GUI_ICON_SIDE,
                                       GUI_ICON_SIDE);
    if (app->icon_none && (!SDL_UpdateTexture(app->icon_none, NULL, pixels, GUI_ICON_SIDE * 4) ||
                           !SDL_SetTextureBlendMode(app->icon_none, SDL_BLENDMODE_BLEND_PREMULTIPLIED))) {
        SDL_DestroyTexture(app->icon_none);
        app->icon_none = NULL;
    }
    free(pixels);
    return app->icon_none;
}
static SDL_Texture* game_icon(App* app, const char* path) {
    GuiIcon* icon;
    char* file;
    unsigned char* jpeg;
    void* pixels;
    size_t size = 0, i;
    bool loading;
    for (i = 0; i < GUI_ICONS; ++i)
        if (app->icons[i].path && !strcmp(app->icons[i].path, path))
            return app->icons[i].loading ? icon_loading(app) : app->icons[i].texture;
    icon = &app->icons[app->icons_next];
    app->icons_next = (app->icons_next + 1) % GUI_ICONS;
    free(icon->path);
    SDL_DestroyTexture(icon->texture);
    *icon = (GuiIcon){snx_strdup(path), NULL, false};
    file = icon_file(app, path, &loading);
    if (loading) {
        icon->loading = true;
        return icon_loading(app);
    }
    jpeg = file ? snx_read_file(file, &size) : NULL;
    pixels = jpeg ? malloc((size_t)GUI_ICON_SIDE * GUI_ICON_SIDE * 4) : NULL;
    if (pixels && snx_splash_picture(jpeg, size, GUI_ICON_SIDE, 0, pixels, GUI_ICON_SIDE * 4))
        snprintf(app->message, sizeof app->message, snx_tr("%s could not be drawn: the game shows Wimsy in grey"),
                 file);
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
/* why the dump at path shows no icon, the driver's answer in this session, into out: false when none was given */
static bool dump_icon_why(const App* app, const char* path, char* out, size_t size) {
    char* dir = gui_is_dump(path) ? dump_icon_dir(app, path) : NULL;
    bool failed = dir && icon_failed(&app->dump_icons, dir, out, size);
    free(dir);
    return failed;
}
/* the rows without an icon looked at again: the driver answered for a dump */
static void icons_forget_none(App* app) {
    size_t i;
    for (i = 0; i < GUI_ICONS; ++i)
        if (app->icons[i].path && !app->icons[i].texture) {
            free(app->icons[i].path);
            app->icons[i].path = NULL;
            app->icons[i].loading = false;
        }
}
/* every icon game_icon kept: drawn again from their files when shown, as a game's may have changed */
static void icons_free(App* app) {
    size_t i;
    for (i = 0; i < GUI_ICONS; ++i) {
        free(app->icons[i].path);
        SDL_DestroyTexture(app->icons[i].texture);
        app->icons[i] = (GuiIcon){NULL, NULL, false};
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
/* the box over the screen, as wide as its title or the longest nickname with its picture; a row each, ui.txt's scene
   Users, the focused one among those shown (the box kept on the screen) */
#define SAY_USERS_TITLE SNX_TR_NOOP("Who plays the games")
typedef struct {
    GuiRect box;
    GuiRect rects[UI_USERS_ITEMS];
    UiList lists[UI_USERS_ITEMS]; /* the rows' at UI_USERS_ROWS: row i is g_users[i] */
} GuiUsersBoxes;
/* a box over the screen of count rows of a scene's list: cols wide, its head over them, as high as the rows or as the
   screen; the rows' area, which the scene is laid out in */
static GuiRect over_rows(App* app, int cols, int rows_height, GuiRect* box) {
    int width, height, s = app->scale, h;
    area(app, &width, &height);
    h = SDL_min(rows_height, SDL_max(0, height - head_h(app) - 12 * s));
    *box = over_rect(app, cols, head_h(app) + h + 12 * s);
    return gui_rect(box->x + cell_w(app), box->y + head_h(app) + 6 * s, box->w - 2 * cell_w(app), h);
}
static void users_boxes(App* app, GuiUsersBoxes* u) {
    int i, s = app->scale;
    int longest = SDL_max(16, gui_text_cols(snx_tr(SAY_USERS_TITLE)) + 2);
    for (i = 0; i < g_user_count; ++i)
        longest = SDL_max(longest, gui_text_cols(g_users[i].nickname) + gui_text_cols(app->theme.marker) + 4);
    ui_users_layout(over_rows(app, longest,
                              rows_h(ui_users_layout, ui_users_row, u->rects, UI_USERS_ROWS, s, g_user_count), &u->box),
                    s, u->rects);
    memset(u->lists, 0, sizeof u->lists);
    u->lists[UI_USERS_ROWS].focus = (size_t)app->users_focus;
    list_follow(app, GUI_LIST_USERS, ui_users_follow, ui_users_row, u->rects, UI_USERS_ROWS, (size_t)g_user_count,
                &u->lists[UI_USERS_ROWS]);
}
/* Users' row: the nickname, in the active colour for the one the games play as */
bool ui_users_rows_get_row_info(App* app, size_t row, int width, UiRowInfo* info) {
    (void)width; /* a nickname is 20 columns at most, the box as wide as the longest */
    if (row >= (size_t)g_user_count) return false;
    info->label = g_users[row].nickname;
    if ((int)row == app->user - 1) info->attr |= UI_ATTR_ACTIVE;
    return true;
}
/* each row drawn, then the user's picture against its box's right, two pixels in, as high as the row less four; left of
   the scroll bar while some are hidden, and with the rows as they scroll, inside the list */
static void render_users(App* app) {
    GuiUsersBoxes u;
    const UiList* l;
    size_t i, n;
    int s = app->scale, side = 0;
    users_boxes(app, &u);
    over_box(app, u.box, snx_tr(SAY_USERS_TITLE));
    ui_users_draw(app, u.rects, s, u.lists);
    l = &u.lists[UI_USERS_ROWS];
    n = ui_users_shown(u.rects, s, UI_USERS_ROWS) + (l->shift > 0);
    if (l->shift) ui_clip(app, &u.rects[UI_USERS_ROWS]);
    for (i = 0; i < n && l->first + i < (size_t)g_user_count; ++i) {
        int user = (int)(l->first + i);
        GuiRect b = ui_row_box(app, row_drawn(app, ui_users_row, ui_users_shown, u.rects, UI_USERS_ROWS, l, i));
        side = b.h - 4 * s;
        if (side <= 0) continue;
        if (!app->users_pictures[user] || app->users_side != side) {
            uint32_t size;
            const unsigned char* jpeg = snx_users_picture(&g_users[user], &size);
            bool failed;
            SDL_DestroyTexture(app->users_pictures[user]);
            app->users_pictures[user] = picture_texture(app, jpeg, size, side, &failed);
        }
        if (app->users_pictures[user]) {
            SDL_FRect at = {(float)(b.x + b.w - 2 * s - side), (float)(b.y + 2 * s), (float)side, (float)side};
            SDL_RenderTexture(app->renderer, app->users_pictures[user], NULL, &at);
        }
    }
    if (l->shift) ui_clip(app, NULL);
    app->users_side = side;
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
/* a row under the pointer takes the focus, and a click picks its user */
void ui_users_rows_row_action_cb(App* app, size_t row, uint32_t hit) {
    app->users_focus = (int)row;
    if (hit & UI_HIT_CLICK) users_pick(app, (int)row + 1);
}
/* whether x, y is in r */
static bool in_rect(GuiRect r, float x, float y) {
    return x >= (float)r.x && x < (float)(r.x + r.w) && y >= (float)r.y && y < (float)(r.y + r.h);
}
/* the pointer on it: a row's (ui_users_rows_row_action_cb); a click off the box closes it */
static void users_pointer(App* app, float x, float y, bool click) {
    GuiUsersBoxes u;
    int item;
    size_t row;
    uint32_t hit;
    users_boxes(app, &u);
    hit = ui_users_hit(app, u.rects, app->scale, u.lists, (int)x, (int)y, click ? UI_HIT_CLICK : 0, &item, &row);
    if (hit)
        ui_users_act(app, item, row, hit);
    else if (click && !in_rect(u.box, x, y))
        users_close(app);
}
/* ---- the box of a game's updates and DLCs (gui_game_addons), mods (gui_game_mods) and cheats (gui_game_cheats),
   which opening the game shows, as the game's addons=, mods_off= and cheats_off= have them picked: its updates as a
   choice of one (each version installed, newest first, or No update), its DLCs, mods and cheats a box to check each
   by, Add Mod and Add Cheats, each with its folder's Open; then Save, which keeps them in addons=, mods_off= and
   cheats_off=, and Play, which keeps them and plays the game with them, from the picked update's program, else its
   own ---- */
static void addons_folders_free(App* app);
static void addons_close(App* app) {
    addons_folders_free(app);
    gui_games_free(&app->addons);
    free(app->addons_game);
    free(app->addons_title);
    free(app->addons_checked);
    snx_free_strings(app->mods, app->mods_count);
    free(app->mods_folder);
    free(app->mods_on);
    snx_free_strings(app->cheats, app->cheats_count);
    free(app->cheats_folder);
    free(app->cheats_on);
    app->addons_game = app->addons_title = app->mods_folder = app->cheats_folder = NULL;
    app->addons_checked = app->mods_on = app->cheats_on = NULL;
    app->cheats = NULL;
    app->cheats_count = 0;
    app->mods = NULL;
    app->mods_count = 0;
    app->addons_shown = false;
}
/* folder as a URL the system's file manager opens, owned: file://, on iOS shareddocuments:// (the Files app's), its
   bytes other than letters, digits and -._~/: written %XX, a \\ as / */
static char* file_url(const char* folder) {
    static const char hex[] = "0123456789ABCDEF";
#ifdef SDL_PLATFORM_IOS
    static const char scheme[] = "shareddocuments://";
#else
    static const char scheme[] = "file://";
#endif
    size_t n = strlen(folder), i, at = 0;
    char* url = (char*)malloc(sizeof scheme + 1 + 3 * n + 1);
    if (!url) return NULL;
    memcpy(url, scheme, sizeof scheme - 1);
    at = sizeof scheme - 1;
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
/* folder opened in the system's file manager (Android's and iOS's Files app), made first when it isn't there:
   app->message says where it is, or why it could not be opened; whether it opened */
static bool show_folder(App* app, const char* folder) {
    char why[256], *url;
    bool opened;
    if (snx_mkdirs(folder)) {
        snprintf(app->message, sizeof app->message, snx_tr("Cannot make %s: %s"), folder,
                 snx_os_error(why, sizeof why));
        return false;
    }
#ifdef SDL_PLATFORM_ANDROID /* a file:// URL is no app's to open there: the Files app through MainActivity */
    opened = gui_android_open_folder(folder, why, sizeof why);
    if (opened)
        snprintf(app->message, sizeof app->message, snx_tr("Opened %s"), folder);
    else
        snprintf(app->message, sizeof app->message, snx_tr("Could not open %s: %s"), folder, why);
    return opened;
#endif
    url = file_url(folder);
    opened = url && (app->open_url ? app->open_url : SDL_OpenURL)(url);
    if (opened)
        snprintf(app->message, sizeof app->message, snx_tr("Opened %s"), folder);
    else
        snprintf(app->message, sizeof app->message, snx_tr("Could not open %s: %s"), folder,
                 url ? SDL_GetError() : snx_tr("out of memory"));
    free(url);
    return opened;
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
/* whether game, a program's path, keeps save states: a recompiled game's, not a dump's or a CIA's (states_focus's) */
static bool states_kept(const char* game) { return !gui_is_cia(game) && !gui_is_dump(game); }
/* the box's rows: the updates and No update (with any update), the DLCs, the mods, the cheats, then addons_tail's */
enum {
    ROW_NO_UPDATE = -1,
    ROW_SAVE = -2,
    ROW_PLAY = -3,
    ROW_MODS_FOLDER = -4,
    ROW_CHEATS_FOLDER = -5,
    ROW_ADD_MOD = -6,
    ROW_ADD_CHEATS = -7,
    ROW_OPEN_FOLDER = -8,
    ROW_CLEAN_CACHE = -9,
    ROW_STATES = -10
};
/* the rows after the cheats: Add Mod and Add Cheats, each with its folder's Open after it, Open Folder (the game's other
   folders), Save States (S's box, the way a finger has to it), Clean Cache, Save and Play; on every system, a phone's
   Files app showing the folders */
static const int addons_tail[] = {ROW_ADD_MOD, ROW_MODS_FOLDER, ROW_ADD_CHEATS, ROW_CHEATS_FOLDER, ROW_OPEN_FOLDER,
                                  ROW_STATES,  ROW_CLEAN_CACHE, ROW_SAVE,       ROW_PLAY};
#define ADDONS_TAIL ((int)SDL_arraysize(addons_tail))
/* the box's rows; on Open Folder's page, a folder of the game's each */
static int addons_rows(const App* app) {
    if (app->addons_folders_shown) return GUI_GAME_FOLDERS;
    return (int)(app->addons.count + app->mods_count + app->cheats_count) + (addons_updates(app) ? 1 : 0) + ADDONS_TAIL;
}
/* what row is: its add-on's index in addons, a mod's (mods' index past addons' count), a cheat's (cheats' past the
   mods'), ROW_NO_UPDATE, or one of addons_tail */
static int addons_row(const App* app, int row) {
    int updates = (int)addons_updates(app), rows = addons_rows(app);
    if (row >= rows - ADDONS_TAIL) return addons_tail[row - (rows - ADDONS_TAIL)];
    if (row < updates) return row;
    if (updates && row == updates) return ROW_NO_UPDATE;
    return row - (updates ? 1 : 0);
}
/* a mod's row's item (addons_row): its index in mods, else -1 */
static int addons_mod(const App* app, int item) {
    int mod = item - (int)app->addons.count;
    return mod >= 0 && mod < (int)app->mods_count ? mod : -1;
}
/* a cheat's row's item (addons_row): its index in cheats, else -1 */
static int addons_cheat(const App* app, int item) {
    int cheat = item - (int)(app->addons.count + app->mods_count);
    return item >= 0 && cheat >= 0 ? cheat : -1;
}
/* a cheat cheats_off= cannot name (a | in it), which runs whatever its box says */
static bool cheat_kept(const App* app, int cheat) { return strchr(app->cheats[cheat], SNX_CHEATS_SEPARATOR) != NULL; }
/* Open Folder's rows, GUI_FOLDER_*'s order */
static const char* const game_folder_names[GUI_GAME_FOLDERS] = {SNX_TR_NOOP("Dump"),  SNX_TR_NOOP("Game"),
                                                                SNX_TR_NOOP("Saves"), SNX_TR_NOOP("Settings"),
                                                                SNX_TR_NOOP("Logs"),  SNX_TR_NOOP("Caches")};
static void addons_folders_free(App* app) {
    for (int i = 0; i < GUI_GAME_FOLDERS; ++i) {
        free(app->addons_folders[i]);
        app->addons_folders[i] = NULL;
    }
    app->addons_folders_shown = false;
}
/* why Open Folder's folder i cannot be opened now, in out (it is then greyed): not known, not there, or where the
   system's file manager does not reach; "" when it can */
static void addons_folder_off(const App* app, int i, char* out, size_t size) {
    const char* path = app->addons_folders[i];
    out[0] = 0;
    if (!path)
        snprintf(out, size, "%s", snx_tr("its dump is not known: the game was made before the recompile named it"));
    else if (!snx_is_dir(path))
        snprintf(out, size, snx_tr("%s is not there yet"), path);
#ifdef SDL_PLATFORM_IOS /* the app's Library, where its settings, state and caches are, is not in it */
    else if (!app->data_dir || !snx_path_within(path, app->data_dir))
        snprintf(out, size, snx_tr("%s is out of the Files app's reach, which shows the app's Documents alone"), path);
#endif
}
/* Open Folder: the game's folders as they are now, on its page, the box's focus kept to go back to */
static void addons_folders_open(App* app) {
    char *folder, *name, *base, *data, *dump;
    int recorded = 0;
    addons_folders_free(app);
    addons_where(app, &folder, &name);
    base = snx_path_join(folder, name);
    data = snx_format("%s_data", base);
    dump = snx_game_source(folder, name, &recorded);
    app->addons_folders[GUI_FOLDER_DUMP] = recorded && dump ? snx_dirname(dump) : NULL;
    app->addons_folders[GUI_FOLDER_GAME] = snx_strdup(folder);
    app->addons_folders[GUI_FOLDER_SAVES] = snx_path_join(data, "save");
    app->addons_folders[GUI_FOLDER_SETTINGS] = gui_game_kind_dir(folder, name, SNX_USER_CONFIG);
    app->addons_folders[GUI_FOLDER_LOGS] = gui_game_kind_dir(folder, name, SNX_USER_STATE);
    app->addons_folders[GUI_FOLDER_CACHES] = gui_game_kind_dir(folder, name, SNX_USER_CACHE);
    free(dump);
    free(data);
    free(base);
    free(folder);
    free(name);
    app->addons_back = app->addons_focus;
    app->addons_focus = 0;
    app->addons_folders_shown = true;
}
/* Open Folder's page left for the box's rows, as they were */
static void addons_folders_close(App* app) {
    addons_folders_free(app);
    app->addons_focus = SDL_min(app->addons_back, addons_rows(app) - 1);
}
/* folder i of Open Folder's page opened, or why it cannot be */
static void addons_folder_press(App* app, int i) {
    char why[1024];
    addons_folder_off(app, i, why, sizeof why);
    if (why[0])
        snprintf(app->message, sizeof app->message, "%s: %s", snx_tr(game_folder_names[i]), why);
    else
        show_folder(app, app->addons_folders[i]);
}
/* ---- Clean Cache: the game's shader cache (its cache folder's shader_cache/: the pipeline cache, the translated
   shaders and those prepared ahead of time) and its code cache (<name>.jitcache), deleted when asked, each with its
   size. The game makes both again as it runs; its saves, settings and dump stay ---- */
/* the bytes of the files under path, a folder whole, links not followed */
static uint64_t tree_bytes(const char* path) {
    SDL_PathInfo info;
    char** names = NULL;
    size_t count = 0, i;
    uint64_t total = 0;
    if (snx_is_link(path) || !SDL_GetPathInfo(path, &info)) return 0;
    if (info.type == SDL_PATHTYPE_FILE) return info.size;
    if (info.type != SDL_PATHTYPE_DIRECTORY || snx_list_dir(path, &names, &count)) return 0;
    for (i = 0; i < count; ++i) {
        char* child = snx_path_join(path, names[i]);
        if (child) total += tree_bytes(child);
        free(child);
    }
    snx_free_strings(names, count);
    return total;
}
/* the box's game's two caches, owned: [0] the shader cache's folder, [1] the code cache's file */
static void game_caches(const App* app, char* caches[2]) {
    char *folder, *name, *dir;
    addons_where(app, &folder, &name);
    dir = gui_game_kind_dir(folder, name, SNX_USER_CACHE);
    caches[0] = dir ? snx_path_join(dir, "shader_cache") : NULL;
    caches[1] = gui_aot_cache_path(folder, name);
    free(dir);
    free(folder);
    free(name);
}
/* whether the box's game has a cache to delete */
static bool caches_there(const App* app) {
    char* caches[2];
    bool any;
    game_caches(app, caches);
    any = (caches[0] && snx_path_exists(caches[0])) || (caches[1] && snx_path_exists(caches[1]));
    free(caches[0]);
    free(caches[1]);
    return any;
}
/* Clean Cache pressed: what it deletes asked first (GUI_ASK_CACHE), each with its size; with none, why */
static void caches_ask(App* app) {
    char *caches[2], shader[32], code[32];
    bool there[2];
    game_caches(app, caches);
    for (int i = 0; i < 2; ++i) there[i] = caches[i] && snx_path_exists(caches[i]);
    if (!there[0] && !there[1])
        snprintf(app->message, sizeof app->message, snx_tr("%s has no cache to delete: no %s, no %s"),
                 app->addons_title, caches[0] ? caches[0] : "shader_cache/", caches[1] ? caches[1] : ".jitcache");
    else {
        gui_size_text(shader, sizeof shader, there[0] ? tree_bytes(caches[0]) : 0);
        gui_size_text(code, sizeof code, there[1] ? tree_bytes(caches[1]) : 0);
        snprintf(app->ask_line, sizeof app->ask_line,
                 snx_tr("Delete the caches of %s? The shader cache, %s (%s), and the code cache, %s (%s). The game "
                        "makes them again as it runs; its saves, settings and dump stay."),
                 app->addons_title, there[0] ? shader : snx_tr("none"), caches[0] ? caches[0] : "shader_cache/",
                 there[1] ? code : snx_tr("none"), caches[1] ? caches[1] : ".jitcache");
        gui_confirm_open(&app->confirm, GUI_ASK_CACHE, false);
    }
    free(caches[0]);
    free(caches[1]);
}
/* the deletion taken: each cache gone, the message saying what went, or what could not and why */
static void caches_delete(App* app) {
    char *caches[2], why[256], size[32];
    uint64_t bytes = 0;
    game_caches(app, caches);
    app->message[0] = 0;
    for (int i = 0; i < 2 && !app->message[0]; ++i) {
        uint64_t these;
        if (!caches[i] || !snx_path_exists(caches[i])) continue;
        these = tree_bytes(caches[i]);
        if (snx_remove_tree(caches[i]))
            snprintf(app->message, sizeof app->message, snx_tr("Could not delete %s: %s"), caches[i],
                     snx_os_error(why, sizeof why));
        else
            bytes += these;
    }
    if (!app->message[0]) {
        gui_size_text(size, sizeof size, bytes);
        snprintf(app->message, sizeof app->message, snx_tr("Deleted the caches of %s: %s"), app->addons_title, size);
    }
    free(caches[0]);
    free(caches[1]);
}
/* the box of game, shown as title: with always (M) even when it has nothing beside it, its Add rows to give it some */
static void addons_open(App* app, const char* game, const char* title, bool always) {
    char *folder, *name, *ini, *text;
    SnxAddon* list;
    size_t count, i, j;
    addons_close(app);
    gui_game_addons(game, &app->addons);
    app->mods_count = gui_game_mods(game, &app->mods_folder, &app->mods);
    app->cheats_count = gui_game_cheats(game, &app->cheats_folder, &app->cheats);
    if (!always && !app->addons.count && !app->mods_count && !app->cheats_count) {
        addons_close(app);
        return;
    }
    app->addons_game = snx_strdup(game);
    app->addons_title = snx_strdup(title);
    app->addons_debug = game_debug(app, game, 1, NULL, 0) > 0;
    app->addons_checked = (bool*)calloc(app->addons.count + 1, sizeof *app->addons_checked);
    app->mods_on = (bool*)calloc(app->mods_count + 1, sizeof *app->mods_on);
    app->cheats_on = (bool*)calloc(app->cheats_count + 1, sizeof *app->cheats_on);
    list = (SnxAddon*)malloc(SNX_ADDONS_MAX * sizeof *list);
    if (!app->addons_checked || !app->mods_on || !app->cheats_on || !list) {
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
    text = snx_ini_value(ini, "cheats_off");
    for (i = 0; i < app->cheats_count; ++i) app->cheats_on[i] = !snx_cheat_off(text, app->cheats[i]);
    free(text);
    free(ini);
    free(folder);
    free(name);
    free(list);
    app->addons_recompile = app->recompile;
    app->addons_focus = 0;
    app->addons_shown = true;
}
/* the icon of the box's row: its actions'; none for a choice, which its box or circle marks, nor on Open Folder's
   page, whose title says what its rows do */
static unsigned addons_icon(const App* app, int row) {
    int item = app->addons_folders_shown ? ROW_NO_UPDATE : addons_row(app, row);
    return item == ROW_ADD_MOD                                                               ? GUI_ICON_PACKAGE_PLUS
           : item == ROW_MODS_FOLDER || item == ROW_CHEATS_FOLDER || item == ROW_OPEN_FOLDER ? GUI_ICON_FOLDER_OPEN
           : item == ROW_CLEAN_CACHE                                                         ? GUI_ICON_ERASER
           : item == ROW_ADD_CHEATS                                                          ? GUI_ICON_FILE_PLUS
           : item == ROW_SAVE || item == ROW_STATES                                          ? GUI_ICON_SAVE
           : item == ROW_PLAY && app->addons_recompile                                       ? GUI_ICON_HAMMER
           : item == ROW_PLAY                                                                ? GUI_ICON_PLAY
                                                                                             : GUI_ICON_NONE;
}
static void addons_label(const App* app, int row, char* label, size_t size) {
    int item;
    size_t i;
    bool none = true;
    char what[256]; /* a mod's or a cheat's name with its kind, as an update's and a DLC's are */
    if (app->addons_folders_shown) { /* Open Folder's page */
        snprintf(label, size, "%s%s", app->theme.marker, snx_tr(game_folder_names[row]));
        return;
    }
    item = addons_row(app, row);
    for (i = 0; i < addons_updates(app); ++i) none = none && !app->addons_checked[i];
    if (addons_mod(app, item) >= 0) {
        snprintf(what, sizeof what, snx_tr("Mod %s"), app->mods[addons_mod(app, item)]);
        snprintf(label, size, "%s%s %s", app->theme.marker, app->mods_on[addons_mod(app, item)] ? "[x]" : "[ ]", what);
    } else if (addons_cheat(app, item) >= 0) {
        snprintf(what, sizeof what, snx_tr("Cheat %s"), app->cheats[addons_cheat(app, item)]);
        snprintf(label, size, "%s%s %s", app->theme.marker,
                 app->cheats_on[addons_cheat(app, item)] || cheat_kept(app, addons_cheat(app, item)) ? "[x]" : "[ ]",
                 what);
    } else if (item >= 0)
        snprintf(label, size, "%s%s %s", app->theme.marker,
                 addon_kind(app, (size_t)item) == SNX_ADDON_UPDATE ? (app->addons_checked[item] ? "(*)" : "( )")
                                                                   : (app->addons_checked[item] ? "[x]" : "[ ]"),
                 app->addons.shown[item]);
    else if (item == ROW_NO_UPDATE)
        snprintf(label, size, "%s%s %s", app->theme.marker, none ? "(*)" : "( )", snx_tr("No update"));
    else if (item == ROW_ADD_MOD)
        snprintf(label, size, "%s%s", app->theme.marker, snx_tr("Add Mod"));
    else if (item == ROW_MODS_FOLDER)
        snprintf(label, size, "%s%s", app->theme.marker, snx_tr("Open Mods Folder"));
    else if (item == ROW_ADD_CHEATS)
        snprintf(label, size, "%s%s", app->theme.marker, snx_tr("Add Cheats"));
    else if (item == ROW_CHEATS_FOLDER)
        snprintf(label, size, "%s%s", app->theme.marker, snx_tr("Open Cheats Folder"));
    else if (item == ROW_OPEN_FOLDER)
        snprintf(label, size, "%s%s", app->theme.marker, snx_tr("Open Folder"));
    else if (item == ROW_CLEAN_CACHE)
        snprintf(label, size, "%s%s", app->theme.marker, snx_tr("Clean Cache"));
    else if (item == ROW_STATES)
        snprintf(label, size, "%s%s", app->theme.marker, snx_tr("Save States"));
    else if (item == ROW_SAVE)
        snprintf(label, size, "%s%s", app->theme.marker, snx_tr("Save"));
    else
        snprintf(label, size, "%s%s%s", app->theme.marker, app->addons_recompile ? snx_tr("Recompile") : snx_tr("Play"),
                 app->addons_debug && !app->addons_recompile ? snx_tr("  (Debug on)") : "");
}
/* the box over the screen, as wide as its title or the longest row; a row each, ui.txt's scene Addons, the focused
   one among those shown (the box kept on the screen) */
typedef struct {
    GuiRect box;
    GuiRect rects[UI_ADDONS_ITEMS];
    UiList lists[UI_ADDONS_ITEMS]; /* the rows' at UI_ADDONS_ROWS: row i is addons_row's i */
} GuiAddonsBoxes;
static void addons_boxes(App* app, GuiAddonsBoxes* d) {
    int i, s = app->scale, count = addons_rows(app);
    int longest = SDL_max(16, (int)SDL_utf8strlen(app->addons_title) + 2 +
                                  (app->addons_folders_shown ? 2 + gui_text_cols(snx_tr("Open Folder")) : 0));
    char label[160];
    for (i = 0; i < count; ++i) {
        addons_label(app, i, label, sizeof label);
        longest = SDL_max(longest, (int)SDL_utf8strlen(label) + 4 + (addons_icon(app, i) ? 3 : 0));
    }
    ui_addons_layout(
        over_rows(app, longest, rows_h(ui_addons_layout, ui_addons_row, d->rects, UI_ADDONS_ROWS, s, count), &d->box),
        s, d->rects);
    memset(d->lists, 0, sizeof d->lists);
    d->lists[UI_ADDONS_ROWS].focus = (size_t)app->addons_focus;
    list_follow(app, GUI_LIST_ADDONS, ui_addons_follow, ui_addons_row, d->rects, UI_ADDONS_ROWS, (size_t)count,
                &d->lists[UI_ADDONS_ROWS]);
}
/* Addons' row: an update's choice, a DLC's or a mod's box, Open Mods Folder, Save or Play (ui_draw_row puts the
   marker) */
bool ui_addons_rows_get_row_info(App* app, size_t row, int width, UiRowInfo* info) {
    char why[1024];
    (void)width; /* a long name slides, as the menu's rows do */
    if (row >= (size_t)addons_rows(app)) return false;
    addons_label(app, (int)row, app->row_text, sizeof app->row_text);
    info->label = app->row_text + strlen(app->theme.marker);
    info->icon = addons_icon(app, (int)row);
    /* greyed: a folder Open Folder cannot open, Clean Cache with none, Save States of a dump (which has none) */
    if (app->addons_folders_shown)
        addons_folder_off(app, (int)row, why, sizeof why);
    else
        why[0] = (addons_row(app, (int)row) == ROW_CLEAN_CACHE && !caches_there(app)) ||
                 (addons_row(app, (int)row) == ROW_STATES && !states_kept(app->addons_game));
    if (why[0]) info->attr |= UI_ATTR_OFF;
    return true;
}
static void render_addons(App* app) {
    GuiAddonsBoxes d;
    char title[512];
    addons_boxes(app, &d);
    if (app->addons_folders_shown)
        snprintf(title, sizeof title, "%s: %s", app->addons_title, snx_tr("Open Folder"));
    else
        snprintf(title, sizeof title, "%s", app->addons_title);
    over_box(app, d.box, title);
    ui_addons_draw(app, d.rects, app->scale, d.lists);
}
/* what is picked kept as the game's addons=, mods_off= and cheats_off=: 1, else 0 with app->message saying why. The
   picked
   update's program, absolute and owned, in *program when it is not NULL (NULL with no update picked) */
static int addons_save(App* app, char** program) {
    char *folder, *name, *ini, *text, *off = snx_strdup(""), *cheats_off = snx_strdup("");
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
    for (i = 0; i < app->cheats_count; ++i)
        if (!app->cheats_on[i] && !cheat_kept(app, (int)i)) {
            char* more = snx_format("%s%s%s", cheats_off, cheats_off[0] ? "|" : "", app->cheats[i]);
            free(cheats_off);
            cheats_off = more;
        }
    ini = snx_game_ini(folder, name);
    text = list ? snx_addons_text(list, count) : NULL;
    saved = text && !snx_ini_record(ini, "addons", text) && !snx_ini_record(ini, "mods_off", off) &&
            !snx_ini_record(ini, "cheats_off", cheats_off);
    if (!saved) {
        snprintf(app->message, sizeof app->message, snx_tr("Not saved: %s could not be written"), ini);
        say_failure(app, app->message);
    }
    free(off);
    free(cheats_off);
    free(text);
    free(ini);
    free(list);
    free(folder);
    free(name);
    return saved;
}
/* the row of item, one of addons_tail */
static int addons_row_of(const App* app, int item) {
    int i = 0;
    while (i < ADDONS_TAIL - 1 && addons_tail[i] != item) ++i;
    return addons_rows(app) - ADDONS_TAIL + i;
}
/* the mods, or with row ROW_ADD_CHEATS the cheats, listed again once one was added: each one listed before as the
   box had it, a new one checked; the focus on row, the Add row, and the game's count on Games (its +N) what the box
   has now */
static void addons_relist(App* app, int row) {
    bool cheats = row == ROW_ADD_CHEATS;
    char **names = NULL, *folder = NULL, ***list = cheats ? &app->cheats : &app->mods;
    size_t n = cheats ? gui_game_cheats(app->addons_game, &folder, &names)
                      : gui_game_mods(app->addons_game, &folder, &names),
           *count = cheats ? &app->cheats_count : &app->mods_count, i, j;
    bool *on = (bool*)calloc(n + 1, sizeof *on), **checked = cheats ? &app->cheats_on : &app->mods_on;
    if (!on) {
        snx_free_strings(names, n);
        snprintf(app->message, sizeof app->message, snx_tr("Not enough memory to list %s."), folder);
        free(folder);
        return;
    }
    free(folder);
    for (i = 0; i < n; ++i) {
        on[i] = true;
        for (j = 0; j < *count; ++j)
            if (!strcmp(names[i], (*list)[j])) on[i] = (*checked)[j];
    }
    snx_free_strings(*list, *count);
    free(*checked);
    *list = names;
    *count = n;
    *checked = on;
    app->addons_focus = addons_row_of(app, row);
    for (i = 0; i < app->games.count; ++i)
        if (app->games.paths[i] && snx_same_file(app->games.paths[i], app->addons_game))
            app->games.addons[i] = app->addons.count + app->mods_count + app->cheats_count;
}
/* the last part of path, owned: its name, any separator after it left out; "" for a root */
static char* path_leaf(const char* path) {
    size_t end = strlen(path), start;
    char* leaf;
    while (end && (path[end - 1] == '/' || path[end - 1] == '\\')) --end;
    for (start = end; start && path[start - 1] != '/' && path[start - 1] != '\\'; --start) continue;
    leaf = (char*)snx_alloc(end - start + 1);
    memcpy(leaf, path + start, end - start);
    leaf[end - start] = 0;
    return leaf;
}
/* Add Mod's folder, as the picker gave it (a path, or a phone's URI): copied whole into the game's mods folder under
   its own name, the original left as it is, and checked in the box. Else app->message says why not, with the path or
   the name it is about */
static void mod_add(App* app, const char* picked) {
    char *path = gui_document_path(picked), *name, *to, *exefs, *romfs, why[1024];
    if (!path) { /* Android's tree of an app or a cloud, which no path reaches */
        snprintf(app->message, sizeof app->message, snx_tr("Not added: %s is not a folder of this device's storage"),
                 picked);
        return;
    }
    name = path_leaf(path);
    to = snx_path_join(app->mods_folder, name);
    exefs = snx_path_join(path, "exefs");
    romfs = snx_path_join(path, "romfs");
    if (!snx_is_dir(path))
        snprintf(app->message, sizeof app->message, snx_tr("Not added: %s is not a folder"), path);
    else if (!name[0])
        snprintf(app->message, sizeof app->message, snx_tr("Not added: %s has no name to give the mod"), path);
    else if (name[0] == '.')
        snprintf(app->message, sizeof app->message,
                 snx_tr("Not added: the mod would be named %s, and the mods list hides a name that starts with a dot"),
                 name);
    else if (snx_path_within(path, app->mods_folder))
        snprintf(app->message, sizeof app->message, snx_tr("Not added: %s is in the mods folder already, %s"), path,
                 app->mods_folder);
    else if (snx_path_within(app->mods_folder, path))
        snprintf(app->message, sizeof app->message,
                 snx_tr("Not added: %s holds the mods folder %s, which it would copy into itself"), path,
                 app->mods_folder);
    else if (!snx_is_dir(exefs) && !snx_is_dir(romfs))
        snprintf(app->message, sizeof app->message,
                 snx_tr("Not added: %s has no exefs/ or romfs/ folder, which a mod is made of"), path);
    else if (snx_path_exists(to))
        snprintf(app->message, sizeof app->message, snx_tr("Not added: the mods folder has %s already: %s"), name, to);
    else if (snx_mkdirs(app->mods_folder)) {
        snprintf(app->message, sizeof app->message, snx_tr("Not added: cannot make %s: %s"), app->mods_folder,
                 snx_os_error(why, sizeof why));
        say_failure(app, app->message);
    } else if (!gui_copy_mod(path, to, why, sizeof why)) {
        snprintf(app->message, sizeof app->message, snx_tr("Not added: %s"), why);
        say_failure(app, app->message);
    } else {
        addons_relist(app, ROW_ADD_MOD);
        snprintf(app->message, sizeof app->message, snx_tr("Mod %s added: %s"), name, to);
    }
    free(romfs);
    free(exefs);
    free(to);
    free(name);
    free(path);
}
#define SNX_GUI_CHEATS_MAX (1 << 20) /* a cheats file is text of a few kilobytes: past this it is not one */
/* the 16 hex digits of the build ID of the module at path (an NSO, or an NRO), in capitals as the runtime names a cheats
   file by its main's, into id: 1; else 0, no such file or not a module */
static int module_build_id(const char* path, char id[17]) {
    unsigned char head[0x48];
    FILE* f = snx_fopen(path, "rb");
    size_t got = f ? fread(head, 1, sizeof head, f) : 0;
    int i;
    if (f) fclose(f);
    if (got != sizeof head || (memcmp(head, "NSO0", 4) && memcmp(head + 0x10, "NRO0", 4))) return 0;
    for (i = 0; i < 8; ++i) snprintf(id + 2 * i, 3, "%02X", head[0x40 + i]);
    return 1;
}
/* the build ID of the main module the game plays with as the box has it, which names its cheats file
   (cheats/<build ID>.txt, nx/runtime/cheats.c): a played mod's exefs/main, the last one laid; else the picked update's,
   from its extraction (one read from its dump has none here); else the game's own, in its data folder's exefs/ or at
   its root, as older versions put it, an NRO's as main.nro. 16 hex digits into id: 1; else 0. *file, owned, is the
   module read, or the first one looked for */
static int addons_build_id(const App* app, char id[17], char** file) {
    static const char* const own[] = {"exefs/main", "exefs/main.nro", "main", "main.nro"};
    char *off = snx_strdup(""), **names = NULL, **paths = NULL, *folder, *name, *leaf, *data = NULL;
    size_t n, i;
    int found = 0;
    *file = NULL;
    for (i = 0; i < app->mods_count; ++i) /* mods_off= as the box has it */
        if (!app->mods_on[i]) {
            char* more = snx_format("%s%s%s", off, off[0] ? "/" : "", app->mods[i]);
            free(off);
            off = more;
        }
    n = snx_mods_exefs(app->mods_folder, off, &names, &paths);
    for (i = 0; i < n && !*file; ++i)
        if (!strcmp(names[i], "main")) *file = snx_strdup(paths[i]);
    snx_free_strings(names, n);
    snx_free_strings(paths, n);
    free(off);
    if (*file) return module_build_id(*file, id);
    addons_where(app, &folder, &name);
    for (i = 0; i < addons_updates(app) && !data; ++i)
        if (app->addons_checked[i]) {
            leaf = snx_format("%s_data", app->addons.names[i]);
            data = snx_path_join(folder, leaf);
            free(leaf);
            *file = snx_path_join(data, own[0]); /* the update's main, not the game's: it runs its own */
            found = module_build_id(*file, id);
        }
    if (!data) {
        leaf = snx_format("%s_data", name);
        data = snx_path_join(folder, leaf);
        free(leaf);
        for (i = 0; i < SDL_arraysize(own) && !found; ++i) {
            char* at = snx_path_join(data, own[i]);
            found = module_build_id(at, id);
            if (found || !*file) {
                free(*file);
                *file = at;
            } else
                free(at);
        }
    }
    free(data);
    free(folder);
    free(name);
    return found;
}
/* the cheats in text, a cheats file's: its [name] (or {name}) lines, as snx_cheats_list reads them */
static size_t cheats_in(const char* text) {
    char *copy = snx_strdup(text), *line, *next, name[64];
    size_t n = 0;
    for (line = copy; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        n += snx_cheat_name(line, name, sizeof name) != 0;
    }
    free(copy);
    return n;
}
/* Add Cheats' file, as the picker gave it (a path, or a phone's URI): a .txt with a cheat at least, copied into the
   game's cheats folder by the name the runtime reads, <build ID>.txt. That is its own name when it is one (16 hex
   digits, in capitals), else the build ID of the main module the game plays with (addons_build_id); a file named by
   another build ID than that one is refused, and a file of that name there is never added to. Its cheats checked in
   the box; else app->message says why not, with the file, the name or the build IDs it is about */
static void cheats_add(App* app, const char* picked) {
    char *path = gui_document_path(picked), *leaf, *text = NULL, *module = NULL, *to = NULL, *staged = NULL;
    char name[24], given[17], known[17], why[256];
    size_t size = 0, count = 0, i;
    bool hex;
    int has_id = 0;
#ifdef SDL_PLATFORM_ANDROID
    if (!path) path = gui_android_document_file(picked); /* a download by number: the file under it */
#endif
    if (!path) {
        snprintf(app->message, sizeof app->message, snx_tr("Not added: %s is not a file of this device's storage"),
                 picked);
        return;
    }
    leaf = path_leaf(path);
    hex = strlen(leaf) == 20 && !SDL_strcasecmp(leaf + 16, ".txt");
    for (i = 0; hex && i < 16; ++i) hex = SDL_isxdigit((unsigned char)leaf[i]) != 0;
    for (i = 0; hex && i < 16; ++i) given[i] = (char)SDL_toupper((unsigned char)leaf[i]);
    given[hex ? 16 : 0] = 0;
    if (!snx_is_file(path))
        snprintf(app->message, sizeof app->message, snx_tr("Not added: %s is not a file"), path);
    else if (strlen(leaf) < 4 || SDL_strcasecmp(leaf + strlen(leaf) - 4, ".txt"))
        snprintf(app->message, sizeof app->message, snx_tr("Not added: %s is not a .txt file, which cheats are"), path);
    else if (snx_file_size(path) > SNX_GUI_CHEATS_MAX) /* in bytes: a size rounded to 1 MB would hide it is past */
        snprintf(app->message, sizeof app->message,
                 snx_tr("Not added: %s is %llu bytes, and a cheats file is text of %d bytes (1 MiB) at most"), path,
                 (unsigned long long)snx_file_size(path), SNX_GUI_CHEATS_MAX);
    else if (!(text = (char*)snx_read_file(path, &size)))
        snprintf(app->message, sizeof app->message, snx_tr("Not added: cannot read %s: %s"), path,
                 snx_os_error(why, sizeof why));
    else if (!(count = cheats_in(text)))
        snprintf(app->message, sizeof app->message,
                 snx_tr("Not added: %s has no cheat, no line that starts with [name]"), path);
    else if (!(has_id = addons_build_id(app, known, &module)) && !hex)
        snprintf(app->message, sizeof app->message,
                 snx_tr("Not added: %s is not named <build ID>.txt, and the build ID of the main module the game plays "
                        "with cannot be read from %s"),
                 leaf, module);
    else if (hex && has_id && strcmp(given, known)) /* the runtime reads only the one of the main it runs */
        snprintf(app->message, sizeof app->message,
                 snx_tr("Not added: %s is for build ID %s, and the game plays with build ID %s: its cheats would "
                        "never run"),
                 leaf, given, known);
    else {
        snprintf(name, sizeof name, "%s.txt", hex ? given : known);
        to = snx_path_join(app->cheats_folder, name);
        {
            char* hidden = snx_format(".%s.adding", name);
            staged = snx_path_join(app->cheats_folder, hidden);
            free(hidden);
        }
        if (snx_path_exists(to))
            snprintf(app->message, sizeof app->message, snx_tr("Not added: the cheats folder has %s already: %s"), name,
                     to);
        else if (snx_mkdirs(app->cheats_folder)) {
            snprintf(app->message, sizeof app->message, snx_tr("Not added: cannot make %s: %s"), app->cheats_folder,
                     snx_os_error(why, sizeof why));
            say_failure(app, app->message);
        } else if (snx_write_file(staged, text, size) || snx_replace(staged, to)) {
            snprintf(app->message, sizeof app->message, snx_tr("Not added: cannot write %s: %s"), to,
                     snx_os_error(why, sizeof why));
            if (snx_path_exists(staged)) snx_remove_tree(staged); /* this copy's alone */
            say_failure(app, app->message);
        } else {
            addons_relist(app, ROW_ADD_CHEATS);
            if (!hex)
                snprintf(app->message, sizeof app->message,
                         snx_tr("Added %s: %zu cheats from %s, named by the build ID of the main module the game "
                                "plays with"),
                         to, count, leaf);
            else
                snprintf(app->message, sizeof app->message, snx_tr("Added %s: %zu cheats"), to, count);
        }
    }
    free(staged);
    free(to);
    free(module);
    free(text);
    free(leaf);
    free(path);
}
static void SDLCALL folder_picked(void* userdata, const char* const* files, int filter);
/* Add Mod (row) pressed: the system's folder picker; Add Cheats: its file picker, for a .txt. Its answer is an
   app->addon_picked event (addons_picked) for this box; app->message says why when it cannot open */
static void addons_pick(App* app, int row) {
    char why[sizeof app->message - 64] = "";
    bool folder = row == ROW_ADD_MOD, opened = true;
    const char* cannot = folder ? snx_tr("Add Mod: the folder picker could not open: %s")
                                : snx_tr("Add Cheats: the file picker could not open: %s");
    app->cheats_filter.name = snx_tr("Cheats");
    app->cheats_filter.pattern = "txt";
    if (!app->addon_picked) app->addon_picked = SDL_RegisterEvents(1);
    if (!app->addon_picked) {
        snprintf(app->message, sizeof app->message, cannot, SDL_GetError());
        say_failure(app, app->message);
        return;
    }
    app->addon_adding = row;
    free(app->adding_game);
    app->adding_game = snx_strdup(app->addons_game);
#ifdef SDL_PLATFORM_ANDROID /* SDL has no folder picker there: the system's own, through the app */
    if (folder)
        opened = gui_android_pick_folder(folder_picked, &app->addon_picked, why, sizeof why) != 0;
    else
        SDL_ShowOpenFileDialog(folder_picked, &app->addon_picked, app->window, &app->cheats_filter, 1, NULL, false);
#elif defined(SDL_PLATFORM_IOS) /* nor any picker there: the document picker (dist/apple), which takes no filter */
    opened = gui_apple_pick(app->window, folder, folder_picked, &app->addon_picked, why, sizeof why) != 0;
#else
    /* its answer comes as an event even when it cannot open, which says why */
    if (folder)
        SDL_ShowOpenFolderDialog(folder_picked, &app->addon_picked, app->window, NULL, false);
    else
        SDL_ShowOpenFileDialog(folder_picked, &app->addon_picked, app->window, &app->cheats_filter, 1, NULL, false);
#endif
    if (opened) return;
    app->addon_adding = 0;
    snprintf(app->message, sizeof app->message, cannot, why);
    say_failure(app, app->message);
}
/* the picker's answer: picked (NULL for none), or error when it could not open; to the box it was opened for while
   that one shows, else said not added */
static void addons_picked(App* app, const char* picked, const char* error, bool failed) {
    int row = app->addon_adding;
    bool here = row && app->addons_shown && app->adding_game && !strcmp(app->adding_game, app->addons_game),
         folder = row != ROW_ADD_CHEATS;
    app->addon_adding = 0;
    free(app->adding_game);
    app->adding_game = NULL;
    if (failed) {
        snprintf(app->message, sizeof app->message,
                 folder ? snx_tr("Add Mod: the folder picker could not open: %s")
                        : snx_tr("Add Cheats: the file picker could not open: %s"),
                 error ? error : snx_tr("no reason given"));
        say_failure(app, app->message);
    } else if (!picked)
        snprintf(app->message, sizeof app->message, "%s",
                 folder ? snx_tr("Add Mod: no folder was picked.") : snx_tr("Add Cheats: no file was picked."));
    else if (!here)
        snprintf(app->message, sizeof app->message,
                 snx_tr("Not added: %s came back after the box it was picked for closed"), picked);
    else if (folder)
        mod_add(app, picked);
    else
        cheats_add(app, picked);
}
static void launch(App* app, char* absolute, bool prepared);
static void states_open(App* app, const char* game, const char* title);
/* row pressed: an update picked (the others not: one runs at a time), No update, a DLC's, a mod's or a cheat's box
   checked or not; Add Mod or Add Cheats, the picker, whose folder or file goes into the mods or cheats folder (mod_add,
   cheats_add); Open Mods Folder or Open Cheats Folder, the folder opened; Open Folder, its page of the game's other
   folders, one opened as it is pressed there; Clean Cache, the caches' deletion asked; Save, the picks kept and the box
   closed; or Play, the picks kept and the game played (or recompiled) through the driver, from the picked update's
   program. A mod's exefs/ changes the program, which the driver recompiles before it plays */
static void addons_press(App* app, int row) {
    int item, selected = app->menu.selected;
    size_t i;
    char* path;
    bool recompile = app->recompile;
    if (app->addons_folders_shown) {
        addons_folder_press(app, row);
        return;
    }
    item = addons_row(app, row);
    if (item == ROW_OPEN_FOLDER) {
        addons_folders_open(app);
        return;
    }
    if (item == ROW_CLEAN_CACHE) {
        caches_ask(app);
        return;
    }
    if (item == ROW_STATES) { /* the box goes, since it would take the keys from the states' */
        char *game = snx_strdup(app->addons_game), *title = snx_strdup(app->addons_title);
        addons_close(app);
        if (states_kept(game))
            states_open(app, game, title);
        else
            snprintf(app->message, sizeof app->message,
                     snx_tr("%s is a dump: its recompiled game keeps the save states"), title);
        free(game);
        free(title);
        return;
    }
    if (addons_mod(app, item) >= 0) {
        app->mods_on[addons_mod(app, item)] = !app->mods_on[addons_mod(app, item)];
        return;
    }
    if (addons_cheat(app, item) >= 0) {
        int cheat = addons_cheat(app, item);
        if (cheat_kept(app, cheat)) /* cheats_off= has no way to name it */
            snprintf(app->message, sizeof app->message,
                     snx_tr("\"%s\" always runs: a | in its name, which cheats_off= puts between two"),
                     app->cheats[cheat]);
        else
            app->cheats_on[cheat] = !app->cheats_on[cheat];
        return;
    }
    if (item == ROW_ADD_MOD || item == ROW_ADD_CHEATS) {
        if (app->mods_folder && app->cheats_folder) addons_pick(app, item);
        return;
    }
    if (item == ROW_MODS_FOLDER) {
        if (app->mods_folder) show_folder(app, app->mods_folder);
        return;
    }
    if (item == ROW_CHEATS_FOLDER) {
        if (app->cheats_folder) show_folder(app, app->cheats_folder);
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
        snprintf(app->message, sizeof app->message, snx_tr("%s: its updates, DLCs, mods and cheats kept"),
                 app->addons_title);
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
    else if (key == GUI_CANCEL && app->addons_folders_shown) /* Open Folder's page: back to the box's rows */
        addons_folders_close(app);
    else if (key == GUI_CANCEL)
        addons_close(app);
}
/* a row under the pointer takes the focus, and a click presses it */
void ui_addons_rows_row_action_cb(App* app, size_t row, uint32_t hit) {
    app->addons_focus = (int)row;
    if (hit & UI_HIT_CLICK) addons_press(app, (int)row);
}
/* the pointer on it: a row's (ui_addons_rows_row_action_cb); a click off the box closes it */
static void addons_pointer(App* app, float x, float y, bool click) {
    GuiAddonsBoxes d;
    int item;
    size_t row;
    uint32_t hit;
    addons_boxes(app, &d);
    hit = ui_addons_hit(app, d.rects, app->scale, d.lists, (int)x, (int)y, click ? UI_HIT_CLICK : 0, &item, &row);
    if (hit)
        ui_addons_act(app, item, row, hit);
    else if (click && !in_rect(d.box, x, y))
        addons_close(app);
}
/* ---- the View box Games' V opens: an option a row (option_keys), its value changed at once, kept in app.ini and
   Games listed again by it ---- */
typedef struct {
    GuiRect box;
    GuiRect rects[UI_VIEW_ITEMS];
    UiList lists[UI_VIEW_ITEMS]; /* the rows' at UI_VIEW_ROWS: view_rows' */
} GuiViewBoxes;
/* the View box's rows: Games' options, then the Side panel, Settings > App's side_box, which F2 turns too */
#define GUI_VIEW_SIDE     GUI_OPTIONS
#define GUI_VIEW_BOX_ROWS (GUI_OPTIONS + 1)
/* the rows the box shows in rows, each its GUI_OPTION_* or GUI_VIEW_SIDE; how many. Icon size is the grid's: greyed
   in the list, still there (#614) */
static int view_rows(const App* app, int* rows) {
    int i, n = 0;
    (void)app;
    for (i = 0; i < GUI_VIEW_BOX_ROWS; ++i) rows[n++] = i;
    return n;
}
/* option i has nothing to change now: Icon size while View is List */
static bool option_off(const App* app, int i) { return i == GUI_OPTION_ICONS && !games_grid(app); }
#define SAY_ICONS_OFF SNX_TR_NOOP("Icon size is the grid's: View is List")
/* row i's name and its value as the View box shows them */
static const char* view_name(int i) { return i == GUI_VIEW_SIDE ? snx_tr("Side panel") : snx_tr(option_keys[i].name); }
static const char* view_value(const App* app, int i) {
    return i == GUI_VIEW_SIDE ? snx_tr(side_names[app->side_box]) : snx_tr(option_keys[i].shown[app->options[i]]);
}
/* the column the values start at: past the marker and the longest name, and two more */
static int option_column(const App* app) {
    int i, cols = 0;
    for (i = 0; i < GUI_VIEW_BOX_ROWS; ++i) cols = SDL_max(cols, gui_text_cols(view_name(i)));
    return gui_text_cols(app->theme.marker) + cols + 2;
}
/* row i as it reads: its name, then from option_column its value between the arrows Left and Right go by */
static void option_row(const App* app, int i, char* out, size_t size) {
    int pad = option_column(app) - gui_text_cols(app->theme.marker) - gui_text_cols(view_name(i));
    snprintf(out, size, "%s%s%*s< %s >", app->theme.marker, view_name(i), pad, "", view_value(app, i));
}
static void view_boxes(App* app, GuiViewBoxes* d) {
    int i, s = app->scale, longest = SDL_max(16, gui_text_cols(snx_tr("View")) + 2), rows[GUI_VIEW_BOX_ROWS],
           n = view_rows(app, rows);
    char label[160];
    for (i = 0; i < n; ++i) {
        option_row(app, rows[i], label, sizeof label);
        longest = SDL_max(longest, gui_text_cols(label) + 4);
    }
    ui_view_layout(over_rows(app, longest, rows_h(ui_view_layout, ui_view_row, d->rects, UI_VIEW_ROWS, s, n), &d->box),
                   s, d->rects);
    memset(d->lists, 0, sizeof d->lists);
    d->lists[UI_VIEW_ROWS].focus = (size_t)app->options_focus;
    list_follow(app, GUI_LIST_VIEW, ui_view_follow, ui_view_row, d->rects, UI_VIEW_ROWS, (size_t)n,
                &d->lists[UI_VIEW_ROWS]);
}
bool ui_view_rows_get_row_info(App* app, size_t row, int width, UiRowInfo* info) {
    int rows[GUI_VIEW_BOX_ROWS];
    (void)width; /* the box is as wide as its longest row */
    if (row >= (size_t)view_rows(app, rows)) return false;
    option_row(app, rows[row], app->row_text, sizeof app->row_text);
    info->label = app->row_text + strlen(app->theme.marker); /* ui_draw_row puts the marker */
    if (option_off(app, rows[row])) info->attr |= UI_ATTR_OFF;
    return true;
}
static void render_view(App* app) {
    GuiViewBoxes d;
    view_boxes(app, &d);
    over_box(app, d.box, snx_tr("View"));
    ui_view_draw(app, d.rects, app->scale, d.lists);
}
static void list_games(App* app);
/* option i a word on (step 1) or back (-1): Games listed again by it, then kept in app.ini, whose failure the message
   says (app_save) while the option holds for the session */
static void option_change(App* app, int i, int step) {
    app->options[i] = (app->options[i] + option_words(i) + step) % option_words(i);
    list_games(app); /* it starts the message again */
    app_save(app);
}
/* the box's row at a word on (step 1) or back (-1): an option (option_change), or the Side panel round its three as
   Settings > App's row goes, kept in app.ini at once; the focus kept among the rows that show after it */
static void view_change(App* app, int at, int step) {
    int rows[GUI_VIEW_BOX_ROWS], n = view_rows(app, rows), i = rows[SDL_min(at, n - 1)];
    if (option_off(app, i))
        snprintf(app->message, sizeof app->message, "%s", snx_tr(SAY_ICONS_OFF));
    else if (i != GUI_VIEW_SIDE)
        option_change(app, i, step);
    else {
        app->side_box = (app->side_box + (step < 0 ? 2 : 1)) % 3;
        layout(app);
        app_save(app);
    }
    app->options_focus = SDL_min(app->options_focus, view_rows(app, rows) - 1);
}
/* a key on the box: Up and Down move round its rows, Left, Right and Select change the focused one's value, Back
   closes it */
static void options_key(App* app, GuiKey key) {
    int rows[GUI_VIEW_BOX_ROWS], n = view_rows(app, rows);
    if (key == GUI_UP || key == GUI_DOWN)
        app->options_focus = (app->options_focus + (key == GUI_UP ? n - 1 : 1)) % n;
    else if (key == GUI_LEFT || key == GUI_RIGHT || key == GUI_SELECT)
        view_change(app, app->options_focus, key == GUI_LEFT ? -1 : 1);
    else if (key == GUI_CANCEL)
        app->options_shown = false;
}
/* a row under the pointer takes the focus, and a click changes its value */
void ui_view_rows_row_action_cb(App* app, size_t row, uint32_t hit) {
    app->options_focus = (int)row;
    if (hit & UI_HIT_CLICK) view_change(app, (int)row, 1);
}
/* the pointer on it: a row's (ui_view_rows_row_action_cb); a click off the box closes it */
static void options_pointer(App* app, float x, float y, bool click) {
    GuiViewBoxes d;
    int item;
    size_t row;
    uint32_t hit;
    view_boxes(app, &d);
    hit = ui_view_hit(app, d.rects, app->scale, d.lists, (int)x, (int)y, click ? UI_HIT_CLICK : 0, &item, &row);
    if (hit)
        ui_view_act(app, item, row, hit);
    else if (click && !in_rect(d.box, x, y))
        app->options_shown = false;
}
/* ---- the box of a game's save states (S on Games): a row each, the newest first, saying its slot, when it was saved
   and the game's play time then; the focused one's picture beside the box. Enter plays the game from it, through its
   program (the game's own or an update's) with --state_load=, and Del deletes it, asked first. A state this build
   can't load is drawn in the failure colour, and Enter says why ---- */
typedef struct GuiState {
    char *path, *program; /* the state's file, and the program it plays in */
    char label[96];       /* its slot, date and play time */
    char why[160];        /* why it can't be loaded, "" when it can */
    uint64_t created;
    uint8_t* thumb; /* thumb_w x thumb_h RGBA, NULL for none */
    int thumb_w, thumb_h;
} GuiState;
#define SAY_STATES_TITLE SNX_TR_NOOP("%s: save states")
static void states_close(App* app) {
    size_t i;
    for (i = 0; i < app->states_count; ++i) {
        free(app->states[i].path);
        free(app->states[i].program);
        free(app->states[i].thumb);
    }
    free(app->states);
    free(app->states_title);
    SDL_DestroyTexture(app->states_texture);
    app->states = NULL;
    app->states_title = NULL;
    app->states_texture = NULL;
    app->states_texture_of = -1;
    app->states_count = 0;
    app->states_shown = false;
}
static int state_newer(const void* a, const void* b) {
    const GuiState *x = (const GuiState*)a, *y = (const GuiState*)b;
    return x->created < y->created ? 1 : x->created > y->created ? -1 : strcmp(x->path, y->path);
}
/* the program a state's file names, beside the game: its path, owned, NULL when it isn't there */
static char* state_program(const char* folder, const char* program) {
    char* path = snx_path_join(folder, program);
    char* exe;
    if (!path || snx_is_file(path)) return path;
    exe = snx_format("%s.exe", path);
    free(path);
    if (exe && snx_is_file(exe)) return exe;
    free(exe);
    return NULL;
}
/* the states of game (a game's program in its folder) listed, title what it is shown as: the box open, or a message
   saying there are none */
static void states_open(App* app, const char* game, const char* title) {
    char *folder = snx_dirname(game), *dir = NULL, **names = NULL;
    const char* name;
    size_t count = 0, i;
    SnxU64 id = 0;
    states_close(app);
    for (name = folder + strlen(folder); name > folder && name[-1] != '/' && name[-1] != '\\'; --name) continue;
    dir = snx_path_join(folder, "states");
    if (snx_title_parse(name, &id) && !snx_list_dir(dir, &names, &count) && count)
        app->states = (GuiState*)calloc(count, sizeof *app->states);
    for (i = 0; app->states && i < count; ++i) {
        GuiState* g = &app->states[app->states_count];
        SnxStateHeader h;
        char program[256], slot[16], stamp[64], when[40];
        uint8_t* thumb;
        SDL_DateTime dt;
        uint64_t sec;
        /* the game's own program's, or an update's (<game>_<update>) */
        if (!snx_state_name(names[i], program, sizeof program, slot, sizeof slot) ||
            strncmp(program, name, strlen(name)) || (program[strlen(name)] && program[strlen(name)] != '_'))
            continue;
        g->path = snx_path_join(dir, names[i]);
        g->program = state_program(folder, program);
        if (!g->path || !g->program || snx_state_peek(g->path, &h, &thumb, g->why, sizeof g->why)) {
            free(g->path);
            free(g->program);
            memset(g, 0, sizeof *g);
            continue;
        }
        snx_state_stamp(g->program, stamp, sizeof stamp);
        if (snx_state_fits(&h, id, stamp, g->why, sizeof g->why)) g->why[0] = (char)SDL_toupper(g->why[0]);
        g->created = h.created;
        g->thumb = thumb;
        g->thumb_w = (int)h.thumb_w;
        g->thumb_h = (int)h.thumb_h;
        if (SDL_TimeToDateTime((SDL_Time)h.created * SDL_NS_PER_SECOND, &dt, true))
            snprintf(when, sizeof when, "%04d-%02d-%02d %02d:%02d", dt.year, dt.month, dt.day, dt.hour, dt.minute);
        else
            when[0] = 0;
        sec = h.guest_ns / 1000000000u;
        if (strcmp(slot, "auto"))
            snprintf(g->label, sizeof g->label, snx_tr("Slot %s  %s  %u:%02u:%02u"), slot, when, (unsigned)(sec / 3600),
                     (unsigned)(sec / 60 % 60), (unsigned)(sec % 60));
        else
            snprintf(g->label, sizeof g->label, snx_tr("Auto  %s  %u:%02u:%02u"), when, (unsigned)(sec / 3600),
                     (unsigned)(sec / 60 % 60), (unsigned)(sec % 60));
        app->states_count++;
    }
    snx_free_strings(names, count);
    free(dir);
    free(folder);
    if (!app->states_count) {
        states_close(app);
        snprintf(app->message, sizeof app->message,
                 snx_tr("%s has no save states: F7 saves one while it plays, the menu's Save states page too."), title);
        return;
    }
    qsort(app->states, app->states_count, sizeof *app->states, state_newer);
    app->states_title = snx_format(snx_tr(SAY_STATES_TITLE), title);
    app->states_focus = 0;
    app->states_shown = true;
}
/* S on Games: the box of the focused game's save states; else why not */
static void states_focus(App* app) {
    size_t row = app->browser.selected, head = app->browser.head;
    size_t index = app->browser.page * app->browser.page_size + row - head;
    bool recent = row && row < head && (int)row - 1 < recent_shown(app), game = row >= head && index < app->games.count;
    const GuiRecentGame* r = recent ? &app->recent.game[recent_index(app, (int)row - 1)] : NULL;
    if (r && r->driver == GUI_SWITCH && r->prepared)
        states_open(app, r->path, r->name);
    else if (game && !gui_is_cia(app->games.names[index]) && !gui_is_dump(app->games.names[index]))
        states_open(app, app->games.paths[index], app->games.shown[index]);
    else
        snprintf(app->message, sizeof app->message, "%s",
                 snx_tr("S lists the save states of a recompiled Switch game in focus."));
}
/* the box over the screen, as wide as its title or the longest row; a row each, ui.txt's scene States, the focused one
   among those shown (the box kept on the screen); the picture's place at its right, or under it, empty when neither
   has room */
typedef struct {
    GuiRect box, picture;
    GuiRect rects[UI_STATES_ITEMS];
    UiList lists[UI_STATES_ITEMS]; /* the rows' at UI_STATES_ROWS: row i is states[i] */
} GuiStatesBoxes;
static void states_boxes(App* app, GuiStatesBoxes* d) {
    int s = app->scale, count = (int)app->states_count, width, height, w = 160 * s, h = 90 * s;
    int longest = SDL_max(16, (int)SDL_utf8strlen(app->states_title) + 2);
    size_t i;
    for (i = 0; i < app->states_count; ++i)
        longest = SDL_max(longest, (int)SDL_utf8strlen(app->states[i].label) + gui_text_cols(app->theme.marker) + 4);
    ui_states_layout(
        over_rows(app, longest, rows_h(ui_states_layout, ui_states_row, d->rects, UI_STATES_ROWS, s, count), &d->box),
        s, d->rects);
    memset(d->lists, 0, sizeof d->lists);
    d->lists[UI_STATES_ROWS].focus = (size_t)app->states_focus;
    list_follow(app, GUI_LIST_STATES, ui_states_follow, ui_states_row, d->rects, UI_STATES_ROWS, (size_t)count,
                &d->lists[UI_STATES_ROWS]);
    area(app, &width, &height);
    if (d->box.x + d->box.w + 4 * s + w <= width)
        d->picture = gui_rect(d->box.x + d->box.w + 4 * s, d->box.y, w, h);
    else if (d->box.y + d->box.h + 4 * s + h <= height)
        d->picture = gui_rect(d->box.x, d->box.y + d->box.h + 4 * s, w, h);
    else
        d->picture = gui_rect(0, 0, 0, 0);
}
/* States' row: the state's slot, date and play time, in the failure colour when this build can't load it */
bool ui_states_rows_get_row_info(App* app, size_t row, int width, UiRowInfo* info) {
    (void)width;
    if (row >= app->states_count) return false;
    info->label = app->states[row].label;
    if (app->states[row].why[0]) info->attr |= UI_ATTR_FAILURE;
    return true;
}
/* the focused state's picture, made into a texture when the focus came to it */
static void render_states(App* app) {
    GuiStatesBoxes d;
    const GuiState* g;
    states_boxes(app, &d);
    over_box(app, d.box, app->states_title);
    ui_states_draw(app, d.rects, app->scale, d.lists);
    if (d.picture.w <= 0 || app->states_focus < 0 || (size_t)app->states_focus >= app->states_count) return;
    g = &app->states[app->states_focus];
    if (app->states_texture_of != app->states_focus) {
        SDL_DestroyTexture(app->states_texture);
        app->states_texture = NULL;
        if (g->thumb && g->thumb_w > 0 && g->thumb_h > 0) {
            app->states_texture = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                                    g->thumb_w, g->thumb_h);
            if (app->states_texture && !SDL_UpdateTexture(app->states_texture, NULL, g->thumb, g->thumb_w * 4)) {
                SDL_DestroyTexture(app->states_texture);
                app->states_texture = NULL;
            }
        }
        app->states_texture_of = app->states_focus;
    }
    outline(app,
            gui_rect(d.picture.x - app->scale, d.picture.y - app->scale, d.picture.w + 2 * app->scale,
                     d.picture.h + 2 * app->scale),
            app->theme.ink);
    if (app->states_texture) {
        SDL_FRect at = {(float)d.picture.x, (float)d.picture.y, (float)d.picture.w, (float)d.picture.h};
        SDL_RenderTexture(app->renderer, app->states_texture, NULL, &at);
    } else
        fill_rect(app, d.picture, app->theme.page);
}
/* row pressed: the game played from its state, the box closed; one this build can't load says why */
static void state_play(App* app, int row) {
    const GuiState* g = &app->states[row];
    int selected = app->menu.selected;
    bool recompile = app->recompile;
    if (g->why[0]) {
        snprintf(app->message, sizeof app->message, snx_tr("It can't be played from that state: %s."), g->why);
        return;
    }
    app->state_load = snx_format("--state_load=0:%s", g->path);
    {
        char* program = snx_absolute(g->program);
        states_close(app);
        app->recompile = false;
        app->menu.selected = GUI_SWITCH;
        launch(app, program, true);
    }
    free(app->state_load);
    app->state_load = NULL;
    if (!app->menu.running) app->menu.selected = selected;
    app->recompile = recompile;
}
/* Del on the box: the focused state's deletion asked first (GUI_ASK_STATE) */
static void state_delete_ask(App* app) {
    if ((size_t)app->states_focus >= app->states_count) return;
    snprintf(app->ask_line, sizeof app->ask_line, snx_tr("Delete the save state %s?"),
             app->states[app->states_focus].label);
    gui_confirm_open(&app->confirm, GUI_ASK_STATE, false);
}
/* the deletion taken: the file gone, the box without it, closed when it was the last */
static void state_delete(App* app) {
    GuiState* g;
    if ((size_t)app->states_focus >= app->states_count) return;
    g = &app->states[app->states_focus];
    if (remove(g->path)) {
        snprintf(app->message, sizeof app->message, snx_tr("Not deleted: %s could not be removed"), g->path);
        say_failure(app, app->message);
        return;
    }
    free(g->path);
    free(g->program);
    free(g->thumb);
    memmove(g, g + 1, (app->states_count - (size_t)app->states_focus - 1) * sizeof *g);
    app->states_count--;
    app->states_texture_of = -1;
    if (!app->states_count) {
        states_close(app);
        return;
    }
    if ((size_t)app->states_focus >= app->states_count) app->states_focus = (int)app->states_count - 1;
}
/* a key on the box: Up and Down move, Select plays from the state, Back closes it */
static void states_key(App* app, GuiKey key) {
    int count = (int)app->states_count;
    if (key == GUI_UP || key == GUI_DOWN)
        app->states_focus = (app->states_focus + (key == GUI_UP ? count - 1 : 1)) % count;
    else if (key == GUI_SELECT)
        state_play(app, app->states_focus);
    else if (key == GUI_CANCEL)
        states_close(app);
}
/* a row under the pointer takes the focus, and a click plays from it */
void ui_states_rows_row_action_cb(App* app, size_t row, uint32_t hit) {
    app->states_focus = (int)row;
    if (hit & UI_HIT_CLICK) state_play(app, (int)row);
}
/* the pointer on it: a row's (ui_states_rows_row_action_cb); a click off the box and its picture closes it */
static void states_pointer(App* app, float x, float y, bool click) {
    GuiStatesBoxes d;
    int item;
    size_t row;
    uint32_t hit;
    states_boxes(app, &d);
    hit = ui_states_hit(app, d.rects, app->scale, d.lists, (int)x, (int)y, click ? UI_HIT_CLICK : 0, &item, &row);
    if (hit)
        ui_states_act(app, item, row, hit);
    else if (click && !in_rect(d.box, x, y) && !in_rect(d.picture, x, y))
        states_close(app);
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
#define SAY_COMPILE_TITLE  SNX_TR_NOOP("SCARLETNX  /  COMPILE")
static const char* screen_title(const App* app, char* out, size_t size) {
    char* c;
    if (app->setup_on && !app->editing) return snx_tr("SCARLETNX  /  SETUP");
    if (!app->editing && app->menu.view == GUI_LOAD_RECOMPILE && !app->menu.running && !app->finished)
        return snx_tr(SAY_COMPILE_TITLE);
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
static void job_speed(App* app, char* out, size_t size);
/* the status box: the usage (the menu's, its run's and the queue's recompile's), the disk and the working folder, then
   while a recompile of the queue runs how it goes and, when the taskbar cannot show it, why; wrapped to width
   columns; with draw, drawn in r */
static int status_rows(App* app, int width, GuiRect r, int y, int draw) {
    char usage[256], disk[96], cwd[1100], *apart, speed[256], how[200], bar[300];
    const char* lines[6];
    int i, rows = 0;
    bool job = gui_queue_running(&app->queue) >= 0;
    SDL_Color soft = rgb(app->theme.soft), ink = rgb(app->theme.ink);
    gui_usage_text(usage, sizeof usage, app->usage.available,
                   app->usage.memory_bytes + (job ? app->job_usage.memory_bytes : 0),
                   app->usage.cpu_percent + (job ? app->job_usage.cpu_percent : 0), SDL_GetNumLogicalCPUCores());
    disk_text(app, disk, sizeof disk);
    snprintf(cwd, sizeof cwd, "%s %s", snx_tr("Working folder:"), app->cwd ? app->cwd : snx_tr("(unavailable)"));
    speed[0] = bar[0] = 0;
    if (job) {
        job_speed(app, how, sizeof how);
        snprintf(speed, sizeof speed, snx_tr("Background recompile: %s"), how);
    }
    if (job && app->taskbar_error[0])
        snprintf(bar, sizeof bar, snx_tr("Taskbar: no progress shown (%s)"), app->taskbar_error);
    lines[0] = usage;
    lines[1] = "";
    lines[2] = disk;
    lines[3] = cwd;
    lines[4] = speed;
    lines[5] = bar;
    /* the usage too wide for a row: the memory and the CPU a row each, at the two spaces between them, so a row breaks
       between two values and never inside one ("(0.5 cores)") */
    if (gui_text_cols(usage) > width && (apart = strstr(usage, "  ")) != NULL) {
        *apart = 0;
        lines[1] = apart + 2;
    }
    for (i = 0; i < 6; ++i) {
        if (!lines[i][0]) continue;
        if (draw)
            rows += wrap_in(app, r, r.x, y + rows * cell_h(app), lines[i], i == 3, i == 3 ? ink : soft);
        else
            rows += wrap_rows(app, lines[i], width, i == 3);
    }
    return rows;
}
/* the screens the tabs go round, Tab and Shift+Tab or a click on one: the menu, Games and the Settings, then the
   Compile tab while the queue holds a recompile */
enum { GUI_TAB_MENU, GUI_TAB_GAMES, GUI_TAB_SETTINGS, GUI_TAB_RECOMPILE, GUI_TABS };
static const char* const tab_names[GUI_TABS] = {SNX_TR_NOOP("Menu"), SNX_TR_NOOP("Games"), SNX_TR_NOOP("Settings"),
                                                SNX_TR_NOOP("Compile")};
static int tab_shown(const App* app) {
    if (app->setup_on && !app->editing) return app->setup_page;
    if (app->editing) return GUI_TAB_SETTINGS;
    return app->menu.view == GUI_LOAD_GAMES       ? GUI_TAB_GAMES
           : app->menu.view == GUI_LOAD_RECOMPILE ? GUI_TAB_RECOMPILE
                                                  : GUI_TAB_MENU;
}
/* the Compile tab's label: how far the one running is (Compile alone before it says), or for a while once one
   ended (GUI_JOB_SAID_MS), and while none runs, how the queue's ended: FAILED while one failed, else OK */
#define GUI_JOB_SAID_MS 5000
enum { GUI_TAB_GOES, GUI_TAB_OK, GUI_TAB_FAILED };
static int recompile_tab(const App* app, char* out, size_t size) {
    const GuiQueue* q = &app->queue;
    int run = gui_queue_running(q), failed = 0, ok = 0, i;
    Uint64 last = 0, now = now_ms(app);
    for (i = 0; i < q->count; ++i) {
        failed |= q->job[i].state == GUI_JOB_FAILED;
        ok |= q->job[i].state == GUI_JOB_OK;
        if (q->job[i].state >= GUI_JOB_OK) last = SDL_max(last, q->job[i].ended_ms);
    }
    if (run >= 0 && (!last || now - last >= GUI_JOB_SAID_MS || now < last)) {
        int percent = gui_queue_percent(q); /* the queue's, each row its own game's */
        if (percent < 0)
            snprintf(out, size, "%s", snx_tr("Compile"));
        else
            snprintf(out, size, snx_tr("Compile %d%%"), percent);
        return GUI_TAB_GOES;
    }
    snprintf(out, size, "%s", failed ? snx_tr("Compile FAILED") : ok ? snx_tr("Compile OK") : snx_tr("Compile"));
    return failed ? GUI_TAB_FAILED : ok ? GUI_TAB_OK : GUI_TAB_GOES;
}
/* the tabs while the setup shows: its pages, the notices' then each step's, numbered */
enum { SETUP_NOTICE = SNX_SETUP_STEPS }; /* a page's: a step's, or the notices' */
static const char* const setup_step_names[SNX_SETUP_STEPS] = {SNX_TR_NOOP("Appearance"), SNX_TR_NOOP("Install"),
                                                              SNX_TR_NOOP("Folders"), SNX_TR_NOOP("Keys"),
                                                              SNX_TR_NOOP("User")};
/* each step's icon in its tab, before its name where its number was (the page's head says it, 1/5): the tabs keep
   the width they had; the notices' after them */
static const unsigned setup_step_icons[SNX_SETUP_STEPS + 1] = {
    GUI_ICON_PALETTE, GUI_ICON_HARD_DRIVE_DOWNLOAD, GUI_ICON_FOLDER, GUI_ICON_KEY_ROUND, GUI_ICON_USER, GUI_ICON_INFO};
static int tab_count(const App* app) {
    if (app->setup_on && !app->editing) return app->setup_count;
    return app->queue.count ? GUI_TABS : GUI_TAB_RECOMPILE;
}
/* page i's number among the steps, from 1; 0 for the notices' */
static int setup_number(const App* app, int i) {
    return app->setup_pages[i] == SETUP_NOTICE ? 0 : i + (app->setup_pages[0] != SETUP_NOTICE);
}
static void tab_label(const App* app, int i, char* out, size_t size) {
    if ((!app->setup_on || app->editing) && i == GUI_TAB_RECOMPILE)
        recompile_tab(app, out, size);
    else if (!app->setup_on || app->editing)
        snprintf(out, size, "%s", snx_tr(tab_names[i]));
    else {
        char icon[4];
        gui_icon_utf8(setup_step_icons[app->setup_pages[i]], icon);
        snprintf(out, size, "%s %s", icon,
                 app->setup_pages[i] == SETUP_NOTICE ? snx_tr("Notice")
                                                     : snx_tr(setup_step_names[app->setup_pages[i]]));
    }
}
/* the columns tab i's name takes: the Compile tab's as many as its longest, so the head stays put as it changes */
static int tab_cols(const App* app, int i) {
    char label[64];
    if ((!app->setup_on || app->editing) && i == GUI_TAB_RECOMPILE) {
        snprintf(label, sizeof label, snx_tr("Compile %d%%"), 100);
        return SDL_max(gui_text_cols(label),
                       SDL_max(gui_text_cols(snx_tr("Compile OK")), gui_text_cols(snx_tr("Compile FAILED"))));
    }
    tab_label(app, i, label, sizeof label);
    return gui_text_cols(label);
}
/* the row of tabs' height: a button's, as the foot's keys' rows */
static int tabs_h(const App* app) { return button_h(app); }
/* tab i in the row of tabs, a button (GUI_BUTTON_*): a column wider than its name either side, a column after the one
   before it */
static GuiRect tab_rect(const App* app, GuiRect row, int i) {
    int x = row.x, k;
    for (k = 0; k < i; ++k) x += (tab_cols(app, k) + 2 * GUI_BUTTON_PAD + GUI_BUTTON_GAP) * cell_w(app);
    return gui_rect(x, row.y, (tab_cols(app, i) + 2 * GUI_BUTTON_PAD) * cell_w(app), row.h);
}
/* the tabs, the one shown as the focused box is; on the head's band in its colours swapped. The Compile tab once
   its queue ended: OK in the focus colour, FAILED on the failure colour blinking at the theme's blink_ms, as Share
   Log does */
static void draw_tabs(App* app, GuiRect row, int band) {
    const GuiTheme* t = &app->theme;
    GuiRgb ink = band ? t->page : t->ink, page = band ? t->ink : t->page, focus = band ? t->page : t->focus;
    int i, shown = tab_shown(app);
    char label[64];
    if (!row.h) return;
    for (i = 0; i < tab_count(app); ++i) {
        GuiRect r = tab_rect(app, row, i);
        int on = i == shown, inverted = on && t->focus_bar, ended = GUI_TAB_GOES;
        bool failing;
        if ((!app->setup_on || app->editing) && i == GUI_TAB_RECOMPILE) ended = recompile_tab(app, label, sizeof label);
        failing = ended == GUI_TAB_FAILED && !(t->blink_ms && (now_ms(app) / t->blink_ms) % 2);
        if (failing)
            fill_rect(app, r, t->failure);
        else if (inverted)
            fill_rect(app, r, focus);
        else if (t->panels == GUI_PANELS_FILL)
            fill_rect(app, r, t->dim);
        if (!failing && !inverted && (t->panels == GUI_PANELS_OUTLINE || on)) outline(app, r, on ? focus : t->dim);
        tab_label(app, i, label, sizeof label);
        text_in(app, r, r.x + GUI_BUTTON_PAD * cell_w(app), r.y + (r.h - cell_h(app)) / 2, label, /* in its middle */
                rgb(failing                     ? t->page
                    : inverted                  ? page
                    : on || ended == GUI_TAB_OK ? focus
                                                : ink));
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
/* the pixels a title of letters size times the text's has in the head, as head_place gives them: beside the tabs and
   the user */
static int head_room(const App* app, int size) {
    GuiRect none = {0, 0, 0, 0}, last = tab_rect(app, none, tab_count(app) - 1);
    int width, height, tabs = tabs_h(app), title = size * cell_h(app), badge;
    if (app->theme.title_shadow) title += size * app->scale;
    area(app, &width, &height);
    badge = user_width(app, SDL_max(title, tabs));
    width -= 2 * cell_w(app) + (badge ? badge + 2 * cell_w(app) : 0);
    return width - (last.x + last.w) - 2 * cell_w(app);
}
/* The head's title over the tabs' screens and its letters' size: a window too narrow for the main screens' titles
   (the menu's, Games', the keys' and the Settings') whole at the theme's size takes a size smaller, down to the
   text's, and only then leaves out the same first parts of every one, "SCARLETNX  /" first, so the head stays one
   and the same from screen to screen. The size is the English titles', so the head is as big in every language: a
   translation too long for it leaves out first parts too. A screen deeper in the Settings leaves out what more it
   must, and a last part still too wide is cut. The setup's head, its steps for tabs, keeps its title as it is */
static const char* head_title(const App* app, bool tabs, char* out, size_t size, int* letters) {
    const char *titles[5], *english[5];
    char own[160];
    const char* title = screen_title(app, own, sizeof own);
    int big = title_size(app), parts = 0, room = 0, i, n = app->queue.count ? 5 : 4; /* the Compile tab's with it */
    *letters = big;
    snprintf(out, size, "%s", title);
    if (!tabs || !title[0] || (app->setup_on && !app->editing)) return out; /* the setup's steps are its tabs */
    titles[0] = english[0] = app->theme.title[GUI_SCREEN_MENU];
    titles[1] = english[1] = app->theme.title[GUI_SCREEN_GAMES];
    titles[2] = english[2] = app->theme.title[GUI_SCREEN_KEYS];
    english[3] = SAY_SETTINGS_TITLE;
    titles[3] = snx_tr(SAY_SETTINGS_TITLE);
    english[4] = SAY_COMPILE_TITLE;
    titles[4] = snx_tr(SAY_COMPILE_TITLE);
    for (parts = 0; parts < 4; ++parts) {
        for (*letters = big; *letters >= 1; --*letters) {
            room = head_room(app, *letters);
            for (i = 0; i < n && title_px(app, title_tail(english[i], parts), *letters) <= room; ++i) {}
            if (i == n) break;
        }
        if (*letters >= 1) break;
    }
    if (parts == 4) { /* not even their last parts fit: no title on any, the tabs say which screen it is */
        *letters = 1;
        out[0] = 0;
        return out;
    }
    for (parts = 0; parts < 3; ++parts) { /* at that size, the first parts the translations leave out, alike */
        for (i = 0; i < n && title_px(app, title_tail(titles[i], parts), *letters) <= room; ++i) {}
        if (i == n) break;
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
/* the head laid out from x, y in width: the tabs at its left, the title after them against its right when both fit,
   else under them; the user against the right of the title's row, two columns past it as the title is past the tabs,
   or of the tabs' when there is no title; on a row of its own when it does not fit there. Its rectangles in out (NULL:
   measured only); its height, half a row of text between its rows */
static int head_place(const GuiHead* in, int x, int y0, int width, GuiPlace* out) {
    int gap = in->ch / 2, y = y0, badge = in->badge && in->badge_w ? in->badge_w + 2 * in->cw : 0;
    GuiRect tabs = {0, 0, 0, 0}, title = tabs, user = tabs;
    if (in->title && in->tabs && in->tabs_w + 2 * in->cw + in->title_w + badge <= width) {
        int row = SDL_max(in->tabs, in->title);
        if (badge) row = SDL_max(row, in->badge);
        tabs = gui_rect(x, y + row - in->tabs, in->tabs_w, in->tabs);
        title = gui_rect(x + width - badge - in->title_w, y + row - in->title, in->title_w, in->title);
        if (badge) user = gui_rect(x + width - in->badge_w, y + row - in->badge, in->badge_w, in->badge);
        y += row + gap;
    } else {
        int beside = in->tabs && !in->title && in->tabs_w + badge <= width;
        if (in->tabs) {
            int row = beside && in->badge > in->tabs ? in->badge : in->tabs;
            tabs = gui_rect(x, y + row - in->tabs, in->tabs_w, in->tabs);
            if (beside && badge) user = gui_rect(x + width - in->badge_w, y + row - in->badge, in->badge_w, in->badge);
            y += row + gap;
        }
        if (in->title) {
            int room = badge && in->title_w + badge <= width, row = in->title;
            if (room && in->badge > row) row = in->badge;
            title = gui_rect(x, y + row - in->title, room ? width - badge : width, in->title);
            if (room) user = gui_rect(x + width - in->badge_w, y + row - in->badge, in->badge_w, in->badge);
            y += row + gap;
        }
        if (badge && !user.h && in->badge_w <= width) { /* too narrow beside either: its own row, or none */
            user = gui_rect(x + width - in->badge_w, y, in->badge_w, in->badge);
            y += in->badge + gap;
        }
    }
    if (out) {
        out->tabs = tabs;
        out->title = title;
        out->badge = user;
    }
    return y > y0 ? y - y0 - gap : 0;
}
/* The screen's boxes, laid out by ui.txt's Screen (a list and the side box with GUI_SHOWS_SIDE, else one box) or Wide
   (GUI_SHOWS_WIDE: a run's output, the theme editor's keys): the head's measures and the keys first, which its
   callbacks read */
static void place_screen(App* app, GuiPlace* p, const char* keys, int shows) {
    const GuiTheme* t = &app->theme;
    GuiFrame* f = &app->frame;
    GuiRect none = {0, 0, 0, 0}, r[UI_SCREEN_ITEMS > UI_WIDE_ITEMS ? UI_SCREEN_ITEMS : UI_WIDE_ITEMS];
    int width, height, s = app->scale, size;
    char out[160];
    const char* title = head_title(app, shows & GUI_SHOWS_TABS, out, sizeof out, &size);
    area(app, &width, &height);
    memset(f, 0, sizeof *f);
    memset(p, 0, sizeof *p);
    f->head.cw = cell_w(app);
    f->head.ch = cell_h(app);
    if (shows & GUI_SHOWS_TABS) {
        GuiRect last = tab_rect(app, none, tab_count(app) - 1);
        f->head.tabs = tabs_h(app);
        f->head.tabs_w = last.x + last.w;
    }
    if (title[0]) {
        int shadow = t->title_shadow ? size * s : 0;
        f->head.title = size * cell_h(app) + shadow;
        f->head.title_w = gui_text_cols(title) * size * cell_w(app) + shadow;
    }
    /* the user as high as the head's row: the title's and the tabs', or the tabs' with no title */
    f->head.badge = f->head.title ? SDL_max(f->head.title, f->head.tabs) : f->head.tabs;
    f->head.badge_w = user_width(app, f->head.badge);
    if (!f->head.badge_w) f->head.badge = 0;
    f->keys = keys;
    f->side = (shows & GUI_SHOWS_SIDE) && app->side_box != GUI_SIDE_OFF;
    f->status = (shows & GUI_SHOWS_STATUS) && (shows & GUI_SHOWS_SIDE) && t->footer != GUI_STYLE_NONE;
    f->tips_foot = app->setup_on || (shows & GUI_SHOWS_FOOT_TIPS);
    /* side_box = on keeps the side box where Screen leaves it out, under the list as Wide lays it narrow */
    if ((shows & GUI_SHOWS_WIDE) || (f->side && app->side_box == GUI_SIDE_ON && UI_SCREEN_NARROW(width, s))) {
        ui_wide_layout(app, gui_rect(0, 0, width, height), s, r);
        p->head = r[UI_WIDE_HEAD];
        p->tips = f->tips_foot ? r[UI_WIDE_FOOT_TIPS] : r[UI_WIDE_TIPS];
        p->list = r[UI_WIDE_BODY_LIST];
        p->side = r[UI_WIDE_BODY_SIDE_ABOUT];
        p->status = r[UI_WIDE_BODY_SIDE_STATUS];
        p->keys = r[UI_WIDE_FOOT_KEYS];
    } else {
        ui_screen_layout(app, gui_rect(0, 0, width, height), s, r);
        p->head = r[UI_SCREEN_HEAD];
        p->tips = f->tips_foot ? r[UI_SCREEN_FOOT_TIPS] : r[UI_SCREEN_TIPS];
        p->list = r[UI_SCREEN_BODY_LIST];
        p->side = r[UI_SCREEN_BODY_SIDE_ABOUT];
        p->status = r[UI_SCREEN_BODY_SIDE_STATUS];
        p->keys = r[UI_SCREEN_FOOT_KEYS];
    }
    p->stacked = p->side.h && p->side.y > p->list.y;
    head_place(&f->head, p->head.x, p->head.y, p->head.w, p);
}
/* ---- the frame's callbacks, ui.txt's Screen and Wide alike: what place_screen measured ---- */
static bool frame_head_shown(const App* app) {
    return app->frame.head.tabs || app->frame.head.title || app->frame.head.badge;
}
/* the status box at width: its head and its rows, a half step under the text's letters */
static int frame_status(App* app, int width) {
    GuiRect none = {0, 0, 0, 0};
    int room = width - 2 * cell_w(app), h = head_h(app) + 4 * app->scale, half = text_half(app);
    h += status_rows(app, room / cell_w(app), none, 0, 0) * cell_h(app);
    app->half = half;
    return h;
}
/* the keys' box at width, in the foot's letters: as many rows as its sentences wrap to (two at least with the tips
   beside it, wrapped) */
static int frame_keys(App* app, int width) {
    const GuiTheme* t = &app->theme;
    GuiRect none = {0, 0, 0, 0};
    int s = app->scale, h;
    app->scale = foot_scale(app);
    h = hints_h(app, SDL_max(!app->frame.tips_foot || !t->tips_wrap ? 1 : 2,
                             hints(app, app->frame.keys,
                                   foot_text(app, gui_rect(0, 0, width, 0), t->footer_box).w / cell_w(app), none, 0, 0,
                                   0, NULL))) +
        4 * app->scale;
    app->scale = s;
    return h;
}
/* the keys' box at least as wide as its widest sentence, which a row cannot break, and the key back left of it */
static int frame_keys_least(App* app) {
    const GuiTheme* t = &app->theme;
    const char *apart = strchr(app->frame.keys, '\x1f'), *at = apart ? apart + 1 : app->frame.keys;
    char key[128], what[128];
    int s = app->scale, cols = 0, back = hints_cols(app->frame.keys), least;
    while (hint_next(&at, key, what, sizeof key)) cols = SDL_max(cols, hint_width(key, what));
    app->scale = foot_scale(app);
    least = (cols + (back ? back + 3 * GUI_BUTTON_GAP : 0) + (t->footer_box ? 4 : 0)) * cell_w(app);
    app->scale = s;
    return least;
}
bool ui_screen_head_shown_cb(App* app) { return frame_head_shown(app); }
int ui_screen_head_measure_cb(App* app, int width, int scale) {
    (void)scale;
    return head_place(&app->frame.head, 0, 0, width, NULL);
}
bool ui_screen_tips_shown_cb(App* app) { return !app->frame.tips_foot; }
/* the tips' row under the head: a row of their letters, a half step under the text's, and foot_text's margins */
int ui_screen_tips_measure_cb(App* app, int width, int scale) {
    (void)width;
    return 8 * (scale + foot_scale(app)) + 4 * scale;
}
bool ui_screen_body_side_shown_cb(App* app) { return app->frame.side; }
bool ui_screen_body_side_status_shown_cb(App* app) { return app->frame.status; }
int ui_screen_body_side_status_measure_cb(App* app, int width, int scale) {
    (void)scale;
    return frame_status(app, width);
}
bool ui_screen_foot_tips_shown_cb(App* app) { return app->frame.tips_foot; }
int ui_screen_foot_keys_measure_cb(App* app, int width, int scale) {
    (void)scale;
    return frame_keys(app, width);
}
int ui_screen_foot_keys_min_width_cb(App* app, int scale) {
    (void)scale;
    return frame_keys_least(app);
}
bool ui_wide_head_shown_cb(App* app) { return frame_head_shown(app); }
int ui_wide_head_measure_cb(App* app, int width, int scale) { return ui_screen_head_measure_cb(app, width, scale); }
bool ui_wide_tips_shown_cb(App* app) { return !app->frame.tips_foot; }
int ui_wide_tips_measure_cb(App* app, int width, int scale) { return ui_screen_tips_measure_cb(app, width, scale); }
bool ui_wide_body_side_shown_cb(App* app) { return app->frame.side; }
bool ui_wide_body_side_status_shown_cb(App* app) { return app->frame.status; }
int ui_wide_body_side_status_measure_cb(App* app, int width, int scale) {
    (void)scale;
    return frame_status(app, width);
}
bool ui_wide_foot_tips_shown_cb(App* app) { return app->frame.tips_foot; }
int ui_wide_foot_keys_measure_cb(App* app, int width, int scale) {
    (void)scale;
    return frame_keys(app, width);
}
int ui_wide_foot_keys_min_width_cb(App* app, int scale) {
    (void)scale;
    return frame_keys_least(app);
}
static void status_box(App* app, GuiRect r) {
    GuiRect text = inner(app, r);
    int y, half;
    if (!r.h) return;
    y = pane(app, r, snx_tr("Status"));
    half = text_half(app); /* its rows a half step under the text's letters */
    status_rows(app, text.w / cell_w(app), gui_rect(text.x, y, text.w, r.y + r.h - y), y, 1);
    app->half = half;
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
                                        SNX_TR_NOOP("Runs a Linux ARM program, 64 or 32-bit"),
                                        SNX_TR_NOOP("Recompiles a 3DS game, then runs it"),
                                        SNX_TR_NOOP("Recompiles a PS Vita homebrew .vpk, then runs it"),
                                        SNX_TR_NOOP("Runs a GBA ROM on the emulated AGB board")};
    if (item >= GUI_SWITCH && item <= GUI_GBA) return snx_tr(loads[item - GUI_SWITCH]);
    if (item == GUI_KEYS) return snx_tr("Installs prod.keys, which games are extracted with");
    if (item == GUI_EXIT) return snx_tr("Closes ScarletNX");
    {
        GuiMenu menu = app->menu;
        menu.selected = item;
        return gui_setting_description(&menu);
    }
}
static void menu_boxes(App* app, GuiMenuBoxes* m) {
    char keys[256];
    place_screen(app, &m->place, shown_keys(app, keys, sizeof keys),
                 GUI_SHOWS_STATUS | GUI_SHOWS_TABS | GUI_SHOWS_SIDE);
    ui_menu_layout(m->place.list, app->scale, m->rects);
    memset(m->lists, 0, sizeof m->lists);
    m->lists[UI_MENU_ENTRIES].focus = (size_t)app->menu.selected;
    list_follow(app, GUI_LIST_MENU, ui_menu_follow, ui_menu_row, m->rects, UI_MENU_ENTRIES, GUI_ITEM_COUNT,
                &m->lists[UI_MENU_ENTRIES]);
}
/* the last run's output, under what the side box says */
static void last_output(App* app, GuiRect r, int y) {
    SDL_Color soft = rgb(app->theme.soft);
    int i, x;
    if (!app->has_output || entering(app)) return;
    y += cell_h(app);
    y += wrap_in(app, r, r.x, y, snx_tr("Last output:"), 0, soft) * cell_h(app);
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
   keys while no prod.keys is in use, so a first start shows what to do first, and Share Log after a run crashed until
   its log is saved; greyed, a load whose driver is not there */
unsigned ui_menu_entries_attr_cb(App* app, size_t row) {
    unsigned attr = app->menu.activated && (size_t)app->menu.active == row ? UI_ATTR_ACTIVE : 0;
    if (row == GUI_KEYS) {
        char* in_use = keys_in_use(app);
        if (!in_use) attr |= UI_ATTR_FAILURE | UI_ATTR_BLINK;
        free(in_use);
    }
    if (row == GUI_LOG && app->log_due) attr |= UI_ATTR_FAILURE | UI_ATTR_BLINK;
    if (row <= GUI_GBA && (loads_off(app) >> row & 1u)) attr |= UI_ATTR_OFF;
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
/* ---- Games: "Recent Games" over the recent rows, then a page of the games, as many as the list's box holds ---- */
typedef struct {
    GuiPlace place;
    GuiRect rects[UI_GAMES_ITEMS];
    UiList
        lists[UI_GAMES_ITEMS]; /* Add's, and Rows': the label and the recent rows under games_lead, then the page's */
    /* the grid's, ui.txt's GamesGrid, in place of Rows when View is Grid: its tiles are Rows' rows */
    GuiRect grid_rects[UI_GAMES_GRID_ITEMS];
    UiList grid_lists[UI_GAMES_GRID_ITEMS];
    size_t start; /* the page's first game */
    int lead;     /* games_lead */
} GuiGamesBoxes;
/* Rows' rows, the label and the recent ones under games_lead, then the page's */
static size_t games_rows(const App* app) { return (size_t)games_lead(app) + gui_browser_items(&app->browser); }
/* whether Rows' row is a label, which the grid puts on a line of its own: "Recent Games", a group's heading */
static bool games_row_label(const App* app, size_t row) {
    size_t lead = (size_t)games_lead(app);
    if (row < lead) return row == 0;
    return gui_games_heading(&app->games, app->browser.page * app->browser.page_size + row - lead);
}
/* the grid's flow over its rows from the first, as tall as they take: where each goes, its line and its column */
static void grid_flow(App* app, const GuiGamesBoxes* g, UiFlow* f) {
    ui_games_grid_flow(g->grid_rects, app->scale, UI_GAMES_GRID_TILES, f);
    ui_flow_sized(f, &g->grid_lists[UI_GAMES_GRID_TILES], app->scale);
    f->area.h = SDL_MAX_SINT32 / 2;
    f->max = (size_t)-1;
}
/* the grid's first row shown: the first of the first line from where the focus's line is in the middle of those that
   show, as Rows keeps its focus; for a focus the pointer put there, from the line shown first last (firsts), or as
   little moved as it takes for the focus's line to show whole. From where the last line is at the bottom once the
   lines after the focus all show; the focus's line itself where it is taller than the list */
/* the grid's first row whose line starts at from pixels down its rows or past it */
static size_t grid_row_at(App* app, const GuiGamesBoxes* g, int from) {
    UiFlow f;
    GuiRect at, area = g->grid_rects[UI_GAMES_GRID_TILES];
    size_t rows = games_rows(app), r;
    int line = -1;
    grid_flow(app, g, &f);
    for (r = 0; r < rows && ui_flow_next(&f, games_row_label(app, r), &at); ++r) {
        if (f.line == line) continue; /* a line's first alone */
        line = f.line;
        if (at.y - area.y >= from) return r;
    }
    return 0;
}
/* how many pixels down the grid's rows row's place starts and how tall it is, and how tall they all are */
static void grid_extent(App* app, const GuiGamesBoxes* g, size_t row, int* top, int* height, int* bottom) {
    UiFlow f;
    GuiRect at, area = g->grid_rects[UI_GAMES_GRID_TILES];
    size_t rows = games_rows(app), r;
    *top = *height = *bottom = 0;
    grid_flow(app, g, &f);
    for (r = 0; r < rows && ui_flow_next(&f, games_row_label(app, r), &at); ++r) {
        if (r == row) {
            *top = at.y - area.y;
            *height = at.h;
        }
        *bottom = at.y + at.h - area.y;
    }
}
/* the grid's first row of the line y pixels down its rows is in, and how far down that line starts */
static size_t grid_line_at(App* app, const GuiGamesBoxes* g, int y, int* top) {
    UiFlow f;
    GuiRect at, area = g->grid_rects[UI_GAMES_GRID_TILES];
    size_t rows = games_rows(app), r, found = 0;
    int line = -1;
    *top = 0;
    grid_flow(app, g, &f);
    for (r = 0; r < rows && ui_flow_next(&f, games_row_label(app, r), &at); ++r) {
        if (f.line == line) continue; /* a line's first alone */
        line = f.line;
        if (at.y - area.y > y) break;
        found = r;
        *top = at.y - area.y;
    }
    return found;
}
/* the grid's scroll bar while its rows are taller than the area, as Rows' (games_bar) */
static bool grid_bar(App* app, const GuiGamesBoxes* g, GuiRect* track, GuiRect* thumb) {
    GuiRect area = g->grid_rects[UI_GAMES_GRID_TILES];
    int top, height, bottom;
    grid_extent(app, g, 0, &top, &height, &bottom);
    return bar_place(app, gui_rect(g->place.list.x, area.y, g->place.list.w, area.h), bottom, area.h,
                     (long long)app->list_y[GUI_LIST_GRID], track, thumb);
}
static size_t grid_first(App* app, const GuiGamesBoxes* g, size_t focus) {
    UiFlow f;
    GuiRect at, area = g->grid_rects[UI_GAMES_GRID_TILES];
    size_t rows = games_rows(app), r, kept = app->firsts[GUI_LIST_GRID];
    int top = 0, height = 0, bottom = 0, shown = -1, from;
    grid_flow(app, g, &f);
    for (r = 0; r < rows && ui_flow_next(&f, games_row_label(app, r), &at); ++r) {
        if (r == focus) {
            top = at.y - area.y;
            height = at.h;
        }
        if (r == kept) shown = at.y - area.y;
        bottom = at.y + at.h - area.y;
    }
    if (shown < 0) shown = bottom; /* rows that went: past the last, which the bottom's limit brings back */
    if (!app->pointed)
        from = top - (area.h - height) / 2;
    else if (top < shown)
        from = top;
    else if (top + height > shown + area.h)
        from = top + height - area.h;
    else
        from = shown;
    return grid_row_at(app, g, SDL_min(SDL_min(from, bottom - area.h), top));
}
/* the list's rows: their height a row's step, how many pixels they take all together and how many the area shows */
static void games_extent(App* app, const GuiGamesBoxes* g, int* row_h, int* total, int* view) {
    GuiRect a = ui_games_row(g->rects, app->scale, UI_GAMES_ROWS, 0),
            b = ui_games_row(g->rects, app->scale, UI_GAMES_ROWS, 1);
    *row_h = SDL_max(1, b.y - a.y);
    *total = (int)((size_t)g->lead + gui_browser_items(&app->browser)) * *row_h - (*row_h - a.h);
    *view = g->rects[UI_GAMES_ROWS].h;
}
static void games_boxes(App* app, GuiGamesBoxes* g) {
    size_t selected = app->browser.selected, head = app->browser.head;
    games_place(app, &g->place);
    ui_games_layout(g->place.list, app->scale, g->rects);
    ui_games_grid_layout(g->place.list, app->scale, g->grid_rects);
    g->start = app->browser.page * app->browser.page_size;
    g->lead = games_lead(app);
    memset(g->lists, 0, sizeof g->lists);
    memset(g->grid_lists, 0, sizeof g->grid_lists);
    g->grid_lists[UI_GAMES_GRID_TILES].tile_w = icon_tiles[app->options[GUI_OPTION_ICONS]].w;
    g->grid_lists[UI_GAMES_GRID_TILES].tile_h = icon_tiles[app->options[GUI_OPTION_ICONS]].h;
    g->lists[UI_GAMES_ROWS].focus = selected < head ? selected + 1 : (size_t)g->lead + selected - head;
    if (games_grid(app)) {
        UiList* tiles = &g->grid_lists[UI_GAMES_GRID_TILES];
        size_t focus = g->lists[UI_GAMES_ROWS].focus, *held = &app->bar_focus[GUI_LIST_GRID];
        int top, height, bottom, h;
        float y;
        tiles->focus = focus;
        grid_extent(app, g, focus, &top, &height, &bottom);
        if (bottom > g->grid_rects[UI_GAMES_GRID_TILES].h) { /* past the area: the scroll bar, the tiles beside it */
            GuiRect list = g->place.list;
            list.w -= bar_room(app);
            ui_games_grid_layout(list, app->scale, g->grid_rects);
            grid_extent(app, g, focus, &top, &height, &bottom);
        }
        h = g->grid_rects[UI_GAMES_GRID_TILES].h;
        app->list_max[GUI_LIST_GRID] = (float)SDL_max(0, bottom - h);
        y = SDL_clamp(app->list_y[GUI_LIST_GRID], 0.0f, app->list_max[GUI_LIST_GRID]);
        /* where the thumb or a finger put it, until the focus moves: to a tile the pointer put it on that shows, still */
        if (*held == SIZE_MAX) *held = focus + 1;
        if (*held &&
            (*held == focus + 1 || (app->pointed && (float)top >= y && (float)(top + height) <= y + (float)h))) {
            int line;
            *held = focus + 1;
            tiles->first = grid_line_at(app, g, (int)y, &line);
            tiles->shift = (int)y - line;
        } else { /* the focus's line followed; at the end, the last line at the bottom, the first cut where it is */
            int line;
            *held = 0;
            tiles->first = grid_first(app, g, focus);
            grid_extent(app, g, tiles->first, &top, &height, &bottom);
            app->list_y[GUI_LIST_GRID] = SDL_min((float)top, app->list_max[GUI_LIST_GRID]);
            app->list_v[GUI_LIST_GRID] = 0;
            if (app->list_y[GUI_LIST_GRID] < (float)top) {
                tiles->first = grid_line_at(app, g, (int)app->list_y[GUI_LIST_GRID], &line);
                tiles->shift = (int)app->list_y[GUI_LIST_GRID] - line;
            }
        }
        app->firsts[GUI_LIST_GRID] = tiles->first;
        return;
    }
    /* the rows from games_y down, the first one cut where it is; past the area's height, the scroll bar at its right
       and the rows beside it */
    {
        int row_h, total, view;
        float y;
        games_extent(app, g, &row_h, &total, &view);
        if (total > view) {
            GuiRect list = g->place.list;
            list.w -= bar_room(app);
            ui_games_layout(list, app->scale, g->rects);
        }
        y = SDL_clamp(app->games_y, 0.0f, (float)SDL_max(0, total - view));
        g->lists[UI_GAMES_ROWS].first = (size_t)(y / (float)row_h);
        g->lists[UI_GAMES_ROWS].shift = (int)y - (int)g->lists[UI_GAMES_ROWS].first * row_h;
    }
}
/* the list (View List) shown, the one games_y scrolls */
static bool games_scrolls(const App* app) {
    return app->menu.view == GUI_LOAD_GAMES && !games_grid(app) && !app->menu.running && !app->finished;
}
/* the farthest the list scrolls, in pixels */
static float games_max(App* app, const GuiGamesBoxes* g) {
    int row_h, total, view;
    games_extent(app, g, &row_h, &total, &view);
    return (float)SDL_max(0, total - view);
}
/* the scroll bar beside the rows: its track, the list's height, and its thumb, as tall as the share of the rows shown
   and where they are among them; false while they all show */
static bool games_bar(App* app, const GuiGamesBoxes* g, GuiRect* track, GuiRect* thumb) {
    int row_h, total, view;
    games_extent(app, g, &row_h, &total, &view);
    return bar_place(app, gui_rect(g->place.list.x, g->rects[UI_GAMES_ROWS].y, g->place.list.w, view), total, view,
                     (long long)app->games_y, track, thumb);
}
/* the list scrolled to pixel y of the bar's track, the thumb's middle there */
static void games_bar_to(App* app, float y) {
    GuiGamesBoxes g;
    GuiRect track, thumb;
    games_boxes(app, &g);
    if (!games_bar(app, &g, &track, &thumb) || track.h <= thumb.h) return;
    app->games_y = app->games_to =
        SDL_clamp((y - (float)track.y - (float)thumb.h / 2) / (float)(track.h - thumb.h), 0.0f, 1.0f) *
        games_max(app, &g);
    app->games_v = 0;
}
/* a frame of the list's scroll: a fling slowing down to its stop, else the way to games_to, which a key's focus out of
   sight moves to show it (not one the pointer put there); at once with Animations off. A finger or the thumb holding
   it moves it alone */
static void games_scroll_step(App* app) {
    GuiGamesBoxes g;
    Uint64 now = now_ms(app);
    float dt = app->games_tick && now > app->games_tick ? SDL_min((float)(now - app->games_tick) / 1000.0f, 0.1f) : 0,
          max;
    app->games_tick = now;
    if (!games_scrolls(app)) return;
    games_boxes(app, &g);
    max = games_max(app, &g);
    if (app->browser.selected != app->games_seen) { /* the focus moved: shown, unless the pointer put it there */
        int row_h, total, view;
        float top;
        app->games_seen = app->browser.selected;
        games_extent(app, &g, &row_h, &total, &view);
        top = (float)g.lists[UI_GAMES_ROWS].focus * (float)row_h;
        if (!app->pointed && !app->games_held) {
            size_t focus = g.lists[UI_GAMES_ROWS].focus;
            if (top < app->games_to) /* with the heading over it, a group's first */
                app->games_to = focus && games_row_label(app, focus - 1) ? top - (float)row_h : top;
            if (top + (float)row_h > app->games_to + (float)view) app->games_to = top + (float)row_h - (float)view;
            app->games_v = 0;
        }
    }
    if (app->games_held || app->bar_held) return;
    if (app->games_v != 0) { /* flung: on, slowing, until it stops or meets an end */
        app->games_y += app->games_v * dt;
        app->games_v *= SDL_expf(-3.0f * dt);
        if (SDL_fabsf(app->games_v) < 30.0f || app->games_y <= 0 || app->games_y >= max) app->games_v = 0;
        app->games_y = app->games_to = SDL_clamp(app->games_y, 0.0f, max);
        return;
    }
    app->games_to = SDL_clamp(app->games_to, 0.0f, max);
    if (app->animations_off || SDL_fabsf(app->games_to - app->games_y) < 0.5f)
        app->games_y = app->games_to;
    else
        app->games_y += (app->games_to - app->games_y) * SDL_min(1.0f, dt * 14.0f);
}
/* a finger on the list at window pixel x, y, as it goes down (0), moves (1) and lifts (2): on the scroll bar, or two
   letters from its right edge (a thumb's reach, wider than it is drawn), it takes the thumb there; elsewhere, once it has moved more than a tap may, the rows follow it, and let go they glide on at
   the speed of its last move. A tap moves nothing (touch takes it) */
static void games_finger(App* app, float x, float y, int phase) {
    GuiGamesBoxes g;
    GuiRect track, thumb;
    float rx, ry;
    Uint64 now = now_ms(app);
    if (!SDL_RenderCoordinatesFromWindow(app->renderer, x, y, &rx, &ry)) return;
    games_boxes(app, &g);
    if (phase == 0) {
        app->games_held = false;
        app->games_v = 0;
        app->games_hold_y = app->games_y;
        app->games_hold_at = app->games_last_y = ry;
        app->games_last_ms = now;
        app->bar_held = games_bar(app, &g, &track, &thumb) && rx >= (float)(track.x + track.w - 2 * cell_w(app)) &&
                        ry >= (float)track.y && ry < (float)(track.y + track.h);
        if (app->bar_held) games_bar_to(app, ry);
        return;
    }
    if (app->bar_held) {
        games_bar_to(app, ry);
        if (phase == 2) app->bar_held = false;
        return;
    }
    if (phase == 1 && !app->games_held && SDL_fabsf(ry - app->games_hold_at) > (float)cell_h(app) / 2)
        app->games_held = true;
    if (!app->games_held) return;
    app->games_y = app->games_to = SDL_clamp(app->games_hold_y - (ry - app->games_hold_at), 0.0f, games_max(app, &g));
    if (phase == 1) {
        if (now > app->games_last_ms)
            app->games_v = -(ry - app->games_last_y) * 1000.0f / (float)(now - app->games_last_ms);
        app->games_last_y = ry;
        app->games_last_ms = now;
        return;
    }
    app->games_held = false; /* let go: on at its last speed, unless the finger had stopped */
    if (now - app->games_last_ms > 100) app->games_v = 0;
    app->games_tick = now;
}
static bool box_shown(App* app);
static void game_label(const App* app, const GuiGames* games, size_t i, char* out, size_t size);
static void menu_key(App* app, GuiKey key);
/* Rows' row i as the browser counts it (0 is "Recent Games" over the recent rows): a recent row, else a game of the
   page */
static size_t games_selected(const App* app, size_t row) {
    size_t lead = (size_t)games_lead(app);
    return row < lead ? row - 1 : app->browser.head + row - lead;
}
/* Rows' row: "Recent Games", the recent rows and their show more, a page's headings and games; a game in the active
   colour once activated, the pages before and after marked on the first game and the last */
bool ui_games_rows_get_row_info(App* app, size_t row, int width, UiRowInfo* info) {
    char* label = app->row_text;
    size_t lead = (size_t)games_lead(app), items = gui_browser_items(&app->browser), i, start;
    (void)width; /* a game's name slides when it is too long, as the menu's rows */
    start = app->browser.page * app->browser.page_size;
    if (row == 0 && lead) {
        info->label = snx_tr("Recent Games");
        info->attr = UI_ATTR_LABEL;
        return true;
    }
    if (row < lead) {
        int index = (int)row - 1;
        char queued[64];
        const GuiRecentGame* r = index < recent_shown(app) ? &app->recent.game[recent_index(app, index)] : NULL;
        job_suffix(app, r ? r->path : NULL, queued, sizeof queued);
        snprintf(label, sizeof app->row_text, "%s%s%s", r ? r->name : snx_tr("Show More"),
                 index < recent_shown(app) && (app->debug_picks || app->recent_debug[index]) ? snx_tr("  (Debug on)")
                                                                                             : "",
                 queued);
        info->label = label;
        if (r) info->image = r->driver == GUI_SWITCH ? game_icon(app, r->path) : NULL;
        if (!r) info->image = icon_more(app);                /* Show More's cover */
        if (r && !info->image) info->image = icon_none(app); /* a game with none of its own */
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
    if (!info->image) info->image = icon_none(app);
    if (app->browser.activated && app->browser.active == start + i) info->attr |= UI_ATTR_ACTIVE;
    if (i == 0 && app->browser.page > 0) info->attr |= UI_ATTR_MORE_UP;
    if (i + 1 == items && app->browser.page + 1 < gui_browser_pages(&app->browser)) info->attr |= UI_ATTR_MORE_DOWN;
    return true;
}
/* a row hit: the focus on it, and a click opens it; "Recent Games" is no row to take */
void ui_games_rows_row_action_cb(App* app, size_t row, uint32_t hit) {
    if (row == 0 && games_lead(app)) return;
    app->browser.selected = games_selected(app, row);
    if (hit & UI_HIT_CLICK) menu_key(app, GUI_SELECT);
}
/* the grid's tiles are Rows' rows, its one page's */
bool ui_games_grid_tiles_get_row_info(App* app, size_t row, int width, UiRowInfo* info) {
    return ui_games_rows_get_row_info(app, row, width, info);
}
void ui_games_grid_tiles_row_action_cb(App* app, size_t row, uint32_t hit) {
    ui_games_rows_row_action_cb(app, row, hit);
}
/* the focused row of Games: what it is, about it, and a line more for a recent game's or a game's kind */
static bool game_was_old(const App* app, const char* path);
static int row_driver(const GuiGames* games, size_t i);
/* a 3DS game, a Vita VPK or a GBA ROM: a console's package its driver makes a game of */
static bool package(const char* name) { return gui_is_cia(name) || gui_is_vpk(name) || gui_is_gba(name); }
/* a made game's kind, by the console its folder says */
static const char* ready_kind(int driver) {
    return driver == GUI_3DS    ? snx_tr("3DS game, ready to run")
           : driver == GUI_VITA ? snx_tr("Vita game, ready to run")
           : driver == GUI_GBA  ? snx_tr("GBA game, ready to run")
                                : snx_tr("Ready to run");
}
/* after a dump's kind, why it shows no icon (dump_icon_why), in out; the bytes written */
static size_t icon_why_said(const App* app, const char* path, char* out, size_t size) {
    char why[160];
    int n = dump_icon_why(app, path, why, sizeof why) ? snprintf(out, size, snx_tr(". No icon: %s"), why) : 0;
    return n > 0 ? SDL_min((size_t)n, size ? size - 1 : 0) : 0;
}
/* game i's kind, by its file's name: ready (whose console its folder says), a dump or a package; why a dump shows no
   icon; its updates, DLCs and mods; the games/ of an older version that holds it, as its row's (games/) says */
static void game_kind(const App* app, const GuiGames* games, size_t i, char* kind, size_t size) {
    const char* name = games->names[i];
    size_t n;
    if (games->addons[i])
        snprintf(kind, size, snx_tr("Ready to run, with %zu update(s), DLC(s), mod(s) or cheat(s) to pick from"),
                 games->addons[i]);
    else
        snprintf(kind, size, "%s",
                 gui_is_cia(name)    ? snx_tr("3DS game, opened with scarletnx-3ds")
                 : gui_is_vpk(name)  ? snx_tr("Vita VPK, opened with scarletnx-vita")
                 : gui_is_gba(name)  ? snx_tr("GBA ROM, played with scarletnx-agb")
                 : gui_is_dump(name) ? snx_tr("A dump, prepared when it runs")
                                     /* Games' own rows by what list_games read, another list's from its folder */
                                     : ready_kind(games == &app->games ? app->games_drivers[i] : row_driver(games, i)));
    n = strlen(kind);
    if (!games->addons[i] && gui_is_dump(name) && games->paths[i])
        n += icon_why_said(app, games->paths[i], kind + n, size - n);
    if (game_was_old(app, games->paths[i]))
        snprintf(kind + n, size - n, "%s",
                 snx_tr(", in the games/ an older version made: new games go to recompiled/"));
}
/* a recent game's kind: its row's in Games when Games lists it, else what its file is, in the words Games uses */
static void recent_kind(const App* app, const GuiRecentGame* r, char* kind, size_t size) {
    size_t i;
    for (i = 0; i < app->games.count; ++i)
        if (app->games.paths[i] && !strcmp(app->games.paths[i], r->path)) {
            game_kind(app, &app->games, i, kind, size);
            return;
        }
    /* shown, it is a game made or a Linux program (recent_game) */
    snprintf(kind, size, "%s",
             r->driver == GUI_LINUX ? snx_tr("Linux ARM ELF, run with scarletnx-gnuabi") : ready_kind(r->driver));
}
static const char* games_focus(App* app, const GuiGamesBoxes* g, const char** about, char* kind, size_t size) {
    size_t selected = app->browser.selected, head = app->browser.head;
    kind[0] = 0;
    if (selected >= head + gui_browser_items(&app->browser)) { /* no game, nor a recent one */
        *about = snx_tr("Add a folder of games or dumps in Settings > App > Folders (P)");
        return snx_tr("No games");
    }
    if (selected < head && (int)selected < recent_shown(app)) {
        const GuiRecentGame* r = &app->recent.game[recent_index(app, (int)selected)];
        recent_kind(app, r, kind, size);
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
/* a game's row in Games: its name, how many updates, DLCs and mods it has, (games/) when an older version's games/
   holds it, and where it is in the queue of recompiles while it waits or runs there */
static void game_label(const App* app, const GuiGames* games, size_t i, char* out, size_t size) {
    const char* was = game_was_old(app, games->paths[i]) ? snx_tr("  (games/)") : "";
    /* Games' own rows, by what list_games read: another list's are add-ons, which run with their game's */
    const char* debug = games == &app->games && (app->debug_picks || app->games_debug[i]) ? snx_tr("  (Debug on)") : "";
    char queued[64];
    job_suffix(app, games->paths[i], queued, sizeof queued);
    if (games->addons[i]) /* its updates, DLCs and mods, which opening it offers */
        snprintf(out, size, "%s%s  +%zu%s%s%s", app->theme.marker, games->shown[i], games->addons[i], was, debug,
                 queued);
    else
        snprintf(out, size, "%s%s%s%s%s", app->theme.marker, games->shown[i], was, debug, queued);
}
/* the path of the game Games' focus is on, recent (*recent says so) or of the page; NULL on a heading, Show More or
   no game */
static const char* games_focused_path(const App* app, bool* recent) {
    size_t selected = app->browser.selected, head = app->browser.head;
    size_t index = app->browser.page * app->browser.page_size + selected - head;
    *recent = selected < head;
    if (selected < head)
        return (int)selected < recent_shown(app) ? app->recent.game[recent_index(app, (int)selected)].path : NULL;
    if (selected >= head + gui_browser_items(&app->browser) || gui_games_heading(&app->games, index)) return NULL;
    return app->games.paths[index];
}
/* Games' focus on the game at path as Games lists it, its page shown; false when Games does not list it */
static bool games_focus_on(App* app, const char* path) {
    for (size_t i = 0; path && app->browser.page_size && i < app->games.count; ++i)
        if (app->games.paths[i] && !strcmp(app->games.paths[i], path)) {
            app->browser.page = i / app->browser.page_size;
            app->browser.selected = app->browser.head + i % app->browser.page_size;
            return true;
        }
    return false;
}
/* the side box's lines of a game the queue recompiles or will: how far it is, or where it waits, and how to see it */
static int games_queued(App* app, GuiRect r, int y) {
    bool recent;
    const char* path = games_focused_path(app, &recent);
    int i = path ? gui_queue_find(&app->queue, path) : -1, top = y;
    char line[400], where[64];
    SDL_Color soft = rgb(app->theme.soft), ink = rgb(app->theme.ink);
    if (i < 0) return 0;
    if (app->queue.job[i].state == GUI_JOB_RUNNING && app->job_steps.count) {
        char runs[3][112];
        size_t lengths[3];
        step_runs(&app->job_steps.step[app->job_steps.count - 1], app->theme.progress, now_ms(app), runs, lengths);
        snprintf(line, sizeof line, snx_tr("Compiling: %s%s%s"), runs[0], runs[1], runs[2]);
    } else if (app->queue.job[i].state == GUI_JOB_RUNNING)
        snprintf(line, sizeof line, "%s", snx_tr("Compiling: starting"));
    else {
        job_where(app, i, false, where, sizeof where);
        snprintf(line, sizeof line, snx_tr("In the queue: %s"), where);
    }
    y += wrap_in(app, r, r.x, y, line, 0, ink) * cell_h(app);
    y += wrap_in(app, r, r.x, y, snx_tr("Enter or the Compile tab: its output"), 0, soft) * cell_h(app);
    y += wrap_in(app, r, r.x, y, snx_tr("R on another game: it joins the queue"), 0, soft) * cell_h(app);
    return y - top;
}
/* how a game's code was made, as the side panel says it in a line: its -O, and the share of the modules' code the
   analysis made ahead of time, or JIT only; ? for what its stamp does not say (an older version's) */
static void made_text(const SnxGameMade* m, char* out, size_t size) {
    char share[32];
    if (!m->opt) {
        snprintf(out, size, "%s", snx_tr("Made with: ?"));
        return;
    }
    if (!m->aot) {
        snprintf(out, size, snx_tr("Made with: O%c, JIT only"), m->opt);
        return;
    }
    if (!m->code)
        snprintf(share, sizeof share, "?");
    else if (m->found && m->found * 100 < m->code)
        snprintf(share, sizeof share, "<1%%");
    else
        snprintf(share, sizeof share, "%llu%%", m->found * 100 / m->code);
    snprintf(out, size, snx_tr("Made with: O%c, AOT %s"), m->opt, share);
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
    app->debug_picks = gui_settings_debug(settings_of(app), NULL, 1, NULL, 0) > 0;
    debug_focus_update(app);
    games_scroll_step(app);
    games_boxes(app, &g);
    chrome(app, &g.place);
    if (games_grid(app)) {
        GuiRect track, thumb;
        int top, height, bottom;
        app->rows_barred = grid_bar(app, &g, &track, &thumb);
        ui_games_grid_draw(app, g.grid_rects, app->scale, g.grid_lists);
        app->rows_barred = false;
        grid_extent(app, &g, 0, &top, &height, &bottom);
        if (grid_bar(app, &g, &track, &thumb)) { /* a finger's and a thumb's to scroll */
            bar_draw(app, track, thumb, app->bar_hold == GUI_LIST_GRID + 1);
            bar_keep(app, g.grid_rects[UI_GAMES_GRID_TILES], track, thumb, (size_t)bottom,
                     (size_t)g.grid_rects[UI_GAMES_GRID_TILES].h, GUI_LIST_GRID);
        }
    } else {
        GuiRect track, thumb;
        app->rows_barred = games_bar(app, &g, &track, &thumb);
        ui_games_draw(app, g.rects, app->scale, g.lists);
        app->rows_barred = false;
        if (games_bar(app, &g, &track, &thumb)) bar_draw(app, track, thumb, app->bar_held);
    }
    /* beside it the focused row: what it is and its path, whole; the page, and where the games are looked for. The
       tips say what it is (a game's kind) or does, which the box does not say again */
    name = games_focus(app, &g, &about, kind, sizeof kind);
    r = inner(app, g.place.side);
    if (g.place.side.h) {
        y = pane(app, g.place.side, name);
        r = gui_rect(r.x, y, r.w, g.place.side.y + g.place.side.h - y);
        y += wrap_in(app, r, r.x, y, about, app->browser.selected != 0, ink) * cell_h(app);
        if (kind[0]) y += wrap_in(app, r, r.x, y, kind, 0, soft) * cell_h(app);
        if (app->debug_focus[0]) { /* what the game will say over its picture */
            snprintf(line, sizeof line, snx_tr("Debug on: %s"), app->debug_focus);
            y += wrap_in(app, r, r.x, y, line, 0, failure) * cell_h(app);
        }
        if (app->aot_focus && !app->made_here) /* a recompiled game Play compiles first: what was made goes */
            y += wrap_in(app, r, r.x, y, snx_tr("Needs Recompile"), 0, failure) * cell_h(app);
        else if (app->aot_focus) { /* how its code was made: a line, the side box's folders under it */
            made_text(&app->made, line, sizeof line);
            y += wrap_in(app, r, r.x, y, line, 0, soft) * cell_h(app);
        }
        if (app->aot_state == GUI_AOT_CACHE) {
            char size[32];
            gui_size_text(size, sizeof size, app->aot_info.size);
            snprintf(line, sizeof line, snx_tr("JIT cache: %s, O%c"), size, app->aot_info.opt);
            y += wrap_in(app, r, r.x, y, line, 0, soft) * cell_h(app);
            if (app->aot_match)
                snprintf(line, sizeof line, snx_tr("AOT build: O%c"), app->aot_build_opt);
            else /* the two the pack needs equal, named: the cache's JIT level and the build's AOT level */
                snprintf(line, sizeof line, snx_tr("AOT build: O%c, its JIT cache O%c: a pack needs one level"),
                         app->aot_build_opt, app->aot_info.opt);
            y += wrap_in(app, r, r.x, y, line, 0, app->aot_match ? soft : failure) * cell_h(app);
        } else if (app->aot_state == GUI_AOT_NO_CACHE) {
            y += wrap_in(app, r, r.x, y, snx_tr("JIT cache: none yet. Play once to collect one."), 0, soft) *
                 cell_h(app);
        } else if (app->aot_state == GUI_AOT_BAD) {
            y += wrap_in(app, r, r.x, y, snx_tr("JIT cache: unreadable. Play once to rebuild it."), 0, failure) *
                 cell_h(app);
        }
        y += games_queued(app, r, y);
        y += cell_h(app);
        if (!app->games.count)
            y += wrap_in(app, r, r.x, y, snx_tr(SAY_NO_GAMES), 0, soft) * cell_h(app);
        else {
            snprintf(line, sizeof line, snx_tr("%zu game(s)"), games_listed(app)); /* one page, which scrolls */
            y += wrap_in(app, r, r.x, y, line, 0, soft) * cell_h(app);
        }
        y += wrap_in(app, r, r.x, y, app->games_dir ? app->games_dir : "recompiled/", 1, soft) * cell_h(app);
        if (dumps_on(app)) { /* Dumps off: the games made alone */
            char* dumps = app->data_dir ? snx_path_join(app->data_dir, "dumps") : NULL;
            y += wrap_in(app, r, r.x, y, dumps ? dumps : "dumps/", 1, soft) * cell_h(app);
            free(dumps);
        }
        for (i = 0; i < (size_t)app->folder_count; ++i)
            if (dumps_on(app) || app->folders_library[i] == GUI_LIBRARY_RECOMPILED)
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
    place_screen(app, &k->place, shown_keys(app, keys, sizeof keys), GUI_SHOWS_TABS);
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
                 app->keys_path ? app->keys_path : snx_tr("No home folder"));
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
        y += wrap_in(app, r, r.x, y, snx_tr("Keys installed here: installing again replaces them"), 0,
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
        wrap_in(app, text, text.x, y + cell_h(app) / 2, snx_tr("Aborting..."), 0, rgb(t->failure));
    status_box(app, p.status);
    foot(app, &p, app->file ? app->file : "", rgb(t->soft), "");
    app->scale = foot_scale(app); /* the ESC line where the keys go, in their letters */
    text = foot_text(app, p.keys, t->footer_box);
    if (app->finished || app->aborting)
        prompt_in(app, text, text.y, line, app->finished ? snx_tr(SAY_DONE) : snx_tr("Aborting..."), rgb(t->failure));
    else
        text_in(app, text, text.x + text.w - gui_text_cols(snx_tr(SAY_ABORT)) * cell_w(app), text.y, snx_tr(SAY_ABORT),
                rgb(t->failure));
    app->scale = s;
}
/* ---- the Compile tab: the queue's recompile in focus, its loading screen or its output, in letters a half step
   under the text's; beside it the queue, a row each (ui.txt's scene Queue), and the Status box ---- */
typedef struct {
    GuiPlace place;
    GuiRect rects[UI_QUEUE_ITEMS];
    UiList lists[UI_QUEUE_ITEMS]; /* the rows' at UI_QUEUE_ROWS: row i is the queue's job i */
} GuiQueueBoxes;
/* the frame as Wide lays it out, the output three fifths, the queue's rows under the side box's head */
static void queue_boxes(App* app, GuiQueueBoxes* q) {
    char keys[256];
    GuiRect r;
    int y, s = app->scale;
    place_screen(app, &q->place, shown_keys(app, keys, sizeof keys),
                 GUI_SHOWS_STATUS | GUI_SHOWS_TABS | GUI_SHOWS_WIDE | GUI_SHOWS_SIDE);
    r = inner(app, q->place.side);
    y = q->place.side.y + head_h(app) + 2 * s; /* where pane() starts its text */
    ui_queue_layout(gui_rect(r.x, y, r.w, SDL_max(0, q->place.side.y + q->place.side.h - y - 2 * s)), s, q->rects);
    memset(q->lists, 0, sizeof q->lists);
    q->lists[UI_QUEUE_ROWS].focus = (size_t)SDL_max(app->queue_focus, 0);
    list_follow(app, GUI_LIST_QUEUE, ui_queue_follow, ui_queue_row, q->rects, UI_QUEUE_ROWS, (size_t)app->queue.count,
                &q->lists[UI_QUEUE_ROWS]);
}
/* the output box of its console layout in cells of its letters: as the Compile tab lays it out, whichever screen
   shows */
static void job_console_size(App* app, int* cols, int* rows) {
    GuiQueueBoxes q;
    GuiLoadView view = app->menu.view;
    int half, top;
    app->menu.view = GUI_LOAD_RECOMPILE;
    queue_boxes(app, &q);
    app->menu.view = view;
    top = q.place.list.y + head_h(app) + 2 * app->scale;
    half = text_half(app);
    *cols = SDL_max(1, inner(app, q.place.list).w / cell_w(app));
    *rows = SDL_max(1, (q.place.list.y + q.place.list.h - top - 2 * app->scale) / cell_h(app));
    app->half = half;
}
/* the Compile tab's row: its game, AOT pack for B's, and where it is (job_where); one that failed in the failure
   colour */
bool ui_queue_rows_get_row_info(App* app, size_t row, int width, UiRowInfo* info) {
    char where[64];
    const GuiJob* job;
    (void)width; /* a name too long for its row slides, as Games' rows do */
    if (row >= (size_t)app->queue.count) return false;
    job = &app->queue.job[row];
    job_where(app, (int)row, false, where, sizeof where);
    if (job->kind == GUI_JOB_AOT)
        snprintf(app->row_text, sizeof app->row_text, snx_tr("%s, AOT pack  %s"), gui_queue_name(&app->queue, (int)row),
                 where);
    else
        snprintf(app->row_text, sizeof app->row_text, "%s  %s", gui_queue_name(&app->queue, (int)row), where);
    info->label = app->row_text;
    if (job->state == GUI_JOB_FAILED) info->attr |= UI_ATTR_FAILURE;
    return true;
}
static void queue_play(App* app);
/* a row under the pointer takes the focus, its recompile shown beside; a click on one that ended well plays its game,
   as Enter does (a finger's first tap the focus, its second the play) */
void ui_queue_rows_row_action_cb(App* app, size_t row, uint32_t hit) {
    bool again = (int)row == app->queue_focus;
    app->queue_focus = (int)row;
    if ((hit & UI_HIT_CLICK) && again && app->queue.job[row].state == GUI_JOB_OK) queue_play(app);
}
/* a recompile as its panel shows it once it has no output to show: one waiting, or one a later one's replaced */
static void job_note(App* app, GuiRect r, int i) {
    const GuiJob* job = &app->queue.job[i];
    char line[300], where[64];
    GuiRect text = inner(app, r);
    int y = pane(app, r, gui_queue_name(&app->queue, i));
    text = gui_rect(text.x, y, text.w, r.y + r.h - y);
    if (job->state == GUI_JOB_WAITING) {
        job_where(app, i, false, where, sizeof where);
        snprintf(line, sizeof line, snx_tr("Waits in the queue: %s"), where);
    } else
        snprintf(line, sizeof line, "%s", job->said);
    y += wrap_in(app, text, text.x, y, line, 0,
                 rgb(job->state == GUI_JOB_FAILED ? app->theme.failure : app->theme.ink)) *
         cell_h(app);
    wrap_in(app, text, text.x, y, gui_queue_file(&app->queue, i), 1, rgb(app->theme.soft));
}
/* job i's panel in r: its loading screen in its own colours (a theme's steps layout), or its output under the Output
   head (the console layout), its letters a half step under the text's */
static void job_panel(App* app, GuiRect r, int i) {
    GuiRunView v = job_view(app, i);
    const GuiTheme* t = &app->theme;
    if (!v.steps) {
        job_note(app, r, i);
        return;
    }
    if (t->run == GUI_RUN_STEPS) {
        SDL_Rect safe, at;
        int cols = app->cols, rows = app->rows, half;
        char band[400], usage[256], how[200];
        gui_usage_text(usage, sizeof usage, app->job_usage.available, app->job_usage.memory_bytes,
                       app->job_usage.cpu_percent, SDL_GetNumLogicalCPUCores());
        job_speed(app, how, sizeof how);
        snprintf(band, sizeof band, "%s  %s", usage, how);
        fill_rect(app, r, t->run_colors.page);
        SDL_GetRenderViewport(app->renderer, &safe);
        at.x = safe.x + r.x;
        at.y = safe.y + r.y;
        at.w = r.w;
        at.h = r.h;
        SDL_SetRenderViewport(app->renderer, &at);
        half = text_half(app);
        app->cols = SDL_min(r.w / cell_w(app), GUI_MAX_COLS);
        app->rows = SDL_min(r.h / cell_h(app), GUI_MAX_ROWS);
        steps_draw(app, &v, true, band, snx_tr(v.failed ? SAY_QUEUE_FAILED : SAY_QUEUE_DONE));
        app->cols = cols;
        app->rows = rows;
        app->half = half;
        SDL_SetRenderViewport(app->renderer, &safe);
        outline(app, r, t->dim);
    } else {
        GuiRect text = inner(app, r);
        int y = pane(app, r, snx_tr("Output")), half = text_half(app), x, k;
        int cols = text.w / cell_w(app), rows = (r.y + r.h - y - 2 * app->scale) / cell_h(app);
        SDL_SetTextureColorMod(app->font, t->ink.r, t->ink.g, t->ink.b);
        for (k = 0; k < SDL_min(rows, v.console->rows); ++k)
            for (x = 0; x < SDL_min(cols, v.console->cols); ++x)
                if (v.console->cells[k][x] != ' ')
                    glyph(app, (float)(text.x + x * cell_w(app)), (float)(y + k * cell_h(app)), v.console->cells[k][x]);
        app->half = half;
    }
}
/* the tips under the head: how the one in focus is, what runs after it */
static void queue_tips(App* app, char* out, size_t size) {
    int i = app->queue_focus, run = gui_queue_running(&app->queue), waiting = gui_queue_waiting(&app->queue);
    const GuiJob* job;
    size_t n;
    out[0] = 0;
    if (i < 0 || i >= app->queue.count) return;
    job = &app->queue.job[i];
    if (job->state == GUI_JOB_RUNNING)
        snprintf(out, size, snx_tr("Compiling %s: %d%%"), gui_queue_name(&app->queue, i), SDL_max(job->percent, 0));
    else if (job->state == GUI_JOB_WAITING) {
        char where[64];
        job_where(app, i, false, where, sizeof where);
        snprintf(out, size, snx_tr("%s waits in the queue: %s"), gui_queue_name(&app->queue, i), where);
    } else
        snprintf(out, size, "%s: %s", gui_queue_name(&app->queue, i), job->said);
    n = strlen(out);
    if (run >= 0 && run != i && n < size) { /* after how one ended, its own period */
        snprintf(out + n, size - n, "%s", n && out[n - 1] == '.' ? " " : ". ");
        n = strlen(out);
        snprintf(out + n, size - n, snx_tr("%s compiles now (%d%%)"), gui_queue_name(&app->queue, run),
                 SDL_max(app->queue.job[run].percent, 0));
    }
    n = strlen(out);
    if (waiting && n < size) snprintf(out + n, size - n, snx_tr(", %d in the queue"), waiting);
}
static void draw_recompile(App* app) {
    GuiQueueBoxes q;
    char keys[256], tips[600];
    queue_boxes(app, &q);
    chrome(app, &q.place);
    if (app->queue_focus >= 0 && app->queue_focus < app->queue.count) job_panel(app, q.place.list, app->queue_focus);
    if (q.place.side.h) {
        pane(app, q.place.side, snx_tr("Queue"));
        ui_queue_draw(app, q.rects, app->scale, q.lists);
    }
    status_box(app, q.place.status);
    queue_tips(app, tips, sizeof tips);
    screen_foot(app, &q.place, tips, shown_keys(app, keys, sizeof keys));
}
/* the Compile tab's keys, each where it does something: Del on one waiting or ended, ESC aborting the one running or
   back to Games, Enter playing one that ended well */
static void queue_said(const App* app, bool pad, const char** parts, int* n) {
    int i = app->queue_focus;
    GuiJobState state = i >= 0 && i < app->queue.count ? app->queue.job[i].state : GUI_JOB_WAITING;
    said_add(parts, n, pad ? SAY_PAD_QUEUE_MOVE : SAY_QUEUE_MOVE);
    if (state == GUI_JOB_OK) said_add(parts, n, pad ? SAY_PAD_QUEUE_PLAY : SAY_QUEUE_PLAY);
    said_add(parts, n, pad ? SAY_PAD_QUEUE_TAB : SAY_QUEUE_TAB);
    if (state != GUI_JOB_RUNNING) said_add(parts, n, pad ? SAY_PAD_QUEUE_DEL : SAY_QUEUE_DEL);
    said_add(parts, n,
             state == GUI_JOB_RUNNING ? (pad ? SAY_PAD_QUEUE_ABORT : SAY_QUEUE_ABORT)
                                      : (pad ? SAY_PAD_QUEUE_BACK : SAY_QUEUE_BACK));
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
    else if (app->menu.view == GUI_LOAD_RECOMPILE)
        draw_recompile(app);
    else if (pick_screen(app))
        draw_pick(app);
    else
        draw_menu(app);
    if (app->confirm.shown) render_ask(app);
    if (idle && app->dests_shown && !app->confirm.shown) render_dests(app);
    if (idle && app->picks_shown && !app->confirm.shown) render_picks(app);
    if (idle && app->users_shown && !app->confirm.shown) render_users(app);
    if (idle && app->addons_shown && !app->confirm.shown) render_addons(app);
    if (idle && app->options_shown && !app->confirm.shown) render_view(app);
    if (idle && app->states_shown && !app->confirm.shown) render_states(app);
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
        case GUI_VITA: return "libscarletnx-vita.so";
        case GUI_GBA: return "libscarletnx-agb.so";
#else
        case GUI_SWITCH: return "scarletnx";
        case GUI_LINUX: return "scarletnx-gnuabi";
        case GUI_3DS: return "scarletnx-3ds";
        case GUI_VITA: return "scarletnx-vita";
        case GUI_GBA: return "scarletnx-agb";
#endif
        default: return NULL;
    }
}
int (*snx_menu_has_driver)(const char* name) = NULL;
/* whether item's driver is the menu's own program, run with the driver's name first (snx_menu_has_driver) */
static bool driver_in_self(const App* app, int item) {
    const char* name = driver_name(item);
    return app->self && item != GUI_SWITCH && name && snx_menu_has_driver && snx_menu_has_driver(name);
}
static char* find_driver(const App* app, int item) {
    const char* name = driver_name(item);
    if (app->self && (item == GUI_SWITCH || driver_in_self(app, item))) return snx_strdup(app->self);
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
/* a load's driver is there to run: on iOS the Switch's is the app's own, on a thread (launch) */
static bool driver_there(const App* app, int item) {
    char* driver;
#ifdef SDL_PLATFORM_IOS
    if (item == GUI_SWITCH) return true;
#endif
    driver = find_driver(app, item);
    free(driver);
    return driver != NULL;
}
static uint64_t text_hash(uint64_t h, const char* text) {
    for (const char* c = text ? text : ""; *c; ++c) h = (h ^ (unsigned char)*c) * 1099511628211ull;
    return (h ^ 0xffu) * 1099511628211ull; /* where it ends, so "a" then "bc" is not "ab" then "c" */
}
/* the menu's loads whose driver is not there, a bit an item (1u << GUI_LINUX): their rows greyed, Enter on one saying
   which driver (#640). Looked for again once a second, as a driver put in or taken out shows, and at once when the
   folder they are looked for in, the menu's own program or PATH change */
static unsigned loads_off(App* app) {
    uint64_t key = text_hash(text_hash(text_hash(1469598103934665603ull, app->drivers), app->self), getenv("PATH"));
    Uint64 now = SDL_GetTicks();
    if (app->loads_at && key == app->loads_key && now - app->loads_at < 1000) return app->loads_off;
    app->loads_off = 0;
    for (int item = GUI_SWITCH; item <= GUI_GBA; ++item)
        if (!driver_there(app, item)) app->loads_off |= 1u << item;
    app->loads_key = key;
    app->loads_at = SDL_max(now, 1);
    return app->loads_off;
}
/* what argv prints, NULL unless it ends with 0, killed after ten seconds; owned. On a process of its own: the menu's
   run and the queue's recompile go on */
static char* capture(App* app, const char* const* argv) {
    char buffer[4096], *out = snx_strdup("");
    size_t size = 0;
    int status = 1, n;
    Uint64 until = SDL_GetTicks() + 10000;
    GuiProcess* process = gui_process_create();
    if (!process) {
        snprintf(app->message, sizeof app->message, "%s", snx_tr("Cannot start the run: out of memory"));
        status = -1;
    } else if (!gui_process_start(process, argv, app->message, sizeof(app->message)))
        status = -1;
    while (status >= 0 && !gui_process_finished(process, &status) && SDL_GetTicks() < until + 5000) {
        char* grown;
        n = gui_process_read(process, buffer, sizeof buffer);
        if (n > 0 && (grown = realloc(out, size + (size_t)n + 1)) != NULL) {
            out = grown;
            memcpy(out + size, buffer, (size_t)n);
            out[size += (size_t)n] = 0;
        } else if (n <= 0)
            SDL_Delay(1);
        if (SDL_GetTicks() > until) gui_process_terminate(process);
    }
    gui_process_destroy(process);
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
/* recommended.ini's [recommended] into the Settings' rows, which they mark and a run is told; one there that cannot be
   read is said, and no row is marked */
static void recommended_load(App* app) {
    char why[160], *path = app->config_dir ? snx_path_join(app->config_dir, REC_FILE) : NULL;
    RecFile f;
    if (path && rec_file_read(&f, path, why, sizeof why))
        snprintf(app->message, sizeof app->message,
                 snx_tr("%s cannot be read (%s): no row shows [recommended] until a test is applied again"), REC_FILE,
                 why);
    else if (path)
        gui_settings_recommended(&app->settings, &f);
    free(path);
}
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
    recommended_load(app);
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
    app->addons_shown = app->options_shown = false;
    app->states_shown = false;
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
     SNX_TR_NOOP("Built-in themes, then each themes/<name>/theme.ini"), 0, 0, 0, 0, NULL},
    {"", SNX_TR_NOOP("UI scale"), GUI_KEY_ACTION, offsetof(GuiAppForm, text), sizeof(((GuiAppForm*)0)->text), NULL, 0,
     SNX_TR_NOOP("1x to 4x. A new one asks to be kept"), 0, 0, 0, 0, NULL},
    {"", SNX_TR_NOOP("Language"), GUI_KEY_ACTION, offsetof(GuiAppForm, language), sizeof(((GuiAppForm*)0)->language),
     NULL, 0, SNX_TR_NOOP("The launcher's language. Auto: the system's, else English"), 0, 0, 0, 0, NULL},
    {"", SNX_TR_NOOP("Animations"), GUI_KEY_ACTION, offsetof(GuiAppForm, animations),
     sizeof(((GuiAppForm*)0)->animations), NULL, 0,
     SNX_TR_NOOP("Screens and dialogs fade in and out, 0.15 s. Off: none"), 0, 0, 0, 0, NULL},
    {"", SNX_TR_NOOP("Side panel"), GUI_KEY_ACTION, offsetof(GuiAppForm, side), sizeof(((GuiAppForm*)0)->side), NULL, 0,
     SNX_TR_NOOP(
         "The panel beside the list. Auto: where it fits. On: under the list where it does not. F2 turns it off and on"),
     0, 0, 0, 0, NULL},
    {"", SNX_TR_NOOP("Background recompile"), GUI_KEY_ACTION, offsetof(GuiAppForm, background),
     sizeof(((GuiAppForm*)0)->background), NULL, 0,
     SNX_TR_NOOP("The Compile tab's: R's, B's and a game's first. Full: every core. Gentle: half of them, at a low "
                 "priority. Either goes gently while a game plays"),
     0, 0, 0, 0, NULL},
    {"", SNX_TR_NOOP("Theme Editor"), GUI_KEY_ACTION, 0, 0, NULL, 0,
     SNX_TR_NOOP("This theme's keys with a preview, saved as themes/<name>/theme.ini"), 0, 0, 0, 0, NULL},
    {"", SNX_TR_NOOP("Folders"), GUI_KEY_ACTION, offsetof(GuiAppForm, places), sizeof(((GuiAppForm*)0)->places), NULL,
     0, SNX_TR_NOOP("The data and the extractions, and the libraries Games lists: recompiled games and dumps"), 0, 0, 0,
     0, NULL},
    {"", SNX_TR_NOOP("Clean Old Settings"), GUI_KEY_ACTION, 0, 0, NULL, 0,
     SNX_TR_NOOP("Removes the settings this version no longer reads. Asks first"), 0, 0, 0, 0, NULL},
    {"", SNX_TR_NOOP("Move Old Files"), GUI_KEY_ACTION, offsetof(GuiAppForm, moves), sizeof(((GuiAppForm*)0)->moves),
     NULL, 0, SNX_TR_NOOP("Moves what an older version left to where this one looks. Asks first"), 0, 0, 0, 0, NULL},
    {"", SNX_TR_NOOP("Reset to Defaults"), GUI_KEY_ACTION, 0, 0, NULL, 0,
     SNX_TR_NOOP("Every setting of the groups after App to its default. Asks first"), 0, 0, 0, 0, NULL}};
enum {
    GUI_APP_THEME,
    GUI_APP_TEXT,
    GUI_APP_LANGUAGE,
    GUI_APP_ANIMATIONS,
    GUI_APP_SIDE,
    GUI_APP_BACKGROUND,
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
/* UI scale's steps, 1x to 4x: step k is text_size k + 1 */
#define GUI_SIZE_STEPS GUI_TEXT_MAX
/* a text_size as Settings show it, the scale in effect: 1x while a start has yet to measure its window */
static void size_name(int size, char* out, size_t n) { snprintf(out, n, "%dx", SDL_max(size, 1)); }
/* the UI scale as Settings show it: the one kept, and the one in use where the window is too short for it (layout) */
static void size_shown(const App* app, char* out, size_t n) {
    if (app->scale < app->menu.text_size)
        snprintf(out, n, snx_tr("%dx (%dx in this window)"), app->menu.text_size, app->scale);
    else
        size_name(app->menu.text_size, out, n);
}
static void app_values(App* app) {
    GuiAppForm* f = &app->app_form;
    char games[4200], work[4200];
    int moved;
    snprintf(f->theme, sizeof f->theme, "%s", app->theme.name);
    size_shown(app, f->text, sizeof f->text);
    if (app->language)
        snprintf(f->language, sizeof f->language, "%s", snx_gui_languages[app->language - 1].name);
    else /* and what it is now */
        snprintf(f->language, sizeof f->language, snx_tr("Auto: %s"), snx_gui_languages[snx_tr_language()].name);
    snprintf(f->animations, sizeof f->animations, "%s", app->animations_off ? snx_tr("Off") : snx_tr("On"));
    snprintf(f->side, sizeof f->side, "%s", snx_tr(side_names[app->side_box]));
    snprintf(f->background, sizeof f->background, "%s", snx_tr(background_names[app->background_gentle]));
    moved = 0;
    for (int i = 0; i < GUI_PLACES; ++i) moved += app->place[i] != NULL;
    if (moved && app->folder_count)
        snprintf(f->places, sizeof f->places, snx_tr("%d custom, %d Extra(s)"), moved, app->folder_count);
    else if (moved)
        snprintf(f->places, sizeof f->places, snx_tr("%d custom"), moved);
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
static const char dumps_row[] = SNX_TR_NOOP("Dumps put here are listed (.nsp, .xci and .nro), its subfolders' too "
                                            "while it says + subfolders. It stays on the list");
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
        snprintf(note, sizeof note, snx_tr("%s removed from the library. Its games stay in the folder"), path);
    free(path);
    open_folders(app, row);
    snprintf(app->editor.note, sizeof app->editor.note, "%s", note);
}
/* a game folder taken off the Library asked about first (GUI_ASK_FOLDER): only the list changes */
static void folder_off_ask(App* app, int folder) {
    app->folder_off = folder;
    snprintf(app->ask_line, sizeof app->ask_line,
             app->folders_library[folder] == GUI_LIBRARY_RECOMPILED
                 ? snx_tr("Remove %s from the Recompiled Library? Its games stay in the folder.")
                 : snx_tr("Remove %s from the Dumps Library? Its games stay in the folder."),
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
   folders in it asks first whether their games are listed too (Only This Folder, With Subfolders); another is added
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
/* the icon of a group by its name (group_title's English), and of a group's row that opens a page by the page's: none
   for one these do not name, which gui_sdl's test refuses for every section and page of settings.txt */
static unsigned group_icon(const char* name) {
    static const struct {
        const char* name;
        unsigned icon;
    } icons[] = {{"App", GUI_ICON_APP_WINDOW},  {"Launch", GUI_ICON_ROCKET},
                 {"Game", GUI_ICON_JOYSTICK},   {"Display", GUI_ICON_MONITOR},
                 {"Graphics", GUI_ICON_IMAGE},  {"Performance", GUI_ICON_GAUGE},
                 {"Overlay", GUI_ICON_POINTER}, {"Controllers", GUI_ICON_GAMEPAD_2},
                 {"Network", GUI_ICON_WIFI},    {"User", GUI_ICON_USER},
                 {"Debug", GUI_ICON_BUG}};
    for (size_t i = 0; i < SDL_arraysize(icons); ++i)
        if (!strcmp(icons[i].name, name)) return icons[i].icon;
    return GUI_ICON_NONE;
}
static unsigned page_icon(const char* page) {
    return !strcmp(page, "LLVM Internals") ? GUI_ICON_CPU
           : !strcmp(page, "Buttons")      ? GUI_ICON_LAYOUT_GRID
           : !strcmp(page, "Button map")   ? GUI_ICON_GAMEPAD_DIRECTIONAL
                                           : GUI_ICON_NONE;
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
    app->clean_found = -1; /* looked at again as its rows show */
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
static const char* const place_holds[GUI_PLACES] = {
    SNX_TR_NOOP("recompiled/, dumps/, themes/ and prod.keys"), SNX_TR_NOOP("The recompiled games"),
    SNX_TR_NOOP("Each dump's extraction"), SNX_TR_NOOP("The games' logs, threads.txt and recent.txt"),
    SNX_TR_NOOP("units/, the shader caches and the dumps' icons")};
/* Settings' row: its folder holds app.ini, which names the others, so the folder is moved outside the launcher */
#ifdef SNX_XDG
static const char config_row[] =
    SNX_TR_NOOP("app.ini, which names the other folders, and each game's .ini: XDG_CONFIG_HOME moves it");
#else
static const char config_row[] = SNX_TR_NOOP(
    "app.ini, which names the other folders, and each game's .ini: a portable copy (.scarletnx beside the program) "
    "keeps its own");
#endif
/* the folders as Folders has them: each one picked, else its default from the data folder, the menu's own one first */
/* the folder place takes while none is picked, with the data folder in use (its own default: home_dir): owned */
static char* place_default(const App* app, int place) {
    if (place == GUI_PLACE_DATA) return app->home_dir ? snx_strdup(app->home_dir) : NULL;
    if (place == GUI_PLACE_STATE || place == GUI_PLACE_CACHE) /* the menu's own, as menu_folder finds it */
        return snx_menu_kind_default(app->home_dir ? app->home_dir : app->data_dir,
                                     place == GUI_PLACE_STATE ? SNX_USER_STATE : SNX_USER_CACHE);
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
    free(app->state_dir);
    app->state_dir =
        app->place[GUI_PLACE_STATE] ? snx_strdup(app->place[GUI_PLACE_STATE]) : place_default(app, GUI_PLACE_STATE);
    free(app->cache_dir);
    app->cache_dir =
        app->place[GUI_PLACE_CACHE] ? snx_strdup(app->place[GUI_PLACE_CACHE]) : place_default(app, GUI_PLACE_CACHE);
    free(app->keys_path);
    app->keys_path = device_keys_path(app);
}
static const char* place_path(const App* app, int place) {
    return place == GUI_PLACE_DATA    ? app->data_dir
           : place == GUI_PLACE_GAMES ? app->games_dir
           : place == GUI_PLACE_STATE ? app->state_dir
           : place == GUI_PLACE_CACHE ? app->cache_dir
                                      : app->work_dir;
}
/* a folder outside the app's own storage on Android, where every file access goes through its checks (FUSE) */
static bool outside_app(const App* app, const char* path) {
    return app->own_dir && !snx_path_within(path, app->own_dir);
}
static const char* const group_titles[GUI_FOLDER_GROUPS + 1] = {
    SNX_TR_NOOP("App Folders"), SNX_TR_NOOP("Dumps Library"), SNX_TR_NOOP("Recompiled Library"),
    SNX_TR_NOOP("Defaults")}; /* the last's, Reset to Defaults' */
/* row r's kind (GuiFolderRow), its place, folder or library in *at and its group in *group; -1 past the rows (the
   last, Reset to Defaults, is the form's own) */
static int folder_row(const App* app, int r, int* at, int* group) {
    /* -1: Settings, between Data and the two whose folders app.ini in it names */
    static const int app_places[] = {GUI_PLACE_DATA, -1, GUI_PLACE_STATE, GUI_PLACE_CACHE, GUI_PLACE_WORK};
    static const int libraries[GUI_LIBRARIES] = {GUI_LIBRARY_DUMPS, GUI_LIBRARY_RECOMPILED}; /* the dumps first */
    int i, n = 0, k, l;
    *group = GUI_GROUP_APP;
    for (i = 0; i < (int)SDL_arraysize(app_places); ++i)
        if (n++ == r) {
            *at = app_places[i];
            return app_places[i] < 0 ? GUI_ROW_CONFIG : GUI_ROW_PLACE;
        }
    for (k = 0; k < GUI_LIBRARIES; ++k) {
        l = libraries[k];
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
           : kind == GUI_ROW_CONFIG ? app->config_dir
           : kind == GUI_ROW_DUMPS  ? app->library_dumps
           : kind == GUI_ROW_FOLDER ? app->folders[at]
                                    : NULL;
}
/* Folders' screen lists each group's title before its rows (every group has some), then a blank entry and the last
   row in a group of its own, Defaults: the list's entries, row r's, and entry e's row (-1 a title, its group in
   *group; the last's group GUI_FOLDER_GROUPS, the blank one's GUI_FOLDER_GROUPS + 1) */
static int folder_entries(const App* app) { return folder_rows(app) + GUI_FOLDER_GROUPS + 3; }
static int folder_entry(const App* app, int r) {
    int at, group, rows = folder_rows(app);
    if (r >= rows) return rows + GUI_FOLDER_GROUPS + 2;
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
    *group = e == rows + GUI_FOLDER_GROUPS ? GUI_FOLDER_GROUPS + 1 : GUI_FOLDER_GROUPS;
    return e < rows + GUI_FOLDER_GROUPS + 2 ? -1 : rows;
}
/* the key Folders' focused row takes, in parts, when it does something there: a place picks another, and one picked
   goes back to its default; an Extra is taken off; Add a folder adds one; Reset to Defaults resets a place picked.
   How many (0 or 1, 2 for a place picked) */
/* Games' focused row a game's of any console (a recent run's or a listed one's; a heading none, a Linux program none),
   which R recompiles; *mods when it is a made Switch game, whose mods M opens */
static bool games_game_focus(const App* app, bool* mods) {
    size_t row = app->browser.selected, head = app->browser.head;
    size_t index = app->browser.page * app->browser.page_size + row - head;
    *mods = false;
    if (row < head && (int)row < recent_shown(app)) {
        const GuiRecentGame* r = &app->recent.game[recent_index(app, (int)row)];
        if (r->driver == GUI_LINUX) return false;
        *mods = r->driver == GUI_SWITCH && !gui_is_dump(r->path);
        return true;
    }
    if (row < head || index >= app->games.count || gui_games_heading(&app->games, index)) return false;
    *mods = !gui_is_dump(app->games.names[index]) && app->games_drivers[index] == GUI_SWITCH;
    return true;
}
/* Games' keys the focused row takes, in parts, when it does something there: R on a game, M on a made Switch one */
static void games_said(const App* app, bool pad, const char** parts, int* n) {
    bool mods;
    if (!games_game_focus(app, &mods)) return;
    said_add(parts, n, pad ? SAY_PAD_GAMES_R : SAY_GAMES_R);
    if (mods && (pad ? SAY_PAD_GAMES_M : SAY_GAMES_M)) said_add(parts, n, pad ? SAY_PAD_GAMES_M : SAY_GAMES_M);
    if (mods)
        said_add(parts, n, pad ? SAY_PAD_GAMES_STATES : SAY_GAMES_STATES); /* its save states' box: S, a pad's X */
}
/* a setting's Del (X), said where the focused row is not at its default */
static void settings_said(const App* app, bool pad, const char** parts, int* n) {
    const char* said = pad ? SAY_PAD_SETTINGS_DEFAULT : SAY_SETTINGS_DEFAULT;
    if (!said || app->form != GUI_FORM_GROUP || app->editor.typing) return;
    if (gui_settings_at_start(&app->settings, setting_of(app, app->editor.row))) return;
    said_add(parts, n, said);
}
static void folders_said(const App* app, bool pad, const char** parts, int* n) {
    int at, group, picked = 0, kind = folder_row(app, app->editor.row, &at, &group);
    for (int i = 0; i < GUI_PLACES; ++i) picked += app->place[i] != NULL;
    if (kind == GUI_ROW_PLACE) said_add(parts, n, pad ? SAY_PAD_CHANGE : SAY_CHANGE);
    if (kind == GUI_ROW_PLACE && app->place[at] && (pad ? SAY_PAD_DEFAULT : SAY_DEFAULT))
        said_add(parts, n, pad ? SAY_PAD_DEFAULT : SAY_DEFAULT);
    if (kind == GUI_ROW_FOLDER) said_add(parts, n, pad ? SAY_PAD_PICK : SAY_PICK);
    if (kind == GUI_ROW_FOLDER && (pad ? SAY_PAD_DELETE : SAY_DELETE))
        said_add(parts, n, pad ? SAY_PAD_DELETE : SAY_DELETE);
    if (kind == GUI_ROW_DUMPS || kind == GUI_ROW_CONFIG) said_add(parts, n, pad ? SAY_PAD_OPEN : SAY_OPEN);
    if (kind == GUI_ROW_ADD) said_add(parts, n, pad ? SAY_PAD_ADD : SAY_ADD);
    if (kind < 0 && picked) said_add(parts, n, pad ? SAY_PAD_RESET : SAY_RESET);
}
/* the library row r's group lists, -1 for the app's folders and the last */
static int folder_row_library(const App* app, int r) {
    int at, group;
    folder_row(app, r, &at, &group);
    return group == GUI_GROUP_RECOMPILED ? GUI_LIBRARY_RECOMPILED : group == GUI_GROUP_DUMPS ? GUI_LIBRARY_DUMPS : -1;
}
static const char extra_row[] = SNX_TR_NOOP("Removes it from the library; its games stay in the folder");
static const char add_recompiled_row[] =
    SNX_TR_NOOP("Pick, drop or paste a folder of recompiled games: Games lists them too");
static const char add_dumps_row[] = SNX_TR_NOOP("Pick, drop or paste a folder of dumps: Games lists them too");
/* Settings > App > Folders, every folder on one screen, a box a group: the app's (Data, Settings shown only, Logs,
   Caches, Extractions), then each library's Main, its Extras and Add a folder; the last row puts the places back to
   their defaults. A row is its name, its folder cut in its middle and what it is; above, what it holds, under the
   boxes its folder whole */
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
                     : app->place[at]                           ? snx_tr("custom")
                                                                : snx_tr("default"));
            snprintf(app->place_about[at], sizeof app->place_about[at], snx_tr("%s, %s free"), snx_tr(place_holds[at]),
                     left);
            snprintf(app->place_line[at], sizeof app->place_line[at], "%s%s%s", path ? path : snx_tr("(none)"),
                     why ? " " : "", why ? unfit_text(why) : "");
        } else if (kind == GUI_ROW_CONFIG) {
            k->key = SNX_TR_NOOP("Settings");
            k->about = config_row;
            snprintf(value, size, "%s", snx_tr("read-only"));
        } else if (kind == GUI_ROW_ADD) {
            k->key = SNX_TR_NOOP("Add Folder");
            k->about = at == GUI_LIBRARY_RECOMPILED ? add_recompiled_row : add_dumps_row;
        } else {
            k->key = kind == GUI_ROW_DUMPS ? SNX_TR_NOOP("Main") : SNX_TR_NOOP("Extra");
            k->about = kind == GUI_ROW_DUMPS ? dumps_row : extra_row;
            snprintf(value, size, "%s",
                     !path || !snx_is_dir(path) ? snx_tr("not found")
                     : kind == GUI_ROW_DUMPS    ? (app->dumps_shallow ? "" : snx_tr("+ subfolders"))
                     : app->folders_deep[at]    ? snx_tr("+ subfolders")
                                                : "");
        }
    }
    gui_editor_form(&app->editor, app->folder_keys, rows, &app->app_form, SNX_TR_NOOP("Reset to Defaults"));
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
/* the places a run reads from app.ini, not from the launcher: Logs and Caches */
static bool place_read_by_runs(int place) { return place == GUI_PLACE_STATE || place == GUI_PLACE_CACHE; }
/* the places picked kept in app.ini, first, as Logs and Caches reach the runs only through it: 0, else nonzero with
   those two back to before[] and note saying so */
static int places_keep(App* app, char* const* before, char* note, size_t size) {
    bool back = false;
    if (!app_write(app)) return 0;
    for (int i = 0; i < GUI_PLACES; ++i)
        if (place_read_by_runs(i) &&
            (before[i] ? !app->place[i] || strcmp(before[i], app->place[i]) : !!app->place[i])) {
            free(app->place[i]);
            app->place[i] = before[i] ? snx_strdup(before[i]) : NULL;
            back = true;
        }
    snprintf(note, size,
             back ? snx_tr("Logs and Caches not changed: %s, which the runs read them from, could not be written")
                  : snx_tr("Used, not kept for the next start: %s could not be written"),
             app->app_file);
    return 1;
}
#define GUI_RUN_LOGS 8 /* launchers open at once on one Logs folder, each with run logs of its own */
/* the run logs of the Logs folder in use, made as the menu opens it (open_files) and as Folders' Logs changes: the
   first pair no other launcher holds, last-run.log and last-recompile.log, else last-run-2.log and last-recompile-2.log
   and on (#675), held until the menu closes or takes another folder; the log says which */
static void run_log_follow(App* app) {
    char* state = menu_folder(app, SNX_USER_STATE, !app->look_only);
    int n, held = 1;
    if (!state) return;
#ifndef SDL_PLATFORM_IOS /* there in Documents' logs/, which the Files app shows (initialize) */
    if (!app->look_only) gui_log_folder(state);
#endif
    if (app->run_locked) snx_unlock_file(app->run_lock);
    app->run_locked = false;
    free(app->run_log);
    free(app->job_log);
    app->run_log = app->job_log = NULL;
    for (n = 1; n <= GUI_RUN_LOGS && !app->run_log; ++n) {
        char* run = n == 1 ? snx_strdup("last-run.log") : snx_format("last-run-%d.log", n);
        char* job = n == 1 ? snx_strdup("last-recompile.log") : snx_format("last-recompile-%d.log", n);
        char* path = snx_path_join(state, run);
        held = app->look_only ? 0 : snx_lock_file(path, &app->run_lock);
        if (held <= 0) {
            app->run_locked = held == 0 && !app->look_only;
            app->run_log = path;
            app->job_log = snx_path_join(state, job);
            path = NULL;
        }
        free(path);
        free(job);
        free(run);
    }
    if (held < 0) { /* the first that could not be opened: kept unheld, as before */
        char why[160];
        gui_log("%s cannot be held for this launcher's runs (%s): another launcher may write it too", app->run_log,
                snx_os_error(why, sizeof why));
    } else if (!app->run_log) {
        snprintf(app->message, sizeof app->message,
                 snx_tr("%d launchers hold the run logs of %s: this one's runs keep none"), GUI_RUN_LOGS, state);
        say_failure(app, app->message);
    } else if (!app->look_only)
        gui_log("its runs' logs %s and %s", app->run_log, app->job_log);
    free(state);
}
/* a place picked, absolute, or its default (NULL), kept in app.ini */
static void place_use(App* app, int place, char* absolute) {
    char note[sizeof app->editor.note], *before[GUI_PLACES];
    int failed;
    for (int i = 0; i < GUI_PLACES; ++i) before[i] = app->place[i];
    app->place[place] = absolute;
    failed = places_keep(app, before, note, sizeof note);
    free(before[place]);
    places_apply(app);
    space_start(app);
    if (place == GUI_PLACE_STATE) run_log_follow(app);
    if (app->editing && app->form == GUI_FORM_FOLDERS) open_folders(app, folder_row_of(app, GUI_ROW_PLACE, place));
    if (failed)
        say_failure(app, note);
    else
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
    snprintf(app->ask_line, sizeof app->ask_line, snx_tr("Reset to Defaults? %s. Nothing is moved."), list);
    gui_confirm_open(&app->confirm, GUI_ASK_PLACES, false);
}
/* its answer: every place back to its default */
static void places_reset(App* app) {
    char note[sizeof app->editor.note], *before[GUI_PLACES];
    int failed;
    for (int i = 0; i < GUI_PLACES; ++i) {
        before[i] = app->place[i];
        app->place[i] = NULL;
    }
    failed = places_keep(app, before, note, sizeof note);
    bool logs = before[GUI_PLACE_STATE] && !app->place[GUI_PLACE_STATE]; /* Logs back to its default */
    for (int i = 0; i < GUI_PLACES; ++i) free(before[i]);
    places_apply(app);
    space_start(app);
    if (logs) run_log_follow(app);
    if (app->editing && app->form == GUI_FORM_FOLDERS) open_folders(app, folder_rows(app)); /* the last row */
    if (failed) say_failure(app, note);
    place_note(app, failed ? note : snx_tr("Reset to Defaults: nothing was moved"));
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
/* the games made in dir (recompiled/ or a folder of the Library's recompiled ones), each with the dump it names */
static void made_read_dir(MadeGames* made, const char* dir) {
    char** names = NULL;
    size_t count = 0, i;
    MadeGame* grown;
    if (!dir || snx_list_dir(dir, &names, &count) || !count) return;
    if (!(grown = (MadeGame*)realloc(made->game, (made->count + count) * sizeof *grown))) {
        snx_free_strings(names, count);
        return;
    }
    made->game = grown;
    for (i = 0; i < count; ++i) {
        char *program = gui_folder_game(dir, names[i]), *folder, *dump;
        int recorded = 0;
        if (!program) continue;
        folder = snx_path_join(dir, names[i]);
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
/* the games made in recompiled/ and in the Library's recompiled folders, the places a dump's game is made in
   (dest_dirs): a game the driver recompiles stays where it is */
static void made_read(const App* app, MadeGames* made) {
    *made = (MadeGames){0};
    made_read_dir(made, app->games_dir);
    for (int i = 0; i < app->folder_count; ++i)
        if (app->folders_library[i] == GUI_LIBRARY_RECOMPILED &&
            !(app->games_dir && snx_same_file(app->folders[i], app->games_dir)))
            made_read_dir(made, app->folders[i]);
}
/* the program of the game made from dump, else NULL */
#ifdef SDL_PLATFORM_IOS
/* a file's own name, without its folder */
static const char* leaf_of(const char* path) {
    const char* leaf = path;
    for (const char* s = path; *s; ++s)
        if (*s == '/' || *s == '\\') leaf = s + 1;
    return leaf;
}
#endif
static const char* made_program(const MadeGames* made, const char* dump) {
    size_t i;
    for (i = 0; i < made->count; ++i)
        if (snx_same_file(made->game[i].dump, dump)) return made->game[i].program;
#ifdef SDL_PLATFORM_IOS
    /* a dump picked outside dumps/ was prepared from its copy there, and the picked file may not be readable any more:
       the copy keeps its name */
    for (i = 0; i < made->count; ++i)
        if (*leaf_of(dump) && !strcmp(leaf_of(made->game[i].dump), leaf_of(dump))) return made->game[i].program;
#endif
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
/* a Switch game's program in its folder (<folder>/<name>, a game's or its update's, .exe on Windows) that this build
   made with the mods its game plays now: its driver plays it as it is. One another build made, or this one with other
   mods, its driver recompiles first */
static bool made_here(const char* program) {
    char *base = snx_strdup(program), *made, *expected;
    bool here;
#ifdef _WIN32
    if (strlen(base) > 4 && !SDL_strcasecmp(base + strlen(base) - 4, ".exe")) base[strlen(base) - 4] = 0;
#endif
    made = snx_game_mods_stamp(base);
    expected = snx_game_stamp_for(SNX_ABI, made);
    here = snx_game_stamp_is(base, expected);
    free(expected);
    free(made);
    free(base);
    return here;
}
/* the game of the library made from file (a dump, package or ROM) when another build made it, or this one with other
   mods: Play on its dump compiles it again first, as its driver would on the run's screen */
static bool made_elsewhere(const App* app, const char* file) {
    MadeGames made;
    const char* program;
    bool elsewhere;
    made_read(app, &made);
    program = made_program(&made, file);
    elsewhere = program && gui_game_folder(program) && !made_here(program);
    made_free(&made);
    return elsewhere;
}
/* the recent rows as Games has their games: a dump or a package made into a game of recompiled/ since runs that game's
   program from then on, whichever console's, and a game of a folder of its own goes by the name Games shows it by, its
   control.nacp's */
static void recent_as_games(App* app) {
    MadeGames made;
    bool changed = false;
    int i;
    made_read(app, &made);
    for (i = app->recent.count - 1; i >= 0; --i) { /* from the oldest: an entry dropped moves none still to look at */
        const GuiRecentGame* r = &app->recent.game[i];
        const char* program = r->driver != GUI_LINUX && !r->prepared ? made_program(&made, r->path) : NULL;
        if (program && gui_recent_made(&app->recent, i, program)) changed = true;
    }
    made_free(&made);
    for (i = 0; i < app->recent.count; ++i) /* one file under two spellings (/var and /private/var) is one game */
        for (int j = i + 1; j < app->recent.count;)
            if (app->recent.game[i].driver == app->recent.game[j].driver &&
                snx_same_file(app->recent.game[i].path, app->recent.game[j].path)) {
                gui_recent_remove(&app->recent, j);
                changed = true;
            } else
                ++j;
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
    out = dump_out(app, file, nx ? NULL : app->menu.selected == GUI_VITA ? "vita" : "3ds");
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
/* Games' P, or the foot's: Settings > App > Folders, at the recompiled library's Add a folder, ESC back to Games */
static void folders_from_games(App* app) {
    app->group = 0;
    open_folders(app, folder_row_of(app, GUI_ROW_ADD, GUI_LIBRARY_RECOMPILED));
    app->folders_from_games = true;
    space_start(app);
}
static void picks_open_row(App* app, int row);
/* Select (with del, Del) on a row of Folders: a folder's box, to open it or, as the row allows, change it, put back its
   default or take it off the library (picks_open_row; dumps/ and Settings say what they are too); Del on a place asks
   before its default, on an Extra before it is taken off; Add a folder opens the picker for its library */
static void folder_select(App* app, int row, bool del) {
    int at, group, kind = folder_row(app, row, &at, &group);
    if (kind == GUI_ROW_PLACE && del)
        place_default_ask(app, at);
    else if (kind == GUI_ROW_FOLDER && del)
        folder_off_ask(app, at);
    else if ((kind == GUI_ROW_DUMPS || kind == GUI_ROW_CONFIG) && !del) {
        snprintf(app->editor.note, sizeof app->editor.note, "%s",
                 snx_tr(kind == GUI_ROW_DUMPS ? dumps_row : config_row));
        picks_open_row(app, row);
    } else if ((kind == GUI_ROW_PLACE || kind == GUI_ROW_FOLDER) && !del)
        picks_open_row(app, row);
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
    app->view_keys[0].about = SNX_TR_NOOP("The settings of All, or of player 1 to 8");
    gui_editor_form(&app->editor, app->view_keys, app->view_count, app->settings.values, NULL);
    app->editor.flat = true;
}
#define SAY_SETUP_NICKNAME SNX_TR_NOOP("nickname 4 to 20 of A-Z a-z 0-9 space . _ -")
/* the User group: its one setting, who the games play as, shown with every user of users.ini to rename */
static bool users_group(const App* app, int g) {
    return g > 0 && g < app->groups && !strcmp(app->group_keys[g].key, "User");
}
/* its rows, Current user then a user each with its nickname: how many */
static int users_fill(App* app) {
    int i, plays = settings_user(app);
    snprintf(app->users_form.plays, sizeof app->users_form.plays, snx_tr("User %d: %s"), plays,
             plays >= 1 && plays <= g_user_count ? g_users[plays - 1].nickname : snx_tr("none in users.ini"));
    app->users_keys[0] = (GuiThemeKey){"",
                                       SNX_TR_NOOP("Current user"),
                                       GUI_KEY_ACTION,
                                       offsetof(GuiUsersForm, plays),
                                       sizeof app->users_form.plays,
                                       NULL,
                                       0,
                                       SNX_TR_NOOP("The user the games play as"), 0, 0, 0, 0, NULL};
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
            SNX_TR_NOOP("Its nickname, as the games and Eden rooms show it: 4 to 20 of A-Z a-z 0-9 space . _ -"), 0,
            0, 0, 0, NULL};
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
/* Current user a step either way, round the users, kept as Settings' user */
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
    if (!app->users_dir || snx_users_rename(app->users_dir, number, nick, why, sizeof why)) {
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
static void list_games(App* app);
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
    else if (form == GUI_FORM_FOLDERS && app->folders_from_games) { /* Games again, its list with what was added */
        app->folders_from_games = false;
        list_games(app);
    } else if (form == GUI_FORM_FOLDERS && app->folders_from_menu)
        app->folders_from_menu = false; /* the menu again */
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
/* ---- UI scale: picked in a dialog over the App's rows, on its slider's steps, then used on OK and kept once said so,
   else back by itself after a while, as a screen mode is: a size can leave the menu too small or too big ---- */
#define GUI_TEXT_KEEP_MS 15000
#define GUI_WARN_MS       3500 /* a resolution warning's line and its row's flash */
#define GUI_WARN_BLINK_MS 150  /* the flash's on and off, the row in the warning colour then its own */
#define GUI_SIZE_BAR      19   /* the slider's columns where they fit: six a step, and the knob's */
/* the dialog's choices while it picks, then while it asks */
static const char* const size_choices[2][2] = {{SNX_TR_NOOP("OK"), SNX_TR_NOOP("Cancel")},
                                               {SNX_TR_NOOP("Keep"), SNX_TR_NOOP("Back")}};
static bool size_picking(const App* app) { return app->sizes_shown && !app->size_until; }
/* the dialog, its slider on the size in use a step on (step -1, 0 or 1), OK in focus */
static void sizes_open(App* app, int step) {
    app->size_pick = SDL_clamp(app->menu.text_size + step, 1, GUI_TEXT_MAX);
    app->size_until = 0;
    app->sizes.count = 2;
    app->sizes.focus = 0;
    app->sizes_shown = true;
}
/* the columns the widest step's name takes, where the dialog shows the one picked */
static int size_name_cols(void) {
    char name[16];
    size_name(GUI_TEXT_MAX, name, sizeof name);
    return gui_text_cols(name);
}
/* a size the window of now shows as a smaller one (scale_fits, as layout keeps its rows): greyed in the dialog */
static bool size_grey(const App* app, int size) {
    int width, height;
    area(app, &width, &height);
    return scale_fits(size, height, GUI_LEAST_ROWS) < size;
}
/* why size is greyed, in out: the height it needs and the window's */
static void size_reason(const App* app, int size, char* out, size_t n) {
    int width, height;
    area(app, &width, &height);
    snprintf(out, n, snx_tr("%dx needs a window %d pixels high: this one is %d"), size,
             rows_height(GUI_LEAST_ROWS, size), height);
}
/* the size picked greyed, so OK does not take it: true, and why in out (n 0 for none) */
static bool size_why(const App* app, char* out, size_t n) {
    if (!size_picking(app) || !size_grey(app, app->size_pick)) return false;
    if (n) size_reason(app, app->size_pick, out, n);
    return true;
}
/* the dialog's boxes: its choices' (box the whole dialog's), the slider's row and the question's rows under it, or
   why the size picked is greyed */
typedef struct {
    GuiChoices c;
    GuiRect body, line;
} GuiSizesBoxes;
/* the dialog in the middle of the screen in every theme, as a box over it is (over_rect): its title on its head, the
   slider and the size picked under it, the question once OK took it, then its choices, as many rows as fit */
static void sizes_place(App* app, GuiSizesBoxes* b) {
    GuiChoices* c = &b->c;
    int s = app->scale, cw = cell_w(app), ch = cell_h(app), width, height, cols, rows = 0, top, line_h, list_h;
    const char* text;
    char why[128];
    area(app, &width, &height);
    memset(b, 0, sizeof *b);
    c->of = GUI_CHOICES_SIZES;
    cols = SDL_max(GUI_SIZE_BAR + 1 + size_name_cols(), gui_text_cols(snx_tr("UI scale")) + 2);
    for (; (text = choice_text(app, c->of, c->count, app->row_text, sizeof app->row_text)); ++c->count)
        cols = SDL_max(cols, gui_text_cols(text) + gui_text_cols(app->theme.marker) + 2);
    if (app->size_until) {
        cols = SDL_max(cols, gui_text_cols(app->size_line));
        rows = wrap_rows(app, app->size_line, SDL_min(cols, width / cw - 4), 0);
    } else if (size_grey(app, GUI_TEXT_MAX)) { /* room for why, the largest's: the box still while the slider steps */
        size_reason(app, GUI_TEXT_MAX, why, sizeof why);
        cols = SDL_max(cols, gui_text_cols(why));
        rows = wrap_rows(app, why, SDL_min(cols, width / cw - 4), 0);
    }
    top = head_h(app) + 4 * s;             /* the slider's row, under the head */
    line_h = rows ? rows * ch + 4 * s : 0; /* the question's or why, under it */
    list_h = rows_h(ui_choices_layout, ui_choices_row, c->rects, UI_CHOICES_ROWS, s, c->count);
    list_h = SDL_min(list_h, SDL_max(0, height - top - ch - line_h - 10 * s)); /* the box kept in the area */
    c->box = over_rect(app, cols, top + ch + line_h + 6 * s + list_h + 4 * s);
    b->body = gui_rect(c->box.x + cw, c->box.y + top, c->box.w - 2 * cw, ch);
    b->line = gui_rect(c->box.x + cw, b->body.y + ch + 4 * s, c->box.w - 2 * cw, rows * ch);
    c->at = gui_rect(c->box.x + cw, b->body.y + ch + line_h + 6 * s, c->box.w - 2 * cw, list_h);
    choices_rows(app, c, app->sizes.focus);
}
/* the slider in the dialog's body: GUI_SIZE_BAR columns, fewer where they do not fit, whatever the step */
static GuiRect sizes_bar(const App* app, const GuiSizesBoxes* b) {
    int cols = SDL_clamp(b->body.w / cell_w(app) - 1 - size_name_cols(), GUI_SIZE_STEPS, GUI_SIZE_BAR);
    return gui_rect(b->body.x, b->body.y, cols * cell_w(app), cell_h(app));
}
static void slider_draw(App* app, GuiRect bar, int at, bool focused, unsigned grey);
/* the dialog, a box over the screen: its head, the slider on the size picked (in the focus's colour while it picks)
   and its name after it, the steps the window shows as smaller ones greyed; the question or why the size picked is
   greyed in the warning's colour, then its choices */
static void render_sizes(App* app) {
    const GuiTheme* t = &app->theme;
    GuiSizesBoxes b;
    GuiRect bar;
    char name[64], why[128];
    unsigned grey = 0;
    int k;
    for (k = 0; k < GUI_SIZE_STEPS; ++k)
        if (size_grey(app, k + 1)) grey |= 1u << k;
    sizes_place(app, &b);
    over_box(app, b.c.box, snx_tr("UI scale"));
    bar = sizes_bar(app, &b);
    slider_draw(app, bar, app->size_pick - 1, size_picking(app), grey);
    size_name(app->size_pick, name, sizeof name);
    text_in(app, b.body, bar.x + bar.w + cell_w(app), b.body.y, name,
            rgb(grey >> (app->size_pick - 1) & 1u ? t->soft : t->ink));
    if (app->size_until)
        wrap_in(app, b.line, b.line.x, b.line.y, app->size_line, 0, rgb(t->warning));
    else if (size_why(app, why, sizeof why))
        wrap_in(app, b.line, b.line.x, b.line.y, why, 0, rgb(t->warning));
    app->choices_of = b.c.of;
    ui_choices_draw(app, b.c.rects, app->scale, b.c.lists);
}
static void text_left(App* app);
/* OK: the size picked in use, asked about until GUI_TEXT_KEEP_MS from now, Back in focus; the one in use already
   closes the dialog, with nothing to ask; a greyed one is not taken, the dialog saying why as it does */
static void size_take(App* app) {
    if (size_why(app, NULL, 0)) return;
    if (app->size_pick == app->menu.text_size) {
        app->sizes_shown = false;
        return;
    }
    app->size_was = app->menu.text_size;
    app->menu.text_size = app->size_pick;
    layout(app);
    app->size_until = now_ms(app) + GUI_TEXT_KEEP_MS;
    app->sizes.focus = 1;
    text_left(app);
    app_values(app);
}
/* the slider at step k (Left/Right, a drag, a tap) while the dialog picks: nothing else changes until OK */
static void slider_to(App* app, int k) {
    if (size_picking(app)) app->size_pick = SDL_clamp(k, 0, GUI_SIZE_STEPS - 1) + 1;
}
/* the slider in bar: a line, a tick a step, the knob on step at, in the focus's colour while focused; the steps of
   grey's bits (step k's 1 << k) and the knob on one in the soft colour, as a greyed row's text; kept for the pointer
   (slider_at) */
static void slider_draw(App* app, GuiRect bar, int at, bool focused, unsigned grey) {
    const GuiTheme* t = &app->theme;
    int s = app->scale, cw = cell_w(app), k, mid = bar.y + bar.h / 2;
    int span = bar.w - cw; /* the knob a cell wide, its left from bar.x to bar.x + span */
    GuiRgb line = t->soft, knob = grey >> at & 1u ? t->soft : focused ? t->focus : t->ink;
    app->slider_bar = bar;
    if (span <= 0) return;
    fill_rect(app, gui_rect(bar.x + cw / 2, mid - s, span, 2 * s), line);
    for (k = 0; k < GUI_SIZE_STEPS; ++k)
        fill_rect(app, gui_rect(bar.x + cw / 2 + k * span / (GUI_SIZE_STEPS - 1) - s / 2, mid - 3 * s, s, 6 * s),
                  grey >> k & 1u ? t->soft : t->ink);
    fill_rect(
        app, gui_rect(bar.x + at * span / (GUI_SIZE_STEPS - 1), mid - cell_h(app) / 2 + 2 * s, cw, cell_h(app) - 4 * s),
        knob);
}
/* the step under pixel x, y of the slider drawn last, -1 off it */
static int slider_at(const App* app, float x, float y) {
    GuiRect b = app->slider_bar;
    int span = b.w - cell_w(app);
    if (!b.w || span <= 0 || x < (float)b.x || x >= (float)(b.x + b.w) || y < (float)b.y || y >= (float)(b.y + b.h))
        return -1;
    return SDL_clamp(
        (int)SDL_floorf(((x - (float)b.x - (float)cell_w(app) / 2) * (GUI_SIZE_STEPS - 1) + span / 2.0f) / (float)span),
        0, GUI_SIZE_STEPS - 1);
}
static void size_answer(App* app, bool keep);
/* choice i of the dialog: OK or Cancel while it picks, Keep or Back while it asks */
static void size_choice(App* app, int i) {
    if (app->size_until)
        size_answer(app, i == 0);
    else if (i == 0)
        size_take(app);
    else
        app->sizes_shown = false;
}
/* a key on the dialog: Left/Right step the slider while it picks, else go round the choices as Up/Down do; Enter takes
   the one in focus, ESC is Cancel or Back */
static void sizes_key(App* app, GuiKey key) {
    if (size_picking(app) && (key == GUI_LEFT || key == GUI_RIGHT)) {
        slider_to(app, app->size_pick - 1 + (key == GUI_LEFT ? -1 : 1));
        return;
    }
    switch (gui_choice_key(&app->sizes, key)) {
        case GUI_CHOICE_TAKE: size_choice(app, app->sizes.focus); break;
        case GUI_CHOICE_BACK: size_choice(app, 1); break;
        default: break;
    }
}
/* a choice under the pointer takes the focus, a click takes it (ui_choices_rows_row_action_cb); a click off the dialog
   is Cancel or Back */
static void sizes_pointer(App* app, float x, float y, bool click) {
    GuiSizesBoxes b;
    bool on;
    sizes_place(app, &b);
    if (!choices_pointer(app, &b.c, x, y, click, &on) && click && !on) size_choice(app, 1);
}
/* the new size kept in app.ini (the setup's once it is done), or the one before it back (Back, ESC, or no answer in
   time); the dialog closes */
static void size_answer(App* app, bool keep) {
    app->size_until = 0;
    app->sizes_shown = false;
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
        size_answer(app, false);
        return;
    }
    size_name(app->size_was, was, sizeof was);
    snprintf(app->size_line, sizeof app->size_line, snx_tr("Keep this UI scale? Back to %s in %d s"), was,
             (int)((app->size_until - now + 999) / 1000));
}
/* Move Old Files and Clean Old Settings move or rewrite what a recompile of the queue may use (a game's folder, its
   .ini, where the driver records its dump): what (English) waits while the queue has one to run, the note saying
   which; true then */
static bool queue_waits(App* app, const char* what) {
    int run = gui_queue_running(&app->queue);
    if (!jobs_live(app)) return false;
    if (run < 0) run = gui_queue_next(&app->queue);
    snprintf(app->editor.note, sizeof app->editor.note, snx_tr("%s is compiling (%d%%): %s waits until the queue ends"),
             gui_queue_name(&app->queue, run), SDL_max(app->queue.job[run].percent, 0), snx_tr(what));
    return true;
}
/* the App's rows: the theme and the language go round, either way, UI scale opens its dialog, and the editor opens */
static void app_action(App* app, int row, int step) {
    if (row == GUI_APP_THEME)
        next_theme(app, step < 0 ? -1 : 1);
    else if (row == GUI_APP_TEXT) { /* Enter its dialog; Left/Right too, its slider a step that way */
        sizes_open(app, step < 0 ? -1 : step > 0);
        return;
    } else if (row == GUI_APP_ANIMATIONS) {
        app->animations_off = !app->animations_off;
    } else if (row == GUI_APP_SIDE) {
        app->side_box = (app->side_box + (step < 0 ? 2 : 1)) % 3;
        layout(app);
    } else if (row == GUI_APP_BACKGROUND) { /* the one running follows at once (job_priority), its threads as started */
        app->background_gentle = !app->background_gentle;
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
        if (!step && !queue_waits(app, SNX_TR_NOOP("Clean Old Settings"))) clean_row(app);
        return;
    } else if (row == GUI_APP_MOVE) {
        if (!step && !queue_waits(app, SNX_TR_NOOP("Move Old Files"))) move_row(app);
        return;
    } else if (row == GUI_APP_RESET) {
        if (!step) settings_reset_ask(app);
        return;
    }
    if (row == GUI_APP_THEME || row == GUI_APP_TEXT || row == GUI_APP_LANGUAGE || row == GUI_APP_ANIMATIONS ||
        row == GUI_APP_SIDE || row == GUI_APP_BACKGROUND)
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
/* settings.ini written as the settings are, a failure in the note and its box; the head and Current user after it */
static void write_settings(App* app) {
    gui_settings_sync(&app->settings);
    if (app->settings_file && gui_settings_save(&app->settings, app->settings_file)) {
        snprintf(app->editor.note, sizeof app->editor.note, snx_tr("Not saved: %s could not be written"),
                 app->settings_file);
        say_failure(app, app->editor.note);
    }
    if (app->user != settings_user(app)) users_read(app); /* the head shows the user picked */
    if (app->form == GUI_FORM_USERS) users_fill(app);     /* Current user says it too */
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
/* the pad of player (1 to 8) among the n plugged in, as the games give them their players (snx_pad_slot): each, in the
   order they came, the player that names it, else the first that names none; 0 for none */
static SDL_JoystickID player_pad(const App* app, const SDL_JoystickID* ids, int n, int player) {
    const char* players[SNX_GUI_PLAYERS];
    char key[32], guid[33], before[33];
    unsigned taken = 0;
    bool took[64] = {false}; /* each pad before: it took a player */
    int i, k, p;
    for (p = 0; p < SNX_GUI_PLAYERS; ++p) {
        snprintf(key, sizeof key, "gamepad_p%d_device", p + 1);
        players[p] = gui_settings_value(&app->settings, key);
    }
    for (i = 0; i < n && i < (int)SDL_arraysize(took); ++i) {
        int alike = 0, slot;
        SDL_GUIDToString(SDL_GetGamepadGUIDForID(ids[i]), guid, sizeof guid);
        for (k = 0; k < i; ++k) { /* those before it with its GUID that took a player */
            SDL_GUIDToString(SDL_GetGamepadGUIDForID(ids[k]), before, sizeof before);
            alike += took[k] && !SDL_strcasecmp(before, guid);
        }
        slot = snx_pad_slot(guid, alike, players, SNX_GUI_PLAYERS, taken);
        took[i] = slot >= 0;
        if (slot >= 0) taken |= 1u << slot;
        if (slot == player - 1) return ids[i];
    }
    return 0;
}
/* the pad the page shows, when none is open: a player's map its player's (player_pad), every player's map the first
   plugged in */
static void pad_attach(App* app) {
    SDL_JoystickID* ids;
    int i, n = 0;
    if (app->gamepad || !app->pads_read) return;
    ids = SDL_GetGamepads(&n);
    if (app->pad_player > 0 && ids && n > 0) {
        SDL_JoystickID mine = player_pad(app, ids, n, app->pad_player);
        char key[32];
        const char* device;
        snprintf(key, sizeof key, "gamepad_p%d_device", app->pad_player);
        device = gui_settings_value(&app->settings, key);
        if (mine && !(app->gamepad = SDL_OpenGamepad(mine)))
            snprintf(app->pad.note, sizeof app->pad.note, "The controller could not be opened: %s", SDL_GetError());
        else if (!mine) /* the page's texts are English, in the font's bytes, as the game's menu draws them */
            snprintf(app->pad.note, sizeof app->pad.note,
                     "No controller plugged in is player %d's (its Controller: %s): the %d there are other players'",
                     app->pad_player, device && device[0] ? device : "Any", n);
    }
    for (i = 0; app->pad_player <= 0 && ids && i < n && !app->gamepad; i++) {
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
/* two Joy-Cons one pad, or two, as the games take them (gamepad_joycon_pair, 1 when not picked): before SDL's gamepads
   start, which read it then */
static void joycon_pairs(App* app) {
    const char* pair = gui_settings_value(&app->settings, "gamepad_joycon_pair");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS, pair && !atoi(pair) ? "0" : "1");
}
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
    joycon_pairs(app);
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
    place_screen(app, &place, shown_keys(app, keys, sizeof keys), GUI_SHOWS_TABS);
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
/* a resolution past 4x, under 0.5x, or one that is no whole number: its row flashes in the warning colour and the
   foot says why, for a moment; one that is no whole number says Eden's line as well */
static void resolution_warn(App* app, int why, int experimental) {
    const char* base = why == 0 ? snx_tr("Over 4x: this takes a lot of GPU memory, and a game may slow down or crash. "
                                         "Beyond this, severe graphical issues may occur")
                      : why == 1 ? snx_tr("Under 0.5x: the picture may look very low quality. Beyond this, severe "
                                          "graphical issues may occur")
                                 : "";
    app->warn_until = now_ms(app) + GUI_WARN_MS;
    snprintf(app->warn_key, sizeof app->warn_key, "%s", "resolution");
    if (experimental)
        snprintf(app->editor.note, sizeof app->editor.note, "%s%s%s", base, base[0] ? ". " : "",
                 snx_tr("Non-standard resolution, experimental."));
    else
        snprintf(app->editor.note, sizeof app->editor.note, "%s", base);
}

/* the warning's second over: its line goes and its row keeps its own colour */
static void warn_tick(App* app) {
    if (!app->warn_until || now_ms(app) < app->warn_until) return;
    app->warn_until = 0;
    app->warn_key[0] = 0;
    app->editor.note[0] = 0;
}
static void typed_keyboard(App* app);
static void editor_key(App* app, GuiKey key) {
    GuiEditorDo what;
    int was_typing = app->editor.typing;
    if (app->warn_until) app->editor.note[0] = 0;
    app->warn_until = 0;
    app->warn_key[0] = 0;
    if (app->form == GUI_FORM_USERS && app->editor.typing && key == GUI_SELECT && app->editor.row > 0 &&
        !snx_users_nickname_valid(app->editor.text)) { /* a nickname refused in its dialog, which goes on */
        snprintf(app->editor.note, sizeof app->editor.note, snx_tr("%s is not a nickname: %s"), app->editor.text,
                 snx_tr(SAY_SETUP_NICKNAME));
        return;
    }
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
    if (key == GUI_SELECT && was_typing && !app->editor.typing && app->form == GUI_FORM_GROUP &&
        app->editor.object == (char*)app->settings.values) {
        int row = setting_of(app, app->editor.row);
        const char* setting = row >= 0 && row < app->settings.count ? app->settings.values[row].key : NULL;
        if (setting && !strcmp(setting, "resolution")) {
            const char* chosen = gui_settings_value(&app->settings, "resolution");
            double value = chosen ? strtod(chosen, NULL) : 0;
            int whole = value == (double)(long long)value;
            if (value > 4.0)
                resolution_warn(app, 0, !whole);
            else if (value < 0.5)
                resolution_warn(app, 1, !whole);
            else if (!whole) /* any scale in the range that is no whole number */
                resolution_warn(app, 2, 1);
        }
    }
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
        else if (what == GUI_EDITOR_SAVE) /* Reset to Defaults */
            places_reset_ask(app);
        if (what != GUI_EDITOR_LEAVE) return;
    }
    if (app->form == GUI_FORM_USERS) { /* Current user steps or picks; a nickname typed is written */
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
#ifdef SNX_TOUCH_UI
    if (app->form == GUI_FORM_GROUP) { /* the menu on the whole screen too, as the games, unless fullscreen is 0 */
        const char* fullscreen = gui_settings_value(&app->settings, "fullscreen");
        SDL_SetWindowFullscreen(app->window, !fullscreen || strcmp(fullscreen, "0"));
    }
#endif
    typed_keyboard(app); /* a phone's keyboard comes up with the dialog */
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
                                     : snx_tr("Data, Logs, Caches, Extractions and the recompiled library's Main");
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
    int theme = app->form == GUI_FORM_THEME, g;
    place_screen(app, &f->place, shown_keys(app, keys, sizeof keys),
                 GUI_SHOWS_TABS | GUI_SHOWS_SIDE | (theme ? GUI_SHOWS_WIDE : 0));
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
    app->form_beside = !theme && app->form != GUI_FORM_FOLDERS && f->place.list.w && f->place.list.h &&
                       f->place.side.w && f->place.side.h;
    { /* the note whole, a half step under the text's letters */
        int s = app->scale, w = inner(app, f->place.side).w, line, half = text_half(app);
        rows = note ? wrap_rows(app, note, w / cell_w(app), 0) : 1;
        line = cell_h(app);
        app->half = half;
        f->info = gui_rect(f->place.side.x, f->place.side.y + f->place.side.h - rows * line - 4 * s, f->place.side.w,
                           rows * line + 2 * s);
    }
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
        list_follow(app, GUI_LIST_FORM_LIST, ui_form_list_follow, ui_form_list_row, f->left, UI_FORM_LIST_ROWS,
                    (size_t)count, &f->left_lists[UI_FORM_LIST_ROWS]);
    if (!theme && app->form != GUI_FORM_SETTINGS) f->left_lists[UI_FORM_LIST_ROWS].focus = SIZE_MAX;
    if (theme || app->form == GUI_FORM_SETTINGS) { /* nothing, or the rows to see before opening their group */
        GuiEditor peek;
        UiList* side = &f->right_lists[UI_FORM_SIDE_ROWS];
        side->focus = SIZE_MAX;
        if (theme) return;
        /* from its first row; a finger or the thumb there opens the group (list_take) */
        side->count = (size_t)gui_editor_rows(form_rows(app, &peek));
        side->store = &app->firsts[GUI_LIST_FORM_SIDE];
        app->list_step[GUI_LIST_FORM_SIDE] = ui_form_side_row(f->right, app->scale, UI_FORM_SIDE_ROWS, 1).y -
                                             ui_form_side_row(f->right, app->scale, UI_FORM_SIDE_ROWS, 0).y;
        return;
    }
    count = app->form == GUI_FORM_FOLDERS ? folder_entries(app) : gui_editor_rows(&app->editor);
    f->right_lists[UI_FORM_SIDE_ROWS].focus =
        (size_t)(app->form == GUI_FORM_FOLDERS ? folder_entry(app, app->editor.row) : app->editor.row);
    list_follow(app, GUI_LIST_FORM_SIDE, ui_form_side_follow, ui_form_side_row, f->right, UI_FORM_SIDE_ROWS,
                (size_t)count, &f->right_lists[UI_FORM_SIDE_ROWS]);
}
/* the columns of text a row width pixels wide has past its marker's half cell (ui_draw_row's) */
static int form_room(const App* app, int width) {
    return (ui_row_box(app, gui_rect(0, 0, width, 0)).w - cell_w(app) * 3 / 2) / cell_w(app);
}
/* a numeric setting's value right-aligned in four slots (0.25, 16.0), the group's numbers in a column */
#define GUI_NUMBER_VALUE 4
/* a row of a form as its box shows it, its marker first: its label, the value past the widest label (labels
   columns), in room columns; a value typed is typed in its dialog (render_typed) */
static void form_line(App* app, const GuiEditor* e, int r, int labels, int room, char* line, size_t size) {
    char label[256], value[400];
    const GuiThemeKey* k = gui_editor_key_of(e, r);
    const char* path;
    int pad, srow = -1;
    gui_editor_label(e, r, label, sizeof label);
    if (k && k->numeric && !k->whole && e->object == (char*)app->settings.values)
        srow = e->keys == app->view_keys   ? app->view_rows[k - app->view_keys]
               : e->keys == app->peek_keys ? app->peek_rows[k - app->peek_keys]
                                           : (int)(k - app->settings.keys);
    if (srow >= 0) { /* its value right in four slots */
        const GuiSetting* s = &app->settings.values[srow];
        const char* picked = gui_settings_value(&app->settings, s->key);
        const char* number = picked ? picked : s->fallback ? s->fallback : "";
        int cols = gui_text_cols(number);
        snprintf(value, sizeof value, "%*s%.*s%s%s", SDL_max(GUI_NUMBER_VALUE - cols, 0), "",
                 SDL_min(cols, GUI_NUMBER_VALUE), number, picked ? "" : " ", picked ? "" : snx_tr("[default]"));
    } else if (k && e->object == (char*)app->settings.values) { /* a setting: a pick, else its default and [default] */
        int row = e->keys == app->view_keys   ? app->view_rows[k - app->view_keys]
                  : e->keys == app->peek_keys ? app->peek_rows[k - app->peek_keys]
                                              : (int)(k - app->settings.keys);
        gui_settings_shown(&app->settings, row, value, sizeof value);
        if (player_key(app->settings.values[row].key, "device") && !app->settings.values[row].text[0])
            snprintf(value, sizeof value, "%s", snx_tr("Any [default]")); /* the next to connect */
        else if (player_key(app->settings.values[row].key, "map")) /* it opens the button map, as All's page does */
            snprintf(value, sizeof value, "%s",
                     app->settings.values[row].text[0] ? snx_tr("Custom >") : snx_tr("As All >"));
    } else
        gui_editor_value(e, r, value, sizeof value);
    if (k && e->keys == app->folder_keys && (path = folder_row_path(app, r)) != NULL) {
        /* its folder, then what it is at the row's end; with no room for both the folder stays whole and what it is
           goes, and one too long alone is cut in its middle (whole under the boxes) */
        char state[64], cut[300];
        int left = room - gui_text_cols(app->theme.marker) - labels - 1, cols = gui_text_cols(path), gap;
        snprintf(state, sizeof state, "%s", value);
        gap = left - cols - gui_text_cols(state);
        if (cols > left) {
            gui_path_fit(path, SDL_max(left, 4), cut, sizeof cut);
            snprintf(value, sizeof value, "%s", cut);
        } else if (state[0] && gap >= 2)
            snprintf(value, sizeof value, "%s%*s%s", path, gap, "", state);
        else
            snprintf(value, sizeof value, "%s", path);
    }
    pad = SDL_max(labels - gui_text_cols(label), 0); /* the labels' columns, not their bytes */
    if (k && k->numeric && !k->whole) pad = 0;      /* the scale's value left, its row under the box's width */
    if (value[0]) { /* a row too long for its box gives up the labels' column first, and slides only past that */
        int over = gui_text_cols(app->theme.marker) + gui_text_cols(label) + pad + 1 + gui_text_cols(value) - room;
        if (over > 0) pad = SDL_max(pad - over, 0);
        snprintf(line, size, "%s%s%*s %s", app->theme.marker, label, pad, "", value);
    } else /* no value to line up: padding would make a short row too long for its box, and slide */
        snprintf(line, size, "%s%s", app->theme.marker, label);
}
static int clean_inis(App* app, GuiCleaned* done);
static int settings_unread(App* app, char* out, size_t size);
/* what Clean Old Settings would take out: the [Deprecated] sections and old keys, looked for once after App opens (it
   reads every game's .ini) and again after a clean and with F5 */
static int clean_left(App* app) {
    char unread[16];
    if (app->clean_found < 0)
        app->clean_found = clean_inis(app, NULL) + settings_unread(app, unread, sizeof unread) +
                           app_read(app, false, unread, sizeof unread, NULL, 0);
    return app->clean_found;
}
/* a row of App or Folders with nothing to do now, greyed, whose Enter says why (#614): nothing to clean or move, every
   setting at its default, no folder picked */
static bool form_row_off(App* app, const GuiEditor* e, int r) {
    int picked = 0;
    if (e->keys == app_keys)
        return (r == GUI_APP_MOVE && !app->lefts) || (r == GUI_APP_CLEAN && !clean_left(app)) ||
               (r == GUI_APP_RESET && app->settings.keys && !gui_settings_changed(&app->settings));
    if (app->form != GUI_FORM_FOLDERS || e != &app->editor || r != folder_rows(app)) return false;
    for (int i = 0; i < GUI_PLACES; ++i) picked += app->place[i] != NULL;
    return !picked;
}
/* the icon of row r of e: App's rows that open a screen or do something, Folders' Add Folder and its last row, a
   group's row that opens its page; none for a row that holds a value */
static unsigned form_icon(const App* app, const GuiEditor* e, int r) {
    const GuiThemeKey* k = gui_editor_key_of(e, r);
    int at, group;
    if (e->keys == app_keys)
        return r == GUI_APP_EDITOR   ? GUI_ICON_PAINTBRUSH
               : r == GUI_APP_PLACES ? GUI_ICON_FOLDER
               : r == GUI_APP_CLEAN  ? GUI_ICON_ERASER
               : r == GUI_APP_MOVE   ? GUI_ICON_FOLDER_INPUT
               : r == GUI_APP_RESET  ? GUI_ICON_ROTATE_CCW
                                     : GUI_ICON_NONE;
    if (e->keys == app->folder_keys)
        return !k                                               ? GUI_ICON_ROTATE_CCW /* Reset to Defaults */
               : folder_row(app, r, &at, &group) == GUI_ROW_ADD ? GUI_ICON_FOLDER_PLUS
                                                                : GUI_ICON_NONE;
    if (k && e->object == (char*)app->settings.values) {
        int row = e->keys == app->view_keys   ? app->view_rows[k - app->view_keys]
                  : e->keys == app->peek_keys ? app->peek_rows[k - app->peek_keys]
                                              : (int)(k - app->settings.keys);
        if (app->settings.values[row].kind == GUI_SETTING_PAGE) return page_icon(k->key);
    }
    return GUI_ICON_NONE;
}
/* row r of e as a list of ui.txt asks for it, in a row width pixels wide: false past its last. In a form with icons a
   row without one starts where their text does, its values in their column; but for Folders', whose rows hold paths
   that need the room (a long one would lose what it is, default, beside it) */
static bool form_row_info(App* app, const GuiEditor* e, size_t r, int labels, int width, UiRowInfo* info) {
    char* line = app->row_text;
    int i, rows = gui_editor_rows(e), gutter = 0;
    if (r >= (size_t)rows) return false;
    for (i = 0; i < rows && !gutter && e->keys != app->folder_keys; ++i) gutter = form_icon(app, e, i) != GUI_ICON_NONE;
    info->icon = form_icon(app, e, (int)r);
    gutter = gutter && !info->icon;
    form_line(app, e, (int)r, labels, form_room(app, width) - (gutter || info->icon ? 3 : 0), line + 3 * gutter,
              sizeof app->row_text - 3);
    if (gutter) { /* the marker, then the icon's columns and the one after it */
        size_t n = strlen(app->theme.marker);
        memmove(line, line + 3, n);
        memset(line + n, ' ', 3);
    }
    info->label = line + strlen(app->theme.marker); /* ui_draw_row puts the marker */
    if (form_row_off(app, e, (int)r)) info->attr |= UI_ATTR_OFF;
    if (app->warn_until && now_ms(app) < app->warn_until && app->warn_key[0] &&
        e->object == (char*)app->settings.values) {
        int row = setting_of(app, (int)r);
        if (row >= 0 && row < app->settings.count && app->settings.values[row].key &&
            !strcmp(app->settings.values[row].key, app->warn_key) && (now_ms(app) / GUI_WARN_BLINK_MS) % 2 == 0)
            info->attr |= UI_ATTR_WARNING;
    }
    return true;
}
/* FormList's row: the theme editor's keys; else the groups, the one open beside them in the focus's look */
bool ui_form_list_rows_get_row_info(App* app, size_t row, int width, UiRowInfo* info) {
    if (app->form == GUI_FORM_THEME)
        return form_row_info(app, &app->editor, row, form_labels(&app->editor), width, info);
    if (row >= (size_t)app->groups) return false;
    info->label = snx_tr(app->group_keys[row].key);
    info->icon = group_icon(app->group_keys[row].key);
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
    if ((r = folder_entry_row(app, (int)row, &group)) < 0 &&
        group > GUI_FOLDER_GROUPS) { /* the blank before Defaults */
        info->label = "";
        info->attr = UI_ATTR_LABEL;
        return true;
    }
    if (r < 0) { /* a group's title, over its rows */
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
    *color = rgb(e->note[0] ? (app->warn_until ? app->theme.warning
                             : e->leaving ? app->theme.failure
                                          : app->theme.focus)
                            : app->theme.ink);
    if (e->note[0]) return e->note;
    if (k) return snx_tr(k->about);
    if (app->form == GUI_FORM_FOLDERS) return snx_tr("Nothing is moved"); /* Reset to Defaults: what, under the boxes */
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
    size_t i, n = ui_form_list_shown(f->left, s, UI_FORM_LIST_ROWS) + (l->shift > 0);
    if (l->shift) ui_clip(app, &f->left[UI_FORM_LIST_ROWS]);
    for (i = 0; i < n && l->first + i < (size_t)gui_editor_rows(e); ++i) {
        int r = (int)(l->first + i), x;
        const GuiThemeKey* k = gui_editor_key_of(e, r);
        GuiRect row = row_drawn(app, ui_form_list_row, ui_form_list_shown, f->left, UI_FORM_LIST_ROWS, l, i),
                b = ui_row_box(app, row);
        char line[700];
        if (!k || k->type != GUI_KEY_COLOUR) continue;
        form_line(app, e, r, labels, form_room(app, row.w), line, sizeof line);
        x = b.x + cw / 2 + gui_text_cols(line) * cw + cw;
        if (x + 2 * cw <= b.x + b.w - cw) /* a cell short of the box's end */
            swatch(app, gui_rect(x, ui_text_y(app, b) + 2 * s, 2 * cw, cell_h(app) - 5 * s),
                   *(const GuiRgb*)((const char*)(e->object ? e->object : (const char*)&e->theme) + k->at));
    }
    if (l->shift) ui_clip(app, NULL);
}
/* Folders' groups, a box each as far as it shows: from its title down to its last row shown, half a cell wider
   either side, Defaults' too, the right one at the rows' end beside a scroll bar; with the rows as they scroll, inside
   the list; drawn under them */
static void folder_frames(App* app, const GuiFormBoxes* f) {
    const UiList* l = &f->right_lists[UI_FORM_SIDE_ROWS];
    size_t i, shown = ui_form_side_shown(f->right, app->scale, UI_FORM_SIDE_ROWS), n = shown + (l->shift > 0);
    int g, group, cw = cell_w(app), bw = ui_bar_width(app, l->count, shown);
    GuiRect inside = f->right[UI_FORM_SIDE_ROWS];
    inside.x -= cw / 2;
    inside.w += cw;
    if (l->shift) ui_clip(app, &inside);
    for (g = 0; g <= GUI_FOLDER_GROUPS; ++g) {
        GuiRect a = {0}, z = {0}, box;
        for (i = 0; i < n && l->first + i < (size_t)folder_entries(app); ++i) {
            folder_entry_row(app, (int)(l->first + i), &group);
            if (group != g) continue;
            z = row_drawn(app, ui_form_side_row, ui_form_side_shown, f->right, UI_FORM_SIDE_ROWS, l, i);
            if (!a.h) a = z;
        }
        if (!a.h) continue;
        box = gui_rect(a.x - cw / 2, a.y, bw ? a.w + cw / 2 : a.w + cw, z.y + z.h - a.y);
        if (app->theme.panels == GUI_PANELS_FILL) fill_rect(app, box, app->theme.page);
        outline(app, box, app->theme.dim);
    }
    if (l->shift) ui_clip(app, NULL);
}
static void draw_picker(App* app);
/* ---- a value typed: in a dialog over its form, as UI scale's is, the row's label its title, then the text with its
   cursor in a field, what the row takes, and what the editor says of what was typed; OK and Cancel under them ---- */
#define GUI_TYPED_FIELD 24 /* the field's columns at the least */
static const char* const typed_choices[2] = {SNX_TR_NOOP("OK"), SNX_TR_NOOP("Cancel")};
#define SAY_NICKNAME_TAKES SNX_TR_NOOP("A nickname, 4 to 20 of A-Z a-z 0-9 space . _ -")
/* what the row typed takes, in out: its key's, a nickname's on Settings > User */
static void typed_takes(const App* app, char* out, size_t size) {
    if (app->form == GUI_FORM_USERS && app->editor.row > 0)
        snprintf(out, size, "%s", snx_tr(SAY_NICKNAME_TAKES));
    else
        gui_editor_takes(&app->editor, out, size);
}
/* the dialog's boxes: its choices' (box the whole dialog's), the field, what it takes and the editor's note */
typedef struct {
    GuiChoices c;
    GuiRect field, takes, note;
} GuiTypedBoxes;
static void typed_place(App* app, GuiTypedBoxes* b) {
    GuiChoices* c = &b->c;
    int s = app->scale, cw = cell_w(app), ch = cell_h(app), width, height, cols, takes_rows, note_rows = 0, fixed;
    int list_h, y;
    char title[256], takes[256];
    const char* text;
    area(app, &width, &height);
    memset(b, 0, sizeof *b);
    c->of = GUI_CHOICES_TYPED;
    gui_editor_label(&app->editor, app->editor.row, title, sizeof title);
    typed_takes(app, takes, sizeof takes);
    cols = SDL_max(SDL_max(gui_text_cols(title) + 2, gui_text_cols(takes)), GUI_TYPED_FIELD);
    for (; (text = choice_text(app, c->of, c->count, app->row_text, sizeof app->row_text)); ++c->count)
        cols = SDL_max(cols, gui_text_cols(text) + gui_text_cols(app->theme.marker) + 2);
    cols = SDL_min(cols, width / cw - 4);
    takes_rows = wrap_rows(app, takes, cols, 0);
    if (app->editor.note[0]) note_rows = wrap_rows(app, app->editor.note, cols, 0);
    fixed = head_h(app) + 4 * s + ch + 4 * s + 4 * s + takes_rows * ch + (note_rows ? 4 * s + note_rows * ch : 0) +
            6 * s + 4 * s;
    list_h = rows_h(ui_choices_layout, ui_choices_row, c->rects, UI_CHOICES_ROWS, s, c->count);
    list_h = SDL_min(list_h, SDL_max(0, height - fixed)); /* the box kept in the area */
    c->box = over_rect(app, cols, fixed + list_h);
    y = c->box.y + head_h(app) + 4 * s;
    b->field = gui_rect(c->box.x + cw, y, c->box.w - 2 * cw, ch + 4 * s);
    y += ch + 8 * s;
    b->takes = gui_rect(c->box.x + cw, y, c->box.w - 2 * cw, takes_rows * ch);
    y += takes_rows * ch;
    if (note_rows) {
        b->note = gui_rect(c->box.x + cw, y + 4 * s, c->box.w - 2 * cw, note_rows * ch);
        y += 4 * s + note_rows * ch;
    }
    c->at = gui_rect(c->box.x + cw, y + 6 * s, c->box.w - 2 * cw, list_h);
    choices_rows(app, c, 0); /* OK's look: Enter takes it */
}
/* the dialog over its form: the text framed in the focus's colour, its end and the cursor in view; what it takes in
   the soft colour, the editor's note in the warning's */
static void render_typed(App* app) {
    const GuiTheme* t = &app->theme;
    GuiTypedBoxes b;
    char title[256], takes[256], line[sizeof app->editor.text + 4];
    int cw = cell_w(app), room;
    typed_place(app, &b);
    gui_editor_label(&app->editor, app->editor.row, title, sizeof title);
    over_box(app, b.c.box, title);
    outline(app, b.field, t->focus);
    snprintf(line, sizeof line, "%s_", app->editor.text);
    room = (b.field.w - cw) / cw;
    text_in(app, b.field, b.field.x + cw / 2, b.field.y + 2 * app->scale,
            gui_cols_skip(line, SDL_max(gui_text_cols(line) - room, 0)), rgb(t->ink));
    typed_takes(app, takes, sizeof takes);
    wrap_in(app, b.takes, b.takes.x, b.takes.y, takes, 0, rgb(t->soft));
    if (app->editor.note[0]) wrap_in(app, b.note, b.note.x, b.note.y, app->editor.note, 0, rgb(t->warning));
    app->choices_of = b.c.of;
    ui_choices_draw(app, b.c.rects, app->scale, b.c.lists);
}
/* choice i of the dialog: OK takes what was typed, as Enter, Cancel leaves the row as it was, as ESC */
static void typed_choice(App* app, int i) { editor_key(app, i ? GUI_CANCEL : GUI_SELECT); }
/* the pointer on the dialog: a choice under it takes the focus and a click takes it; a click on the field brings a
   phone's keyboard back, one off the box is Cancel */
static void typed_pointer(App* app, float x, float y, bool click) {
    GuiTypedBoxes b;
    bool on;
    typed_place(app, &b);
    if (choices_pointer(app, &b.c, x, y, click, &on) || !click) return;
    if (in_rect(b.field, x, y))
        SDL_StartTextInput(app->window);
    else if (!on)
        typed_choice(app, 1);
}
/* the system's keyboard while a value is typed, a phone's a number pad for a whole number that is never under 0 (SDL's
   number pad has no point and no minus), else its letters; none once it is not */
static void typed_keyboard(App* app) {
    const GuiThemeKey* k = gui_editor_key_of(&app->editor, app->editor.row);
    bool pad = k && ((k->numeric && k->whole && k->lo >= 0) || k->type == GUI_KEY_NUMBER);
    if (app->editor.typing && !SDL_TextInputActive(app->window)) {
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_TYPE_NUMBER,
                              pad ? SDL_TEXTINPUT_TYPE_NUMBER : SDL_TEXTINPUT_TYPE_TEXT);
        SDL_StartTextInputWithProperties(app->window, props);
        SDL_DestroyProperties(props);
    } else if (!app->editor.typing && SDL_TextInputActive(app->window))
        SDL_StopTextInput(app->window);
}
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
        if (form_note(app)) { /* a half step under the text's letters, as form_boxes measured it */
            GuiRect text = inner(app, f.info);
            int half = text_half(app);
            wrap_in(app, text, text.x, f.info.y, form_note(app), 0, rgb(app->theme.soft));
            app->half = half;
        }
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
        if (i == GUI_PICK_HEX) { /* the colour as #RRGGBB, typed in its dialog, the one it is now and the one it had */
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
    if (i < 3)
        picker_set(app, &p, i, x);
    else if (i == GUI_PICK_HEX) {
        e->channel = GUI_PICK_HEX;
        editor_key(app, GUI_SELECT);
    } else if (i == GUI_PICK_ROWS && (x < (float)p.box.x || x >= (float)(p.box.x + p.box.w) || y < (float)p.box.y ||
                                      y >= (float)(p.box.y + p.box.h)))
        gui_editor_pick_end(e, true);
}
/* a row of a form under the pointer: focused, or on the focused one Select, as a key would */
static void form_row_pointer(App* app, int r) {
    if (r == app->editor.row)
        editor_key(app, GUI_SELECT);
    else
        app->editor.row = r;
}
/* FormList's row under the pointer. The theme editor's keys and the groups take the focus as it goes over them, a
   click on the focused one selecting it; on a group's screen, whose keys move in its rows, a click opens another group
   from its row, as Enter there */
void ui_form_list_rows_row_action_cb(App* app, size_t row, uint32_t hit) {
    int r = (int)row;
    if (app->form == GUI_FORM_THEME || app->form == GUI_FORM_SETTINGS) {
        if (hit & UI_HIT_CLICK)
            form_row_pointer(app, r);
        else
            app->editor.row = r;
        return;
    }
    if (!(hit & UI_HIT_CLICK)) return;
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
        if (app->form != GUI_FORM_SETTINGS) app->editor.row = r;
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
#ifdef SNX_TOUCH_UI
#define SAY_SETUP SNX_TR_KEYS("Tap: choose. Back.")
#else
#define SAY_SETUP SNX_TR_KEYS("Tap: choose.")
#endif
#define SAY_SETUP_FIRST SAY_SETUP
#ifdef SDL_PLATFORM_ANDROID
#define SAY_SETUP_TYPING SNX_TR_KEYS("Type the nickname. Back.")
#else
#define SAY_SETUP_TYPING SNX_TR_KEYS("Type the nickname.")
#endif
#define SAY_SETUP_NOTICE SAY_SETUP
#else
#define SAY_SETUP        SNX_TR_KEYS("Up/Down: choose. Enter: next. ESC: back.")
#define SAY_SETUP_FIRST  SNX_TR_KEYS("Up/Down: choose. Enter: next. ESC: exit.")
#define SAY_SETUP_TYPING SNX_TR_KEYS("Type the nickname. Enter: done. ESC: back.")
#define SAY_SETUP_NOTICE SNX_TR_KEYS("Left/Right: choose. Enter: OK. ESC: exit.")
#endif
#ifdef SNX_TOUCH_UI
#define SAY_SETUP_LOOK       SAY_SETUP
#define SAY_SETUP_LOOK_FIRST SAY_SETUP
#define SAY_SETUP_SIZE       SAY_SETUP
#define SAY_SETUP_SIZE_FIRST SAY_SETUP
#define SAY_SETUP_ON         SAY_SETUP
#define SAY_SETUP_ON_FIRST   SAY_SETUP
#else /* the Appearance page's, as Settings > App's rows: Enter changes Language and Theme, opens UI scale's dialog,
         which Left/Right open too, its slider a step on */
#define SAY_SETUP_LOOK       SNX_TR_KEYS("Up/Down: row. Left/Right: change. Enter: change. ESC: back.")
#define SAY_SETUP_LOOK_FIRST SNX_TR_KEYS("Up/Down: row. Left/Right: change. Enter: change. ESC: exit.")
#define SAY_SETUP_SIZE       SNX_TR_KEYS("Up/Down: row. Left/Right: change. Enter: open. ESC: back.")
#define SAY_SETUP_SIZE_FIRST SNX_TR_KEYS("Up/Down: row. Left/Right: change. Enter: open. ESC: exit.")
#define SAY_SETUP_ON         SNX_TR_KEYS("Up/Down: row. Enter: next. ESC: back.")
#define SAY_SETUP_ON_FIRST   SNX_TR_KEYS("Up/Down: row. Enter: next. ESC: exit.")
#endif
#define GUI_SETUP_CHOICES 4 /* a page's, at most: Folders' and Keys' three, and Skip on the first page */
/* a page's boxes above YoRHa's choices at least, else beside them: a notice's, its head and four lines, and a row */
#define GUI_SETUP_CONTENT_ROWS 6
#define GUI_SETUP_CONTENT_COLS 24 /* and the columns beside them it takes */
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
/* the first page shows Skip after its own choices (setup_choice.count of them): every page left taken as its defaults
   have it (setup_skip) */
static int setup_skips(const App* app) { return app->setup_page == 0; }
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
    app->setup_typing = true; /* its keyboard comes up with the next frame (setup_keyboard) */
    snprintf(app->setup_name, sizeof app->setup_name, "Player");
    app->setup_choice.count = 1;
    app->setup_choice.focus = 0;
}
/* the nickname's keyboard, a frame after its page asks for it: Skip types the nickname and takes it in one call, and
   on Android a keyboard asked for and dropped before it showed stays up (SDL hides only one Android says is shown) */
static void setup_keyboard(App* app) {
    if (app->setup_on && app->setup_typing && !SDL_TextInputActive(app->window)) SDL_StartTextInput(app->window);
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
/* a start's UI scale where app.ini had none: written there now, or with the setup's picks once it is done, in the
   folders Install picks */
static void scale_keep(App* app) {
    if (app->scale_new && app->setup_on)
        app->setup_save = true;
    else if (app->scale_new)
        app_save(app);
    app->scale_new = false;
}
/* the setup at the start, unless --no-setup leaves it out: the pages setup.ini has left, or none */
static void setup_start(App* app) {
    char why[512], *file = snx_setup_file(app->home_dir ? app->home_dir : app->data_dir);
    int i;
    app->setup_on = false;
    app->setup_count = app->setup_page = 0;
    if (snx_setup_skip || !file) {
        free(file);
        scale_keep(app);
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
    scale_keep(app);
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
    free(app->job_log);
    free(app->users_dir);
    app->settings_file = app->folders_file = app->app_file = app->run_log = app->job_log = app->users_dir = NULL;
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
static void setup_skip(App* app);
static void setup_take(App* app) {
    int page = setup_page_of(app), choice = app->setup_choice.focus;
    char note[1100], *portable, *prod, shown[1024];
    bool there;
    if (setup_skips(app) && choice == app->setup_choice.count) {
        setup_skip(app);
        return;
    }
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
/* Skip: the page from its start, then each page taken as its defaults have it, what Next, OK or Enter on each would
   take (the focus setup_enter gives it, the nickname it types), until the setup is done and setup.ini says so. A page
   whose default fails (a user not written, no uid) stays, saying why; so would one whose default takes nothing */
static void setup_skip(App* app) {
    setup_enter(app);
    for (;;) {
        int page = app->setup_page, top = app->setup_top;
        bool typing = app->setup_typing;
        setup_take(app);
        if (!app->setup_on || app->setup_failed || app->confirm.shown) return; /* done, or its failure said */
        if (page == app->setup_page && top == app->setup_top && typing == app->setup_typing) {
            setup_fail(app, snx_tr("Skip stops on this page: it has no default to take"));
            return;
        }
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
/* the Appearance's row changed a step either way, at once, as Settings > App's and the View box's: the language among
   the menu's (Auto again on the system's, where the setup found Auto), the theme among the themes, the Side panel
   round its three, View and Icon size round their words, Icon size saying why it does not in the list (#614) */
static void appearance_change(App* app, int row, int step) {
    if (row == GUI_APPEARANCE_LANGUAGE) {
        app->language = (snx_tr_language() + step + snx_gui_language_count) % snx_gui_language_count + 1;
        if (!app->setup_language && app->language - 1 == language_auto()) app->language = 0;
        language_apply(app);
    } else if (row == GUI_APPEARANCE_TEXT) { /* its dialog, as App's: its slider a step that way */
        sizes_open(app, step < 0 ? -1 : 1);
        return;
    } else if (row == GUI_APPEARANCE_SIDE) {
        app->side_box = (app->side_box + (step < 0 ? 2 : 1)) % 3;
        layout(app);
    } else if (row == GUI_APPEARANCE_ICONS && option_off(app, GUI_OPTION_ICONS)) {
        snprintf(app->message, sizeof app->message, "%s", snx_tr(SAY_ICONS_OFF));
        return;
    } else if (row == GUI_APPEARANCE_VIEW || row == GUI_APPEARANCE_ICONS) {
        int i = row == GUI_APPEARANCE_VIEW ? GUI_OPTION_VIEW : GUI_OPTION_ICONS;
        app->options[i] = (app->options[i] + option_words(i) + step) % option_words(i);
    } else
        next_theme(app, step);
    app->setup_save = true;
}
static void setup_key(App* app, GuiKey key) {
    GuiChoice all = app->setup_choice; /* the page's choices, and Skip on the first */
    GuiChoiceDo did;
    int step;
    all.count += setup_skips(app);
    if (setup_page_of(app) == SNX_SETUP_LANGUAGE) {
        /* on the choices under the rows: Left/Right go round them, Down to the next until the last, Up to the one
           before until the first, then the rows' */
        if (app->setup_row == GUI_APPEARANCE_NEXT && all.count > 1 &&
            (key == GUI_LEFT || key == GUI_RIGHT || (key == GUI_DOWN && all.focus < all.count - 1) ||
             (key == GUI_UP && all.focus > 0))) {
            gui_choice_key(&all, key);
            app->setup_choice.focus = all.focus;
            return;
        }
        switch (gui_appearance_key(&app->setup_row, key, &step)) {
            case GUI_APPEARANCE_CHANGE: appearance_change(app, app->setup_row, step); break;
            case GUI_APPEARANCE_SIZES: sizes_open(app, 0); break;
            case GUI_APPEARANCE_ON: setup_take(app); break;
            case GUI_APPEARANCE_BACK: setup_back(app); break;
            default: break;
        }
        if (app->setup_on && setup_page_of(app) == SNX_SETUP_LANGUAGE && app->setup_row == GUI_APPEARANCE_NEXT &&
            (key == GUI_UP ||
             key == GUI_DOWN)) /* onto the choices: the first from above, the last from the rows' top */
            app->setup_choice.focus = key == GUI_UP ? all.count - 1 : 0;
        return;
    }
    did = gui_choice_key(&all, key);
    app->setup_choice.focus = all.focus;
    switch (did) {
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
    if (app->sizes_shown) { /* UI scale's dialog, over the Appearance page */
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
    static const char* const folders[] = {SNX_TR_NOOP("Keep"), SNX_TR_NOOP("Change Recompiled Folder"),
                                          SNX_TR_NOOP("Add a folder")};
    static const char* const keys[] = {SNX_TR_NOOP("Use These"), SNX_TR_NOOP("Browse..."), SNX_TR_NOOP("Later")};
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
    if (setup_skips(app)) labels[count++] = snx_tr("Skip");
    return count;
}
static const char* setup_question(const App* app, char* out, size_t size) {
    switch (setup_page_of(app)) {
        case SETUP_NOTICE:
            return app->setup.version ? snx_tr("Changed since the last version") : snx_tr("Changed in older versions");
        case SNX_SETUP_LANGUAGE: return snx_tr("Appearance");
        case SNX_SETUP_INSTALL: return snx_tr("Install Type");
        case SNX_SETUP_FOLDERS: return snx_tr("Folders");
        case SNX_SETUP_KEYS: return snx_tr("Keys");
        default:
            if (app->setup_typing)
                snprintf(out, size, snx_tr("Nickname: %s_"), app->setup_name);
            else if (app->setup_users < 0)
                snprintf(out, size, "%s", snx_tr("users.ini is not a list of users"));
            else
                snprintf(out, size,
                         app->setup_users == 1 ? snx_tr("users.ini has %d user") : snx_tr("users.ini has %d users"),
                         app->setup_users);
            return out;
    }
}
static const char* setup_tip(const App* app) {
    switch (setup_page_of(app)) {
        case SETUP_NOTICE: return snx_tr("Tip: --no-setup skips the setup");
        case SNX_SETUP_LANGUAGE: return snx_tr("Tip: games keep their own language: Settings > Display > Language");
        case SNX_SETUP_INSTALL: return snx_tr("Tip: a portable copy is a folder you can copy");
        case SNX_SETUP_FOLDERS: return snx_tr("Tip: Settings > App > Folders changes them");
        case SNX_SETUP_KEYS: return snx_tr("Tip: Menu > Install Keys changes them");
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
/* the page's choice drawn in focus: the Appearance's choices only while the focus is on them, not on a row; -1 for
   none */
static int setup_focus(const App* app) {
    if (setup_page_of(app) == SNX_SETUP_LANGUAGE)
        return app->setup_row == GUI_APPEARANCE_NEXT ? app->setup_choice.focus : -1;
    return app->setup_choice.focus;
}
/* ---- the Appearance page's box: at the top of r, as high as its rows; under its head the rows, ui.txt's scene
   Appearance ---- */
static const char* const appearance_names[GUI_APPEARANCE_NEXT] = {
    SNX_TR_NOOP("Language"),   SNX_TR_NOOP("Theme"), SNX_TR_NOOP("UI scale"),
    SNX_TR_NOOP("Side panel"), SNX_TR_NOOP("View"),  SNX_TR_NOOP("Icon size")}; /* Settings > App's, the View box's */
typedef struct {
    GuiRect box; /* the page's box, its head over the rows */
    GuiRect rects[UI_APPEARANCE_ITEMS];
    UiList lists[UI_APPEARANCE_ITEMS]; /* the rows' at UI_APPEARANCE_ROWS: row i is GUI_APPEARANCE_* i */
} GuiAppearanceBoxes;
static void appearance_boxes(App* app, GuiRect r, GuiAppearanceBoxes* a) {
    int s = app->scale, top = head_h(app) + 4 * s, h;
    h = rows_h(ui_appearance_layout, ui_appearance_row, a->rects, UI_APPEARANCE_ROWS, s, GUI_APPEARANCE_NEXT);
    a->box = gui_rect(r.x, r.y, r.w, SDL_min(top + h + 2 * s, r.h));
    ui_appearance_layout(gui_rect(a->box.x + cell_w(app), a->box.y + top, a->box.w - 2 * cell_w(app),
                                  SDL_min(h, a->box.h - top - 2 * s)),
                         s, a->rects);
    memset(a->lists, 0, sizeof a->lists);
    a->lists[UI_APPEARANCE_ROWS].focus = (size_t)SDL_min(app->setup_row, GUI_APPEARANCE_NEXT - 1);
    list_follow(app, GUI_LIST_APPEARANCE, ui_appearance_follow, ui_appearance_row, a->rects, UI_APPEARANCE_ROWS,
                GUI_APPEARANCE_NEXT, &a->lists[UI_APPEARANCE_ROWS]);
    if (app->setup_row == GUI_APPEARANCE_NEXT) a->lists[UI_APPEARANCE_ROWS].focus = SIZE_MAX; /* on its choices */
}
/* the column a row's value starts at, in the row's text: past the marker and the longest name */
static int appearance_column(const App* app) {
    int i, cols = 0;
    for (i = 0; i < GUI_APPEARANCE_NEXT; ++i) cols = SDL_max(cols, gui_text_cols(snx_tr(appearance_names[i])));
    return gui_text_cols(app->theme.marker) + cols + 2;
}
/* row i as it reads: its name, then from appearance_column its value, between the arrows Left and Right go by, UI
   scale's lined up with their text, its dialog Enter's */
static void appearance_row(const App* app, int i, char* out, size_t size) {
    char value[400];
    int pad = appearance_column(app) - gui_text_cols(app->theme.marker) - gui_text_cols(snx_tr(appearance_names[i]));
    if (i == GUI_APPEARANCE_LANGUAGE)
        snprintf(value, sizeof value, "< %s >", snx_gui_languages[snx_tr_language()].name);
    else if (i == GUI_APPEARANCE_THEME)
        snprintf(value, sizeof value, "< %s >", app->theme.name);
    else if (i == GUI_APPEARANCE_SIDE)
        snprintf(value, sizeof value, "< %s >", snx_tr(side_names[app->side_box]));
    else if (i == GUI_APPEARANCE_VIEW || i == GUI_APPEARANCE_ICONS) {
        int o = i == GUI_APPEARANCE_VIEW ? GUI_OPTION_VIEW : GUI_OPTION_ICONS;
        snprintf(value, sizeof value, "< %s >", snx_tr(option_keys[o].shown[app->options[o]]));
    } else {
        char shown[96];
        size_shown(app, shown, sizeof shown);
        snprintf(value, sizeof value, "  %s", shown);
    }
    snprintf(out, size, "%s%s%*s%s", app->theme.marker, snx_tr(appearance_names[i]), pad, "", value);
}
typedef struct {
    GuiPlace place;
    GuiRect content; /* the page's boxes */
    GuiChoices choices;
    char question[96], tab[64];
} GuiSetupBoxes;
static void setup_boxes(App* app, GuiSetupBoxes* b) {
    int n = setup_number(app, app->setup_page);
    char question[sizeof b->question];
    GuiRect area;
    place_screen(app, &b->place, snx_tr(setup_keys_said(app)), GUI_SHOWS_TABS | GUI_SHOWS_FOOT_TIPS);
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
        GuiAppearanceBoxes a;
        int under;
        appearance_boxes(app, area, &a);
        under = a.box.y + a.box.h + cell_h(app) - area.y;
        area = gui_rect(area.x, area.y + under, area.w, SDL_max(0, area.h - under));
    }
    choices_place(app, GUI_CHOICES_SETUP, area, b->question, b->tab, setup_focus(app), &b->choices);
    b->content = b->place.list;
    if (app->theme.dialog == GUI_DIALOG_YORHA) { /* above the choices, or beside them down to the question where that
                                                    leaves too few rows (a phone's choices, a finger's row each) */
        int above = b->choices.at.y - cell_h(app) / 2 - b->content.y;
        int cursor = app->theme.cursor[0] ? gui_text_cols(app->theme.cursor) : 0;
        int beside = b->choices.at.x - (cursor + 1) * cell_w(app) - b->content.x;
        int foot = (b->choices.tab.h ? b->choices.tab.y : b->choices.box.y) - cell_h(app) / 2;
        if (above >= GUI_SETUP_CONTENT_ROWS * cell_h(app) || beside < GUI_SETUP_CONTENT_COLS * cell_w(app))
            b->content.h = SDL_max(0, above);
        else
            b->content = gui_rect(b->content.x, b->content.y, beside, SDL_max(0, foot - b->content.y));
    }
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
            GuiAppearanceBoxes a;
            appearance_boxes(app, r, &a);
            pane(app, a.box, snx_tr("Appearance"));
            ui_appearance_draw(app, a.rects, app->scale, a.lists);
            break;
        }
        case SNX_SETUP_INSTALL:
            setup_split(app, setup_now_box(app, r), 3, &a, &b);
            for (i = 0; i < 4; ++i) {
                path = snx_menu_dir(NULL, kinds[i]); /* Logs and Caches as Folders picked them */
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
                setup_box(app, r, snx_tr("First User"), lines, n);
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
    choices_draw(app, &b.choices, b.question, b.tab);
    foot(app, &b.place, app->message[0] ? app->message : setup_tip(app),
         rgb(app->setup_failed ? t->failure
             : app->message[0] ? t->focus
                               : t->ink),
         snx_tr(setup_keys_said(app)));
    if (app->sizes_shown) render_sizes(app); /* UI scale's dialog, over the Appearance page */
    if (app->confirm.shown) render_ask(app);
}
/* the pointer on the page: a choice under it takes the focus, a click takes it (ui_choices_rows_row_action_cb), as
   one of the Appearance's rows (ui_appearance_rows_row_action_cb) */
static void setup_pointer(App* app, float x, float y, bool click) {
    GuiSetupBoxes b;
    GuiAppearanceBoxes a;
    bool on;
    int item;
    size_t row;
    uint32_t hit;
    setup_boxes(app, &b);
    if (choices_pointer(app, &b.choices, x, y, click, &on)) return;
    if (setup_page_of(app) != SNX_SETUP_LANGUAGE) return;
    appearance_boxes(app, b.content, &a);
    hit = ui_appearance_hit(app, a.rects, app->scale, a.lists, (int)x, (int)y, click ? UI_HIT_CLICK : 0, &item, &row);
    if (hit) ui_appearance_act(app, item, row, hit);
}
/* Appearance's row: its name, then its value at appearance_column (ui_draw_row puts the marker) */
bool ui_appearance_rows_get_row_info(App* app, size_t row, int width, UiRowInfo* info) {
    if (row >= GUI_APPEARANCE_NEXT) return false;
    (void)width; /* a row too long for it slides */
    appearance_row(app, (int)row, app->row_text, sizeof app->row_text);
    info->label = app->row_text + strlen(app->theme.marker);
    if (row == GUI_APPEARANCE_ICONS && option_off(app, GUI_OPTION_ICONS)) info->attr |= UI_ATTR_OFF;
    return true;
}
/* a row under the pointer takes the focus; a click changes its value a step on, or back on its left arrow, which the
   pointer (app->hover_x) is on, and on UI scale opens its dialog */
void ui_appearance_rows_row_action_cb(App* app, size_t row, uint32_t hit) {
    GuiSetupBoxes b;
    GuiAppearanceBoxes a;
    int arrow;
    app->setup_row = (int)row;
    if (!(hit & UI_HIT_CLICK)) return;
    if (row == GUI_APPEARANCE_TEXT) {
        sizes_open(app, 0);
        return;
    }
    setup_boxes(app, &b);
    appearance_boxes(app, b.content, &a);
    arrow = ui_row_box(app, a.rects[UI_APPEARANCE_ROWS]).x + cell_w(app) / 2 + appearance_column(app) * cell_w(app);
    appearance_change(app, (int)row,
                      app->hover_x >= (float)arrow && app->hover_x < (float)(arrow + cell_w(app)) ? -1 : 1);
}
/* what the menu keeps for the next start, as it closes: each of its files there and its folder, in a box */
static void draw_closing(App* app) {
    const char* files[4] = {app->settings_file, app->app_file, app->folders_file, app->recent_file};
    static const char* const what[4] = {SNX_TR_NOOP("Settings"), SNX_TR_NOOP("Settings > App"),
                                        SNX_TR_NOOP("Library folders"), SNX_TR_NOOP("Recent Games")};
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
/* F1's box over the screen: every key it has (screen_said), a row a sentence, the keys in the focus colour */
static void render_help(App* app) {
    const GuiTheme* t = &app->theme;
    char said[1024], key[128], what[256];
    const char* at;
    int rows = 0, keys = 0, whats = 0, y;
    GuiRect r, text;
    screen_said(app, true, said, sizeof said);
    for (at = said; hint_next(&at, key, what, sizeof key); ++rows) {
        keys = SDL_max(keys, gui_text_cols(key));
        whats = SDL_max(whats, gui_text_cols(what));
    }
    r = over_rect(app, SDL_min(keys + 2 + whats, app->cols - 4), head_h(app) + rows * cell_h(app) + 8 * app->scale);
    fill_rect(app, r, t->page); /* over the screen whatever its boxes are */
    y = pane(app, r, snx_tr("Keys"));
    text = inner(app, r);
    for (at = said; hint_next(&at, key, what, sizeof key); y += cell_h(app)) {
        text_in(app, text, text.x, y, key, rgb(t->focus));
        text_in(app, text, text.x + (keys + 2) * cell_w(app), y, what, rgb(t->ink));
    }
}
static void render(App* app) {
    layout(app);
    app->slider_bar = gui_rect(0, 0, 0, 0); /* none until a screen with UI scale's draws it */
    app->nbars = 0;
    lists_glide(app);
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
        else if (app->editor.typing)
            render_typed(app); /* over the picker too, its #RRGGBB typed */
        else if (app->sizes_shown)
            render_sizes(app);
        else if (app->picks_shown)
            render_picks(app); /* a Folders row's box */
        else if (app->users_shown)
            render_users(app); /* opened from the form's head, as from the menu's */
    } else
        draw_screen(app);
    if (app->cleaned_shown) render_files(app, 1); /* over the menu, or the App's rows it was started from */
    if (app->help_shown) render_help(app);
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
#ifdef SDL_PLATFORM_IOS
/* a dump picked outside dumps/ is prepared from its copy there, so the game's recorded source is not the file the recent
   list has: that entry becomes the game made, and the one the game already has stays alone */
static void recent_made_here(App* app) {
    const GuiStep* made = gui_steps_last(&app->steps, "READY");
    char* program = made && app->file && app->games_dir ? gui_folder_game(app->games_dir, made->detail) : NULL;
    int i;
    if (!program) return;
    for (i = 0; i < app->recent.count; ++i)
        if (!strcmp(app->recent.game[i].path, app->file) || snx_same_file(app->recent.game[i].path, app->file)) {
            if (gui_recent_made(&app->recent, i, program)) recent_changed(app);
            break;
        }
    free(program);
}
#endif
static void list_games(App* app);
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
    if (ok) app->playing = true; /* the queue's recompile goes gently until the menu shows again */
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
/* the run in its notification, which keeps the app going behind others: the menu's while one runs, else the queue's
   recompile running. The game, the step on the screen and the percent, as they change, a second apart at most
   (Android drops notifications that come faster); once neither runs, how the last ended stays, while the app is behind
   others (seen here, the end needs no word) */
static void tell(App* app) {
    char name[96], runs[3][112], line[sizeof app->told];
    size_t lengths[3];
    int job = gui_queue_running(&app->queue), percent;
    const GuiSteps* steps = app->menu.running ? &app->steps : job >= 0 ? &app->job_steps : NULL;
    Uint64 now = SDL_max(SDL_GetTicks(), 1);
    if (!steps) {
        if (app->telling) gui_android_ran(app->hidden && app->told_result ? snx_tr(app->told_result) : NULL);
        app->telling = false;
        app->told_ms = 0;
        return;
    }
    percent = gui_steps_percent(steps);
    line[0] = 0;
    if (steps->count) {
        step_runs(&steps->step[steps->count - 1], app->theme.progress, now_ms(app), runs, lengths);
        snprintf(line, sizeof line, "%s%s%s", runs[0], runs[1], runs[2]);
    }
    if (app->telling && app->told_ms &&
        (now - app->told_ms < 1000 || (percent == app->told_percent && !strcmp(line, app->told))))
        return;
    if (app->menu.running)
        run_name_of(&app->steps, app->native != 0, app->file, name, sizeof name);
    else
        snprintf(name, sizeof name, "%s", gui_queue_name(&app->queue, job));
    gui_android_running(name, line, percent);
    snprintf(app->told, sizeof app->told, "%s", line);
    app->told_percent = percent;
    app->told_ms = now;
    app->telling = true;
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
        char *data, *npdm, *name, *old, now[SNX_PATH_SIZE], id[17];
        unsigned char* bytes;
        size_t size = 0;
        n = strlen(names[i]);
        if (n <= 5 || strcmp(names[i] + n - 5, "_data")) continue;
        data = snx_path_join(folder, names[i]);
        name = snx_format("%.*s", (int)(n - 5), names[i]);
        npdm = data ? snx_path_join(data, "exefs/main.npdm") : NULL;
        bytes = npdm && snx_is_file(npdm) ? (unsigned char*)snx_read_file(npdm, &size) : NULL;
        old = name ? snx_path_join(folder, name) : NULL;
        if (old && data && snx_is_dir(data) &&
            !snx_path_format(now, sizeof now, "%s/%s/", folder,
                             snx_title_text(snx_npdm_title(bytes, size), id) ? id : name))
            left_line(app, old, now, true);
        free(bytes);
        free(npdm);
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
        char portable[SNX_PATH_SIZE];
        old = snx_path_join(app->drivers, "games");
        if (old && app->games_dir && strcmp(old, app->games_dir) &&
            !snx_path_format(portable, sizeof portable, "%s/" SNX_PORTABLE_DIR "/recompiled", app->drivers))
            left_line(app, old, portable, snx_is_dir(old));
    }
    free(old);
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
/* ---- Clean Old Settings (clean.c): [Deprecated] out of each game's .ini the menu reaches, and the keys the menu's own
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
    snprintf(what, sizeof what, failed ? snx_tr("Not written without %s") : snx_tr("Removed %s"), unread);
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
/* the moves of Move Old Files, made with apply, else what they would be, an entry each in done: each folder app->left
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
/* App's Move Old Files: what it would move, in a box (GUI_ASK_MOVE); nothing left, said; nothing that can move, the
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
             plan.count > moves ? snx_tr("Move Old Files? %d move, %d stay where they are")
                                : snx_tr("Move Old Files? %d move"),
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
                 keys > 1 ? snx_tr("Clean Old Settings? [Deprecated] in %d .ini, %d old keys")
                          : snx_tr("Clean Old Settings? [Deprecated] in %d .ini, %d old key"),
                 inis, keys);
    else if (inis)
        snprintf(line, size, snx_tr("Clean Old Settings? [Deprecated] in %d .ini"), inis);
    else
        snprintf(line, size,
                 keys > 1 ? snx_tr("Clean Old Settings? %d old keys") : snx_tr("Clean Old Settings? %d old key"), keys);
    gui_confirm_open(&app->confirm, GUI_ASK_CLEAN, false);
}
/* Settings > App's Reset to Defaults: how many settings it changes, in a box (GUI_ASK_RESET), else that none */
static void settings_reset_ask(App* app) {
    int changed = gui_settings_changed(&app->settings);
    if (!changed) {
        snprintf(app->editor.note, sizeof app->editor.note, "%s", snx_tr("Every setting is at its default"));
        return;
    }
    snprintf(app->ask_line, sizeof app->ask_line,
             changed > 1 ? snx_tr("Reset to Defaults? %d settings change")
                         : snx_tr("Reset to Defaults? %d setting changes"),
             changed);
    gui_confirm_open(&app->confirm, GUI_ASK_RESET, false);
}
/* a change asked about (GUI_ASK_CLEAN to GUI_ASK_FOLDER) taken: done, its report or note shown, the screen as it left
   things; Back changes nothing */
static void change_answer(App* app, GuiAsk ask, bool taken) {
    if (!taken) return;
    if (ask == GUI_ASK_CLEAN) {
        clean_old(app);
        app->clean_found = -1;
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
    else if (ask == GUI_ASK_STATE)
        state_delete(app);
    else if (ask == GUI_ASK_CACHE)
        caches_delete(app);
}
/* the functions an AOT pack used, from the driver's line, -1 when it printed none */
static int aot_built(const GuiSteps* steps) {
    int i, n;
    for (i = steps->notes - 1; i >= 0; --i)
        if (sscanf(steps->note[i].text, "AOT pack: %d", &n) == 1) return n;
    return -1;
}
static bool watch_get(App* app, uint64_t* stamp);
static void job_pump(App* app);
static void taskbar(App* app);
static void games_back(App* app, const char* path);
#ifdef SDL_PLATFORM_ANDROID
static void tell(App* app);
#endif
static void pump(App* app) {
    keys_left(app);
    text_left(app);
    setup_keyboard(app);
    warn_tick(app);
    job_pump(app);
#ifdef SDL_PLATFORM_IOS
    icons_pump(app);
#endif
    if (!app->queue.count && app->menu.view == GUI_LOAD_RECOMPILE)
        games_back(app, NULL); /* its tab went with its last line */
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
        int status;
        if (gui_process_finished(app->process, &status)) {
            app->menu.running = false;
            /* Normal exits wait for acknowledgement. ESC already acknowledged
               an abort, so return straight to the menu after nonblocking reaping. */
            app->finished = !app->aborting;
            app->abnormal_exit = app->aborting || status != 0;
            if (!app->aborting) app->log_due = status != 0; /* an ESC is no crash, and keeps what was due */
            users_read(app);                                /* users.ini may have changed while it ran */
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
            if (app->aborting)
                snprintf(app->message, sizeof(app->message), "%s", snx_tr("Aborted with ESC."));
            else if (app->abnormal_exit)
                snprintf(app->message, sizeof(app->message), snx_tr("Program ended abnormally (status %d)."), status);
            else
                snprintf(app->message, sizeof(app->message), snx_tr("The run ended with status %d."), status);
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
            /* seen here, the end needs no word; behind other apps one stays until the menu shows (tell) */
            app->told_result = app->abnormal_exit ? SNX_TR_NOOP("Failed") : SNX_TR_NOOP("Ready");
#elif defined(SDL_PLATFORM_IOS)
            /* a phone has no ESC to dismiss the end of the run: a prepared game goes back to the menu, where it is
               listed; a failure stays on the screen with its messages */
            if (!app->abnormal_exit) {
                recent_made_here(app);
                recent_as_games(app); /* the game's name, now that it is the entry */
                list_games(app);      /* the list the run left behind has not the game made */
                leave_run(app);
            }
#endif
        }
    }
#ifdef SDL_PLATFORM_ANDROID
    if (app->play_later && !app->hidden) {
        app->play_later = false;
        if (ready(app)) leave_run(app);
    }
#endif
#ifdef SDL_PLATFORM_ANDROID
    tell(app);
#endif
    taskbar(app);
    Uint64 now = SDL_GetTicks();
    if (!app->sampled || now - app->sampled >= 500) {
        app->usage = gui_process_usage(app->process);
        app->job_usage = gui_process_usage_of(app->job_process);
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
    /* games put in or taken out of the folders while the menu is open, as the watcher's hash of them says: Games and
       its list follow, the list on the file it had. The first hash after a listing is what the list holds, the same
       as the one before it or not */
    uint64_t stamp;
    if (!app->menu.running && !app->finished && watch_get(app, &stamp) &&
        (stamp != app->games_stamp || app->games_fresh)) {
        bool fresh = app->games_fresh;
        app->games_stamp = stamp;
        app->games_fresh = false;
        if (!fresh) {
            recent_as_games(app);
            if (app->menu.view == GUI_LOAD_GAMES) {
                GuiBrowser was = app->browser;
                list_games(app);
                app->games_fresh = false; /* the hash it was listed by is this one, the folders the same */
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
/* the box's question */
static void dests_question(const App* app, char* question, size_t size) {
    const char *file = app->dest_file ? app->dest_file : "", *leaf = file + strlen(file);
    /* its name: a \ separates on Windows only, elsewhere it can be a name's letter */
#ifdef _WIN32
    while (leaf > file && leaf[-1] != '/' && leaf[-1] != '\\') --leaf;
#else
    while (leaf > file && leaf[-1] != '/') --leaf;
#endif
    snprintf(question, size, snx_tr("%s: into which folder of the recompiled library?"), leaf);
}
/* choice i: the folder from home, cut in its middle to fit, what it is and its free room; NULL past the last */
static const char* dest_label(App* app, int i, char* out, size_t size) {
    const char* dirs[GUI_FOLDERS_MAX + 1];
    char home[512], left[32];
    int room = SDL_max(app->cols - 40, 16);
    if (i >= dest_dirs(app, dirs) || i >= app->dests.count) return NULL;
    snx_home_short(dirs[i], home, sizeof home);
    gui_path_fit(home, room, out, SDL_min(size, 300));
    gui_size_text(left, sizeof left, free_space(dirs[i]));
    snprintf(out + strlen(out), size - strlen(out),
             i                             ? snx_tr("  %s free")
             : app->place[GUI_PLACE_GAMES] ? snx_tr("  custom, %s free")
                                           : snx_tr("  default, %s free"),
             left);
    return out;
}
static void dests_place(App* app, GuiChoices* c, char* question, size_t size) {
    dests_question(app, question, size);
    choices_place(app, GUI_CHOICES_DESTS, choices_over(app), question, snx_tr("Recompile"), app->dests.focus, c);
}
static void render_dests(App* app) {
    GuiChoices c;
    char question[640];
    dests_place(app, &c, question, sizeof question);
    box_shade(app);
    choices_draw(app, &c, question, snx_tr("Recompile"));
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
/* a folder under the pointer takes the focus, a click takes it (ui_choices_rows_row_action_cb); a click off the box
   is Back */
static void dests_pointer(App* app, float x, float y, bool click) {
    GuiChoices c;
    char question[640];
    bool on;
    dests_place(app, &c, question, sizeof question);
    if (!choices_pointer(app, &c, x, y, click, &on) && click && !on) dest_take(app, -1);
}
/* the picks box's choices, a NULL after the last: Share Log's, how the logs go; a Folders row's, as its kind allows
   (a place's changed or put back to its default, an Extra taken off the library, dumps/ and Settings only opened) */
static const char* const share_picks[] = {SNX_TR_NOOP("Share Zip"), SNX_TR_NOOP("Share Log"),
                                          SNX_TR_NOOP("Open Logs Folder"), NULL};
static const char* const place_picks[] = {SNX_TR_NOOP("Open Folder"), SNX_TR_NOOP("Change Folder"),
                                          SNX_TR_NOOP("Reset to Default"), NULL};
static const char* const extra_picks[] = {SNX_TR_NOOP("Open Folder"), SNX_TR_NOOP("Remove from Library"), NULL};
static const char* const open_picks[] = {SNX_TR_NOOP("Open Folder"), NULL};
/* a dumps library row's: its subfolders' games listed or not, (*) or ( ) before it as they are */
#define SAY_SUBFOLDERS SNX_TR_NOOP("Subfolders")
static const char* const dumps_picks[] = {SNX_TR_NOOP("Open Folder"), SAY_SUBFOLDERS, NULL};
static const char* const dumps_extra_picks[] = {SNX_TR_NOOP("Open Folder"), SAY_SUBFOLDERS,
                                                SNX_TR_NOOP("Remove from Library"), NULL};
static const char* const* picks_list(const App* app) {
    int at, group, kind;
    if (app->picks_of == GUI_PICKS_SHARE) return share_picks;
    kind = folder_row(app, app->pick_row, &at, &group);
    if (kind == GUI_ROW_DUMPS) return dumps_picks;
    if (kind == GUI_ROW_FOLDER && app->folders_library[at] == GUI_LIBRARY_DUMPS) return dumps_extra_picks;
    return kind == GUI_ROW_PLACE ? place_picks : kind == GUI_ROW_FOLDER ? extra_picks : open_picks;
}
static const char* pick_label(App* app, int i) {
    const char* const* picks = picks_list(app);
    int n = 0, at, group;
    while (picks[n]) ++n;
    if (i < 0 || i >= n) return NULL;
    if (strcmp(picks[i], SAY_SUBFOLDERS)) return snx_tr(picks[i]);
    snprintf(
        app->pick_text, sizeof app->pick_text, "%s %s",
        (folder_row(app, app->pick_row, &at, &group) == GUI_ROW_DUMPS ? !app->dumps_shallow : app->folders_deep[at])
            ? "(*)"
            : "( )",
        snx_tr(SAY_SUBFOLDERS));
    return app->pick_text;
}
static void picks_open(App* app, GuiPicksOf of) {
    app->picks_of = of;
    app->picks.focus = 0;
    for (app->picks.count = 0; pick_label(app, app->picks.count);) ++app->picks.count;
    app->picks_shown = true;
}
static void picks_open_row(App* app, int row) {
    app->pick_row = row;
    picks_open(app, GUI_PICKS_FOLDER);
}
/* the box's question into out and its tab: how the logs go, or the Folders row's name and its folder from home */
static void picks_question(App* app, char* out, size_t size, const char** tab) {
    char home[512];
    const char* path;
    *tab = app->picks_of == GUI_PICKS_SHARE ? snx_tr("Share Log") : snx_tr("Folders");
    if (app->picks_of == GUI_PICKS_SHARE) {
        snprintf(out, size, "%s", snx_tr("How to share the logs?"));
        return;
    }
    path = folder_row_path(app, app->pick_row);
    snx_home_short(path ? path : "", home, sizeof home);
    snprintf(out, size, "%s: %s", snx_tr(app->folder_keys[app->pick_row].key), path ? home : snx_tr("none"));
}
static void picks_place(App* app, GuiChoices* c, char* question, size_t size, const char** tab) {
    picks_question(app, question, size, tab);
    choices_place(app, GUI_CHOICES_PICKS, choices_over(app), question, *tab, app->picks.focus, c);
}
static void render_picks(App* app) {
    GuiChoices c;
    char question[640];
    const char* tab;
    picks_place(app, &c, question, sizeof question, &tab);
    box_shade(app);
    choices_draw(app, &c, question, tab);
}
static void share_log(App* app, bool zip);
static char* logs_folder(const App* app);
static const char* logs_none(void);
/* a dumps library row's subfolders listed from now on, or no longer: kept (dumps/ in app.ini, an Extra in folders.txt),
   the games listed again and the row's note saying which; one that cannot be kept holds until the menu closes, said */
static void subfolders_switch(App* app, int kind, int at) {
    char note[sizeof app->editor.note];
    const char* path;
    int row = app->pick_row, failed;
    bool deep;
    if (kind == GUI_ROW_DUMPS) {
        app->dumps_shallow = !app->dumps_shallow;
        deep = !app->dumps_shallow;
        failed = app_save(app);
    } else {
        deep = app->folders_deep[at] = !app->folders_deep[at];
        failed = folders_save(app);
    }
    path = folder_row_path(app, row);
    if (failed)
        snprintf(note, sizeof note, "%s", app->editor.note);
    else
        snprintf(note, sizeof note,
                 deep ? snx_tr("%s: its subfolders' games are listed too")
                      : snx_tr("%s: its own games alone are listed, not its subfolders'"),
                 path ? path : "");
    list_games(app);
    open_folders(app, row);
    snprintf(app->editor.note, sizeof app->editor.note, "%s", note);
}
/* choice i taken, or none (-1: Back): the logs as one zip, as the one log of before, or their folder opened; a Folders
   row's folder opened, another picked, its default asked for or its Extra taken off */
static void pick_take(App* app, int i) {
    int at, group, kind = folder_row(app, app->pick_row, &at, &group);
    app->picks_shown = false;
    if (i < 0) return;
    if (app->picks_of == GUI_PICKS_SHARE) {
        char* logs = i < 2 ? NULL : logs_folder(app); /* the launcher's logs and the games' */
        if (i < 2)
            share_log(app, i == 0);
        else if (logs)
            show_folder(app, logs);
        else
            snprintf(app->message, sizeof app->message, "%s", logs_none());
        free(logs);
    } else if (i == 0) {
        const char* path = folder_row_path(app, app->pick_row);
        if (path)
            show_folder(app, path);
        else
            snprintf(app->editor.note, sizeof app->editor.note, snx_tr("%s has no folder to open"),
                     snx_tr(app->folder_keys[app->pick_row].key));
    } else if (kind == GUI_ROW_PLACE && i == 1) {
        app->place_row = at;
        open_folder_picker(app);
    } else if (kind == GUI_ROW_PLACE)
        place_default_ask(app, at);
    else if (!strcmp(picks_list(app)[i], SAY_SUBFOLDERS)) { /* the box stays, its mark as it is now */
        int row = app->pick_row;
        subfolders_switch(app, kind, at);
        picks_open_row(app, row);
        app->picks.focus = i;
    } else if (kind == GUI_ROW_FOLDER)
        folder_off_ask(app, at);
}
static void picks_key(App* app, GuiKey key) {
    switch (gui_choice_key(&app->picks, key)) {
        case GUI_CHOICE_TAKE: pick_take(app, app->picks.focus); break;
        case GUI_CHOICE_BACK: pick_take(app, -1); break;
        default: break;
    }
}
/* a choice under the pointer takes the focus, a click takes it (ui_choices_rows_row_action_cb); a click off the box is
   Back */
static void picks_pointer(App* app, float x, float y, bool click) {
    GuiChoices c;
    char question[640];
    const char* tab;
    bool on;
    picks_place(app, &c, question, sizeof question, &tab);
    if (!choices_pointer(app, &c, x, y, click, &on) && click && !on) pick_take(app, -1);
}
static void job_add(App* app, const char* const* argv, const char* file, bool native, bool prepared, GuiJobKind kind);
/* file, a dump whose game is made: asked whether that game plays or the dump recompiles (GUI_ASK_MADE), file kept */
static void made_ask(App* app, char* file) {
    MadeGames made;
    const char* program;
    char *shown, *folder;
    made_read(app, &made);
    program = made_program(&made, file);
    shown = program ? gui_game_shown(program) : snx_strdup(file);
    folder = program ? snx_dirname(program) : snx_strdup("?");
    snprintf(app->ask_line, sizeof app->ask_line, snx_tr("%s is compiled already, in %s"), shown, folder);
    free(folder);
    free(shown);
    made_free(&made);
    free(app->made_file);
    app->made_file = file;
    app->made_item = app->menu.selected;
    gui_confirm_open(&app->confirm, GUI_ASK_MADE, false);
}
/* the answer about a made game's dump: Play runs the dump, its driver playing the game made (one gone since, or one
   another build made, compiles first), Recompile recompiles it as R does; Back neither */
static void made_answer(App* app, bool taken, bool second) {
    char* file = app->made_file;
    app->made_file = NULL;
    if (!taken || !file) {
        free(file);
        return;
    }
    int item = app->menu.selected; /* the menu keeps its item, as a recent game's run does (open_recent) */
    app->menu.selected = app->made_item;
    app->recompile = second;
    app->made_ok = !second;
    launch(app, file, false);
    app->recompile = app->made_ok = false;
    app->menu.selected = item;
}
static void queue_show(App* app, int i);
static void launch(App* app, char* absolute, bool prepared) {
    int queued;
    if (!absolute || !snx_is_file(absolute)) {
        snprintf(app->message, sizeof(app->message), "%s",
                 snx_tr("That path is not a readable file. Try again or press ESC."));
        free(absolute);
        return;
    }
    /* a game the queue recompiles, or will, cannot be played meanwhile (its folder holds a new library with the old
       runtime copy and stamp, the program written last): its recompile shows instead */
    if (!app->recompile && (queued = gui_queue_find(&app->queue, absolute)) >= 0) {
        queue_show(app, queued);
        free(absolute);
        return;
    }
#ifdef SDL_PLATFORM_IOS
    /* the driver runs on a thread of the app, the game in it once the menu closes: one at a time */
    if (!app->recompile && jobs_live(app)) {
        int run = gui_queue_running(&app->queue);
        snprintf(app->message, sizeof app->message,
                 snx_tr("%s is compiling (%d%%): the game plays once the queue ends"),
                 gui_queue_name(&app->queue, run >= 0 ? run : gui_queue_next(&app->queue)),
                 run >= 0 ? SDL_max(app->queue.job[run].percent, 0) : 0);
        free(absolute);
        return;
    }
#endif
    /* a dump, package or ROM a game was made from already: played or recompiled, as the player answers (made_answer;
       DraVee, 2026-10-24) */
    if (!app->recompile && !app->made_ok && !prepared && app->menu.selected != GUI_LINUX && made_from(app, absolute)) {
        made_ask(app, absolute);
        return;
    }
    /* a game opened that no game of the library was made from yet compiles first through the queue, as R's recompile
       does: the Compile tab shows it, and it plays once it ended if the tab still shows it (job_play). So does a Switch
       game another build made, which its driver would recompile on the run's screen: its program opened, or its dump
       with Play answered */
    bool compile = !app->recompile && !prepared && app->menu.selected != GUI_LINUX && !made_from(app, absolute);
#ifndef SDL_PLATFORM_IOS /* the games there are packs the app loads, with no stamp */
    compile = compile || (!app->recompile && !app->compiled && prepared && app->menu.selected == GUI_SWITCH &&
                          gui_game_folder(absolute) && !made_here(absolute));
    compile = compile || (app->made_ok && app->menu.selected == GUI_SWITCH && made_elsewhere(app, absolute));
#endif
    bool make = app->recompile || compile; /* the driver's -r: a run that makes the game and plays nothing */
    bool native = prepared && app->menu.selected == GUI_SWITCH;
    /* another console's made game plays through its driver in its folder (--game=), as its copy of the runtime does */
    char* game_dir = prepared && app->menu.selected != GUI_SWITCH ? snx_dirname(absolute) : NULL;
    char* game_option = game_dir ? snx_format("--game=%s", game_dir) : NULL;
    free(game_dir);
    /* a game in a folder of its own goes through the driver, which recompiles it first when another build made it; one
       an older version laid out beside the others runs as it is, as every game on iOS, which runs no other program */
#ifdef SDL_PLATFORM_IOS
    bool direct = native && !app->recompile;
#else
    bool direct = native && !app->recompile && !gui_game_folder(absolute);
#endif
    char* driver = direct ? NULL : find_driver(app, app->menu.selected);
#ifdef SDL_PLATFORM_IOS /* the app's own driver, which gui_process_start runs on a thread: its name is only argv[0] */
    if (!direct && !driver && app->menu.selected == GUI_SWITCH) driver = snx_strdup("scarletnx");
#endif
    if (!direct && !driver) {
        snprintf(app->message, sizeof(app->message), snx_tr("Cannot find %s beside ScarletNX or on PATH."),
                 driver_name(app->menu.selected) ? driver_name(app->menu.selected) : snx_tr("the selected driver"));
        say_failure(app, app->message);
        free(absolute);
        free(game_option);
        return;
    }
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
    if (direct) {
        if (play(app, absolute) && app->recent_file && !gui_recent_add(&app->recent, absolute, GUI_SWITCH, true))
            recent_changed(app);
        free(absolute);
        free(game_option);
        return;
    }
#endif
    /* the folders picked in Settings > App > Folders, which a Switch, 3DS, Vita or GBA run works in: refused when one
       cannot be */
    bool folders = (app->menu.selected == GUI_SWITCH && !native) ||
                   (!game_option &&
                    (app->menu.selected == GUI_3DS || app->menu.selected == GUI_VITA || app->menu.selected == GUI_GBA));
    if (folders && !places_fit(app, app->message, sizeof app->message)) {
        free(driver);
        free(absolute);
        free(game_option);
        return;
    }
    /* a dump made into the folder the player picks, when the recompiled library has more than one that can take it; one
       made already is recompiled where it is */
    if (app->menu.selected == GUI_SWITCH && !prepared && !app->dest_out && !made_from(app, absolute)) {
        const char* dirs[GUI_FOLDERS_MAX + 1];
        if (dest_dirs(app, dirs) > 1) {
            dests_open(app, absolute, prepared);
            free(driver);
            free(game_option);
            return;
        }
    }
    /* a dump not extracted yet may need more room than its disks have: asked first, Run Anyway pressed twice */
    if (folders && !app->room_ok && !room_fits(app, absolute)) {
        free(app->made_file);
        free(app->room_file);
        app->room_file = absolute;
        app->room_item = app->menu.selected;
        app->room_prepared = prepared;
        app->room_recompile = app->recompile;
        gui_confirm_open(&app->confirm, GUI_ASK_ROOM, true);
        free(driver);
        free(game_option);
        return;
    }
    /* the driver, the file, the settings, --repo and its folder, the picked games and extraction folders, NULL */
    GuiSettings* s = settings_of(app);
    const char** argv = (const char**)snx_alloc((size_t)(s->count + 18) * sizeof(*argv));
    char* out = NULL; /* a dump's extraction in the picked extractions folder */
    size_t n = 1;
    argv[0] = direct ? absolute : driver;
    if (!direct && driver_in_self(app, app->menu.selected)) argv[n++] = driver_name(app->menu.selected);
    if (make) argv[n++] = "-r";
    if (!direct) argv[n++] = game_option ? game_option : absolute;
    /* Switch options only, never guest argv for the CPU-only Linux driver or the 3DS one; R's recompile takes no
       option of a run. The drivers work in the menu's data folder, so they list the same games */
    if (make && app->menu.selected == GUI_SWITCH && gui_settings_tmpfs(s)) argv[n++] = gui_settings_tmpfs(s);
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
    if (app->menu.selected == GUI_SWITCH && !make) n += gui_settings_options(s, 1, argv + n);
    /* which of them Recommended Settings gave, for the log's settings line */
    if (app->menu.selected == GUI_SWITCH && !make) n += gui_settings_marks(s, argv + n);
    /* Play from a save state: the runtime loads it before the game's first frame */
    if (app->menu.selected == GUI_SWITCH && !make && app->state_load) argv[n++] = app->state_load;
    /* Debug's SLP vectorizer, which R's recompile takes too: a run passes it with the game's settings */
    if (app->menu.selected == GUI_SWITCH && make) n += gui_settings_one(s, "slp_vectorizer", argv + n);
    /* Extract dump, which both drivers take, for a run and for R's recompile: off, nothing is extracted */
    if ((app->menu.selected == GUI_3DS || (app->menu.selected == GUI_SWITCH && make)) && gui_settings_extract(s))
        argv[n++] = gui_settings_extract(s);
    if (folders || (native && !direct)) {
        argv[n++] = "--data";
        argv[n++] = app->data_dir;
    }
    /* a CIA's game is a folder of the recompiled library as a Switch game is, named after its title ID, its saves in
       it */
    if (folders && (app->place[GUI_PLACE_GAMES] || app->dest_out)) {
        argv[n++] = "--outdir";
        argv[n++] = app->dest_out ? app->dest_out : app->games_dir;
    }
    /* the GBA's window starts fullscreen as a Switch game's does; a compile opens none */
    if (app->menu.selected == GUI_GBA && !make) n += gui_settings_one(s, "fullscreen", argv + n);
    /* the picked extractions folder, which Extract to tmpfs leaves for the temporary one, and a Switch dump read in
       place (Extract dump off) has no use for */
    if (folders && app->place[GUI_PLACE_WORK] && app->menu.selected == GUI_SWITCH && !nro_file(absolute) &&
        !gui_settings_tmpfs(s) && gui_settings_extract(s)) {
        out = dump_out(app, absolute, NULL); /* an NRO has no extraction */
        argv[n++] = "--out";
        argv[n++] = out;
    }
    argv[n] = NULL;
    /* R's, B's and a first compile, whichever console's: the queue runs them behind the screens, one at a time */
    if (make) {
        job_add(app, argv, absolute, native, prepared,
                compile         ? GUI_JOB_COMPILE
                : app->aot_pack ? GUI_JOB_AOT
                                : GUI_JOB_RECOMPILE);
        free(argv);
        free(out);
        free(app->dest_out); /* the run's, which argv held until here: a next one asks again */
        app->dest_out = NULL;
        free(driver);
        free(absolute);
        return;
    }
#ifndef SDL_PLATFORM_ANDROID /* there the data folder's config/, which every game reads (gamepad.c) */
    pad_mappings_share(app);
#endif
#ifdef SDL_PLATFORM_IOS
    icons_wait(app); /* the one driver thread, a dump's icon's first */
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
    free(game_option); /* argv held it until the process started */
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
/* the answer to a run's room: Run Anyway, pressed twice, runs it as it was asked; Back runs nothing */
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
/* ---- the queue's runner: a recompile started when none runs, read each frame, the next one started as it ends ---- */
/* a game the menu started plays: its run's process, or on Android the game's activity, in front of the menu */
static bool game_plays(const App* app) {
#ifdef SDL_PLATFORM_ANDROID
    return app->menu.running || app->playing;
#else
    return app->menu.running;
#endif
}
/* a recompile of the queue goes gently: Background recompile Gentle, or a game playing */
static bool job_gentle(const App* app) { return app->background_gentle || game_plays(app); }
/* how the one running goes, for the Status box: Full, or Gentle and why (the option, a game playing, a priority the
   system keeps low until it ends), with the threads it was started with when they were halved */
static void job_speed(App* app, char* out, size_t size) {
    size_t n;
    if (app->job_kept_low && !job_gentle(app))
        snprintf(out, size, snx_tr("Gentle until it ends: %s"), app->job_said);
    else if (!app->job_low)
        snprintf(out, size, "%s", snx_tr("Full"));
    else if (!app->background_gentle)
        snprintf(out, size, "%s", snx_tr("Gentle, a game plays"));
    else
        snprintf(out, size, "%s", snx_tr("Gentle"));
    n = strlen(out);
    if (app->job_threads && n < size) snprintf(out + n, size - n, snx_tr(", %d threads"), app->job_threads);
    n = strlen(out);
    if (app->job_said[0] && !app->job_kept_low && n < size) /* the priority it could not have */
        snprintf(out + n, size - n, " (%s)", app->job_said);
}
/* the threads a recompile of the queue takes at full speed: the AOT threads picked, else one a core */
static int job_cores(App* app) {
    const char* picked = gui_settings_value(settings_of(app), "aot_threads");
    int n = picked ? SDL_atoi(picked) : 0;
    return n > 0 ? n : SDL_max(1, SDL_GetNumLogicalCPUCores());
}
/* the Compile tab, its focus on job i */
static void queue_show(App* app, int i) {
    if (app->editing) leave_editor(app);
    app->menu.view = GUI_LOAD_RECOMPILE;
    app->queue_focus = SDL_max(0, i);
    app->message[0] = 0;
}
/* the output box of the Compile tab's console layout, in cells of the letters a half step under the text's */
static void job_console_size(App* app, int* cols, int* rows);
/* the first recompile waiting, when none runs: its argv, half the threads and a low priority while it goes gently
   (job_gentle); one that cannot start ends there, saying why, and the next is tried */
static void job_start(App* app) {
    const char* argv[GUI_QUEUE_ARGS + 1];
    char threads[32], why[256];
    int i, n, cols, rows;
    GuiJob* job;
    bool gentle;
    while ((i = gui_queue_next(&app->queue)) >= 0 && gui_queue_running(&app->queue) < 0) {
        job = &app->queue.job[i];
        n = gui_queue_argv(&app->queue, i, argv);
        gentle = job_gentle(app);
        app->job_threads = gentle ? SDL_max(1, job_cores(app) / 2) : 0;
        if (gentle) { /* after the settings' --aot_threads=, which the last one given replaces */
            snprintf(threads, sizeof threads, "--aot_threads=%d", app->job_threads);
            argv[n++] = threads;
            argv[n] = NULL;
        }
        app->job_said[0] = 0;
        app->job_kept_low = false;
        if (!gui_process_low(app->job_process, gentle, why, sizeof why)) /* iOS's thread: its threads alone */
            snprintf(app->job_said, sizeof app->job_said, "%s", why);
        app->job_low = gentle;
#ifdef SDL_PLATFORM_IOS
        icons_wait(app); /* the one driver thread, a dump's icon's first */
#endif
        if (!gui_process_start(app->job_process, argv, why, sizeof why)) {
            job->state = GUI_JOB_FAILED;
            job->status = -1;
            job->ended_ms = SDL_max(now_ms(app), 1);
            snprintf(job->said, sizeof job->said, "%s", why);
            snprintf(app->message, sizeof app->message, "%s: %s", gui_queue_name(&app->queue, i), why);
            continue;
        }
        job->state = GUI_JOB_RUNNING;
        job->started_ms = SDL_max(now_ms(app), 1);
        job->percent = -1;
        app->job_aborting = false;
        gui_steps_reset(&app->job_steps);
        job_console_size(app, &cols, &rows);
        gui_console_init(&app->job_console, cols, rows);
        /* the log holds the queue's recompiles since one started with no ended one's line left: a failure's output
           stays for Share Log while the next ones run */
        bool fresh = true;
        for (int k = 0; k < app->queue.count; ++k) fresh = fresh && app->queue.job[k].state < GUI_JOB_OK;
        if (app->job_out) fclose(app->job_out);
        app->job_out = app->job_log ? snx_fopen(app->job_log, fresh ? "wb" : "ab") : NULL;
        for (int k = 0; app->job_out && argv[k]; ++k) fprintf(app->job_out, "%s%s", k ? " " : "$ ", argv[k]);
        if (app->job_out) fputc('\n', app->job_out);
#ifdef SDL_PLATFORM_ANDROID
        tell(app); /* now, while the menu may show: Android 12 starts no notification's service for an app behind */
#endif
    }
}
/* the tips for job i, which joined the queue behind the one running: where it waits, and how far that one went */
static void join_said(App* app, int i, int run, const char* name) {
    char where[64];
    job_where(app, i, false, where, sizeof where);
    snprintf(app->message, sizeof app->message, snx_tr("%s joins the queue: %s. %s compiles now (%d%%)"), name, where,
             gui_queue_name(&app->queue, run), SDL_max(app->queue.job[run].percent, 0));
}
/* R's, B's or a first compile's run, its argv as launch() made it: into the queue, started at once when none runs, the
   Compile tab then showing it; else the tips say where it waits, but for a compile, a game opened to play, whose line
   the tab shows. Games' recent rows have it first, as a run that starts */
static void job_add(App* app, const char* const* argv, const char* file, bool native, bool prepared, GuiJobKind kind) {
    char name[256], listed[4200];
    const char* shown = NULL;
    bool added;
    int i, run = gui_queue_running(&app->queue);
    /* the path Games or the recent games list it by, and the name Games shows it as */
    snprintf(listed, sizeof listed, "%s", file);
    for (size_t g = 0; !shown && g < app->games.count; ++g)
        if (app->games.paths[g] && !gui_games_heading(&app->games, g) &&
            (!strcmp(app->games.paths[g], file) || snx_same_file(app->games.paths[g], file))) {
            shown = app->games.shown[g];
            snprintf(listed, sizeof listed, "%s", app->games.paths[g]);
        }
    for (int r = 0; !shown && r < app->recent.count; ++r)
        if (!strcmp(app->recent.game[r].path, file)) shown = app->recent.game[r].name;
    if (shown)
        snprintf(name, sizeof name, "%s", shown);
    else {
        const char *leaf = file + strlen(file), *dot;
        while (leaf > file && leaf[-1] != '/' && leaf[-1] != '\\') --leaf;
        dot = strrchr(leaf, '.');
        snprintf(name, sizeof name, "%.*s", (int)(dot && dot != leaf ? (size_t)(dot - leaf) : strlen(leaf)), leaf);
    }
    i = gui_queue_add(&app->queue, argv, file, listed, name, kind, native, &added);
    if (i < 0) {
        snprintf(app->message, sizeof app->message,
                 snx_tr("%s is not in the queue: it holds %d compiles, and its run %d options at most"), name,
                 GUI_QUEUE_MAX, GUI_QUEUE_ARGS - 2);
        return;
    }
    if (added) app->queue.job[i].console = app->menu.selected;
    if (app->recent_file && !gui_recent_add(&app->recent, file, app->menu.selected, prepared)) {
        /* the rows under the recent games moved: Games' focus kept on the game R was pressed on, the recent one first */
        bool recent = false;
        const char* was = app->menu.view == GUI_LOAD_GAMES ? games_focused_path(app, &recent) : NULL;
        char* focus = was ? snx_strdup(was) : NULL;
        recent_changed(app);
        layout(app); /* its page as long as the rows over it leave it */
        if (focus && recent)
            app->browser.selected = 0;
        else if (focus)
            games_focus_on(app, focus);
        free(focus);
    }
    if (!added || run < 0 || kind == GUI_JOB_COMPILE) { /* the tab first: a start that fails says why there */
        queue_show(app, i);
        job_start(app);
        if (!added)
            snprintf(app->message, sizeof app->message, snx_tr("%s is in the queue already"), name);
        else if (run >= 0)
            join_said(app, i, run, name);
        return;
    }
    join_said(app, i, run, name);
}
/* job i's line out of the queue, the focus kept on the row it was on */
static void queue_drop(App* app, int i) {
    gui_queue_remove(&app->queue, i);
    if (app->queue_focus > i) --app->queue_focus;
    app->queue_focus = SDL_clamp(app->queue_focus, 0, SDL_max(app->queue.count - 1, 0));
}
/* a first compile's game, once it ended well: played when the Compile tab still shows its line and no game runs, as
   the run that compiled and played it at once went (DraVee's pick, 2026-10-24); else the tips say it is ready */
static void job_play(App* app, int i) {
    const char* name = gui_queue_name(&app->queue, i);
    bool shown = app->menu.view == GUI_LOAD_RECOMPILE && app->queue_focus == i && !app->menu.running && !app->editing &&
                 !app->setup_on;
    MadeGames made;
    const char* program;
    made_read(app, &made);
    /* a made game's own recompile plays its program; a dump's, the game made from it */
    program =
        app->queue.job[i].native ? gui_queue_file(&app->queue, i) : made_program(&made, gui_queue_file(&app->queue, i));
    if (!program)
        snprintf(app->message, sizeof app->message,
                 snx_tr("%s: compiled, but no game of %s names it as its source, so it does not play"), name,
                 app->games_dir ? app->games_dir : "recompiled/");
    else if (!shown)
        snprintf(app->message, sizeof app->message, snx_tr("%s: compiled, Enter on it plays it"), name);
    else {
        app->menu.selected = app->queue.job[i].console;
        app->compiled = true;
        launch(app, snx_strdup(program), true);
        app->compiled = false;
    }
    made_free(&made);
}
/* the recompile running ended with status: its line kept as it ended (its output with it, for its row), the end of
   its log; one aborted taken out. Games and the recent ones follow what it made */
static void job_ended(App* app, int i, int status) {
    GuiJob* job = &app->queue.job[i];
    bool aborted = app->job_aborting, failed = aborted || status != 0;
    const char* name = gui_queue_name(&app->queue, i);
    const GuiStep* named = gui_steps_last(&app->job_steps, "RECOMPILE");
    char took[64];
    Uint64 now = SDL_max(now_ms(app), 1);
    gui_steps_finish(&app->job_steps, !failed);
    if (app->job_out) {
        fprintf(app->job_out, "\n[%s, status %d]\n", aborted ? "aborted" : "ended", status);
        fclose(app->job_out);
        app->job_out = NULL;
    }
    /* the recent game named as its recompile named it, as a run of the menu's is */
    if (app->recent_file && named && named->detail[0] && !job->native &&
        gui_recent_rename(&app->recent, gui_queue_file(&app->queue, i), named->detail))
        recent_changed(app);
    recent_as_games(app); /* a dump's recompile made its game */
    space_start(app);     /* it took disk */
    if (job->kind == GUI_JOB_AOT) app->aot_checked = 0;
    app->job_aborting = false;
    app->job_low = app->job_kept_low = false;
    /* Android's question before an abort, asked of this one: the next is no answer's */
    if (app->confirm.shown && app->confirm.ask == GUI_ASK_ABORT && !app->menu.running) app->confirm.shown = false;
    if (aborted) {
        snprintf(app->message, sizeof app->message, snx_tr("%s: aborted with ESC"), name);
        queue_drop(app, i);
        return;
    }
    job->state = failed ? GUI_JOB_FAILED : GUI_JOB_OK;
    job->status = status;
    job->ended_ms = now;
    if (gui_steps_percent(&app->job_steps) >= 0) job->percent = gui_steps_percent(&app->job_steps);
    {
        Uint64 s = (now - SDL_min(job->started_ms, now)) / 1000;
        if (s >= 60)
            snprintf(took, sizeof took, snx_tr("%d min %d s"), (int)(s / 60), (int)(s % 60));
        else
            snprintf(took, sizeof took, snx_tr("%d s"), (int)s);
    }
    if (job->kind == GUI_JOB_AOT) {
        int built = aot_built(&app->job_steps);
        if (failed)
            snprintf(job->said, sizeof job->said, snx_tr("AOT pack build failed (status %d)."), status);
        else if (built > 0)
            snprintf(job->said, sizeof job->said, snx_tr("AOT pack built from %d cached function%s."), built,
                     built == 1 ? "" : "s");
        else if (built == 0)
            snprintf(job->said, sizeof job->said, "%s",
                     snx_tr("AOT pack built, but no cached function matched the game."));
        else
            snprintf(job->said, sizeof job->said, "%s", snx_tr("AOT pack built."));
    } else if (job->kind == GUI_JOB_COMPILE) {
        if (failed)
            snprintf(job->said, sizeof job->said, snx_tr("Compile failed (status %d)."), status);
        else
            snprintf(job->said, sizeof job->said, snx_tr("Compiled in %s."), took);
    } else if (failed)
        snprintf(job->said, sizeof job->said, snx_tr("Recompile failed (status %d)."), status);
    else
        snprintf(job->said, sizeof job->said, snx_tr("Recompiled in %s."), took);
    app->ended_console = app->job_console;
    app->ended_steps = app->job_steps;
    app->ended_serial = job->serial;
    if (failed) app->log_due = true; /* Share Log blinks, its log holding this one's output */
    snprintf(app->message, sizeof app->message, "%s: %s", name, job->said);
#ifdef SDL_PLATFORM_ANDROID
    app->told_result = failed                         ? SNX_TR_NOOP("Failed")
                       : job->kind == GUI_JOB_AOT     ? SNX_TR_NOOP("AOT pack built")
                       : job->kind == GUI_JOB_COMPILE ? SNX_TR_NOOP("Compiled")
                                                      : SNX_TR_NOOP("Recompiled");
#endif
    if (job->kind == GUI_JOB_COMPILE && !failed) job_play(app, i);
}
/* the priority of the one running as job_gentle says, now: low while a game plays, the option's once it ended; one the
   system keeps low stays so until it ends (job_kept_low), job_said saying why */
static void job_priority(App* app) {
    bool want = job_gentle(app);
    char why[128];
    if (want == app->job_low || (!want && app->job_kept_low)) return;
    if (!gui_process_low(app->job_process, want, why, sizeof why)) {
        snprintf(app->job_said, sizeof app->job_said, "%s", why);
        if (!want) {
            app->job_kept_low = true;
            return;
        }
    }
    app->job_low = want;
}
/* the recompile running: its output read, a bounded amount a frame as a run's is; once it ended, the next one */
static void job_pump(App* app) {
    int i = gui_queue_running(&app->queue), status;
    char buffer[4096];
    if (i < 0) return;
    for (int reads = 0; reads < 16; ++reads) {
        int n = gui_process_read(app->job_process, buffer, sizeof buffer);
        if (n <= 0) break;
        if (app->job_out) fwrite(buffer, 1, (size_t)n, app->job_out);
        gui_console_feed(&app->job_console, buffer, (size_t)n);
        gui_steps_feed(&app->job_steps, buffer, (size_t)n, now_ms(app));
    }
    app->queue.job[i].percent = gui_steps_percent(&app->job_steps);
    job_priority(app);
    if (!gui_process_finished(app->job_process, &status)) return;
    job_ended(app, i, status);
    job_start(app);
}
/* the taskbar's progress, SDL's (Windows' taskbar, a Linux desktop's launcher entry): how far the queue's compiles have
   gone (gui_queue_percent), its failure once none runs while one that failed is listed, else none. Given SDL only as it changes;
   what SDL said when it could not show it is kept for the Status box, and said once on stderr */
static void taskbar(App* app) {
    int run = gui_queue_running(&app->queue), state = SDL_PROGRESS_STATE_NONE, value = 0, i;
    bool ok;
    if (run >= 0) {
        int percent = gui_queue_percent(&app->queue);
        value = SDL_max(percent, 0);
        state = percent < 0 ? SDL_PROGRESS_STATE_INDETERMINATE : SDL_PROGRESS_STATE_NORMAL;
    } else
        for (i = 0; i < app->queue.count; ++i)
            if (app->queue.job[i].state == GUI_JOB_FAILED) {
                state = SDL_PROGRESS_STATE_ERROR;
                value = 100;
            }
    if (!app->window || (state == app->taskbar_state && value == app->taskbar_value)) return;
    SDL_ClearError();
    ok = SDL_SetWindowProgressValue(app->window, (float)value / 100.0f) &&
         SDL_SetWindowProgressState(app->window, (SDL_ProgressState)state);
    app->taskbar_state = state;
    app->taskbar_value = value;
    if (ok) {
        app->taskbar_error[0] = 0;
        return;
    }
    if (!app->taskbar_error[0])
        gui_log("the taskbar shows no progress of the recompile: %s",
                SDL_GetError()[0] ? SDL_GetError() : "SDL gave no reason");
    snprintf(app->taskbar_error, sizeof app->taskbar_error, "%s",
             SDL_GetError()[0] ? SDL_GetError() : snx_tr("SDL gave no reason"));
}
/* ESC on the one running, or Abort taken: its tree killed now, pump reaping it */
static void job_abort(App* app) {
    if (app->job_aborting || gui_queue_running(&app->queue) < 0) return;
    gui_process_terminate(app->job_process);
    app->job_aborting = true;
}
/* Enter on one that ended well, or a click on it: its game played as Games plays it, its line seen and gone. A dump's
   recompile plays the game it made */
static void queue_play(App* app) {
    int i = app->queue_focus, item = app->menu.selected;
    MadeGames made;
    const char* program = NULL;
    char* file;
    bool native;
    if (i < 0 || i >= app->queue.count || app->queue.job[i].state != GUI_JOB_OK) return;
    native = app->queue.job[i].native;
    if (!native) {
        made_read(app, &made);
        program = made_program(&made, gui_queue_file(&app->queue, i));
    }
    file = snx_strdup(program ? program : gui_queue_file(&app->queue, i));
    if (!native) made_free(&made);
    queue_drop(app, i);
    app->menu.selected = GUI_SWITCH;
    launch(app, file, native || program);
    app->menu.selected = item;
}
/* Del on the Compile tab: one waiting taken out of the queue, one that ended its line gone; the one running stays,
   ESC aborts it */
static void queue_take_out(App* app) {
    int i = app->queue_focus;
    char name[256];
    if (i < 0 || i >= app->queue.count) return;
    snprintf(name, sizeof name, "%s", gui_queue_name(&app->queue, i));
    if (app->queue.job[i].state == GUI_JOB_RUNNING) {
        snprintf(app->message, sizeof app->message, snx_tr("%s compiles now: ESC aborts it, Del takes out one waiting"),
                 name);
        return;
    }
    snprintf(app->message, sizeof app->message,
             app->queue.job[i].state == GUI_JOB_WAITING ? snx_tr("%s taken out of the queue")
                                                        : snx_tr("%s's line taken off the queue"),
             name);
    queue_drop(app, i);
}
/* back to Games from the Compile tab, where R was pressed: listed again (a recompile made its game), the focus on
   the game at path (the recompile in focus's, as Games lists it), else on the row it was on */
static void games_back(App* app, const char* path) {
    char* game = path ? snx_strdup(path) : NULL;
    GuiBrowser was = app->browser;
    app->menu.view = GUI_LOAD_GAMES;
    list_games(app);
    if (!games_focus_on(app, game)) { /* none in Games: the row it was on */
        app->browser.page = was.page;
        app->browser.selected = was.selected;
        gui_browser_resize(&app->browser, app->browser.page_size);
    }
    free(game);
}
/* the game of the Compile tab's focus, as Games lists it: what Tab and ESC there go back to; NULL with none */
static const char* queue_game(const App* app) {
    int i = app->queue_focus;
    return i >= 0 && i < app->queue.count ? gui_queue_listed(&app->queue, i) : NULL;
}
/* a key on the Compile tab: Up and Down go over the queue, Enter plays one that ended well, ESC aborts the one
   running (asked first where ask_abort says so) or goes back to Games, a line that ended seen and gone with it */
static void queue_key(App* app, GuiKey key) {
    int i = app->queue_focus, n = app->queue.count;
    if (!n) return;
    i = SDL_clamp(i, 0, n - 1);
    if (key == GUI_UP || key == GUI_DOWN)
        app->queue_focus = (i + (key == GUI_UP ? n - 1 : 1)) % n;
    else if (key == GUI_SELECT)
        queue_play(app);
    else if (key == GUI_CANCEL && app->queue.job[i].state == GUI_JOB_RUNNING) {
        if (app->ask_abort)
            gui_confirm_open(&app->confirm, GUI_ASK_ABORT, false);
        else
            job_abort(app);
    } else if (key == GUI_CANCEL) {
        char* game = snx_strdup(gui_queue_listed(&app->queue, i)); /* its line may go */
        if (app->queue.job[i].state >= GUI_JOB_OK) queue_drop(app, i);
        app->message[0] = 0;
        games_back(app, game);
        free(game);
    }
}
/* the recent game shown at row shown, run as the menu would: its driver, the options the menu has now */
static void open_recent(App* app, int shown) {
    int index = recent_index(app, shown);
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
/* the console a made game's program plays, by its folder's .ini (snx_game_system): the driver that plays it */
static int program_driver(const char* program) {
    const char* leaf = program;
    for (const char* c = program; *c; ++c)
        if (*c == '/' || *c == '\\') leaf = c + 1;
    char *folder = snx_dirname(program), *name = snx_strdup(leaf);
#ifdef _WIN32
    if (strlen(name) > 4 && !SDL_strcasecmp(name + strlen(name) - 4, ".exe")) name[strlen(name) - 4] = 0;
#endif
    SnxSystem system = folder ? snx_game_system(folder, name) : SNX_SYSTEM_SWITCH;
    free(name);
    free(folder);
    return system == SNX_SYSTEM_3DS    ? GUI_3DS
           : system == SNX_SYSTEM_VITA ? GUI_VITA
           : system == SNX_SYSTEM_GBA  ? GUI_GBA
                                       : GUI_SWITCH;
}
/* the driver a file's name says: a package its console's, a dump the Switch's; -1 for a name that says none */
static int named_driver(const char* name) {
    return gui_is_cia(name)    ? GUI_3DS
           : gui_is_vpk(name)  ? GUI_VITA
           : gui_is_gba(name)  ? GUI_GBA
           : gui_is_dump(name) ? GUI_SWITCH
                               : -1;
}
/* the driver that plays row i of games: a package its console's, a dump the Switch's, and a made game, ready to run,
   the one of the console its folder says; a heading the Switch's, which runs nothing */
static int row_driver(const GuiGames* games, size_t i) {
    if (gui_games_heading(games, i)) return GUI_SWITCH;
    int named = named_driver(games->names[i]);
    return named >= 0 ? named : program_driver(games->paths[i]);
}
/* a game of Games' page, at index of the list, played by its row's driver (row_driver) */
static void open_game(App* app, size_t index) {
    int item = app->menu.selected;
    if (gui_games_heading(&app->games, index)) return;               /* a group's heading runs nothing */
    if (gui_queue_find(&app->queue, app->games.paths[index]) >= 0) { /* it cannot play yet: its recompile shows */
        int queued = gui_queue_find(&app->queue, app->games.paths[index]);
        queue_show(app, queued);
        if (app->recompile) /* R again */
            snprintf(app->message, sizeof app->message, snx_tr("%s is in the queue already"),
                     gui_queue_name(&app->queue, queued));
        return;
    }
    if (app->games.addons[index]) { /* its updates and DLCs: which one runs is asked first */
        addons_open(app, app->games.paths[index], app->games.shown[index], false);
        if (app->addons_shown) return;
    }
    const char* name = app->games.names[index];
    app->menu.selected = app->games_drivers[index];
    launch(app, snx_absolute(app->games.paths[index]), !gui_is_dump(name) && !package(name));
    if (!app->menu.running) app->menu.selected = item;
}
/* R on Games: the focused game recompiled where it is, or its dump or package made again, by its console's driver,
   playing nothing; a Linux program runs as it is */
static void recompile_focus(App* app) {
    size_t row = app->browser.selected, head = app->browser.head;
    size_t index = app->browser.page * app->browser.page_size + row - head;
    bool recent = row < head && (int)row < recent_shown(app),
         game = row >= head && index < app->games.count && !gui_games_heading(&app->games, index);
    if (!recent && !game)
        snprintf(app->message, sizeof app->message, "%s", snx_tr("No game in focus to recompile."));
    else if (recent && app->recent.game[recent_index(app, (int)row)].driver == GUI_LINUX)
        snprintf(app->message, sizeof app->message, "%s", snx_tr("R recompiles games: a Linux program runs as it is."));
    else {
        app->recompile = true;
        if (recent)
            open_recent(app, (int)row);
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
/* the Debug settings a run of the game at path (its program) takes off their default (gui_settings_debug), with the
   picks and the GDB stub or its .ini's alone: how many. None for a package or a game of another console, whose
   runtime has its own */
static int game_debug(App* app, const char* path, int picks, char* out, size_t size) {
    char *folder = path && gui_game_folder(path) ? snx_dirname(path) : NULL, *name = folder ? aot_leaf(folder) : NULL;
    char* ini = name ? snx_game_ini(folder, name) : NULL;
    /* a made game by the console its folder says; else a dump, which the Switch driver prepares and plays */
    int nx = name ? snx_game_system(folder, name) == SNX_SYSTEM_SWITCH : path && !package(path), n = 0;
    if (size) out[0] = 0;
    if (nx) n = gui_settings_debug(settings_of(app), ini, picks, out, size);
    free(ini);
    free(name);
    free(folder);
    return n;
}
/* the recent rows' .ini's part, as list_games reads the others' */
static void recent_debug_read(App* app) {
    for (int i = 0; i < recent_shown(app); ++i)
        app->recent_debug[i] = game_debug(app, app->recent.game[recent_index(app, i)].path, 0, NULL, 0) > 0;
}
/* the focused row's Debug settings, for the side panel: looked at again when the focus moves or a second passes */
static void debug_focus_update(App* app) {
    bool recent;
    const char* path = games_focused_path(app, &recent);
    if (path && app->debug_path && !strcmp(path, app->debug_path) && now_ms(app) - app->debug_checked < 1000) return;
    free(app->debug_path);
    app->debug_path = path ? snx_strdup(path) : NULL;
    app->debug_checked = now_ms(app);
    game_debug(app, path, 1, app->debug_focus, sizeof app->debug_focus);
}
static int aot_focus_game(App* app, AotFocus* f) {
    size_t row = app->browser.selected, head = app->browser.head;
    size_t index = app->browser.page * app->browser.page_size + row - head;
    bool recent = row < head && (int)row < recent_shown(app), game = row >= head && index < app->games.count;
    const char* path = NULL;
    f->path = NULL;
    f->folder = f->name = NULL;
    if (recent) {
        if (app->recent.game[recent_index(app, (int)row)].driver != GUI_SWITCH) return 0;
        path = app->recent.game[recent_index(app, (int)row)].path;
    } else if (game) {
        if (gui_games_heading(&app->games, index) || gui_is_cia(app->games.names[index]) ||
            gui_is_vpk(app->games.names[index]) || gui_is_gba(app->games.names[index]))
            return 0; /* no path */
        path = app->games.paths[index];
    } else {
        return 0;
    }
    if (!gui_game_folder(path)) return 0;
    f->folder = snx_dirname(path);
    f->name = f->folder ? aot_leaf(f->folder) : NULL;
    {
        /* the base program, not an add-on's: one pack for the game, the add-ons run from it. Its name is the folder's,
           .exe on Windows */
        char* file = aot_leaf(path);
        size_t n = file ? strlen(file) : 0;
        int base;
        if (n > 4 && !SDL_strcasecmp(file + n - 4, ".exe")) file[n - 4] = 0;
        base = f->folder && f->name && file && !strcmp(file, f->name);
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
    app->made = (SnxGameMade){0, -1, 0, 0};
    app->made_here = true;
}
static bool made_here(const char* program);
/* the focused game's cache, for the side panel and the B key, and how its code was made and whether this build plays
   it as it is: looked at again when the focus moves or a second passes */
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
    {
        char* base = snx_path_join(f.folder, f.name);
        snx_game_made_read(base, &app->made);
        free(base);
    }
    app->made_here = made_here(f.path);
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
                 snx_tr("The JIT cache's level (JIT optimization O%c) and the game's AOT optimization O%c differ. An "
                        "AOT pack needs them equal: set Optimization level to O%c before building, or set JIT "
                        "optimization to O%c and play once to make a matching cache."),
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
    app->aot_pack = false; /* the queue keeps its argv, the cache's path in it */
    free(app->aot_cache);
    app->aot_cache = NULL;
done:
    free(cache);
    free(f.folder);
    free(f.name);
}
/* M on Games: the box of the focused game's updates, DLCs, mods and cheats, shown even when it has none of them for
   its Add Mod and Add Cheats; else why not */
static void mods_focus(App* app) {
    size_t row = app->browser.selected, head = app->browser.head;
    size_t index = app->browser.page * app->browser.page_size + row - head;
    bool recent = row < head && (int)row < recent_shown(app),
         game = row >= head && index < app->games.count && !gui_games_heading(&app->games, index); /* it has no path */
    const char *path = NULL, *title = NULL;
    char* folder = NULL;
    if (recent && app->recent.game[recent_index(app, (int)row)].driver == GUI_SWITCH) {
        path = app->recent.game[recent_index(app, (int)row)].path;
        title = app->recent.game[recent_index(app, (int)row)].name;
    } else if (game) {
        path = app->games.paths[index];
        title = app->games.shown[index];
    }
    if (path) gui_game_mods(path, &folder, NULL); /* a recompiled game's, in a folder of its own, has one */
    if (folder)
        addons_open(app, path, title, true);
    else
        snprintf(app->message, sizeof app->message, "%s",
                 snx_tr("M opens the mods and cheats of a recompiled Switch game in focus."));
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
/* a file dropped or picked on a load whose name says another console's (Android's picker shows every file): refused,
   saying the load that opens it, or the driver it needs that is not there; true then */
static bool drop_elsewhere(App* app, const char* path) {
    int named = named_driver(path);
    if (named < 0 || named == app->menu.selected) return false;
    app->loads_at = 0; /* looked for again, as Enter on a load does */
    if (loads_off(app) >> named & 1u)
        snprintf(app->message, sizeof app->message,
                 snx_tr("%s opens with %s: cannot find %s beside ScarletNX or on PATH."), last_part(path),
                 snx_tr(entry_text(named)), driver_name(named));
    else
        snprintf(app->message, sizeof app->message, snx_tr("%s opens with %s, not %s."), last_part(path),
                 snx_tr(entry_text(named)), snx_tr(entry_text(app->menu.selected)));
    say_failure(app, app->message);
    return true;
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
        if (!drop_elsewhere(app, absolute))
            launch(app, absolute, false);
        else
            free(absolute);
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
/* Share Log's files, each a path and the name it takes in the zip, both owned */
typedef struct {
    GuiZipFile* files;
    size_t count, cap;
} LogFiles;
static void log_files_add(LogFiles* l, char* path, char* name) {
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->files = (GuiZipFile*)snx_realloc(l->files, l->cap * sizeof *l->files);
    }
    l->files[l->count++] = (GuiZipFile){path, name};
}
static void log_files_free(LogFiles* l) {
    for (size_t i = 0; i < l->count; ++i) {
        free((char*)l->files[i].path);
        free((char*)l->files[i].name);
    }
    free(l->files);
}
static bool named_log(const char* name, const char* prefix) {
    size_t n = strlen(name), k = strlen(prefix);
    return n > k + 3 && !strncmp(name, prefix, k) && !strcmp(name + n - 4, ".log");
}
/* the launcher's logs in folder, under launcher/: its log a day and the older versions' one, its fatal errors, and the
   last run's and recompile's of each launcher open */
static void launcher_logs(LogFiles* l, const char* folder) {
    char** names = NULL;
    size_t count = 0, i;
    if (!folder || snx_list_dir(folder, &names, &count)) return;
    for (i = 0; i < count; ++i)
        if (named_log(names[i], "scarletnx-console") || !strcmp(names[i], SNX_ERROR_LOG) ||
            named_log(names[i], "last-run") || named_log(names[i], "last-recompile"))
            log_files_add(l, snx_path_join(folder, names[i]), snx_format("launcher/%s", names[i]));
    snx_free_strings(names, count);
}
/* path under launcher/ unless it is there already, when it is a file */
static void launcher_log(LogFiles* l, const char* path) {
    const char* leaf;
    if (!path || !snx_is_file(path)) return;
    for (size_t i = 0; i < l->count; ++i)
        if (!strcmp(l->files[i].path, path)) return;
    for (leaf = path + strlen(path); leaf > path && leaf[-1] != '/' && leaf[-1] != '\\'; --leaf) continue;
    log_files_add(l, snx_strdup(path), snx_format("launcher/%s", leaf));
}
/* folder, a game's in the state folder, as the zip names it: as Games shows the game, with the folder after it in
   brackets (its title ID, or the name its recompile gave it), the letters a zip's folder cannot take as _ */
static char* zip_game_label(const App* app, const char* folder) {
    const char* shown = NULL;
    char *label, *c;
    for (size_t i = 0; i < app->games.count && !shown; ++i) {
        char* dir = app->games.paths[i] ? snx_dirname(app->games.paths[i]) : NULL;
        const char* leaf = dir ? dir + strlen(dir) : NULL;
        while (leaf && leaf > dir && leaf[-1] != '/' && leaf[-1] != '\\') --leaf;
        if (leaf && !strcmp(leaf, folder) && app->games.shown[i] && strcmp(app->games.shown[i], folder))
            shown = app->games.shown[i];
        free(dir);
    }
    label = shown ? snx_format("%s [%s]", shown, folder) : snx_strdup(folder);
    for (c = label; *c; ++c)
        if ((unsigned char)*c < 0x20 || strchr("/\\:*?\"<>|", *c)) *c = '_';
    return label;
}
/* when path was last written, as a run's log is named: 2026-10-28_14-05-33 */
static void log_stamp(SnxU64 when, char* out, size_t size) {
    time_t t = (time_t)when;
    struct tm tm;
    out[0] = 0;
#ifdef _WIN32
    if (!localtime_s(&tm, &t)) strftime(out, size, "%Y-%m-%d_%H-%M-%S", &tm);
#else
    if (localtime_r(&t, &tm)) strftime(out, size, "%Y-%m-%d_%H-%M-%S", &tm);
#endif
}
/* the games' logs written in the last GUI_LOG_DAYS days, under games/<zip_game_label>/: each game's of this run by when
   it was last written, as it is named once the next run moves it into logs/, and its runs before in logs/; an older
   version's games/<game>.log under its name */
static void games_logs(const App* app, LogFiles* l) {
    char *state = snx_user_path(app->games_dir, SNX_USER_STATE), **names = NULL, stamp[32];
    size_t count = 0, i, j;
    SnxU64 since = (SnxU64)time(NULL) - (SnxU64)GUI_LOG_DAYS * 86400;
    if (!state || snx_list_dir(state, &names, &count)) {
        free(state);
        return;
    }
    for (i = 0; i < count; ++i) {
        char *path = snx_path_join(state, names[i]), *own, *log, *runs, *label, **old = NULL;
        size_t olds = 0;
        if (!snx_is_dir(path)) {
            if (named_log(names[i], "") && snx_file_time(path) >= since)
                log_files_add(l, path, snx_format("games/%s", names[i]));
            else
                free(path);
            continue;
        }
        label = zip_game_label(app, names[i]);
        own = snx_format("%s.log", names[i]);
        log = snx_path_join(path, own);
        if (snx_file_time(log) >= since) {
            log_stamp(snx_file_time(log), stamp, sizeof stamp);
            log_files_add(l, log, snx_format("games/%s/%s.log", label, stamp));
        } else
            free(log);
        runs = snx_path_join(path, "logs");
        if (!snx_list_dir(runs, &old, &olds))
            for (j = 0; j < olds; ++j) {
                char* run = snx_path_join(runs, old[j]);
                if (named_log(old[j], "") && snx_file_time(run) >= since)
                    log_files_add(l, run, snx_format("games/%s/%s", label, old[j]));
                else
                    free(run);
            }
        snx_free_strings(old, olds);
        free(runs);
        free(own);
        free(label);
        free(path);
    }
    snx_free_strings(names, count);
    free(state);
}
/* the folder Open Logs Folder opens and Share Log's share/ is in, owned: Folders' Logs; on iOS Documents' logs/, the
   launcher's own log's, as the Files app shows Documents alone and Logs is in Library. NULL for none */
static char* logs_folder(const App* app) {
#ifdef SDL_PLATFORM_IOS
    char console[4096];
    gui_log_path(SNX_CONSOLE_LOG, console, sizeof console);
    (void)app;
    return console[0] ? snx_dirname(console) : NULL;
#else
    const char* logs = place_path(app, GUI_PLACE_STATE);
    return logs ? snx_strdup(logs) : NULL;
#endif
}
/* why logs_folder gave none */
static const char* logs_none(void) {
#ifdef SDL_PLATFORM_IOS
    return snx_tr("the launcher's log has no folder in Documents");
#else
    return snx_tr("Settings > App > Folders' Logs names no folder");
#endif
}
/* Share Log's folder, share/ in logs_folder, emptied of its files: 0, else -1 with why in app->message */
static int share_folder_empty(App* app, const char* share) {
    char why[256], **names = NULL;
    size_t count = 0, i;
    int rc = 0;
    if (snx_mkdirs(share) || snx_list_dir(share, &names, &count)) {
        snprintf(app->message, sizeof app->message, snx_tr("The logs could not be saved: cannot make %s: %s"), share,
                 snx_os_error(why, sizeof why));
        return -1;
    }
    for (i = 0; i < count && !rc; ++i) {
        char* path = snx_path_join(share, names[i]);
        if (path && snx_is_file(path) && remove(path)) {
            snprintf(app->message, sizeof app->message,
                     snx_tr("The logs could not be saved: %s, of what was shared before, could not be deleted: %s"),
                     path, snx_os_error(why, sizeof why));
            rc = -1;
        }
        free(path);
    }
    snx_free_strings(names, count);
    return rc;
}
/* the message on the screen, the summary of the newest game log, the last run's output, the launcher's own log and its
   fatal errors, and that game log, in share/ of logs_folder, emptied first: with zip as summary.txt of
   scarletnx-logs-<when>.zip beside the launcher's logs and the games' of the last GUI_LOG_DAYS days (gui/logzip.h),
   else as scarletnx.log alone; then that folder opened in the system's file manager, to share from there */
static void share_log(App* app, bool zip) {
    char *game, *run, *recompiled, *said, *fatal, *played, *summary, *text, *saved = NULL, *share, *logs;
    char console[4096], errors[4096];
    bool written = false, ready;
    left_behind(app);
    game = newest_log(app);
    run = log_tail(app->run_log, 1 << 20);
    recompiled = log_tail(app->job_log, 1 << 20);
    gui_log_path(SNX_CONSOLE_LOG, console, sizeof console);
    gui_log_path(SNX_ERROR_LOG, errors, sizeof errors);
    said = log_tail(console[0] ? console : NULL, 1 << 20);
    fatal = log_tail(errors[0] ? errors : NULL, 64 << 10);
    played = log_tail(game, 4 << 20);
    summary = game ? log_summary(game) : snx_strdup("");
    text = snx_format(
        "%s\nMessage: %s\n%s%s%s%s\n%s== the last run: %s ==\n%s\n\n== the last recompile of the queue: %s "
        "==\n%s\n\n== the launcher's log: %s ==\n%s\n\n== fatal errors: %s ==\n%s\n\n== %s ==\n%s\n",
        snx_build_info(), app->message, app->left ? app->left : "", app->left_clear ? SAY_LEFT_CLEAR "\n" : "",
        app->cleaned.count ? "Clean Old Settings:\n" : "", app->cleaned.text ? app->cleaned.text : "", summary,
        app->run_log ? app->run_log : "", run, app->job_log ? app->job_log : "", recompiled,
        console[0] ? console : "none", said, errors[0] ? errors : "none", fatal, game ? game : "no game log", played);
    logs = logs_folder(app);
    share = logs ? snx_path_join(logs, "share") : NULL;
    if (!share) snprintf(app->message, sizeof(app->message), snx_tr("The logs could not be saved: %s"), logs_none());
    ready = share && !share_folder_empty(app, share); /* else why, in the message */
    if (ready && !zip) {
        saved = snx_path_join(share, "scarletnx.log");
        written = saved && !snx_write_file(saved, text, strlen(text));
        if (saved && !written) {
            char why[256];
            snprintf(app->message, sizeof(app->message), snx_tr("The log could not be saved to %s: %s"), saved,
                     snx_os_error(why, sizeof why));
        }
    } else if (ready) {
        char stamp[32], *zip_name, error[sizeof app->message], *at[3];
        LogFiles files = {0};
        /* the launcher's log's folder, and the run logs' wherever they are kept apart */
        at[0] = console[0] ? snx_dirname(console) : NULL;
        at[1] = app->run_log ? snx_dirname(app->run_log) : NULL;
        at[2] = app->job_log ? snx_dirname(app->job_log) : NULL;
        for (int k = 0; k < 3; ++k) {
            bool seen = false;
            for (int j = 0; j < k && !seen; ++j) seen = at[j] && at[k] && !strcmp(at[j], at[k]);
            if (at[k] && !seen) launcher_logs(&files, at[k]);
        }
        launcher_log(&files, app->run_log); /* this launcher's two, whatever their names */
        launcher_log(&files, app->job_log);
        games_logs(app, &files);
        log_stamp((SnxU64)time(NULL), stamp, sizeof stamp);
        zip_name = snx_format("scarletnx-logs-%s.zip", stamp);
        saved = snx_path_join(share, zip_name);
        written = saved && !gui_logs_zip(saved, text, files.files, files.count, error, sizeof error);
        if (saved && !written)
            snprintf(app->message, sizeof(app->message), snx_tr("The logs could not be saved: %s"), error);
        log_files_free(&files);
        free(zip_name);
        for (int k = 0; k < 3; ++k) free(at[k]);
    }
    if (written) {
        const char* leaf = saved + strlen(saved);
        char said_open[sizeof app->message];
        while (leaf > saved && leaf[-1] != '/' && leaf[-1] != '\\') --leaf;
        app->log_due = false; /* the crash's log is out */
        if (show_folder(app, share)) /* the system's file manager shares it from there, as it shares any file */
            snprintf(app->message, sizeof(app->message), snx_tr("%s is in %s, opened"), leaf, share);
        else {
            snprintf(said_open, sizeof said_open, "%s", app->message);
            snprintf(app->message, sizeof(app->message), snx_tr("%s is in %s; %s"), leaf, share, said_open);
        }
    } else
        say_failure(app, app->message);
    free(saved);
    free(share);
    free(logs);
    free(text);
    free(summary);
    free(played);
    free(fatal);
    free(said);
    free(recompiled);
    free(run);
    free(game);
}
/* gui_games_group's made: a dump a game of recompiled/ was made from already, of made_read's */
static bool made_dump(const char* path, const void* made) { return made_program((const MadeGames*)made, path) != NULL; }
/* the Games list: recompiled/, dumps/ (with its subfolders) and the Library's folders, every kind, in two groups: the
   games ready to run, then the dumps not made already, which Dumps off leaves out; each by what it is shown as in each
   folder, or with Sort's Last opened the game that last ran first. The first folder that cannot be read is named, the
   others still listed */
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
    /* Dumps off: the dumps' folders not read, and the dumps and CIAs the others hold left out of the groups */
    if (dumps_on(app) && dumps && snx_is_dir(dumps) && /* none: places_apply said it could not be made */
        !gui_games_add_deep(games, dumps, GUI_GAMES_DUMP | GUI_GAMES_CIA, app->dumps_shallow ? 0 : GUI_FOLDER_DEPTH,
                            error, sizeof error) &&
        !message[0])
        snprintf(message, size, "%s", error);
    for (i = 0; i < app->folder_count; ++i) /* a library's folder, its library's games */
        if ((dumps_on(app) || app->folders_library[i] == GUI_LIBRARY_RECOMPILED) &&
            !gui_games_add_deep(games, app->folders[i],
                                (app->folders_library[i] == GUI_LIBRARY_RECOMPILED ? GUI_GAMES_RUN : GUI_GAMES_DUMP) |
                                    GUI_GAMES_CIA,
                                app->folders_deep[i] ? GUI_FOLDER_DEPTH : 0, error, sizeof error) &&
            !message[0])
            snprintf(message, size, snx_tr("%s It is in Settings > App > Folders."), error);
    made_read(app, &made);
    if (!gui_games_group(games, snx_tr("Recompiled"), dumps_on(app) ? snx_tr("Dumps") : NULL, made_dump, &made) &&
        !message[0])
        snprintf(message, size, "%s", snx_tr("Not enough memory to list the games in their groups."));
    if (by_last_opened(app) && !gui_games_by_last_opened(games) && !message[0])
        snprintf(message, size, "%s", snx_tr("Not enough memory to sort the games by the last opened: by title."));
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
/* the names dir holds into h, and with depth those of its folders, down that many levels, hidden ones left alone; it
   ends where it is once *stop is set */
static uint64_t stamp_dir(uint64_t h, const char* dir, int depth, const int* stop) {
    char** names = NULL;
    size_t count = 0, k;
    const char* c;
    if (!dir || snx_list_dir(dir, &names, &count)) count = (size_t)-1; /* none: not the same as an empty one */
    for (k = 0; k < count && names && !(stop && __atomic_load_n(stop, __ATOMIC_RELAXED)); ++k) {
        for (c = names[k];; ++c) {
            h = (h ^ (unsigned char)*c) * 1099511628211ull;
            if (!*c) break;
        }
        if (depth > 0 && names[k][0] != '.') {
            char* sub = snx_path_join(dir, names[k]);
            if (sub && snx_is_dir(sub)) h = stamp_dir(h, sub, depth - 1, stop);
            free(sub);
        }
    }
    h = (h ^ (uint64_t)count) * 1099511628211ull;
    if (names) snx_free_strings(names, count);
    return h;
}
/* the names the folders hold (each down its depth), hashed: a game put in, taken out or renamed changes it, and so
   does a folder that comes or goes */
static uint64_t stamp_dirs(char* const* dirs, const int* depths, int count, const int* stop) {
    uint64_t h = 1469598103934665603ull; /* FNV-1a */
    int i;
    for (i = 0; i < count; ++i) h = stamp_dir(h, dirs[i], depths[i], stop);
    return h;
}
/* the folders Games lists, each owned (NULL none) with its depth: recompiled/, the player's (a folder kept with its
   subfolders, theirs too), dumps/ and an older version's games/; how many */
static int watch_dirs(const App* app, char** dirs, int* depths) {
    int i, n = 0;
    dirs[n] = app->games_dir ? snx_strdup(app->games_dir) : NULL;
    depths[n++] = 0;
    for (i = 0; i < app->folder_count; ++i) {
        dirs[n] = snx_strdup(app->folders[i]);
        depths[n++] = app->folders_deep[i] ? GUI_FOLDER_DEPTH : 0;
    }
    dirs[n] = app->data_dir ? snx_path_join(app->data_dir, "dumps") : NULL;
    depths[n++] = app->dumps_shallow ? 0 : GUI_FOLDER_DEPTH;
    dirs[n] = games_old(app);
    depths[n++] = 0;
    return n;
}
/* the folders Games lists, handed to the watcher: their hash taken from its next look */
static void watch_set(App* app) {
    GuiWatch* w = &app->watch;
    char* dirs[GUI_WATCH_DIRS];
    int depths[GUI_WATCH_DIRS], n = watch_dirs(app, dirs, depths), i;
    if (!w->lock && !(w->lock = SDL_CreateMutex())) {
        snprintf(app->message, sizeof app->message, snx_tr("Games is not watched for new games (%s): F5 lists them"),
                 SDL_GetError());
        for (i = 0; i < n; ++i) free(dirs[i]);
        return;
    }
    SDL_LockMutex(w->lock);
    for (i = 0; i < w->count; ++i) free(w->dirs[i]);
    memcpy(w->dirs, dirs, sizeof dirs);
    memcpy(w->depths, depths, sizeof depths);
    w->count = n;
    ++w->set;
    SDL_UnlockMutex(w->lock);
}
/* a look at the folders: their hash, kept unless they were handed again while it was taken */
static void watch_pass(GuiWatch* w) {
    char* dirs[GUI_WATCH_DIRS];
    int depths[GUI_WATCH_DIRS], n, i;
    unsigned set;
    uint64_t h;
    if (!w->lock) return;
    SDL_LockMutex(w->lock);
    for (i = n = 0; i < w->count; ++i, ++n) {
        dirs[n] = w->dirs[i] ? snx_strdup(w->dirs[i]) : NULL;
        depths[n] = w->depths[i];
    }
    set = w->set;
    SDL_UnlockMutex(w->lock);
    h = stamp_dirs(dirs, depths, n, &w->stop);
    for (i = 0; i < n; ++i) free(dirs[i]);
    SDL_LockMutex(w->lock);
    if (w->set == set && !__atomic_load_n(&w->stop, __ATOMIC_RELAXED)) {
        w->stamp = h;
        w->stamped = set;
    }
    SDL_UnlockMutex(w->lock);
}
static int SDLCALL watch_run(void* data) {
    GuiWatch* w = (GuiWatch*)data;
    int i;
    while (!__atomic_load_n(&w->stop, __ATOMIC_RELAXED)) {
        watch_pass(w);
        for (i = 0; i < 20 && !__atomic_load_n(&w->stop, __ATOMIC_RELAXED); ++i) SDL_Delay(50); /* a second */
    }
    return 0;
}
/* the last hash of the folders as last handed, when the watcher has one */
static bool watch_get(App* app, uint64_t* stamp) {
    GuiWatch* w = &app->watch;
    bool got;
    if (!w->lock) return false;
    SDL_LockMutex(w->lock);
    got = w->stamped && w->stamped == w->set;
    *stamp = w->stamp;
    SDL_UnlockMutex(w->lock);
    return got;
}
/* the watcher's thread, looking from now on; without one Games follows its folders only by F5, said */
static void watch_start(App* app) {
    watch_set(app);
    if (app->watch.lock && !(app->watch.thread = SDL_CreateThread(watch_run, "games", &app->watch)))
        snprintf(app->message, sizeof app->message, snx_tr("Games is not watched for new games (%s): F5 lists them"),
                 SDL_GetError());
}
static void watch_stop(App* app) {
    GuiWatch* w = &app->watch;
    int i;
    __atomic_store_n(&w->stop, 1, __ATOMIC_RELAXED);
    if (w->thread) SDL_WaitThread(w->thread, NULL);
    for (i = 0; i < w->count; ++i) free(w->dirs[i]);
    if (w->lock) SDL_DestroyMutex(w->lock);
    *w = (GuiWatch){0};
}
static void list_games(App* app) {
    icons_free(app); /* a game made since has one now */
    games_scan(app, &app->games, app->message, sizeof app->message);
    /* each row's driver, read from its folder once here rather than at each frame that shows its kind */
    free(app->games_drivers);
    app->games_drivers = (int*)snx_alloc((app->games.count + 1) * sizeof *app->games_drivers);
    for (size_t i = 0; i < app->games.count; ++i) app->games_drivers[i] = row_driver(&app->games, i);
    free(app->games_debug);
    app->games_debug = (bool*)snx_alloc((app->games.count + 1) * sizeof *app->games_debug);
    for (size_t i = 0; i < app->games.count; ++i)
        app->games_debug[i] = app->games.paths[i] && game_debug(app, app->games.paths[i], 0, NULL, 0) > 0;
    recent_as_games(app); /* a recent dump made since, by any run, shows as its game */
    recent_debug_read(app);
    app->debug_checked = 0; /* and the focused row's, at the next draw */
    app->games_fresh = true;
    watch_set(app);
    memset(&app->browser, 0, sizeof(app->browser));
    app->browser.count = app->games.count;
    app->browser.head = games_head(app);
    layout(app);
    /* with no recent game first, the focus on the first game, past a group's heading */
    while (!app->browser.head && app->browser.selected < gui_browser_items(&app->browser) &&
           gui_games_heading(&app->games, app->browser.selected))
        ++app->browser.selected;
}
/* the screen a tab opens: the menu, Games or the Settings. A theme edited and not saved keeps its screen, the note
   saying why */
static void go_tab(App* app, int tab) {
    if (app->editing) {
        if (app->editor.typing || tab == GUI_TAB_SETTINGS) return;
        if (app->form == GUI_FORM_THEME && app->editor.dirty) {
            snprintf(app->editor.note, sizeof app->editor.note, "%s",
                     snx_tr("Not saved: Ctrl+S saves it, ESC again leaves without it"));
            return;
        }
        keep_settings(app);
        if (app->form == GUI_FORM_PAD) pad_close(app);
        device_stop(app);
        leave_editor(app);
        app->theme_from_app = app->folders_from_games = app->folders_from_menu = false;
    }
    if (tab == tab_shown(app)) return;
    app->message[0] = 0;
    if (tab == GUI_TAB_RECOMPILE) { /* the one running in focus, else the last that ended */
        int run = gui_queue_running(&app->queue);
        queue_show(app, run >= 0 ? run : app->queue.count - 1);
        return;
    }
    if (tab == GUI_TAB_GAMES && app->menu.view == GUI_LOAD_RECOMPILE) {
        games_back(app, queue_game(app));
        return;
    }
    app->menu.view = tab == GUI_TAB_GAMES ? GUI_LOAD_GAMES : GUI_LOAD_NONE;
    if (tab == GUI_TAB_GAMES)
        list_games(app);
    else if (tab == GUI_TAB_SETTINGS)
        open_settings(app);
}
/* the focus on the grid's row */
static void grid_focus(App* app, size_t row) {
    app->browser.page = 0;
    app->browser.selected = games_selected(app, row);
}
/* a key on the grid: Left and Right the row before or after the focus; Up and Down the tile above or under it, PgUp
   and PgDn as many lines up or down as show (the first or the last line where fewer are left), on the focus's
   column or its line's last. A label passed over, nothing past the first row or the last */
static void grid_key(App* app, GuiKey key) {
    GuiGamesBoxes g;
    UiFlow f;
    GuiRect at;
    size_t rows = games_rows(app), focus, r, to;
    int line, col, step = key == GUI_UP || key == GUI_PAGE_UP ? -1 : 1, lines = 1, want, target = -1, far = -1;
    games_boxes(app, &g);
    focus = g.grid_lists[UI_GAMES_GRID_TILES].focus;
    if (focus >= rows) return;
    if (key == GUI_LEFT || key == GUI_RIGHT) {
        for (to = focus;;) {
            if (key == GUI_LEFT ? !to : to + 1 >= rows) return;
            to = key == GUI_LEFT ? to - 1 : to + 1;
            if (!games_row_label(app, to)) break;
        }
        grid_focus(app, to);
        return;
    }
    grid_flow(app, &g, &f);
    for (r = 0; r <= focus && ui_flow_next(&f, games_row_label(app, r), &at); ++r) {}
    line = f.line;
    col = f.col - 1; /* the focus's: placing it moved the column past it */
    if (key == GUI_PAGE_UP || key == GUI_PAGE_DOWN)
        lines = SDL_max(1, (g.grid_rects[UI_GAMES_GRID_TILES].h + f.gap) / (f.tile_h + f.gap));
    want = line + step * lines;
    /* the tiles' line nearest want at it or past it, and the farthest one that way */
    grid_flow(app, &g, &f);
    for (r = 0; r < rows && ui_flow_next(&f, games_row_label(app, r), &at); ++r) {
        if (games_row_label(app, r)) continue;
        if ((step < 0 ? f.line <= want : f.line >= want) && (target < 0 || step * (target - f.line) > 0))
            target = f.line;
        if (step * (f.line - line) > 0 && (far < 0 || step * (f.line - far) > 0)) far = f.line;
    }
    if (target < 0 && lines > 1) target = far;
    if (target < 0) return;
    grid_flow(app, &g, &f);
    for (r = 0, to = rows; r < rows && ui_flow_next(&f, games_row_label(app, r), &at) && f.line <= target; ++r)
        if (f.line == target && !games_row_label(app, r) && (to == rows || f.col - 1 <= col)) to = r;
    if (to < rows) grid_focus(app, to);
}
/* the tab step tabs from the one shown, round them: Tab and Shift+Tab, a pad's shoulders. Tab on the Compile tab
   goes back to Games, where R was pressed, the recompile going on */
static void screen_tab(App* app, int step) {
    int n = tab_count(app);
    if (app->menu.running || app->finished || app->confirm.shown || app->splash) return;
    if (step > 0 && tab_shown(app) == GUI_TAB_RECOMPILE && !app->editing && !app->setup_on)
        go_tab(app, GUI_TAB_GAMES);
    else
        go_tab(app, ((tab_shown(app) + step) % n + n) % n);
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
    if (app->menu.view == GUI_LOAD_RECOMPILE) {
        queue_key(app, key);
        return;
    }
    if (app->menu.view == GUI_LOAD_GAMES) {
        /* Resizes and key events can arrive in the same SDL poll batch. */
        layout(app);
        if (games_grid(app) && key != GUI_SELECT && key != GUI_CANCEL) {
            grid_key(app, key);
            return;
        }
        if (key == GUI_LEFT || key == GUI_RIGHT || key == GUI_PAGE_UP || key == GUI_PAGE_DOWN) {
            /* the list's one page: a screen of rows up or down, nothing past its first row or its last */
            GuiGamesBoxes g;
            int row_h, total, view, n;
            GuiKey step = key == GUI_LEFT || key == GUI_PAGE_UP ? GUI_UP : GUI_DOWN;
            games_boxes(app, &g);
            games_extent(app, &g, &row_h, &total, &view);
            for (n = SDL_max(1, view / row_h - 1); n > 0; --n) {
                if (step == GUI_UP ? !app->browser.selected
                                   : app->browser.selected + 1 >= app->browser.head + gui_browser_items(&app->browser))
                    break;
                gui_browser_key_skip(&app->browser, step, games_heading_at, &app->games);
            }
            return;
        }
        GuiBrowserAction action = gui_browser_key_skip(&app->browser, key, games_heading_at, &app->games);
        size_t row = app->browser.selected;
        if (action == GUI_BROWSER_BACK) {
            app->menu.view = GUI_LOAD_NONE;
            app->message[0] = 0;
        } else if (action == GUI_BROWSER_HEAD && (int)row < recent_shown(app))
            open_recent(app, (int)row);
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
    /* a load whose driver is not there opens no screen: it says which driver, looked for again now (#640) */
    if (key == GUI_SELECT && app->menu.view == GUI_LOAD_NONE && !app->menu.running && app->menu.selected <= GUI_GBA) {
        app->loads_at = 0;
        if (loads_off(app) >> app->menu.selected & 1u) {
            snprintf(app->message, sizeof(app->message), snx_tr("Cannot find %s beside ScarletNX or on PATH."),
                     driver_name(app->menu.selected));
            return;
        }
    }
    switch (gui_menu_key(&app->menu, key)) {
        case GUI_MENU_SETTINGS: open_settings(app); break;
        case GUI_MENU_LOG: picks_open(app, GUI_PICKS_SHARE); break;
        case GUI_MENU_STAY:
            if (app->menu.quit && jobs_live(app)) { /* Exit: the queue's recompiles go with the menu, asked first */
                app->menu.quit = false;
                gui_confirm_open(&app->confirm, GUI_ASK_EXIT, false);
            }
            break;
        case GUI_MENU_FOLDERS: /* Settings > App > Folders, as P opens it from Games: Back comes here again */
            app->group = 0;
            open_folders(app, 0);
            app->folders_from_menu = true;
            break;
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
    if (!app->menu.running) { /* the Compile tab's question: the queue's one running */
        job_abort(app);
        return;
    }
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
    else if (ask == GUI_ASK_MADE)
        made_answer(app, key == GUI_SELECT, second);
    else if (ask == GUI_ASK_CLEAN || ask == GUI_ASK_RESET || ask == GUI_ASK_PLACES || ask == GUI_ASK_PLACE ||
             ask == GUI_ASK_FOLDER || ask == GUI_ASK_MOVE || ask == GUI_ASK_STATE || ask == GUI_ASK_CACHE)
        change_answer(app, ask, second);
    else if (ask == GUI_ASK_CLOSE && second) { /* the run ends with the menu, the queue's recompile too */
        if (app->menu.running) abort_run(app);
        job_abort(app);
        closing_start(app);
    } else if (ask == GUI_ASK_EXIT && second)
        closing_start(app);
    else if (ask == GUI_ASK_ABORT && second)
        abort_run(app);
}
/* the pointer on the question, at pixel x, y: a choice under it takes the focus, a click presses it
   (ui_choices_rows_row_action_cb); a click off the box is Back */
static void ask_pointer(App* app, float x, float y, bool click) {
    GuiChoices c;
    bool on;
    ask_place(app, &c);
    if (!choices_pointer(app, &c, x, y, click, &on) && click && !on) ask_key(app, GUI_CANCEL);
}
static const char* choice_text(App* app, int of, int i, char* out, size_t size) {
    const char* labels[GUI_SETUP_CHOICES];
    GuiAsk ask = app->confirm.ask;
    if (i < 0) return NULL;
    switch (of) {
        case GUI_CHOICES_ASK:
            if (i >= (ask_choices[ask][1] ? 2 : 1)) return NULL;
            if (!i) return snx_tr(ask_choices[ask][0]);
            snprintf(out, size, "%s%s%s", snx_tr(ask_choices[ask][1]), app->confirm.armed ? ": " : "",
                     app->confirm.armed ? snx_tr(SAY_AGAIN) : "");
            return out;
        case GUI_CHOICES_SIZES: return i < 2 ? snx_tr(size_choices[app->size_until != 0][i]) : NULL;
        case GUI_CHOICES_DESTS: return dest_label(app, i, out, size);
        case GUI_CHOICES_PICKS: return pick_label(app, i);
        case GUI_CHOICES_TYPED: return i < 2 ? snx_tr(typed_choices[i]) : NULL;
        default: return i < setup_labels(app, labels) ? labels[i] : NULL;
    }
}
/* Choices' row: the choice's text, drawn with the theme's marker */
bool ui_choices_rows_get_row_info(App* app, size_t row, int width, UiRowInfo* info) {
    (void)width; /* a choice too long for its row slides, as the menu's rows do */
    info->label = row < 64 ? choice_text(app, app->choices_of, (int)row, app->row_text, sizeof app->row_text) : NULL;
    if (app->choices_of == GUI_CHOICES_SIZES && !row && size_why(app, NULL, 0)) info->attr |= UI_ATTR_OFF; /* OK */
    return info->label != NULL;
}
/* a choice under the pointer takes the focus, and a click takes it: the question's pressed, UI scale's size or the
   folder taken, the setup page's step taken */
void ui_choices_rows_row_action_cb(App* app, size_t row, uint32_t hit) {
    int i = (int)row;
    switch (app->choices_of) {
        case GUI_CHOICES_ASK:
            gui_confirm_focus(&app->confirm, i);
            if (hit & UI_HIT_CLICK) ask_key(app, GUI_SELECT);
            break;
        case GUI_CHOICES_SIZES:
            app->sizes.focus = i;
            if (hit & UI_HIT_CLICK) size_choice(app, i);
            break;
        case GUI_CHOICES_DESTS:
            app->dests.focus = i;
            if (hit & UI_HIT_CLICK) dest_take(app, i);
            break;
        case GUI_CHOICES_PICKS:
            app->picks.focus = i;
            if (hit & UI_HIT_CLICK) pick_take(app, i);
            break;
        case GUI_CHOICES_TYPED:
            if (hit & UI_HIT_CLICK) typed_choice(app, i);
            break;
        default:
            app->setup_choice.focus = i;
            app->setup_row = GUI_APPEARANCE_NEXT;
            if (hit & UI_HIT_CLICK) setup_take(app);
            break;
    }
}
/* whether pixel x, y is on the head's user */
static bool user_hit(App* app, float x, float y) {
    GuiPlace p;
    place_screen(app, &p, "", GUI_SHOWS_TABS);
    return p.badge.h && x >= (float)p.badge.x && x < (float)(p.badge.x + p.badge.w) && y >= (float)p.badge.y &&
           y < (float)(p.badge.y + p.badge.h);
}
/* the tab at pixel x, y, -1 for none */
static int tab_hit(App* app, float x, float y) {
    GuiPlace p;
    int i;
    place_screen(app, &p, "", GUI_SHOWS_TABS);
    for (i = 0; i < tab_count(app); ++i) {
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
    if (app->menu.view == GUI_LOAD_RECOMPILE) {
        GuiQueueBoxes q;
        size_t row;
        uint32_t hit;
        queue_boxes(app, &q);
        hit = ui_queue_hit(app, q.rects, app->scale, q.lists, (int)x, (int)y, click ? UI_HIT_CLICK : 0, &i, &row);
        if (hit) ui_queue_act(app, i, row, hit);
        return;
    }
    if (app->menu.view == GUI_LOAD_GAMES) {
        GuiGamesBoxes g;
        size_t row;
        uint32_t hit;
        games_boxes(app, &g);
        if (games_grid(app)) {
            hit = ui_games_grid_hit(app, g.grid_rects, app->scale, g.grid_lists, (int)x, (int)y,
                                    click ? UI_HIT_CLICK : 0, &i, &row);
            if (hit) ui_games_grid_act(app, i, row, hit);
            return;
        }
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
void ui_menu_entries_load_3ds_game_action_cb(App* app, uint32_t hit) { menu_entry(app, GUI_3DS, hit); }
void ui_menu_entries_load_vita_vpk_action_cb(App* app, uint32_t hit) { menu_entry(app, GUI_VITA, hit); }
void ui_menu_entries_load_gba_rom_action_cb(App* app, uint32_t hit) { menu_entry(app, GUI_GBA, hit); }
void ui_menu_entries_install_keys_action_cb(App* app, uint32_t hit) { menu_entry(app, GUI_KEYS, hit); }
void ui_menu_entries_settings_action_cb(App* app, uint32_t hit) { menu_entry(app, GUI_SETTINGS, hit); }
void ui_menu_entries_share_log_action_cb(App* app, uint32_t hit) { menu_entry(app, GUI_LOG, hit); }
void ui_menu_entries_folders_action_cb(App* app, uint32_t hit) { menu_entry(app, GUI_FOLDERS, hit); }
void ui_menu_entries_exit_action_cb(App* app, uint32_t hit) { menu_entry(app, GUI_EXIT, hit); }
/* the keys the foot names, a word each ("Up/Down" is two): what a click or a tap on the word presses. Space selects
   where the foot names it, as Enter; Add a folder's opens the folder picker, as Enter on its row. A word is found by
   its English or its translation, which the foot's sentences keep (scripts/build-languages.py checks them) */
static const struct {
    const char* word;
    SDL_Keycode key;
    SDL_Keymod mod;
    bool pad; /* a pad's button (pad_menu.h's key), found first while a pad drives the menu and only then: B is ESC */
} hint_keys[] = {{SNX_TR_KEY("A"), SDLK_RETURN, 0, true},
                 {SNX_TR_KEY("B"), SDLK_ESCAPE, 0, true},
                 {SNX_TR_KEY("X"), SDLK_DELETE, 0, true},
                 {SNX_TR_KEY("Y"), SDLK_R, 0, true},
                 {SNX_TR_KEY("Cross"), SDLK_RETURN, 0, true},
                 {SNX_TR_KEY("Circle"), SDLK_ESCAPE, 0, true},
                 {SNX_TR_KEY("Square"), SDLK_DELETE, 0, true},
                 {SNX_TR_KEY("Triangle"), SDLK_R, 0, true},
                 {SNX_TR_KEY("LB"), SDLK_TAB, SDL_KMOD_LSHIFT, true},
                 {SNX_TR_KEY("RB"), SDLK_TAB, 0, true},
                 {SNX_TR_KEY("LT"), SDLK_PAGEUP, 0, true},
                 {SNX_TR_KEY("RT"), SDLK_PAGEDOWN, 0, true},
                 {SNX_TR_KEY("Start"), SDLK_F5, 0, true},
                 {SNX_TR_KEY("L3"), SDLK_M, 0, true},
                 {SNX_TR_KEY("R3"), SDLK_B, 0, true},
                 {SNX_TR_KEY("ESC"), SDLK_ESCAPE, 0, false},
                 {SNX_TR_KEY("Back"), SDLK_AC_BACK, 0, false},
                 {SNX_TR_KEY("Tab"), SDLK_TAB, 0, false},
                 {SNX_TR_KEY("Enter"), SDLK_RETURN, 0, false},
                 {SNX_TR_KEY("Space"), SDLK_RETURN, 0, false},
                 {SNX_TR_KEY("click"), SDLK_RETURN, 0, false},
                 {SNX_TR_KEY("Tap"), SDLK_RETURN, 0, false},
                 {SNX_TR_KEY("A"), SDLK_RETURN, 0, false},
                 {SNX_TR_KEY("Up"), SDLK_UP, 0, false},
                 {SNX_TR_KEY("Down"), SDLK_DOWN, 0, false},
                 {SNX_TR_KEY("Left"), SDLK_LEFT, 0, false},
                 {SNX_TR_KEY("Right"), SDLK_RIGHT, 0, false},
                 {SNX_TR_KEY("PgUp"), SDLK_PAGEUP, 0, false},
                 {SNX_TR_KEY("PgDn"), SDLK_PAGEDOWN, 0, false},
                 {SNX_TR_KEY("PgDown"), SDLK_PAGEDOWN, 0, false},
                 {SNX_TR_KEY("Del"), SDLK_DELETE, 0, false},
                 {SNX_TR_KEY("Take out"), SDLK_DELETE, 0, false}, /* a finger's, on the Compile tab */
                 {SNX_TR_KEY("Games"), SDLK_TAB, 0, false},
                 {SNX_TR_KEY("R"), SDLK_R, 0, false},
                 {SNX_TR_KEY("Recompile"), SDLK_R, 0, false},
                 {SNX_TR_KEY("B"), SDLK_B, 0, false},
                 {SNX_TR_KEY("AOT pack"), SDLK_B, 0, false},
                 {SNX_TR_KEY("M"), SDLK_M, 0, false},
                 {SNX_TR_KEY("Mods"), SDLK_M, 0, false},
                 {SNX_TR_KEY("P"), SDLK_P, 0, false},
                 {SNX_TR_KEY("Folders"), SDLK_P, 0, false},
                 {SNX_TR_KEY("V"), SDLK_V, 0, false},
                 {SNX_TR_KEY("View"), SDLK_V, 0, false},
                 {SNX_TR_KEY("S"), SDLK_S, 0, false},
                 {SNX_TR_KEY("Save states"), SDLK_S, 0, false}, /* a finger's, on Games */
                 {SNX_TR_KEY("Ctrl+S"), SDLK_S, SDL_KMOD_CTRL, false},
                 {SNX_TR_KEY("Ctrl+V"), SDLK_V, SDL_KMOD_CTRL, false},
                 {SNX_TR_KEY("F5"), SDLK_F5, 0, false},
                 {SNX_TR_KEY("F2"), SDLK_F2, 0, false},
                 {SNX_TR_KEY("F1"), SDLK_F1, 0, false},
                 {SNX_TR_KEY("Select"), SDLK_F1, 0, true},
                 {SNX_TR_KEY("Refresh"), SDLK_F5, 0, false}};
/* the hint_keys entry of the n bytes at word, a pad's buttons' only when pad (a pad drives the menu), -1 for none */
static int hint_word(const char* word, size_t n, bool pad) {
    int i;
    for (i = 0; i < (int)SDL_arraysize(hint_keys); ++i) {
        const char* shown = snx_tr(hint_keys[i].word);
        if (hint_keys[i].pad && !pad) continue;
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
static int hint_pick(const char* key, int col, bool pad) {
    const char *word = key, *end;
    int k = gui_text_cols(key), pick = -1, first = 1, w;
    for (;; word = end + 1) {
        end = strchr(word, '/');
        if (!end) end = word + strlen(word);
        w = hint_word(word, (size_t)(end - word), pad);
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
    int pick = hint_pick(key, col, app->pad_driven);
    SDL_Event e;
    if (pick < 0) return false;
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
        place_screen(app, p, shown_keys(app, keys, size), GUI_SHOWS_TABS); /* render_pad's */
    else if (app->editing) {
        GuiFormBoxes f;
        form_boxes(app, &f);
        *p = f.place;
    } else if (app->menu.view == GUI_LOAD_GAMES)
        games_place(app, p);
    else if (app->menu.view == GUI_LOAD_RECOMPILE) {
        GuiQueueBoxes q;
        queue_boxes(app, &q);
        *p = q.place;
    } else if (pick_screen(app)) {
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
    hit.y = SDL_clamp(y, (float)text.y, (float)(text.y + hints_h(app, rows) - 1));
    hit.key[0] = 0;
    hints(app, say, text.w / cell_w(app), text, text.y, 0, 0, &hit);
    app->scale = s;
    return hit.key[0] && hint_press(app, hit.key, hit.col);
}
/* the window's pixel x, y on UI scale's slider as last drawn, its dialog picking with no question over it */
/* the scroll bar a press at x, y takes, the last drawn first: on its track, a finger's within two letters of its
   right edge (a thumb's reach, wider than it is drawn); -1 for none */
static int bar_at(const App* app, float x, float y, bool touch) {
    int i;
    for (i = app->nbars - 1; i >= 0; --i) {
        GuiRect t = app->bars[i].track;
        if (t.w && x >= (float)(touch ? t.x + t.w - 2 * cell_w(app) : t.x) && x < (float)(t.x + t.w) &&
            y >= (float)t.y && y < (float)(t.y + t.h))
            return i;
    }
    return -1;
}
static bool on_bar(App* app, float window_x, float window_y) {
    float x, y;
    return SDL_RenderCoordinatesFromWindow(app->renderer, window_x, window_y, &x, &y) && bar_at(app, x, y, true) >= 0;
}
/* the held bar's thumb moved to pixel y of its track, its middle there: its list's rows as far down as the thumb is
   down the track, kept there until the list's focus moves */
static void bar_to(App* app, float y) {
    const GuiBar* b;
    float room, at;
    int i;
    for (i = app->nbars - 1; i >= 0 && app->bars[i].list + 1 != app->bar_hold; --i) continue;
    if (i < 0) return;
    b = &app->bars[i];
    room = (float)(b->track.h - b->thumb.h);
    at = room > 0 ? SDL_clamp((y - (float)b->track.y - (float)b->thumb.h / 2) / room, 0.0f, 1.0f) : 0;
    app->list_y[b->list] = SDL_roundf(at * app->list_max[b->list]);
    app->list_v[b->list] = 0;
    app->bar_focus[b->list] = SIZE_MAX;
}
/* the scene list a finger at x, y is on, the last drawn first: plus one, 0 for none */
static int list_at(const App* app, float x, float y) {
    int i;
    for (i = app->nbars - 1; i >= 0; --i)
        if (in_rect(app->bars[i].area, x, y)) return app->bars[i].list + 1;
    return 0;
}
/* a list a finger or a thumb takes: Settings' side panel the focus from the groups, as a click on its row does, and
   the groups' list it back from a group's rows, as ESC does */
static void list_take(App* app, int list) {
    if (!app->editing || app->editor.typing) return;
    if (list == GUI_LIST_FORM_SIDE && app->form == GUI_FORM_SETTINGS) {
        open_group(app, app->editor.row);
        app->firsts[list] = 0; /* the group's rows from their first */
        app->list_y[list] = app->list_v[list] = 0;
        app->bar_focus[list] = 0;
    } else if (list == GUI_LIST_FORM_LIST && (app->form == GUI_FORM_APP || app->form == GUI_FORM_USERS ||
                                              (app->form == GUI_FORM_GROUP && app->page < 0)))
        editor_key(app, GUI_CANCEL);
}
/* a finger on a scene list at window pixel x, y, as it goes down (0), moves (1) and lifts (2): once it has gone up or
   down more than a tap may, the rows of the list under it follow it, and let go they glide on at the speed of its
   last move; passing onto the list beside, Settings' side panel or its groups, that one takes the focus and the drag.
   A tap moves nothing (touch takes it), nor a finger off the lists */
static void list_finger(App* app, float x, float y, int phase) {
    float rx, ry;
    Uint64 now = now_ms(app);
    int at, which;
    if (!SDL_RenderCoordinatesFromWindow(app->renderer, x, y, &rx, &ry)) return;
    at = list_at(app, rx, ry);
    if (phase == 0) {
        app->list_held = 0;
        app->list_hold_x = rx;
        app->list_hold_at = app->list_last_y = ry;
        app->list_last_ms = now;
        if (at) app->list_v[at - 1] = 0; /* a finger stops a glide */
        return;
    }
    if (!app->list_held) {
        float dy = ry - app->list_hold_at;
        if (!at || SDL_fabsf(dy) <= (float)cell_h(app) / 2 || SDL_fabsf(dy) <= 2 * SDL_fabsf(rx - app->list_hold_x))
            return;
        list_take(app, at - 1);
        app->list_held = at;
        app->list_hold_y = app->list_y[at - 1];
    } else if (at && at != app->list_held) { /* onto the list beside: it takes the drag from here */
        list_take(app, at - 1);
        app->list_held = at;
        app->list_hold_y = app->list_y[at - 1];
        app->list_hold_at = ry;
    }
    which = app->list_held - 1;
    app->list_y[which] = SDL_clamp(app->list_hold_y - (ry - app->list_hold_at), 0.0f, app->list_max[which]);
    app->bar_focus[which] = SIZE_MAX;
    if (phase == 1) {
        if (now > app->list_last_ms)
            app->list_v[which] = -(ry - app->list_last_y) * 1000.0f / (float)(now - app->list_last_ms);
        app->list_last_y = ry;
        app->list_last_ms = now;
        return;
    }
    app->list_held = 0; /* let go: on at its last speed, unless the finger had stopped */
    if (now - app->list_last_ms > 100) app->list_v[which] = 0;
    app->lists_tick = now;
}
static bool on_slider(App* app, float window_x, float window_y) {
    float x, y;
    return !app->confirm.shown && size_picking(app) &&
           SDL_RenderCoordinatesFromWindow(app->renderer, window_x, window_y, &x, &y) && slider_at(app, x, y) >= 0;
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
    app->pointed = true;
    app->hover_x = x;
    app->hover_y = y;
    if (app->help_shown) { /* F1's box: a click anywhere closes it */
        if (click) app->help_shown = false;
        return;
    }
    /* UI scale's slider, its dialog picking: a press on it takes the step under it and holds it, the drag going on,
       until the button or the finger lifts */
    if (app->slider_drag && !click) {
        GuiRect b = app->slider_bar;
        if (b.w) slider_to(app, slider_at(app, SDL_clamp(x, (float)b.x, (float)(b.x + b.w - 1)), (float)b.y));
        return;
    }
    if (click && !app->confirm.shown && size_picking(app) && slider_at(app, x, y) >= 0) {
        app->slider_drag = true;
        slider_to(app, slider_at(app, x, y));
        return;
    }
    /* a list's scroll bar: a press on it takes the thumb there, and the drag goes on until the button or the finger
       lifts */
    if (app->bar_hold && !click) {
        bar_to(app, y);
        return;
    }
    if (click && bar_at(app, x, y, touch) >= 0) {
        app->bar_hold = app->bars[bar_at(app, x, y, touch)].list + 1;
        list_take(app, app->bar_hold - 1);
        bar_to(app, y);
        return;
    }
    /* Games' own, its rows scrolled by the pixel */
    if (app->bar_held && !click && !touch) {
        games_bar_to(app, y);
        return;
    }
    if (click && !touch && games_scrolls(app) && !box_shown(app)) {
        GuiGamesBoxes g;
        GuiRect track, thumb;
        games_boxes(app, &g);
        if (games_bar(app, &g, &track, &thumb) && in_rect(track, x, y)) {
            app->bar_held = true;
            games_bar_to(app, y);
            return;
        }
    }
    /* under a question, the folder's too, a click is the question's */
    if (click && !app->confirm.shown && foot_press(app, x, y)) {
        if (touch && picking) app->pick_press = GUI_PICK_ROWS + 1; /* the picker's finger, pressed as it went down */
        return;
    }
    if (app->confirm.shown) /* over a run, the menu, Games or a form */
        ask_pointer(app, x, y, click);
    else if (app->editing && app->editor.typing)
        typed_pointer(app, x, y, click);
    else if (app->sizes_shown)
        sizes_pointer(app, x, y, click);
    else if (app->dests_shown)
        dests_pointer(app, x, y, click);
    else if (app->picks_shown)
        picks_pointer(app, x, y, click);
    else if (app->users_shown)
        users_pointer(app, x, y, click);
    else if (app->addons_shown)
        addons_pointer(app, x, y, click);
    else if (app->options_shown)
        options_pointer(app, x, y, click);
    else if (app->states_shown)
        states_pointer(app, x, y, click);
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
/* the focus of the box over the screen, the one pointer() gives the pointer, and its rows; NULL with none shown */
static int* box_focus(App* app, int* count) {
    int rows[GUI_VIEW_BOX_ROWS];
    GuiChoice* c = app->sizes_shown   ? &app->sizes
                   : app->dests_shown ? &app->dests
                   : app->picks_shown ? &app->picks
                                      : NULL;
    if (c) {
        *count = c->count;
        return &c->focus;
    }
    *count = app->users_shown     ? g_user_count
             : app->addons_shown  ? addons_rows(app)
             : app->options_shown ? view_rows(app, rows)
                                  : (int)app->states_count;
    return app->users_shown     ? &app->users_focus
           : app->addons_shown  ? &app->addons_focus
           : app->options_shown ? &app->options_focus
           : app->states_shown  ? &app->states_focus
                                : NULL;
}
/* a box over the screen, which a finger and the wheel go to before what is under it */
static bool box_shown(App* app) {
    int count;
    return app->confirm.shown || app->help_shown || box_focus(app, &count) != NULL;
}
/* a row up or down for the wheel, which stops at the first and last rows where the keys go round:
   a box's over the screen first, whose rows past its room come into view as its focus moves */
static void scroll(App* app, GuiKey key) {
    int row, count, *focus = box_focus(app, &count);
    if (focus) {
        if (key == GUI_UP ? *focus > 0 : *focus + 1 < count) *focus += key == GUI_UP ? -1 : 1;
        return;
    }
    if (app->editing && app->form == GUI_FORM_PAD) {
        pad_did(app, snx_pad_screen_key(&app->pad, key == GUI_UP ? SNX_PAD_KEY_UP : SNX_PAD_KEY_DOWN));
        return;
    }
    if (app->editing) {
        editor_key(app, key); /* stops at its ends */
        return;
    }
    if (app->menu.view == GUI_LOAD_RECOMPILE) { /* its first row and its last stop it */
        if (key == GUI_UP ? app->queue_focus > 0 : app->queue_focus + 1 < app->queue.count) queue_key(app, key);
        return;
    }
    if (app->menu.view == GUI_LOAD_GAMES) { /* over its pages as the keys go, from the head's first to the last game */
        bool last = app->browser.page + 1 >= gui_browser_pages(&app->browser) &&
                    app->browser.selected + 1 >= app->browser.head + gui_browser_items(&app->browser);
        bool first = !app->browser.selected && (app->browser.head || !app->browser.page); /* the head on every page */
        if (key == GUI_UP ? !first : !last) menu_key(app, key);
        return;
    }
    row = app->menu.selected;
    menu_key(app, key);
    if (key == GUI_UP ? app->menu.selected > row : app->menu.selected < row)
        menu_key(app, key == GUI_UP ? GUI_DOWN : GUI_UP); /* it went round: back */
}
/* a finger lifted at x, y (0 to 1 across the window) that scrolled nothing: a tap clicks there, is ESC on a finished
   run and on a running one's ESC line */
static void touch(App* app, float x, float y) {
    float dx = x - app->finger_x, dy = y - app->finger_y, rx, ry;
    int w, h;
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
    if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND) app->hidden = app->playing = false; /* the game, if any, behind */
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
    app->clean_found = -1; /* Clean Old Settings looks again */
    app->loads_at = 0;     /* the loads' drivers looked for again */
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
        if (!n) snprintf(gone, sizeof gone, "%s", snx_tr("Data, Recompiled, Extractions, Logs and Caches are there"));
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
#define GUI_PAD_TRAVEL 8.0f /* the pixels a mouse moves before it takes the menu back from a pad */
/* the pages that read the pads themselves: the button map, and a Controller row waiting for a press */
static bool pads_own(const App* app) { return app->device_listen || (app->editing && app->form == GUI_FORM_PAD); }
/* a pad pressed last, or the keyboard, the mouse or a finger: the cursor shows for the mouse alone */
static void pad_drive(App* app, bool pad) {
    if (pad == app->pad_driven) return;
    app->pad_driven = pad;
    app->pointer_travel = 0;
    if (pad) {
        app->hovering = false;
        SDL_HideCursor();
    } else
        SDL_ShowCursor();
}
/* which of them e is, a key or the mouse moving before it is taken: false, or true for a mouse moving too little to
   take the menu back from a pad, which then moves nothing. A click, a wheel or a finger lifted takes it back after
   (pointer_leaves), so it presses what the foot showed under it */
static bool input_seen(App* app, const SDL_Event* e) {
    switch (e->type) {
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: pad_drive(app, true); break;
        case SDL_EVENT_KEY_DOWN: /* a pad's press too: the lists follow the focus again */
            app->pointed = false;
            if (e->key.which != GUI_PAD_KEYBOARD) pad_drive(app, false);
            break;
        case SDL_EVENT_MOUSE_WHEEL: app->pointed = false; break; /* its rows move as a key's */
        case SDL_EVENT_MOUSE_MOTION:
            if (!app->pad_driven) break;
            app->pointer_travel += SDL_fabsf(e->motion.xrel) + SDL_fabsf(e->motion.yrel);
            if (app->pointer_travel < GUI_PAD_TRAVEL) return true;
            pad_drive(app, false);
            break;
        default: break;
    }
    return false;
}
/* a pad's event: the key it presses taken as the keyboard's, but by the pages that read the pads themselves, which
   take the event as it is; whether it was taken here */
static bool pads_event(App* app, const SDL_Event* e) {
    SDL_Event key;
    bool pressed = gui_pads_event(&app->pads, e, now_ms(app), &key);
    if (pressed) pad_drive(app, true); /* a stick or a trigger too */
    if (!pressed || pads_own(app)) return false;
    event(app, &key);
    return true;
}
/* SDL could not start the menu's pads: why, from SDL */
static void pads_fail(App* app) {
    app->pads_failed = true;
    snprintf(app->pads_error, sizeof app->pads_error, snx_tr("Controllers cannot be read: %s"), SDL_GetError());
}
/* the menu's pads open until a run's game loads (its LAUNCH), as the game may want them: a pad stops a recompile
   too. The first time once the menu shows, as SDL may take most of a second looking for them */
static void pads_follow(App* app) {
    if (app->menu.running && gui_steps_last(&app->steps, "LAUNCH")) {
        gui_pads_stop(&app->pads);
        return;
    }
    if (!app->pads.started && !app->pads_failed) {
        joycon_pairs(app);
        if (gui_pads_start(&app->pads))
            pad_load_mappings(app);
        else
            pads_fail(app);
    }
    if (app->pads_failed) say_failure(app, app->pads_error); /* each frame until the box can show it, then once */
}
/* the arrow or the page a pad holds pressed again */
static void pads_repeat(App* app) {
    SDL_Event key;
    if (!pads_own(app) && gui_pads_repeat(&app->pads, now_ms(app), &key)) event(app, &key);
}
static bool pointer_leaves(const SDL_Event* e) {
    return e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_WHEEL ||
           e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_PEN_UP;
}
static void event_take(App* app, const SDL_Event* e);
static void event(App* app, const SDL_Event* e) {
    if (input_seen(app, e) || pads_event(app, e)) return;
    event_take(app, e);
    if (pointer_leaves(e)) pad_drive(app, false);
}
static void event_take(App* app, const SDL_Event* e) {
    GuiKey key;
    /* the window closed: asked first while a run goes, unless it asks already; else the screen of what is kept */
    if (e->type == SDL_EVENT_QUIT || e->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        if ((app->menu.running || app->finished || jobs_live(app)) && !app->aborting && !app->closing_ms &&
            !(app->confirm.shown && app->confirm.ask == GUI_ASK_CLOSE)) {
            if (app->finished && !jobs_live(app)) {
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
    /* what Clean Old Settings did, and the box of an older version's files: Up/Down and the wheel go over their files,
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
    /* UI scale's dialog and the box of users take every key while they show, as a question does */
    if (app->sizes_shown && !app->confirm.shown && (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_TEXT_INPUT)) {
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && menu_key_of(&e->key, &key)) sizes_key(app, key);
        return;
    }
    if (app->dests_shown && !app->confirm.shown && (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_TEXT_INPUT)) {
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && menu_key_of(&e->key, &key)) dests_key(app, key);
        return;
    }
    if (app->picks_shown && !app->confirm.shown && (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_TEXT_INPUT)) {
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && menu_key_of(&e->key, &key)) picks_key(app, key);
        return;
    }
    if (app->users_shown && !app->confirm.shown && (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_TEXT_INPUT)) {
        if (e->type == SDL_EVENT_KEY_DOWN && menu_key_of(&e->key, &key)) users_key(app, key);
        return;
    }
    /* so does the box of a game's updates and DLCs, and Games' View box */
    if (app->addons_shown && !app->confirm.shown &&
        (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_TEXT_INPUT)) {
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && menu_key_of(&e->key, &key)) addons_key(app, key);
        return;
    }
    if (app->options_shown && !app->confirm.shown &&
        (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_TEXT_INPUT)) {
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && menu_key_of(&e->key, &key)) options_key(app, key);
        return;
    }
    /* and the box of a game's save states, Del (a pad's X) deleting the one in focus */
    if (app->states_shown && !app->confirm.shown &&
        (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_TEXT_INPUT)) {
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && e->key.key == SDLK_DELETE)
            state_delete_ask(app);
        else if (e->type == SDL_EVENT_KEY_DOWN && menu_key_of(&e->key, &key))
            states_key(app, key);
        return;
    }
    /* a question takes every key while it shows, over the menu, Games or a form (a run's is the run's, below); a held
       key answers nothing */
    if (app->confirm.shown && !app->menu.running &&
        (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_TEXT_INPUT)) {
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && menu_key_of(&e->key, &key)) ask_key(app, key);
        return;
    }
    /* F1's box takes every key while it shows, and any closes it */
    if (app->help_shown && (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_TEXT_INPUT)) {
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) app->help_shown = false;
        return;
    }
    /* F1, or the foot's (a pad's Select): every key of the screen in a box over it; the setup's foot says its own */
    if (e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_F1 && !app->menu.running && !app->finished &&
        !(app->setup_on && !app->editing) && !(app->editing && app->editor.typing)) {
        if (!e->key.repeat) app->help_shown = true;
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
    /* Add Mod's or Add Cheats' picker's answer: a folder or a file, none, or why it could not open */
    if (app->addon_picked && e->type == app->addon_picked) {
        addons_picked(app, (const char*)e->user.data1, (const char*)e->user.data2, e->user.code != 0);
        SDL_free(e->user.data1);
        SDL_free(e->user.data2);
        return;
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
        app->finger_y = e->tfinger.y;
        SDL_GetWindowSize(app->window, &w, &h);
        app->pick_press = 0;                     /* the finger's own, not a mouse's whose lift was missed */
        if (app->editing && app->editor.picking) /* the picker's bars take a finger as it goes down, to drag */
            pointer(app, e->tfinger.x * (float)w, e->tfinger.y * (float)h, true, true);
        else if (on_slider(app, e->tfinger.x * (float)w, e->tfinger.y * (float)h)) { /* so does UI scale's slider */
            pointer(app, e->tfinger.x * (float)w, e->tfinger.y * (float)h, true, true);
            app->pick_press = 1; /* its moves its drag's, its lift no tap */
        } else if (on_bar(app, e->tfinger.x * (float)w, e->tfinger.y * (float)h)) { /* a list's scroll bar too */
            pointer(app, e->tfinger.x * (float)w, e->tfinger.y * (float)h, true, true);
            app->pick_press = 1;
        } else if (games_scrolls(app) && !box_shown(app)) /* the list, which a drag scrolls, or its bar */
            games_finger(app, e->tfinger.x * (float)w, e->tfinger.y * (float)h, 0);
        else /* the other lists, which a drag scrolls too */
            list_finger(app, e->tfinger.x * (float)w, e->tfinger.y * (float)h, 0);
        return;
    }
    if (e->type == SDL_EVENT_FINGER_MOTION && finger_id(&e->tfinger) == app->finger) {
        int w, h;
        SDL_GetWindowSize(app->window, &w, &h);
        if (app->pick_press)
            pointer(app, e->tfinger.x * (float)w, e->tfinger.y * (float)h, false, true);
        else if (games_scrolls(app) && !box_shown(app))
            games_finger(app, e->tfinger.x * (float)w, e->tfinger.y * (float)h, 1);
        else
            list_finger(app, e->tfinger.x * (float)w, e->tfinger.y * (float)h, 1);
        return;
    }
    if ((e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED) &&
        finger_id(&e->tfinger) == app->finger && app->pick_press) { /* the picker, the slider or a bar had it: no tap */
        app->finger = 0;
        app->pick_press = 0;
        app->slider_drag = false;
        app->bar_hold = 0;
        return;
    }
    if ((e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED) &&
        finger_id(&e->tfinger) == app->finger && games_scrolls(app) && !box_shown(app)) {
        int w, h;
        SDL_GetWindowSize(app->window, &w, &h);
        if (!app->bar_held) /* its last move, which may be its only one: farther than a tap, a scroll */
            games_finger(app, e->tfinger.x * (float)w, e->tfinger.y * (float)h, 1);
        if (app->games_held || app->bar_held) { /* a scroll, no tap */
            app->finger = 0;
            games_finger(app, e->tfinger.x * (float)w, e->tfinger.y * (float)h, 2);
            return;
        }
    }
    if ((e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED) &&
        finger_id(&e->tfinger) == app->finger && !(games_scrolls(app) && !box_shown(app))) {
        int w, h;
        SDL_GetWindowSize(app->window, &w, &h);
        /* its last move, which may be its only one: farther than a tap, a scroll and no tap */
        list_finger(app, e->tfinger.x * (float)w, e->tfinger.y * (float)h, 1);
        if (app->list_held) {
            app->finger = 0;
            list_finger(app, e->tfinger.x * (float)w, e->tfinger.y * (float)h, 2);
            return;
        }
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
    /* a notch of the wheel is a row, the way the system turns it: a box's over the screen first */
    if (e->type == SDL_EVENT_MOUSE_WHEEL) {
        int rows = e->wheel.integer_y;
        for (; app->setup_on && !app->splash && !app->confirm.shown && rows; rows += rows > 0 ? -1 : 1)
            setup_key(app, rows > 0 ? GUI_UP : GUI_DOWN);
        if (app->splash || app->menu.running || app->finished || app->confirm.shown || app->left_shown ||
            (app->editing && app->editor.picking))
            return;
        if (games_scrolls(app) && !box_shown(app)) { /* three rows a notch, gliding there; the focus stays */
            GuiGamesBoxes g;
            int row_h, total, view;
            games_boxes(app, &g);
            games_extent(app, &g, &row_h, &total, &view);
            app->games_to = SDL_clamp(app->games_to - (float)(rows * 3 * row_h), 0.0f, games_max(app, &g));
            app->games_v = 0;
            return;
        }
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
        app->pick_press = 0; /* a drag on the picker's bar ends, and one on UI scale's slider, and a list's thumb */
        app->slider_drag = app->bar_held = false;
        app->bar_hold = 0;
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
    if (e->key.key == SDLK_DELETE && !e->key.repeat && app->menu.view == GUI_LOAD_RECOMPILE && !app->finished) {
        queue_take_out(app);
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
    if (e->key.key == SDLK_P && !e->key.repeat && !(e->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) &&
        app->menu.view == GUI_LOAD_GAMES && !app->finished) {
        folders_from_games(app);
        return;
    }
    if (e->key.key == SDLK_V && !e->key.repeat && !(e->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) &&
        app->menu.view == GUI_LOAD_GAMES && !app->finished) {
        app->options_shown = true;
        app->options_focus = 0;
        return;
    }
    /* S, or a pad's X (pad_menu.c's Del, which Games has no other use for) */
    if ((e->key.key == SDLK_S || (e->key.key == SDLK_DELETE && app->pad_driven)) && !e->key.repeat &&
        !(e->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) && app->menu.view == GUI_LOAD_GAMES &&
        !app->finished) {
        layout(app);
        states_focus(app);
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
#ifdef SDL_PLATFORM_IOS
    games_defaults(app, true);
#else
    games_defaults(app, false);
#endif
    app->menu.selected = GUI_SWITCH; /* the first item until Games shows */
    app->clean_found = -1;
#ifdef SNX_TOUCH_UI
    app->fingers = true;
#endif
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
    { /* the log in Documents' logs/, which gui_apple_start made and the Files app shows */
        char *documents = SDL_GetCurrentDirectory(), *logs = documents ? snx_path_join(documents, "logs") : NULL;
        gui_log_folder(logs);
        free(logs);
        SDL_free(documents);
    }
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
    app->job_process = gui_process_create();
    if (!app->process || !app->job_process) return SDL_SetError("%s", snx_tr("Cannot start the menu: out of memory"));
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
    watch_stop(app);
    dump_icons_stop(app);
    SDL_DestroyTexture(app->icon_loading);
    SDL_DestroyTexture(app->icon_none);
    SDL_DestroyTexture(app->icon_more);
    space_stop(app);
    space_end(&app->left_space, &app->left_counter);
    settings_forget(app);
    free(app->settings_file);
    for (int i = 0; i < app->folder_count; ++i) free(app->folders[i]);
    free(app->folder_asked);
    free(app->adding_game);
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
    if (app->run_locked) snx_unlock_file(app->run_lock);
    app->run_locked = false;
    free(app->self);
    free(app->drivers);
    free(app->file);
    free(app->data_dir);
    free(app->home_dir);
    for (int i = 0; i < GUI_PLACES; ++i) free(app->place[i]);
    free(app->work_dir);
    free(app->state_dir);
    free(app->cache_dir);
    free(app->config_dir);
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
    gui_process_destroy(app->job_process); /* the queue's recompile ends with the menu */
    gui_queue_free(&app->queue);
    if (app->job_out) fclose(app->job_out);
    free(app->job_log);
    gui_games_free(&app->games);
    free(app->games_drivers);
    free(app->games_debug);
    free(app->debug_path);
    addons_close(app);
    states_close(app);
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
    gui_pads_stop(&app->pads);
    SDL_Quit();
}
/* the settings, its picks and its game folders in a file each */
static char* settings_path(const char* config) { return config ? snx_path_join(config, "settings.ini") : NULL; }
/* the menu's own files, in its folders of them (menu_folder) alone: an older version's elsewhere are listed by
   left_behind, never read, and with none here the menu starts from its defaults */
static void open_files(App* app) {
    char *prefs = menu_folder(app, SNX_USER_STATE, !app->look_only),
         *config = menu_folder(app, SNX_USER_CONFIG, !app->look_only);
#ifndef SDL_PLATFORM_IOS /* there in Documents' logs/ (initialize); app.ini's Logs folder once it is read */
    if (prefs && !app->look_only) gui_log_folder(prefs);
#endif
    if (prefs) recent_open(app, snx_path_join(prefs, "recent.txt"));
    app->settings_file = settings_path(config);
    free(app->config_dir);
    app->config_dir = config ? snx_strdup(config) : NULL;
    if (app->settings_file) {
        app->folders_file = snx_path_join(config, "folders.txt");
        app->app_file = snx_path_join(config, "app.ini");
        folders_load(app);
        app_load(app);
    }
    if (!app->look_only)
        gui_log("%s starts, its data folder %s", snx_build_info(), app->data_dir ? app->data_dir : "(none)");
    if (prefs) run_log_follow(app);  /* in the Logs folder app.ini picked */
    if (prefs) recent_as_games(app); /* by the recompiled/ app.ini picked */
    app->users_dir = snx_users_dir(app->home_dir ? app->home_dir : app->data_dir);
    if (!app->users_dir) {
        snprintf(app->message, sizeof app->message, "%s", snx_tr("No user shown: out of memory"));
        say_failure(app, app->message);
    }
    users_read(app);
    if (app->message[0] && !app->look_only) gui_log("%s", app->message); /* what app.ini and the theme had wrong */
    free(config);
    free(prefs);
}
const char* snx_menu_language(int i, const char** name) {
    if (i < 0 || i >= snx_gui_language_count) return NULL;
    *name = snx_gui_languages[i].name;
    return snx_gui_languages[i].code;
}
#if !defined(SDL_PLATFORM_ANDROID) && !defined(SDL_PLATFORM_IOS)
/* Move Old Files without the menu, to out: each entry and what it does or would, then how many; with apply, done.
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
        char why[1024];
        SDL_strlcpy(why, SDL_GetError(), sizeof why);
#ifndef SDL_PLATFORM_IOS /* there the folder is known from the start (initialize) */
        { /* the log's folder, as open_files takes it, from the data folder paths_start would find when it did not yet */
            char *drivers = NULL, *data = NULL, *state;
#ifndef SDL_PLATFORM_ANDROID
            if (!app.data_dir) {
                drivers = self ? snx_dirname(self) : SDL_GetBasePath() ? snx_strdup(SDL_GetBasePath()) : NULL;
                data = drivers ? snx_data_dir(drivers) : NULL;
            }
#endif
            state = app.data_dir || data ? snx_menu_dir(app.data_dir ? app.data_dir : data, SNX_USER_STATE) : NULL;
            if (state && !snx_mkdirs(state)) gui_log_folder(state);
            free(state);
            free(data);
            free(drivers);
        }
#endif
        gui_log_fatal(why); /* a phone shows no stderr */
#ifdef SDL_PLATFORM_IOS
        if (app.window) /* a message box there needs the window, which may be what failed */
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "ScarletNX", why, app.window);
#else
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "ScarletNX", why, app.window);
#endif
        cleanup(&app);
        return 1;
    }
    open_files(&app); /* here, not in initialize(), so the tests keep their own */
    setup_start(&app);
    app.games_fresh = true;
    watch_start(&app);
    app.dump_icons.live = true; /* the dumps' icons asked on a thread of their own */
    space_start(&app);          /* the footer's disk */
    left_behind(&app);
    app.left_shown = app.lefts > 0;
    splash_start(&app);
    while (!app.menu.quit) {
        SDL_Event e;
#ifdef SDL_PLATFORM_ANDROID
        /* behind other apps with no run to read, the queue's neither, the menu waits to show again */
        if (app.hidden && !app.menu.running && gui_queue_running(&app.queue) < 0 && SDL_WaitEvent(&e)) event(&app, &e);
#endif
        while (SDL_PollEvent(&e)) event(&app, &e);
        pads_repeat(&app);
        pump(&app);
#ifdef SDL_PLATFORM_ANDROID
        if (!app.hidden)
#endif
            render(&app);
        pads_follow(&app);
        SDL_Delay(16);
    }
    cleanup(&app);
    return 0;
}
#ifndef SNX_GUI_NO_MAIN /* the menu alone: Android's activity runs it, and a build without the Switch runtime */
int main(int argc, char** argv) {
    char why[160];
    if (snx_setup_options(argc, argv, why, sizeof why) < 0) { /* --no-setup leaves the setup out */
        fprintf(stderr, "scarletnx-gui: %s\n", why);
        return 2;
    }
    return snx_gui_main(NULL);
}
#endif
