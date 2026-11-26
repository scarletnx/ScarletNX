/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's menu: Android's back button, a gamepad's Home, the on-screen Home or F2 opens it over the game, in the
   menu's look (src/gui, the YoRHa theme), drawn by the renderer as quads (ui.h). The same on every platform: the
   on-screen controls' options (vpad.h), the touch screen's, the runtime's keys (Shortcuts), the Volume on a slider
   (an INI_SLIDER setting's row: Left/Right step it to its ends, held too, a press on it takes the step under it and a
   drag follows), Capture frame while RenderDoc is in the game (the renderer's capture), and Exit */
#ifndef SNX_OVERLAY_H
#define SNX_OVERLAY_H
#include "common/utilities/notice.h"
#include "common/utilities/run_theme.h"
#include "common/utilities/ui_ring.h"
#include "common/utilities/pad_map.h"

/* the 3DS's, the Vita's and the GBA's notices are drawn as this menu's (common/utilities/notice.h) */
#define OVERLAY_COLS        SNX_MENU_COLS   /* the panel, in cells of the 8x16 font: the narrowest */
#define OVERLAY_COLS_MAX    SNX_NOTICE_COLS /* and the widest, as far as three quarters of the picture let it */
#define OVERLAY_ROWS        SNX_MENU_ROWS   /* the tallest page's: the others are as tall as their items */
/* a slider's columns where its row has them: the knob's travel a column a step of Volume's twenty, and the knob's own */
#define OVERLAY_SLIDER_COLS 21
/* what a menu action leaves to its caller: the save state ones for the slot overlay_slot names; the keyboard's box
   answered (overlay_keyboard_take), or a tap on its line, which asks for the system's keyboard again; Capture frame,
   RenderDoc's capture of the next frame, the menu closed first so the frame is the game's; Assign players, the
   controller applet's dialog over the game paused */
enum {
    OVERLAY_NONE,
    OVERLAY_CLOSE,
    OVERLAY_EXIT,
    OVERLAY_EDIT,
    OVERLAY_SAVE,
    OVERLAY_LOAD,
    OVERLAY_DELETE,
    OVERLAY_KEYBOARD,
    OVERLAY_TYPE,
    OVERLAY_CAPTURE,
    OVERLAY_CONTROLLERS
};

/* What the runtime the menu shows over gives it: each NULL where that runtime has none, which the menu then leaves out or
   says it lacks. The Switch's gives them all (runtime/platform/overlay_host.c); a console's window the few it has */
enum { OVERLAY_ZONE_DRAW, OVERLAY_ZONE_TOAST_LAY, OVERLAY_ZONE_MENU_LAY }; /* the profiler's zones, by what they time */
typedef struct {
    /* the game's own settings (its picks): key set to value, applied and kept; whether the game set key itself, and the
       key given back to the launcher's; the value a startup setting takes at the next start (1 with it in out); a
       count of the picks' changes; the old settings the .ini still has (their count), and taken out */
    void (*set)(const char* key, const char* value);
    int (*picked)(const char* key);
    void (*unpick)(const char* key);
    int (*next_start)(const char* key, char* out, size_t n);
    const _Atomic int* (*picks_changes)(void);
    int (*old_settings)(void);
    void (*clean_old)(void);
    /* the renderer's frame capture (RenderDoc): with ask 0 whether one can be taken, with 1 the next frame asked */
    int (*capture)(int ask);
    /* the input recording and replay (TAS) of the save state slots: a slot's frames (why when none), whether one is
       recorded or replayed now, a recording or a replay started from a slot, nonzero, or 0 with why */
    int (*tas_frames)(int slot, char* why, size_t n);
    int (*tas_recording)(void);
    int (*tas_replaying)(void);
    int (*tas_record)(int slot, char* why, size_t n);
    int (*tas_replay)(int slot, char* why, size_t n);
    /* the TV mode, the console docked: the choice now, and a step down (-1) or up (1), its text into text */
    int (*tv_choice)(void);
    int (*tv_step)(int step, char* text, size_t size);
    /* the touch screen's setting (0 off, 1 the fingers, 2 the mouse too) set at once */
    void (*touch_set)(int mode);
    /* the open gamepads: player slot's i (-1 none); i's name (NULL past the last), face labels and side by side
       sticks (1, 0 past the last), the controls it holds (-1 every one's); a count of the names' changes; one of
       g_settings' gamepad settings stored and applied */
    int (*pad_of_player)(int slot);
    const char* (*pad_name)(int i);
    int (*pad_layout)(int i, int labels[4], int* symmetric);
    unsigned (*pad_held)(int i);
    u32 (*pad_names_version)(void);
    void (*pad_store)(const int* setting);
    /* the button map: player's own (1 to SNX_GAMEPADS) into own, 1 where it has one, 0 where it has none, -1 where
       there is no such player; or every player's (0); and set, stored in the .ini */
    int (*pad_map)(int player, SnxPadMap* own);
    void (*pad_map_set)(int player, const SnxPadMap* own);
    /* whether the screen that prepares the game shows */
    int (*preparing)(void);
    /* the user's picture, side square at x, y, and the picture apart from the atlas (the state's thumbnail, w x h at
       x, y; set, NULL none; its version): 1 when drawn */
    int (*picture)(SnxUiList* l, float x, float y, float side);
    int (*image)(SnxUiList* l, float x, float y, float w, float h);
    void (*image_set)(const u8* rgba);
    const u8* (*image_get)(u32* version);
    /* the controller applet's dialog over the game: whether it shows, and drawn */
    int (*dialog_shown)(void);
    void (*dialog_draw)(SnxUiList* l, int sw, int sh, int scale);
    /* the debug HUD's box (x, y, w, h), 1 while drawn */
    int (*hud_box)(float box[4]);
    /* the profiler's (Tracy): a zone of OVERLAY_ZONE_ begun, its token; its value and text; its end; a plot */
    void* (*zone_begin)(int zone);
    void (*zone_value)(void* zone, u64 value);
    void (*zone_text)(void* zone, const char* text);
    void (*zone_end)(void* zone);
    void (*plot)(const char* name, double value);
} SnxMenuHost;
/* host's from now on (kept, not copied); NULL: none of them */
void overlay_set_host(const SnxMenuHost* host);
/* whether it is open, for the renderer's thread too */
int overlay_shown(void);
void overlay_toggle(void);
/* the display's own scale (SDL's: dpi / 160 on a phone), which the panel's letters follow; 1 until told */
void overlay_set_display_scale(float scale);
int overlay_display_percent(void); /* that scale, in percent */
/* where the panel goes on a picture of sw x sh pixels: its corner and the whole number it is scaled by */
void overlay_place(int sw, int sh, int* x, int* y, int* scale);
/* a screen of fingers (a phone's), as the backend knows it: an item's row is then a finger's row high at the least
   (48 pixels at the display's scale, as the launcher's rows), its letters the same in its middle, and a page shows as
   many items at once as the picture holds. Off until it is said */
void overlay_set_fingers(int on);
int overlay_select(void);      /* the focused item's action: a page, a setting changed, or an OVERLAY_ for the caller */
void overlay_move(int rows);   /* the focus up (-1) or down (1), round the page */
void overlay_scroll(int rows); /* the mouse's wheel: the same, stopping at the ends */
int overlay_side(int step);    /* the focused setting down (-1) or up (1); OVERLAY_CLOSE on F1's page */
int overlay_back(void);        /* the page above; OVERLAY_CLOSE from the first, and from F1's */
/* a direction held: overlay_side, but for the first page's TV mode row, which a press steps once, as each step docks or
   undocks the console under the game; its Volume goes on stepping */
int overlay_side_held(int step);
/* F1: the page of the runtime's keys and the game's buttons on the keyboard, a row each, in place of the page showing,
   the menu open; OVERLAY_CLOSE when it shows already. Up/Down and the wheel go over its rows; any other key, button
   or tap is OVERLAY_CLOSE, back to the game: overlay_other for the keys the menu has no other use for */
/* what F1's page shows besides its rows: the capture key's row while RenderDoc is in the process, and the debugger
   tracing the process ("" for none) */
void overlay_keys_extra(int capture, const char* debugger);
/* whether the mouse key's row shows: the key is the way into an Ounce game's Joy-Con mouse mode (game_mouse at
   Hotkey), or a game holds the mouse in that mode */
void overlay_keys_mouse(int on);
int overlay_keys(void);
int overlay_other(void);
/* a button pressed, as the game reads it (one HidNpadButton bit): the D-pad and the left stick move, A selects, B goes
   back, the rest do nothing but on F1's page. What it did, as overlay_select */
int overlay_pad(u64 button);
/* the pads each poll, the on-screen controls and the connected ones: their buttons and sticks at ms. With the menu
   shown, each button pressed since the last call acts as overlay_pad, the left stick past half way as the D-pad; a
   direction held goes on after SNX_OVERLAY_REPEAT_DELAY ms, every SNX_OVERLAY_REPEAT_MS; a press held as the menu opens
   does not act. What the last one did, or the first that leaves the menu */
#define SNX_OVERLAY_REPEAT_DELAY 400
#define SNX_OVERLAY_REPEAT_MS    120
int overlay_controls(u64 buttons, const i32 sticks[4], u64 ms);
/* the button map page waits for a control pressed on a pad (pad_map.h's): it takes it, and that press is no menu
   key */
int overlay_listening(void);
void overlay_press(int control);
/* The Shortcuts page, the runtime's keys (hotkeys.h) a row each: a row selected waits for a key, the next one pressed
   (with its modifiers; a modifier alone waits on) its key, saved in the game's .ini; Esc or Back, B, a move or a tap
   cancel the wait. A key another row has is refused, said on the page with the key the row keeps. Under ui_lock */
int overlay_shortcut_listening(void);
/* a key pressed while it waits: an SDL_Keycode, hotkeys.h's modifiers */
void overlay_shortcut_key(unsigned key, unsigned mods);
/* Backspace: on the Shortcuts page the row in focus back to its default key, as a key given; elsewhere overlay_back */
int overlay_backspace(void);
/* Delete, and a controller's Y: the setting in focus that the game's picks hold (its value marked *) back to Global,
   the launcher's for every game (options.h's ini_unpick); on the Shortcuts page as Backspace */
int overlay_delete(void);
/* a tap at x, y on a picture of sw x sh: what it did, OVERLAY_CLOSE outside the panel */
int overlay_tap(float x, float y, int sw, int sh);
/* a finger or the mouse (touch.h's SNX_TOUCH_MOUSE) on the open menu, at x, y in the picture's pixels: down off the
   panel on an on-screen control it is the controls', whose presses move the menu (overlay_controls); down on a
   slider it takes the step under it at once and the knob follows it until it lifts, which taps nothing; else, on the
   panel whatever control lies under it, its lift taps the menu where it went down (what that did), or, moved
   further than a finger slips, it scrolls the page as it goes and taps nothing. Reset forgets the one down, which
   lifts without a tap; cancel forgets id's */
void overlay_pointer_down(u64 id, float x, float y, const int safe[4]);
void overlay_pointer_move(u64 id, float x, float y, const int safe[4]);
int overlay_pointer_up(u64 id, float x, float y, int sw, int sh);
void overlay_pointer_cancel(u64 id);
void overlay_pointers_reset(void);
/* the menu, and over the game the notifications until their time is up, at now_ns (plat_ns) */
void overlay_draw(SnxUiList* l, int sw, int sh, u64 now_ns);
/* a notification of one line in the picture's corner for SNX_OVERLAY_TOAST_MS from now_ns: a save state saved or
   loaded, a screenshot taken. After a blank cell, icon (gui/icons.h's), one the launcher draws for the same thing, then
   text; GUI_ICON_NONE for none, which draws text alone, as the 3DS's notice is drawn (common/utilities/notice.h).
   Each one a line of its own, the oldest at the top and each its own time, OVERLAY_TOASTS at most, a new one past
   them taking the oldest's place; one of the same subject takes its line where it is, from its words on: a setting's
   (overlay_setting_toast), its key, another's its icon (a save state's, a controller's). Empty text takes its
   subject's line out. Under ui_lock */
#define SNX_OVERLAY_TOAST_MS SNX_NOTICE_MS
#define OVERLAY_TOASTS       4
void overlay_toast(unsigned icon, const char* text, u64 now_ns);
/* the notification of a setting of the game's .ini changed (its key: fps_limit), after the icon the launcher gives it
   (overlay_setting_icon), its line its key's. Under ui_lock */
void overlay_setting_toast(const char* key, const char* text, u64 now_ns);
/* the icon of the launcher's Settings for a setting by its key: its page's where its page has one, else its group's
   (gui/icons.h's gui_group_icon, of its section in settings.txt); GUI_ICON_NONE for a key no section holds */
unsigned overlay_setting_icon(const char* key);
/* the icon of a runtime key's row on F1's page (hotkeys.h's action), the launcher's F1 box's: the notification of
   what it did shows it (a screenshot's, a pause's); GUI_ICON_NONE for an action with no row */
unsigned overlay_key_icon(int action);
/* A line comes in from its start and goes at its end as the launcher's screens fade in from black and out to it: over
   the fade_in_ms and fade_out_ms of theme (common/utilities/run_theme.h), or of YoRHa's built in for NULL, where the
   launcher's can't be read; 0 for no fade, as it is until told. What it takes into ms, in and out. The alpha of the
   line'th from the top at now_ns, 0 to 255 (255 between the fades, 0 once its time is up or for no such line): what
   the launcher's black leaves of its screen at the same point of a fade as long (run_theme.h's snx_fade_shade), the
   lesser of the two where they meet. While it fades its quads are a faded span of the layer (ui.h's ui_fade_set), the
   same quads at another alpha each frame. Under ui_lock */
void overlay_fades(const SnxRunTheme* theme, u32 ms[2]);
int overlay_toast_alpha(int line, u64 now_ns);
/* a save state's notification (loaded, its slot picked, not saved), after the icon of the menu's Save States, the
   launcher's too. Under ui_lock */
void overlay_state_toast(const char* text, u64 now_ns);
/* the toast of a save state saved to slot (its words, "slot 2"), "Saved over" one that held a state (over) */
void overlay_state_saved(const char* slot, int over, u64 now_ns);
int overlay_toasting(u64 now_ns); /* one shows: from any thread */
/* The settings off their default that change how fast the game runs where nothing on the screen shows it, as the game
   starts (options_perf_notice): notification lines from the game's first frame for SNX_OVERLAY_SETTINGS_MS, a reminder
   that they are not the defaults. game: the game's own keys, after a gamepad; global: the launcher's for every game,
   this run's command line's and how its code was made, after a globe; keys alone between ", ". kind: the values the
   runtime gives this kind of game where no file nor command line gave one (options_kind_value), each "key Word", after
   an info mark and before ", for this kind of game". Any of them "" for none. From any thread; nothing shows before the
   game's first frame, whose present gives the lines (overlay_settings_start) */
#define SNX_OVERLAY_SETTINGS_MS 30000
void overlay_settings_notice(const char* game, const char* global, const char* kind);
/* 1 while a notice waits for the game's first frame: from any thread, the renderer's present, at a relaxed load */
int overlay_settings_waiting(void);
/* the waiting notice's lines, from now_ns, the game's first frame's time. Under ui_lock */
void overlay_settings_start(u64 now_ns);
/* game's, global's and kind's keys as the notice's lines, each line's icon into icon and its text, at most cols letters,
   into text: as many keys a line as fit it, the game's lines first, then global's, then kind's, OVERLAY_SETTINGS_LINES
   in all, one kept for each list after while it has keys. Keys past a list's last line end that line with "+N more", N
   the keys left out; kind's last line ends with ", for this kind of game", its keys narrowed to leave room for it. How
   many lines. One line fewer than the notifications hold, so the one a first run gives as it starts
   (overlay_cold_cache's) takes the room left and never one of these */
#define OVERLAY_SETTINGS_LINES (OVERLAY_TOASTS - 1)
int overlay_settings_lines(const char* game, const char* global, const char* kind, int cols,
                           unsigned icon[OVERLAY_SETTINGS_LINES],
                           char text[OVERLAY_SETTINGS_LINES][OVERLAY_COLS_MAX + 1]);
/* A save state loading for long (state.c, past wait_screen.h's SNX_WAIT_LOADING_MS): the game under it greyed out as the
   launcher greys its screen behind a question (black at half over everything, coming in over OVERLAY_LOADING_FADE_MS
   from shown_ns as its box's shade does), and over it a box in the menu's look, its head the save states' page's, the
   words "Loading the save state", slot under them (its words, "Slot 2") and the launcher's turning ring at their
   right. overlay_loading_done: the load ended well, and it goes with the game's first frame after that
   (overlay_game_drew); overlay_loading_end: it goes now (the load failed, and its notification says why). Under
   ui_lock; overlay_loading_shown from any thread */
#define OVERLAY_LOADING_FADE_MS 150 /* the launcher's GUI_FADE_MS */
void overlay_loading(const char* slot, u64 shown_ns);
void overlay_loading_done(void);
void overlay_loading_end(void);
int overlay_loading_shown(void);
/* the game's frame is shown (the renderer's present, before it makes the frame's layer): a load's box that ended goes.
   Under ui_lock */
void overlay_game_drew(void);
/* overlay_toast from any thread, with or without ui_lock (RenderDoc's notices, gpu/vk/renderdoc.c): its time starts as
   the next frame draws it, the last one given before then shown */
void overlay_notice(unsigned icon, const char* text);
/* The caches of earlier runs found empty as the game starts, which the frames to come wait on: shaders, the shader
   cache (no shader or pipeline of an earlier run), textures, the BC7 cache (no ASTC level encoded before). Its words,
   "Compiling shaders" where the shaders' is, else "Encoding textures"; NULL for neither */
const char* overlay_cold_cache_words(int shaders, int textures);
/* those words as overlay_notice's, once a run, from any thread, after the icon of the stats' line of that work
   (stats.h's SNX_STATS_ICON_COMPILE, SNX_STATS_ICON_ENCODE): 1 when this call gave them. shaders -1, the shader cache
   still being read (gpu/vk's vk_pipeline_cache_cold), gives none yet */
int overlay_cold_cache(int shaders, int textures);
/* The save states' pages: a row per slot, the focused one's thumbnail beside them, and Save, Load and Delete for one
   (Load and Delete asked first). Slot 0 is the one a load saves over first, which only loads or goes */
#define OVERLAY_SLOTS_MAX 33
typedef struct {
    char when[32]; /* when it was saved, and the play time then; "" for an empty slot */
    char why[96];  /* why it can't be loaded, "" when it can */
    u8* thumb;     /* SNX_UI_IMAGE_W x SNX_UI_IMAGE_H RGBA, NULL for none: allocated, the overlay's to free */
} OverlaySlot;
/* the slots listed, n of them from slot 0, taken over, under ui_lock: the frontend's as each page opens and after a
   save or a delete */
void overlay_slots_set(const OverlaySlot* slots, int n);
int overlay_slot(void);        /* the slot the last OVERLAY_SAVE, _LOAD or _DELETE is for */
int overlay_states_shown(void); /* one of the save states' pages shows: the game waits meanwhile */
int overlay_states_stale(void); /* the slots page opened since the last overlay_slots_set: its list is read again */
/* the columns the menu's pages and the stopped panel take at scale on a picture sw wide: as many as three quarters of it
   holds, within OVERLAY_COLS and OVERLAY_COLS_MAX */
int overlay_cols(int sw, int scale);
/* a rectangle of a dialog: its box, a link's rows, an item's row */
typedef struct {
    float x, y, w, h;
} SnxOverlayRect;
/* 1 when x, y falls in r, an empty one taking nothing */
int overlay_error_hit(const SnxOverlayRect* r, float x, float y);

/* The software keyboard's box, for a game that asks the player for text (am.c, through backend_keyboard_show): a
   dialog over the game, the same on every platform, with the game's header, sub and guide texts, the line the player
   types into, how many characters it holds against the game's limits, and the game's OK and Cancel. What the
   window's text input types (a keyboard, an input method, the system's on-screen keyboard) is added to the line up to
   its most; Backspace takes its last character away; Enter, A or a tap on OK gives the line, but one short of its
   least, which the box then says; Esc, B or a tap on Cancel cancels. While it shows and the menu doesn't, it takes
   the keys, the buttons and the fingers, and the menu goes over it. Under ui_lock */
#define OVERLAY_KB_UNITS 1024                       /* the most UTF-16 units a line holds, as the game counts them */
#define OVERLAY_KB_TEXT  (3 * OVERLAY_KB_UNITS + 1) /* its bytes in UTF-8, a 0 after them */
typedef struct {
    char text[OVERLAY_KB_TEXT]; /* the game's initial text, UTF-8: the line starts as it, cut to max */
    /* the game's texts, UTF-8, "" for none: over the line, under it and the OK item's label */
    char header[3 * 64 + 1], sub[3 * 128 + 1], guide[3 * 256 + 1], ok[3 * 8 + 1];
    /* the line's UTF-16 units: the least it is given with, and the most (0, or past OVERLAY_KB_UNITS: that) */
    u32 min, max;
    int password; /* each character drawn as an asterisk */
    int numbers;  /* the game's number pad: a system keyboard of digits */
    u32 ticket;   /* the asker's, handed back with the answer */
} OverlayKeyboard;
/* the box for k, in place of one that shows */
void overlay_keyboard_show(const OverlayKeyboard* k);
/* What the menu's and the box's hints name: the keyboard's keys, or a controller's buttons, as the last of them
   pressed says (the backend's events: a key, or a button, a stick or a trigger past SNX_PAD_TRIGGER; a finger or the
   mouse changes nothing). For a controller, ok and back are the names its face gives the controls that send the
   console's A and B ("B" and "A" on an Xbox pad, "Circle" and "Cross" on a PlayStation one), NULL for "A" and "B";
   OVERLAY_INPUT_NAME bytes each at most. The keyboard's until then, on Android the on-screen controls' (a
   controller's) */
#define OVERLAY_INPUT_NAME 16
void overlay_input_keys(void);
void overlay_input_pad(const char* ok, const char* back);
/* the box for ticket taken away unanswered, as the game takes its keyboard away; another one stays */
void overlay_keyboard_hide(u32 ticket);
int overlay_keyboard_shown(void); /* for any thread */
/* text the window's text input typed, UTF-8: as many of its characters as the line has room for, controls left out */
void overlay_keyboard_type(const char* text);
/* a key on the line: OVERLAY_KEYBOARD once it answers (the box gone), else OVERLAY_NONE */
enum { OVERLAY_KEY_BACKSPACE, OVERLAY_KEY_ENTER, OVERLAY_KEY_CANCEL };
int overlay_keyboard_key(int key);
/* the answer, each taken once: 0 with the line in out (size bytes), 1 cancelled, and the ticket it is for; -1 for none */
int overlay_keyboard_take(u32* ticket, char* out, size_t size);
/* what the window's text input needs while the box shows (1; 0 when it doesn't): the box's ticket, its kind of text,
   and its line on a picture sw x sh, in its pixels, where an input method puts its window */
typedef struct {
    u32 ticket;
    int password, numbers;
    SnxOverlayRect line;
} OverlayTyping;
int overlay_keyboard_typing(int sw, int sh, OverlayTyping* t);

#endif
